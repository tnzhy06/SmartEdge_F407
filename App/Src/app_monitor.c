/**
 * @file    app_monitor.c
 * @brief   系统监控任务（defaultTask）M1 阶段实现
 * @date    2026-11-26
 * @note    每 500 ms 翻转一次板载 LED2（实现 1 Hz 闪烁），每 1000 ms 更新系统运行时间
 */

#include "app_monitor.h"
#include "app_data.h"
#include "bsp_sys.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

/** @brief 监控任务周期（500 ms 翻转 LED2 -> 周期 1 s = 1 Hz 闪烁） */
#define MONITOR_LOOP_MS 500U

/**
 * @brief  打印系统上电启动横幅（含固件阶段、复位原因、RTC 时间与内存信息）
 */
static void PrintBootBanner(void)
{
  AppState_t snap;
  BspDateTime_t dt = {0};
  AppErr_t rtc_err;

  AppData_GetSnapshot(&snap);
  rtc_err = Bsp_RtcGetDateTime(&dt);

  Bsp_CliPrintf("\r\n========================================\r\n");
  Bsp_CliPrintf(" SmartEdge_F407 [M1: Core + CLI + RTC]\r\n");
  Bsp_CliPrintf(" Reset Reason : %s (code=%u, hung=0x%02X)\r\n",
                Bsp_SysResetReasonStr((ResetReason_t)snap.reset_reason),
                (unsigned int)snap.reset_reason,
                (unsigned int)snap.hung_task_mask);
  if (rtc_err == APP_OK)
  {
    Bsp_CliPrintf(" RTC Time     : %04u-%02u-%02u %02u:%02u:%02u (%s)\r\n",
                  (unsigned int)dt.year, (unsigned int)dt.month, (unsigned int)dt.day,
                  (unsigned int)dt.hour, (unsigned int)dt.min, (unsigned int)dt.sec,
                  (Bsp_RtcIsInitialized() != 0U) ? "CALIBRATED" : "DEFAULT");
  }
  else
  {
    Bsp_CliPrintf(" RTC Time     : READ_ERR (%d)\r\n", (int)rtc_err);
  }
  Bsp_CliPrintf(" PA4 Flash CS : %s | FreeRTOS Heap: %lu B\r\n",
                (Bsp_FlashGuardIsHigh() != 0U) ? "HIGH (SAFE)" : "LOW (WARN)",
                (unsigned long)xPortGetFreeHeapSize());
  Bsp_CliPrintf(" Type 'help' for available CLI commands.\r\n");
  Bsp_CliPrintf("========================================\r\n> ");
}

void App_MonitorTask(void)
{
  uint32_t half_sec_cnt = 0U;
  uint32_t uptime_sec = 0U;

  /* 稍等 100 ms 让串口适配器与 CliTask 就绪，再打印开机横幅 */
  osDelay(100U);
  PrintBootBanner();

  for (;;)
  {
    osDelay(MONITOR_LOOP_MS);

    /* 每 500 ms 翻转板载 LED2，人眼可见稳定 1 Hz 心跳灯 */
    Bsp_LedToggle();

    half_sec_cnt++;
    if ((half_sec_cnt & 1U) == 0U)
    {
      uptime_sec++;
      AppData_UpdateSystemTick(uptime_sec, Bsp_RtcGetUnixTime());
    }
  }
}
