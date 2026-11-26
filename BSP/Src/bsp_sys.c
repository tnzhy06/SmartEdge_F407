/**
 * @file    bsp_sys.c
 * @brief   系统基础板级驱动实现（复位原因、RTC/BKP、LED2、PA0 按键、PA4 Flash 隔离）
 * @date    2026-11-26
 * @note    依赖 CubeMX 生成的 hrtc、main.h 引脚宏（SYS_LED/USER_KEY/FLASH_CS_GUARD）
 */

#include "bsp_sys.h"
#include "main.h"
#include "rtc.h"
#include "cmsis_os.h"
#include <stddef.h>

/** @brief 本次上电解析并缓存的复位原因 */
static ResetReason_t s_reset_reason = RESET_REASON_UNKNOWN;

/** @brief 本次 IWDG 复位前从 RTC_BKP_DR3 提取的卡死任务位图 */
static uint16_t s_hung_task_mask = 0U;

/**
 * @brief  挂起 FreeRTOS 任务调度以保护 RTC 原子读写（不关闭硬件中断，保证 TIM6/HAL_GetTick 正常）
 * @return int32_t 先前的内核锁状态；若调度器尚未启动则返回 -1
 */
static int32_t RtcLock(void)
{
  if (osKernelGetState() == osKernelRunning)
  {
    return osKernelLock();
  }
  return -1;
}

/**
 * @brief  恢复 FreeRTOS 任务调度状态
 * @param[in] lock RtcLock() 返回的锁状态
 */
static void RtcUnlock(int32_t lock)
{
  if (lock >= 0)
  {
    (void)osKernelRestoreLock(lock);
  }
}

/**
 * @brief  判断公历年份是否为闰年
 * @param[in] year 公历年份（如 2026）
 * @return uint8_t 1 为闰年，0 为平年
 */
static uint8_t IsLeapYear(uint16_t year)
{
  return (uint8_t)(((year % 4U == 0U) && (year % 100U != 0U)) || (year % 400U == 0U));
}

/**
 * @brief  获取指定年月的当月天数
 * @param[in] year  年份
 * @param[in] month 月份（1..12）
 * @return uint8_t 该月天数（非法月份返回 0）
 */
static uint8_t DaysInMonth(uint16_t year, uint8_t month)
{
  static const uint8_t k_days[12] = {
    31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
  };

  if ((month < 1U) || (month > 12U))
  {
    return 0U;
  }
  if ((month == 2U) && (IsLeapYear(year) != 0U))
  {
    return 29U;
  }
  return k_days[month - 1U];
}

/**
 * @brief  校验日历结构体字段是否全部在合法公历范围内
 * @param[in] dt 待校验的日历结构体指针
 * @return uint8_t 1 合法，0 非法
 */
static uint8_t ValidateDateTime(const BspDateTime_t *dt)
{
  if (dt == NULL)
  {
    return 0U;
  }
  if ((dt->year < 2000U) || (dt->year > 2099U))
  {
    return 0U;
  }
  if ((dt->month < 1U) || (dt->month > 12U))
  {
    return 0U;
  }
  if ((dt->day < 1U) || (dt->day > DaysInMonth(dt->year, dt->month)))
  {
    return 0U;
  }
  if ((dt->hour > 23U) || (dt->min > 59U) || (dt->sec > 59U))
  {
    return 0U;
  }
  return 1U;
}

