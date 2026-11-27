/**
 * @file    app_data.c
 * @brief   全局系统状态中心（g_sys）、控制参数校验与趋势缓存线程安全实现
 * @date    2026-11-27
 * @note    严格遵守短持锁铁律：所有 AppCfg_* 与外设调用均安排在 UnlockState() 之后执行，
 *          彻底杜绝互斥锁重入死锁或持锁执行阻塞 IO
 */

#include "app_data.h"
#include "bsp_st7789.h"
#include "cmsis_os.h"
#include <stddef.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的全局互斥锁句柄 */
extern osMutexId_t sysMutexHandle;

/** @brief 全局系统共享状态实例（位于默认主 RAM） */
static AppState_t g_sys;

/** @brief 历史趋势环形数组（放置于 64 KB CCMRAM，仅 CPU 访问，严禁 DMA） */
static TrendRing_t g_trend __attribute__((section(".ccmram")));

/**
 * @brief  获取全局状态互斥锁（仅在调度器运行后实际加锁）
 */
static void LockState(void)
{
  if ((osKernelGetState() == osKernelRunning) && (sysMutexHandle != NULL))
  {
    (void)osMutexAcquire(sysMutexHandle, osWaitForever);
  }
}

/**
 * @brief  释放全局状态互斥锁
 */
static void UnlockState(void)
{
  if ((osKernelGetState() == osKernelRunning) && (sysMutexHandle != NULL))
  {
    (void)osMutexRelease(sysMutexHandle);
  }
}

/**
 * @brief  将当前控制运行参数同步到持久化配置缓存（必须在已解锁状态下调用）
 */
static void SyncConfigFromSnapshot(void)
{
  int16_t t_start;
  int16_t t_stop;
  uint16_t man_rpm;
  uint8_t bl_pct;

  LockState();
  t_start = g_sys.temp_start_x10;
  t_stop = g_sys.temp_stop_x10;
  man_rpm = g_sys.manual_target_rpm;
  bl_pct = g_sys.backlight_pct;
  UnlockState();

  AppCfg_SyncRuntimeParams(t_start, t_stop, man_rpm, bl_pct);
}

void AppData_Init(const AppConfig_t *cfg, ResetReason_t rst, uint16_t hung_mask)
{
  memset(&g_sys, 0, sizeof(g_sys));

  /* 默认链接脚本不对 .ccmram 做启动清零，此处必须显式 memset 归零 */
  memset(&g_trend, 0, sizeof(g_trend));

  /* 先装载安全默认值，若提供了有效掉电配置再覆盖对应可配参数 */
  g_sys.ctrl_mode = CTRL_MODE_AUTO;
  g_sys.fan_state = FAN_STOP;
  g_sys.manual_target_rpm = 1500U;
  g_sys.temp_start_x10 = 300;
  g_sys.temp_stop_x10 = 270;
  g_sys.backlight_pct = 80U;
  g_sys.ipv4_addr = 0x6401A8C0UL; /* 默认静态 192.168.1.100 */

  if (cfg != NULL)
  {
    g_sys.temp_start_x10 = cfg->temp_start_x10;
    g_sys.temp_stop_x10 = cfg->temp_stop_x10;
    g_sys.manual_target_rpm = cfg->manual_rpm_def;
    g_sys.backlight_pct = cfg->backlight_pct;
    g_sys.ipv4_addr = cfg->static_ip;
  }

  g_sys.reset_reason = (uint16_t)rst;
  g_sys.hung_task_mask = hung_mask;
  g_sys.rtc_unix_ts = Bsp_RtcGetUnixTime();
}

void AppData_ApplyConfig(const AppConfig_t *cfg)
{
  uint8_t bl_pct;

  if (cfg == NULL)
  {
    return;
  }

  LockState();
  g_sys.temp_start_x10 = cfg->temp_start_x10;
  g_sys.temp_stop_x10 = cfg->temp_stop_x10;
  g_sys.manual_target_rpm = cfg->manual_rpm_def;
  g_sys.backlight_pct = cfg->backlight_pct;
  g_sys.ipv4_addr = cfg->static_ip;
  if ((g_sys.ctrl_mode == CTRL_MODE_MANUAL) && (g_sys.manual_run != 0U))
  {
    g_sys.active_target_rpm = g_sys.manual_target_rpm;
  }
  bl_pct = g_sys.backlight_pct;
  UnlockState();

  Bsp_LcdSetBacklight(bl_pct);
}

void AppData_GetSnapshot(AppState_t *out)
{
  if (out == NULL)
  {
    return;
  }

  /* 持锁仅做极速内存拷贝（< 5 us），严禁在锁内执行任何阻塞外设 IO */
  LockState();
  memcpy(out, &g_sys, sizeof(AppState_t));
  UnlockState();
}

void AppData_UpdateSystemTick(uint32_t uptime_sec, uint32_t rtc_unix_ts)
{
  LockState();
  g_sys.uptime_sec = uptime_sec;
  g_sys.rtc_unix_ts = rtc_unix_ts;
  UnlockState();
}

