/**
 * @file    app_monitor.c
 * @brief   系统监控任务（defaultTask）M3 阶段实现
 * @date    2026-11-28
 * @note    每 500 ms 翻转 LED2（1 Hz 心跳）；每 1 s 更新运行秒数并轮询 2 s 防抖配置写盘；
 *          每 10 s 采集一条快照写入 CCMRAM 历史趋势数组供 Trend 页面展示
 */

#include "app_monitor.h"
#include "app_cfg.h"
#include "app_data.h"
#include "bsp_sys.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

/** @brief 监控任务周期（500 ms 翻转 LED2 -> 周期 1 s = 1 Hz 闪烁） */
#define MONITOR_LOOP_MS 500U

/** @brief 历史趋势自动采样周期（单位：秒） */
#define MONITOR_TREND_PERIOD_S 10U

/** @brief 监控任务独享静态状态快照（避免在 1.5 KB 栈的 defaultTask 栈帧上分配大结构体） */
static AppState_t s_mon_snap;

/**
 * @brief  打印系统上电启动横幅（含固件阶段、复位原因、RTC 时间、配置来源与内存信息）
 */
static void PrintBootBanner(void)
{
  BspDateTime_t dt = {0};
  AppErr_t rtc_err;

  AppData_GetSnapshot(&s_mon_snap);
  rtc_err = Bsp_RtcGetDateTime(&dt);

  Bsp_CliPrintf("\r\n========================================\r\n");
  Bsp_CliPrintf(" SmartEdge_F407 [M3: RS-485 + Dual CAN]\r\n");
  Bsp_CliPrintf(" Reset Reason : %s (code=%u, hung=0x%02X)\r\n",
                Bsp_SysResetReasonStr((ResetReason_t)s_mon_snap.reset_reason),
                (unsigned int)s_mon_snap.reset_reason,
                (unsigned int)s_mon_snap.hung_task_mask);
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
  Bsp_CliPrintf(" Config Src   : %s | T=%d.%d/%d.%d C | BL=%u%% | Cal=%u\r\n",
                (AppCfg_IsLoadedFromFlash() != 0U) ? "Sector11" : "DEFAULT",
                (int)(s_mon_snap.temp_start_x10 / 10), (int)(s_mon_snap.temp_start_x10 % 10),
                (int)(s_mon_snap.temp_stop_x10 / 10), (int)(s_mon_snap.temp_stop_x10 % 10),
                (unsigned int)s_mon_snap.backlight_pct,
                (unsigned int)AppCfg_IsTouchCalibrated());
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

      /* 每秒检查一次 2 s 防抖配置写盘请求（写盘耗时 1~2 s，仅在有修改时触发） */
      (void)AppCfg_PollSave();

      /* 每 10 s 推送一个快照点至 CCMRAM 历史趋势环形缓存 */
      if ((uptime_sec % MONITOR_TREND_PERIOD_S) == 0U)
      {
        AppData_GetSnapshot(&s_mon_snap);
        AppData_PushTrendPoint(s_mon_snap.temp_x10, s_mon_snap.humi_x10, s_mon_snap.actual_rpm);
      }
    }
  }
}
