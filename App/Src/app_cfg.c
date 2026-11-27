/**
 * @file    app_cfg.c
 * @brief   Flash Sector 11 掉电持久化配置管理模块实现
 * @date    2026-11-27
 * @note    使用标准 IEEE 802.3 CRC32（0xEDB88320）校验前 124 字节；
 *          访问 s_cfg 仅短持锁拷贝，真正擦写 Sector 11 时完全不持任何互斥锁
 */

#include "app_cfg.h"
#include "bsp_flash.h"
#include "bsp_uart_cli.h"
#include "bsp_xpt2046.h"
#include "cmsis_os.h"
#include "stm32f4xx_hal.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的全局互斥锁句柄（短持保护配置副本） */
extern osMutexId_t sysMutexHandle;

/** @brief 当前内存中的配置缓存 */
static AppConfig_t s_cfg;

/** @brief Flash 写盘专用静态快照缓冲与并发保护标志（避免在 1.5 KB 栈的 defaultTask 上分配 128 B 局部结构体） */
static AppConfig_t s_save_buf;
static uint8_t s_saving = 0U;

/** @brief 强制写盘（FlushNow）等待前一次擦写结束的最长超时（单位：ms，覆盖单次 1~2 s 扇区擦除） */
#define APP_CFG_BUSY_WAIT_MS 2500U

/** @brief 是否存在尚未落盘的配置修改（1=待保存） */
static uint8_t s_dirty = 0U;

/** @brief 最近一次配置变更的系统毫秒时间戳（用于 2 s 防抖合并） */
static uint32_t s_dirty_tick = 0U;

/** @brief 本次开机是否成功从 Sector 11 装载了历史配置 */
static uint8_t s_loaded_from_flash = 0U;

/**
 * @brief  短持全局锁（仅用于微秒级内存拷贝）
 */
static void LockCfg(void)
{
  if ((osKernelGetState() == osKernelRunning) && (sysMutexHandle != NULL))
  {
    (void)osMutexAcquire(sysMutexHandle, osWaitForever);
  }
}

/**
 * @brief  释放全局锁
 */
static void UnlockCfg(void)
{
  if ((osKernelGetState() == osKernelRunning) && (sysMutexHandle != NULL))
  {
    (void)osMutexRelease(sysMutexHandle);
  }
}

/**
 * @brief  计算标准 IEEE 802.3 CRC32（多项式 0xEDB88320，初值/终值异或 0xFFFFFFFF）
 * @param[in] data 输入数据首地址
 * @param[in] len  数据字节数
 * @return uint32_t 32 位 CRC 校验值
 */