void Bsp_SysEarlyInit(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* 1. 尽早使能 PWR 时钟并解锁备份域，确保可以读写 RTC 备份寄存器 */
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();

  /* 2. 按明确优先级解析 RCC->CSR 复位标志（POR 常伴随 PINRST，需先判高优先级标志） */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET)
  {
    s_reset_reason = RESET_REASON_IWDG;
  }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != RESET)
  {
    s_reset_reason = RESET_REASON_WWDG;
  }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST) != RESET)
  {
    s_reset_reason = RESET_REASON_SW;
  }
  else if ((__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST) != RESET) ||
           (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST) != RESET))
  {
    s_reset_reason = RESET_REASON_POR_BOR;
  }
  else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST) != RESET)
  {
    s_reset_reason = RESET_REASON_PIN;
  }
  else
  {
    s_reset_reason = RESET_REASON_UNKNOWN;
  }

  /* 读取完毕后必须立即清除硬件复位标志，否则下次重启会残留历史标志 */
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* 3. 仅当本次确为 IWDG 复位时才解析 RTC_BKP_DR3 中的卡死任务位图，并随后清零防误报 */
  /* 注意：此时 MX_RTC_Init() 尚未执行，hrtc.Instance 仍为 NULL，必须通过 Bsp_BkpRead/Write 直接访问 RTC->BKPxR */
  s_hung_task_mask = 0U;
  if (s_reset_reason == RESET_REASON_IWDG)
  {
    uint32_t dr3 = Bsp_BkpRead(RTC_BKP_DR3);
    if ((dr3 & 0xFF000000UL) == BSP_WDT_HUNG_PREFIX)
    {
      s_hung_task_mask = (uint16_t)(dr3 & 0xFFFFUL);
    }
    Bsp_BkpWrite(RTC_BKP_DR3, 0UL);
  }

  /* 将最新复位原因留存至 RTC_BKP_DR1 便于后续调试追踪 */
  Bsp_BkpWrite(RTC_BKP_DR1, (uint32_t)s_reset_reason);

  /* 4. 抢在任何外设初始化前将 PA4 推挽拉高，隔离天空星板载选贴 SPI Flash (U5) */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  HAL_GPIO_WritePin(FLASH_CS_GUARD_GPIO_Port, FLASH_CS_GUARD_Pin, GPIO_PIN_SET);
  gpio.Pin = FLASH_CS_GUARD_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(FLASH_CS_GUARD_GPIO_Port, &gpio);
}

ResetReason_t Bsp_SysGetResetReason(void)
{
  return s_reset_reason;
}

uint16_t Bsp_SysGetHungTaskMask(void)
{
  return s_hung_task_mask;
}

const char *Bsp_SysResetReasonStr(ResetReason_t reason)
{
  switch (reason)
  {
    case RESET_REASON_POR_BOR:
      return "POR/BOR";
    case RESET_REASON_PIN:
      return "NRST";
    case RESET_REASON_IWDG:
      return "IWDG";
    case RESET_REASON_WWDG:
      return "WWDG";
    case RESET_REASON_SW:
      return "SW";
    default:
      return "UNKNOWN";
  }
}

uint8_t Bsp_RtcIsInitialized(void)
{
  return (Bsp_BkpRead(RTC_BKP_DR0) == BSP_RTC_INIT_MAGIC) ? 1U : 0U;
}

AppErr_t Bsp_RtcGetDateTime(BspDateTime_t *dt)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};
  HAL_StatusTypeDef st_time;
  HAL_StatusTypeDef st_date;
  int32_t lock;

  if (dt == NULL)
  {
    return APP_ERR_PARAM;
  }
  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }

  /* 挂起任务切换确保 GetTime + GetDate 原子执行，防止跨任务抢占解锁影子寄存器或读到跨午夜撕裂数据 */
  lock = RtcLock();
  st_time = HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  st_date = HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
  RtcUnlock(lock);

  if ((st_time != HAL_OK) || (st_date != HAL_OK))
  {
    return APP_ERR_IO;
  }

  dt->year = (uint16_t)(2000U + d.Year);
  dt->month = d.Month;
  dt->day = d.Date;
  dt->hour = t.Hours;
  dt->min = t.Minutes;
  dt->sec = t.Seconds;
  return APP_OK;
}

