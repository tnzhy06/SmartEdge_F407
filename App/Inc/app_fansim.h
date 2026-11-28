/**
 * @file    app_fansim.h
 * @brief   CAN2 独立模拟风扇节点任务（FanSimTask）与 CLI 故障注入接口定义
 * @date    2026-11-28
 * @note    严格对齐 SDD 6.4 节：接收 CAN2 0x101 控制帧，回复 0x201 应答，
 *          周期上报 0x202 状态与 0x701 心跳，并支持堵转/超时/失联三类故障模拟注入
 */

#ifndef APP_FANSIM_H
#define APP_FANSIM_H

#include <stdint.h>

/** @brief 模拟风扇节点故障注入位掩码 */
#define SIM_INJ_STALL   (1U << 0) /**< bit0: 模拟风扇堵转（转速拉 0、上报堵转位，应答与心跳照常） */
#define SIM_INJ_TIMEOUT (1U << 1) /**< bit1: 模拟命令应答超时（仅丢弃 0x201，状态与心跳照常） */
#define SIM_INJ_OFFLINE (1U << 2) /**< bit2: 模拟节点失联（停止发送 0x201/0x202/0x701 全部帧） */
#define SIM_INJ_ALL     (SIM_INJ_STALL | SIM_INJ_TIMEOUT | SIM_INJ_OFFLINE)

/**
 * @brief  设置模拟风扇节点故障注入位掩码（线程安全，供 CliTask 调用）
 * @param[in] mask SIM_INJ_* 按位或组合（传 0 清除全部故障）
 */
void AppFanSim_SetInjectMask(uint8_t mask);

/**
 * @brief  读取当前模拟风扇节点故障注入位掩码（线程安全）
 * @return uint8_t 当前激活的 SIM_INJ_* 位掩码
 */
uint8_t AppFanSim_GetInjectMask(void);

/**
 * @brief  CAN2 模拟风扇节点任务主入口（由 Core/Src/freertos.c StartFanSimTask 调用，永不返回）
 */
void App_FanSimTask(void);

#endif /* APP_FANSIM_H */
