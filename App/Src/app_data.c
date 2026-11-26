/**
 * @file    app_data.c
 * @brief   全局系统状态中心（g_sys）线程安全读写实现
 * @date    2026-11-26
 * @note    使用 CubeMX 在 freertos.c 中创建的 sysMutexHandle 进行短锁保护
 */

#include "app_data.h"
#include "cmsis_os.h"
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

void AppData_Init(ResetReason_t rst, uint16_t hung_mask)
{
  memset(&g_sys, 0, sizeof(g_sys));

  /* 默认链接脚本不对 .ccmram 做启动清零，此处必须显式 memset 归零 */
  memset(&g_trend, 0, sizeof(g_trend));

  /* 按 SRS/SAD 基线装载安全默认值（M5 接入 Sector 11 后再叠加掉电配置） */
  g_sys.ctrl_mode = CTRL_MODE_AUTO;
  g_sys.fan_state = FAN_STOP;
  g_sys.manual_target_rpm = 1500U;
  g_sys.temp_start_x10 = 300; /* 默认 30.0 °C 启动 */
  g_sys.temp_stop_x10 = 270;  /* 默认 27.0 °C 停止 */
  g_sys.backlight_pct = 80U;
  g_sys.reset_reason = (uint16_t)rst;
  g_sys.hung_task_mask = hung_mask;
  g_sys.rtc_unix_ts = Bsp_RtcGetUnixTime();
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