AppErr_t Bsp_RtcSetDateTime(const BspDateTime_t *dt)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};
  HAL_StatusTypeDef st_time;
  HAL_StatusTypeDef st_date;
  int32_t lock;

  if (ValidateDateTime(dt) == 0U)
  {
    return APP_ERR_PARAM;
  }
  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }

  t.Hours = dt->hour;
  t.Minutes = dt->min;
  t.Seconds = dt->sec;
  t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  t.StoreOperation = RTC_STOREOPERATION_RESET;

  d.Year = (uint8_t)(dt->year - 2000U);
  d.Month = dt->month;
  d.Date = dt->day;
  d.WeekDay = RTC_WEEKDAY_MONDAY; /* 本项目业务不使用星期字段，填有效占位值 */

  /* 挂起任务切换保证 SetTime + SetDate + 写 DR0 原子完成，不关中断故 TIM6 时基仍正常计数 */
  lock = RtcLock();
  st_time = HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN);
  st_date = (st_time == HAL_OK) ? HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) : HAL_ERROR;
  if ((st_time == HAL_OK) && (st_date == HAL_OK))
  {
    /* 写入已初始化魔数，确保下次复位执行 MX_RTC_Init() 时不再覆盖当前走时 */
    Bsp_BkpWrite(RTC_BKP_DR0, BSP_RTC_INIT_MAGIC);
  }
  RtcUnlock(lock);

  return ((st_time == HAL_OK) && (st_date == HAL_OK)) ? APP_OK : APP_ERR_IO;
}

uint32_t Bsp_RtcGetUnixTime(void)
{
  BspDateTime_t dt = {0};
  uint32_t days = 0U;

  if (Bsp_RtcGetDateTime(&dt) != APP_OK)
  {
    return 0U;
  }
  /* 二次校验日历合法性，防止硬件异常读出 day==0 导致无符号减 1 下溢 */
  if (ValidateDateTime(&dt) == 0U)
  {
    return 0U;
  }

  /* 累加 1970 年至 dt.year-1 年的完整天数 */
  for (uint16_t y = 1970U; y < dt.year; y++)
  {
    days += (IsLeapYear(y) != 0U) ? 366U : 365U;
  }
  /* 累加当年 1 月至 dt.month-1 月的完整天数 */
  for (uint8_t m = 1U; m < dt.month; m++)
  {
    days += DaysInMonth(dt.year, m);
  }
  days += (uint32_t)(dt.day - 1U);

  return (days * 86400UL) + ((uint32_t)dt.hour * 3600UL) +
         ((uint32_t)dt.min * 60UL) + (uint32_t)dt.sec;
}

uint32_t Bsp_BkpRead(uint32_t dr_idx)
{
  if (dr_idx > RTC_BKP_DR19)
  {
    return 0UL;
  }

  /* 直接通过硬件基址 RTC->BKP0R 读取，不依赖 hrtc.Instance 是否已完成初始化 */
  return *(__IO uint32_t *)((uint32_t)&RTC->BKP0R + (dr_idx * 4UL));
}

void Bsp_BkpWrite(uint32_t dr_idx, uint32_t val)
{
  if (dr_idx > RTC_BKP_DR19)
  {
    return;
  }

  /* 备份域解锁后（PWR->CR.DBP=1）可直接写入 RTC->BKP0R..BKP19R */
  *(__IO uint32_t *)((uint32_t)&RTC->BKP0R + (dr_idx * 4UL)) = val;
}

void Bsp_LedSet(uint8_t on)
{
  HAL_GPIO_WritePin(SYS_LED_GPIO_Port, SYS_LED_Pin,
                    (on != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Bsp_LedToggle(void)
{
  HAL_GPIO_TogglePin(SYS_LED_GPIO_Port, SYS_LED_Pin);
}

uint8_t Bsp_LedIsOn(void)
{
  return (HAL_GPIO_ReadPin(SYS_LED_GPIO_Port, SYS_LED_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

uint8_t Bsp_KeyIsPressed(void)
{
  return (HAL_GPIO_ReadPin(USER_KEY_GPIO_Port, USER_KEY_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

uint8_t Bsp_FlashGuardIsHigh(void)
{
  return (HAL_GPIO_ReadPin(FLASH_CS_GUARD_GPIO_Port, FLASH_CS_GUARD_Pin) == GPIO_PIN_SET)
           ? 1U
           : 0U;
}

void Bsp_SysSoftReset(void)
{
  NVIC_SystemReset();
}
