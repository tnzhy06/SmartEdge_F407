/**
 * @file    app_sensor.h
 * @brief   RS-485 温湿度采集任务（SensorTask）接口定义
 * @date    2026-11-28
 * @note    周期 1000 ms 轮询 KTH2-R 保持寄存器 0x0300~0x0301，连续 3 次失败置位 FAULT_SENSOR_OFFLINE
 */

#ifndef APP_SENSOR_H
#define APP_SENSOR_H

/**
 * @brief  温湿度采集任务主入口（由 Core/Src/freertos.c StartSensorTask 调用，永不返回）
 */
void App_SensorTask(void);

#endif /* APP_SENSOR_H */
