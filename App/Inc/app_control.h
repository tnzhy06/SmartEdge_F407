/**
 * @file    app_control.h
 * @brief   核心温控状态机与 CAN1 控制主站任务（ControlTask）接口定义
 * @date    2026-11-28
 * @note    负责自动滞回线性调速、传感器失效 1500 RPM 安全降级、手动模式执行、
 *          CAN1 0x101 下发及三类 CAN 故障（堵转/超时/失联）独立判定与恢复
 */

#ifndef APP_CONTROL_H
#define APP_CONTROL_H

/**
 * @brief  立即唤醒 ControlTask 重新评估温控状态机并下发 CAN1 指令
 * @note   由 AppCtrl_Set*、SensorTask 与 AppData_ApplyConfig 在解锁后调用，降低控制延迟
 */
void AppControl_NotifyUpdate(void);

/**
 * @brief  仅原子置位 CAN 超时基准重置请求标志（不调用任何 RTOS API，可在调度锁内安全调用）
 * @note   配合 AppCfg 在擦写 Sector 11 后解锁前抢先置位，杜绝低优先级 CliTask 擦写完瞬间被
 *         高优先级 ControlTask 抢占而错过重置窗口
 */
void AppControl_MarkCanTimerReset(void);

/**
 * @brief  重置 CAN 心跳/应答/堵转计时基准并唤醒 ControlTask（供 Sector 11 擦写结束后调用）
 * @note   严格遵循 AGENTS.md 第 5 节红线 10：抵消擦除 128 KB Sector 11 导致的 1~2 s 取指暂停，
 *         防止配置存盘误触发 FAULT_CAN_NODE_OFFLINE 或 FAULT_CAN_CMD_TIMEOUT
 */
void AppControl_ResetCanTimers(void);

/**
 * @brief  核心温控与 CAN1 主站任务主入口（由 Core/Src/freertos.c StartControlTask 调用，永不返回）
 */
void App_ControlTask(void);

#endif /* APP_CONTROL_H */
