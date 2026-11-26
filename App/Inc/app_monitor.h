/**
 * @file    app_monitor.h
 * @brief   系统监控任务（defaultTask）接口定义
 * @date    2026-11-26
 * @note    M1 阶段负责启动日志打印、LED2 1 Hz 心跳闪烁与系统运行秒数/RTC 更新
 */

#ifndef APP_MONITOR_H
#define APP_MONITOR_H

/**
 * @brief  系统监控任务主循环（由 freertos.c 的 StartDefaultTask 直接调用）
 * @note   本函数永不返回；M1 阶段暂不启动 IWDG，避免影响单步断点调试
 */
void App_MonitorTask(void);

#endif /* APP_MONITOR_H */