static uint32_t CalcCrc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;

  for (uint32_t i = 0UL; i < len; i++)
  {
    crc ^= (uint32_t)data[i];
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      if ((crc & 1UL) != 0UL)
      {
        crc = (crc >> 1) ^ 0xEDB88320UL;
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

/**
 * @brief  校验从 Flash 读出的配置结构体头、CRC32 与关键业务字段边界
 * @param[in] cfg 待校验的配置结构体指针
 * @return uint8_t 1 合法，0 损坏或越界
 */
static uint8_t ValidateConfig(const AppConfig_t *cfg)
{
  uint32_t calc_crc;

  if (cfg == NULL)
  {
    return 0U;
  }
  if ((cfg->magic != APP_CFG_MAGIC) || (cfg->version != APP_CFG_VERSION))
  {
    return 0U;
  }

  calc_crc = CalcCrc32((const uint8_t *)cfg, (uint32_t)offsetof(AppConfig_t, crc32));
  if (calc_crc != cfg->crc32)
  {
    return 0U;
  }

  /* 核查业务安全约束：阈值范围 [0..70 °C] 且最小死区 >= 1.0 °C，转速 800..3000，背光 10..100 */
  if ((cfg->temp_stop_x10 < 0) || (cfg->temp_start_x10 > 700) ||
      ((cfg->temp_start_x10 - cfg->temp_stop_x10) < 10))
  {
    return 0U;
  }
  if ((cfg->manual_rpm_def < 800U) || (cfg->manual_rpm_def > 3000U))
  {
    return 0U;
  }
  if ((cfg->backlight_pct < 10U) || (cfg->backlight_pct > 100U))
  {
    return 0U;
  }

  /* 校验触摸仿射浮点系数为有限实数（排除 NaN / Inf） */
  for (uint8_t i = 0U; i < 6U; i++)
  {
    if (!isfinite(cfg->touch_cal[i]))
    {
      return 0U;
    }
  }

  return 1U;
}

/**
 * @brief  无锁状态执行 Sector 11 落盘（先在短锁内拷贝快照并清脏标记，解锁后再擦写 Flash）
 * @param[in] force 1=无论是否脏均立即写入，0=仅当脏且满 2 s 防抖时写入
 * @return AppErr_t APP_OK 无需写入或写入成功；APP_ERR_BUSY 前次写盘尚在进行或等待超时；APP_ERR_IO Flash 写入失败
 */
static AppErr_t SaveInternal(uint8_t force)
{
  uint8_t need_write = 0U;
  uint32_t now = HAL_GetTick();
  AppErr_t err;

  /* 防止 defaultTask 与 CliTask 并发进入擦写流程争用静态 s_save_buf */
  if (__atomic_exchange_n(&s_saving, 1U, __ATOMIC_ACQUIRE) != 0U)
  {
    uint32_t wait_start = HAL_GetTick();
    if (force == 0U)
    {
      return APP_ERR_BUSY;
    }
    /* 强制刷盘（如 cfg save/reset 或 OTA 前）有界等待前次擦写结束，避免并发撞车直接失败 */
    while (__atomic_exchange_n(&s_saving, 1U, __ATOMIC_ACQUIRE) != 0U)
    {
      if ((HAL_GetTick() - wait_start) >= APP_CFG_BUSY_WAIT_MS)
      {
        return APP_ERR_BUSY;
      }
      if (osKernelGetState() == osKernelRunning)
      {
        (void)osDelay(10U);
      }
    }
  }

  LockCfg();
  if ((force != 0U) ||
      ((s_dirty != 0U) && ((now - s_dirty_tick) >= APP_CFG_SAVE_DEBOUNCE_MS)))
  {
    s_save_buf = s_cfg;
    s_dirty = 0U;
    need_write = 1U;
  }
  UnlockCfg();

  if (need_write == 0U)
  {
    __atomic_store_n(&s_saving, 0U, __ATOMIC_RELEASE);
    return APP_OK;
  }

  s_save_buf.magic = APP_CFG_MAGIC;
  s_save_buf.version = APP_CFG_VERSION;
  s_save_buf.crc32 = CalcCrc32((const uint8_t *)&s_save_buf, (uint32_t)offsetof(AppConfig_t, crc32));

  /* 绝对不能在持锁期间擦写 Sector 11（耗时 1~2 s） */
  err = Bsp_FlashEraseWriteSector11(&s_save_buf, (uint32_t)sizeof(s_save_buf));
  if (err == APP_OK)
  {
    LockCfg();
    s_cfg.crc32 = s_save_buf.crc32;
    UnlockCfg();
    Bsp_CliPrintf("[CFG] Saved to Sector 11 (T=%d.%d/%d.%d C, BL=%u%%, Cal=%u)\r\n",
                  (int)(s_save_buf.temp_start_x10 / 10), (int)(s_save_buf.temp_start_x10 % 10),
                  (int)(s_save_buf.temp_stop_x10 / 10), (int)(s_save_buf.temp_stop_x10 % 10),
                  (unsigned int)s_save_buf.backlight_pct,
                  ((s_save_buf.flags & APP_CFG_FLAG_TOUCH_CAL) != 0U) ? 1U : 0U);
  }
  else
  {
    /* 写盘失败时恢复脏标志，待下个周期重试 */
    LockCfg();
    s_dirty = 1U;
    s_dirty_tick = HAL_GetTick();
    UnlockCfg();
    Bsp_CliPrintf("[CFG] ERR: Sector 11 save failed (%d)\r\n", (int)err);
  }

  __atomic_store_n(&s_saving, 0U, __ATOMIC_RELEASE);
  return err;
}

void AppCfg_LoadDefault(AppConfig_t *cfg)
{
  if (cfg == NULL)
  {
    return;
  }

  memset(cfg, 0, sizeof(*cfg));
  cfg->magic = APP_CFG_MAGIC;
  cfg->version = APP_CFG_VERSION;
  cfg->flags = 0U; /* 默认静态 IP，触摸未校准 */
  cfg->temp_start_x10 = 300;
  cfg->temp_stop_x10 = 270;
  cfg->manual_rpm_def = 1500U;
  cfg->backlight_pct = 80U;
  cfg->rs485_slave_id = 0x01U;
  /* 默认网络字节序 IP：192.168.1.100 / 255.255.255.0 / 192.168.1.1 */
  cfg->static_ip = 0x6401A8C0UL;
  cfg->static_mask = 0x00FFFFFFUL;
  cfg->static_gw = 0x0101A8C0UL;
  cfg->ntp_ip = 0UL;
  Bsp_TouchGetDefaultCal(cfg->touch_cal);
  cfg->crc32 = CalcCrc32((const uint8_t *)cfg, (uint32_t)offsetof(AppConfig_t, crc32));
}

uint8_t AppCfg_Init(void)
{
  AppConfig_t flash_cfg;

  s_dirty = 0U;
  s_dirty_tick = 0U;
  s_loaded_from_flash = 0U;

  if ((Bsp_FlashRead(BSP_FLASH_SECTOR11_ADDR, &flash_cfg, (uint32_t)sizeof(flash_cfg)) == APP_OK) &&
      (ValidateConfig(&flash_cfg) != 0U))
  {
    s_cfg = flash_cfg;
    s_loaded_from_flash = 1U;
    return 1U;
  }

  /* Sector 11 为空（全 0xFF）或校验失败时安全回退默认参数 */
  AppCfg_LoadDefault(&s_cfg);
  return 0U;
}

void AppCfg_GetCurrent(AppConfig_t *out)
{
  if (out == NULL)
  {
    return;
  }

  LockCfg();
  *out = s_cfg;
  UnlockCfg();
}

uint8_t AppCfg_IsLoadedFromFlash(void)
{
  return s_loaded_from_flash;
}

uint8_t AppCfg_IsTouchCalibrated(void)
{
  uint16_t flags;

  LockCfg();
  flags = s_cfg.flags;
  UnlockCfg();

  return ((flags & APP_CFG_FLAG_TOUCH_CAL) != 0U) ? 1U : 0U;
}

void AppCfg_SyncRuntimeParams(int16_t start_x10, int16_t stop_x10,
                              uint16_t manual_rpm_def, uint8_t backlight_pct)
{
  LockCfg();
  s_cfg.temp_start_x10 = start_x10;
  s_cfg.temp_stop_x10 = stop_x10;
  s_cfg.manual_rpm_def = manual_rpm_def;
  s_cfg.backlight_pct = backlight_pct;
  s_dirty = 1U;
  s_dirty_tick = HAL_GetTick();
  UnlockCfg();
}

void AppCfg_UpdateTouchCal(const float cal[6])
{
  if (cal == NULL)
  {
    return;
  }

  LockCfg();
  memcpy(s_cfg.touch_cal, cal, sizeof(s_cfg.touch_cal));
  s_cfg.flags |= APP_CFG_FLAG_TOUCH_CAL;
  s_dirty = 1U;
  s_dirty_tick = HAL_GetTick();
  UnlockCfg();
}

void AppCfg_MarkDirty(void)
{
  LockCfg();
  s_dirty = 1U;
  s_dirty_tick = HAL_GetTick();
  UnlockCfg();
}

AppErr_t AppCfg_PollSave(void)
{
  return SaveInternal(0U);
}

AppErr_t AppCfg_FlushNow(void)
{
  return SaveInternal(1U);
}

void AppCfg_ApplyDefaultMemory(AppConfig_t *out_def)
{
  AppConfig_t def;

  AppCfg_LoadDefault(&def);
  LockCfg();
  s_cfg = def;
  s_dirty = 1U;
  s_dirty_tick = HAL_GetTick();
  UnlockCfg();

  /* 同步恢复默认触摸校准矩阵 */
  Bsp_TouchSetCal(def.touch_cal);

  if (out_def != NULL)
  {
    *out_def = def;
  }
}

AppErr_t AppCfg_ResetToDefault(void)
{
  AppCfg_ApplyDefaultMemory(NULL);
  return SaveInternal(1U);
}
