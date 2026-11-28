/**
 * @file    bsp_can.h
 * @brief   STM32F407 双路物理 CAN（CAN1 控制主站 + CAN2 模拟风扇节点）板级驱动接口
 * @date    2026-11-28
 * @note    严格对齐 SDD 第 3.3 / 4.4 节：CAN1 Bank 0 接收 0x201/0x202/0x701 -> FIFO0，
 *          CAN2 Bank 14 (SlaveStartFilterBank=14) 接收 0x101 -> FIFO1；不反向依赖 App 业务模块
 */

#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>
#include "app_err.h"

/**
 * @brief CAN 标准帧队列传输结构体（精确 16 字节，与 CubeMX can1RxQueue/can2RxQueue 对齐）
 */
typedef struct
{
  uint32_t std_id; /**< 11-bit 标准帧 ID（0x000..0x7FF） */
  uint8_t dlc;     /**< 有效载荷长度（0..8，项目协议固定 8） */
  uint8_t data[8]; /**< 原始 8 字节载荷数据 */
  uint8_t pad[3];  /**< 4 字节结构对齐填充位 */
} CanFrame_t;

_Static_assert(sizeof(CanFrame_t) == 16U, "CanFrame_t must be 16 bytes");

/**
 * @brief  配置 CAN1/CAN2 硬件滤波器、启动双通道正常模式并开启 RX0/RX1 接收中断
 * @return AppErr_t APP_OK 启动成功；APP_ERR_IO 滤波器配置、启动或中断使能失败
 * @note   严格遵循红线 8：先确保 CAN1 时钟使能并在 CAN1 上配置 SlaveStartFilterBank=14
 */
AppErr_t Bsp_CanInit(void);

/**
 * @brief  查询双路 CAN 控制器是否已完成滤波器配置并启动就绪
 * @return uint8_t 1 已就绪，0 尚未就绪
 */
uint8_t Bsp_CanIsReady(void);

/**
 * @brief  通过 CAN1（控制主站）发送 1 帧 8 字节标准数据帧
 * @param[in] std_id 11-bit 标准帧 ID（<= 0x7FF）
 * @param[in] data   8 字节发送载荷指针（非空）
 * @return AppErr_t  APP_OK 入邮箱成功；APP_ERR_PARAM 参数非法；
 *                   APP_ERR_NOT_RDY 未初始化；APP_ERR_BUSY 3 个发送邮箱全满；APP_ERR_IO 硬件失败
 * @note   发送前强制检查空闲发送邮箱数，防总线离线时死等卡死
 */
AppErr_t Bsp_Can1Send(uint32_t std_id, const uint8_t data[8]);

/**
 * @brief  通过 CAN2（模拟风扇节点）发送 1 帧 8 字节标准数据帧
 * @param[in] std_id 11-bit 标准帧 ID（<= 0x7FF）
 * @param[in] data   8 字节发送载荷指针（非空）
 * @return AppErr_t  APP_OK 入邮箱成功；APP_ERR_PARAM 参数非法；
 *                   APP_ERR_NOT_RDY 未初始化；APP_ERR_BUSY 3 个发送邮箱全满；APP_ERR_IO 硬件失败
 */
AppErr_t Bsp_Can2Send(uint32_t std_id, const uint8_t data[8]);

/**
 * @brief CAN1/CAN2 硬件寄存器与收发统计诊断快照（供 CLI bus 命令排查物理总线问题）
 */
typedef struct
{
  uint32_t esr;        /**< 错误状态寄存器（含 TEC/REC/LEC/BOFF） */
  uint32_t msr;        /**< 主状态寄存器（bit11=RX 引脚实时电平，bit0=INAK） */
  uint32_t tsr;        /**< 发送状态寄存器（TME/TXOK/TERR） */
  uint32_t tx_ok_cnt;  /**< 成功写入硬件发送邮箱的累计次数 */
  uint32_t tx_err_cnt; /**< 邮箱满或发送失败的累计次数 */
  uint32_t rx_irq_cnt; /**< 接收 FIFO 中断触发累计次数 */
} BspCanChDiag_t;

/**
 * @brief  读取中断接收队列溢出累计丢帧数（供诊断观测）
 * @return uint32_t 累计丢帧计数
 */
uint32_t Bsp_CanGetRxDropCount(void);

/**
 * @brief  读取 CAN1 与 CAN2 的硬件寄存器和中断收发统计快照（内部临界区防跨字段错拍）
 * @param[out] can1_out CAN1 诊断输出结构体指针（可为 NULL）
 * @param[out] can2_out CAN2 诊断输出结构体指针（可为 NULL）
 * @warning 禁止在中断上下文调用
 */
void Bsp_CanGetDiag(BspCanChDiag_t *can1_out, BspCanChDiag_t *can2_out);

/**
 * @brief  CAN1 FIFO0 收到报文中断分发回调（由 Core/Src/can.c 调用）
 */
void Bsp_CanOnRxFifo0Pending(void);

/**
 * @brief  CAN2 FIFO1 收到报文中断分发回调（由 Core/Src/can.c 调用）
 */
void Bsp_CanOnRxFifo1Pending(void);

#endif /* BSP_CAN_H */