void AppData_UpdateRtcTime(uint32_t rtc_unix_ts)
{
  LockState();
  g_sys.rtc_unix_ts = rtc_unix_ts;
  UnlockState();
}

void AppData_PushTrendPoint(int16_t temp_x10, uint16_t humi_x10, uint16_t rpm)
{
  LockState();
  g_trend.temp_x10[g_trend.write_idx] = temp_x10;
  g_trend.humi_x10[g_trend.write_idx] = humi_x10;
  g_trend.rpm[g_trend.write_idx] = rpm;
  g_trend.write_idx = (uint16_t)((g_trend.write_idx + 1U) % TREND_MAX_POINTS);
  if (g_trend.count < TREND_MAX_POINTS)
  {
    g_trend.count++;
  }
  UnlockState();
}

void AppData_GetTrendCopy(TrendRing_t *out)
{
  if (out == NULL)
  {
    return;
  }

  LockState();
  memcpy(out, &g_trend, sizeof(TrendRing_t));
  UnlockState();
}

AppErr_t AppCtrl_SetMode(uint8_t mode)
{
  if ((mode != CTRL_MODE_AUTO) && (mode != CTRL_MODE_MANUAL))
  {
    return APP_ERR_PARAM;
  }

  LockState();
  g_sys.ctrl_mode = mode;
  /* M2 阶段尚未接入 ControlTask，此处同步目标/状态展示字段便于 HMI 交互验证；M3 改为唤醒 ControlTask */
  if (mode == CTRL_MODE_MANUAL)
  {
    if (g_sys.manual_run != 0U)
    {
      g_sys.fan_state = FAN_MAN_RUN;
      g_sys.active_target_rpm = g_sys.manual_target_rpm;
    }
    else
    {
      g_sys.fan_state = FAN_STOP;
      g_sys.active_target_rpm = 0U;
    }
  }
  else
  {
    g_sys.fan_state = FAN_STOP;
    g_sys.active_target_rpm = 0U;
  }
  UnlockState();

  return APP_OK;
}

AppErr_t AppCtrl_SetManualRun(uint8_t run, uint8_t from_modbus)
{
  if (run > 1U)
  {
    return APP_ERR_PARAM;
  }

  LockState();
  if ((from_modbus != 0U) && (g_sys.ctrl_mode == CTRL_MODE_AUTO))
  {
    UnlockState();
    /* 依据 SRS/SAD：自动模式下拒绝上位机写手动启停（对应 Modbus 异常码 0x04） */
    return APP_ERR_PERM;
  }
  g_sys.manual_run = run;
  if (g_sys.ctrl_mode == CTRL_MODE_MANUAL)
  {
    g_sys.fan_state = (run != 0U) ? FAN_MAN_RUN : FAN_STOP;
    g_sys.active_target_rpm = (run != 0U) ? g_sys.manual_target_rpm : 0U;
  }
  UnlockState();

  return APP_OK;
}

AppErr_t AppCtrl_SetManualRpm(uint16_t rpm)
{
  if ((rpm < APP_FAN_RPM_MIN) || (rpm > APP_FAN_RPM_MAX))
  {
    return APP_ERR_PARAM;
  }

  LockState();
  g_sys.manual_target_rpm = rpm;
  if ((g_sys.ctrl_mode == CTRL_MODE_MANUAL) && (g_sys.manual_run != 0U))
  {
    g_sys.active_target_rpm = rpm;
  }
  UnlockState();

  SyncConfigFromSnapshot();
  return APP_OK;
}

AppErr_t AppCtrl_SetThresholds(int16_t start_x10, int16_t stop_x10)
{
  if ((stop_x10 < APP_TEMP_THRESH_MIN_X10) || (start_x10 > APP_TEMP_THRESH_MAX_X10) ||
      (stop_x10 > APP_TEMP_THRESH_MAX_X10) || (start_x10 < APP_TEMP_THRESH_MIN_X10) ||
      ((start_x10 - stop_x10) < APP_TEMP_HYST_MIN_X10))
  {
    return APP_ERR_PARAM;
  }

  LockState();
  g_sys.temp_start_x10 = start_x10;
  g_sys.temp_stop_x10 = stop_x10;
  UnlockState();

  SyncConfigFromSnapshot();
  return APP_OK;
}

AppErr_t AppCtrl_SetBacklight(uint8_t pct)
{
  if ((pct < APP_BACKLIGHT_MIN_PCT) || (pct > APP_BACKLIGHT_MAX_PCT))
  {
    return APP_ERR_PARAM;
  }

  LockState();
  g_sys.backlight_pct = pct;
  UnlockState();

  /* 解锁后再操作定时器比较寄存器并标记 2 s 防抖写盘 */
  Bsp_LcdSetBacklight(pct);
  SyncConfigFromSnapshot();
  return APP_OK;
}
