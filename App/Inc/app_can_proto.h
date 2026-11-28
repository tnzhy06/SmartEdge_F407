/**
 * @file    app_can_proto.h
 * @brief   双路 CAN 4 帧标准协议（0x101/0x201/0x202/0x701）编解码接口定义
 * @date    2026-11-28
 * @note    严格对齐 SAD 5.2 节与 SDD 5.5 节：8 字节小端载荷，Byte 7 为 Byte 0..6 异或校验
 */

#ifndef APP_CAN_PROTO_H
#define APP_CAN_PROTO_H

#include <stdint.h>
#include "app_err.h"

/** @brief 4 类标准 11-bit CAN 帧 ID */
#define CAN_ID_CTRL_CMD   0x101U /**< CAN1 -> CAN2：风扇控制命令帧 */
#define CAN_ID_CMD_ACK    0x201U /**< CAN2 -> CAN1：命令执行应答帧 */
#define CAN_ID_FAN_STATUS 0x202U /**< CAN2 -> CAN1：风扇运行状态周期帧（200 ms） */
#define CAN_ID_FAN_HB     0x701U /**< CAN2 -> CAN1：模拟风扇节点心跳帧（500 ms） */

/** @brief 0x101 控制命令模式字段编码 */
#define CAN_CMD_MODE_AUTO     0U /**< 自动模式 */
#define CAN_CMD_MODE_MANUAL   1U /**< 手动模式 */
#define CAN_CMD_MODE_FAILSAFE 2U /**< 传感器失效安全降级模式 */

/** @brief 0x201 命令应答结果码 */
#define CAN_ACK_OK    0U /**< 命令接收并执行正常 */
#define CAN_ACK_STALL 2U /**< 节点处于堵转故障态 */

/** @brief 0x202 节点运行状态与故障位定义 */
#define CAN_NODE_STATE_STOP  0U        /**< 停转 */
#define CAN_NODE_STATE_RUN   1U        /**< 正常运转 */
#define CAN_NODE_STATE_STALL 2U        /**< 堵转告警 */
#define CAN_NODE_FAULT_STALL (1U << 0) /**< bit0：风扇堵转标志 */

/** @brief 0x701 心跳在线状态码 */
#define CAN_HB_STATE_ONLINE 0x01U

/** @brief 0x101 控制命令帧业务结构体 */
typedef struct
{
  uint8_t seq;         /**< 命令序号（1..255 循环，跳过 0） */
  uint8_t enable;      /**< 启停命令（0=停，1=启） */
  uint16_t target_rpm; /**< 目标转速（0 或 800..3000 RPM） */
  uint16_t mode;       /**< 控制模式（CAN_CMD_MODE_*） */
} CanCmd101_t;

/** @brief 0x201 命令应答帧业务结构体 */
typedef struct
{
  uint8_t echo_seq;      /**< 回显收到的 0x101 seq */
  uint8_t ack;           /**< 执行结果码（CAN_ACK_OK / CAN_ACK_STALL） */
  uint16_t accepted_rpm; /**< 节点接受的目标转速（RPM） */
  uint16_t state;        /**< 节点当前运行状态（CAN_NODE_STATE_*） */
} CanAck201_t;

/** @brief 0x202 运行状态帧业务结构体 */
typedef struct
{
  uint8_t node_state;  /**< 节点状态（CAN_NODE_STATE_*） */
  uint8_t fault_bits;  /**< 节点内部故障位图（CAN_NODE_FAULT_*） */
  uint16_t actual_rpm; /**< 当前模拟实测转速（RPM） */
  uint16_t target_rpm; /**< 节点当前目标转速（RPM） */
  uint8_t cnt;         /**< 状态帧递增序号（0..255） */
} CanStat202_t;

/** @brief 0x701 节点心跳帧业务结构体 */
typedef struct
{
  uint8_t hb_state;   /**< 在线状态码（固定 CAN_HB_STATE_ONLINE=0x01） */
  uint8_t alive_cnt;  /**< 心跳递增存活计数（0..255） */
  uint16_t uptime_s;  /**< 节点运行秒数低 16 位 */
  uint16_t err_flags; /**< 节点错误/注入标志位图 */
} CanHb701_t;

/**
 * @brief  打包 0x101 控制命令帧（小端序 + Byte 7 异或校验）
 * @param[in]  in  待打包命令结构体（非空）
 * @param[out] out 8 字节输出缓冲区（非空）
 */
void CanProto_Pack101(const CanCmd101_t *in, uint8_t out[8]);

/**
 * @brief  解包并校验 0x101 控制命令帧
 * @param[in]  in  8 字节原始 CAN 载荷（非空）
 * @param[out] out 解析输出结构体（非空）
 * @return AppErr_t APP_OK 校验通过；APP_ERR_PARAM 指针为空；APP_ERR_IO 异或校验失败
 */
AppErr_t CanProto_Unpack101(const uint8_t in[8], CanCmd101_t *out);

/**
 * @brief  打包 0x201 命令应答帧
 * @param[in]  in  待打包应答结构体（非空）
 * @param[out] out 8 字节输出缓冲区（非空）
 */
void CanProto_Pack201(const CanAck201_t *in, uint8_t out[8]);

/**
 * @brief  解包并校验 0x201 命令应答帧
 * @param[in]  in  8 字节原始 CAN 载荷（非空）
 * @param[out] out 解析输出结构体（非空）
 * @return AppErr_t APP_OK 校验通过；APP_ERR_PARAM 指针为空；APP_ERR_IO 异或校验失败
 */
AppErr_t CanProto_Unpack201(const uint8_t in[8], CanAck201_t *out);

/**
 * @brief  打包 0x202 风扇运行状态帧
 * @param[in]  in  待打包状态结构体（非空）
 * @param[out] out 8 字节输出缓冲区（非空）
 */
void CanProto_Pack202(const CanStat202_t *in, uint8_t out[8]);

/**
 * @brief  解包并校验 0x202 风扇运行状态帧
 * @param[in]  in  8 字节原始 CAN 载荷（非空）
 * @param[out] out 解析输出结构体（非空）
 * @return AppErr_t APP_OK 校验通过；APP_ERR_PARAM 指针为空；APP_ERR_IO 异或校验失败
 */
AppErr_t CanProto_Unpack202(const uint8_t in[8], CanStat202_t *out);

/**
 * @brief  打包 0x701 模拟风扇节点心跳帧
 * @param[in]  in  待打包心跳结构体（非空）
 * @param[out] out 8 字节输出缓冲区（非空）
 */
void CanProto_Pack701(const CanHb701_t *in, uint8_t out[8]);

/**
 * @brief  解包并校验 0x701 模拟风扇节点心跳帧
 * @param[in]  in  8 字节原始 CAN 载荷（非空）
 * @param[out] out 解析输出结构体（非空）
 * @return AppErr_t APP_OK 校验通过；APP_ERR_PARAM 指针为空；APP_ERR_IO 异或校验失败
 */
AppErr_t CanProto_Unpack701(const uint8_t in[8], CanHb701_t *out);

#endif /* APP_CAN_PROTO_H */
