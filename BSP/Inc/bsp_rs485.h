/**
 * @file    bsp_rs485.h
 * @brief   RS-485 半双工通信板级驱动接口（基于 USART2 DMA + PD4 方向控制）
 * @date    2026-11-28
 * @note    依赖 CubeMX 生成的 huart2、DMA1 Stream5/6 与 RS485_DE_RE (PD4)；
 *          不反向依赖 App 业务模块，仅依赖 app_err.h
 */

#ifndef BSP_RS485_H
#define BSP_RS485_H

#include <stdint.h>
#include "cmsis_os.h"
#include "app_err.h"

/** @brief RS-485 内部静态收发缓冲区容量（字节，位于默认主 RAM 供 DMA1 使用） */
#define BSP_RS485_BUF_SIZE 64U

/** @brief 唤醒采集任务的线程标志位定义 */
#define BSP_RS485_FLAG_RX_DONE (1UL << 0) /**< 收到一帧完整从站应答（IDLE/RX 完成） */
#define BSP_RS485_FLAG_ERR     (1UL << 1) /**< 串口硬件通信错误或接收重启失败 */

/**
 * @brief  初始化 RS-485 驱动并绑定事务等待目标任务（强制 PD4 保持低电平接收态）
 * @param[in] owner_task 负责执行 Bsp_Rs485Transceive() 的任务句柄（如 SensorTaskHandle）
 * @return AppErr_t      APP_OK 成功；APP_ERR_PARAM 任务句柄为空
 */
AppErr_t Bsp_Rs485Init(osThreadId_t owner_task);

/**
 * @brief  执行一次 RS-485 主站半双工收发事务（发送后经 TC 中断自动切接收并等待 IDLE 帧）
 * @param[in]  tx         待发送数据缓冲区指针（非空）
 * @param[in]  tx_len     发送字节数（1..BSP_RS485_BUF_SIZE）
 * @param[out] rx         接收数据缓冲区指针（非空）
 * @param[in]  rx_max     接收缓冲区最大容量（> 0）
 * @param[out] rx_len     实际接收到的字节数指针（非空）
 * @param[in]  timeout_ms 等待从站应答超时时间（单位：ms，含发送与应答往返耗时）
 * @return AppErr_t       APP_OK 收发成功；APP_ERR_PARAM 参数非法；APP_ERR_PERM 中断上下文调用；
 *                        APP_ERR_NOT_RDY 未初始化；APP_ERR_BUSY 并发重入；
 *                        APP_ERR_TIMEOUT 应答超时；APP_ERR_IO 发送/接收硬件错误或超长
 * @warning 本函数阻塞等待线程标志，禁止在中断上下文或持有 sysMutex 时调用
 */
AppErr_t Bsp_Rs485Transceive(const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_max,
                             uint16_t *rx_len, uint32_t timeout_ms);

/**
 * @brief RS-485 底层收发诊断快照（供 CLI bus 命令定位接线/共地/应答问题）
 */
typedef struct
{
  uint32_t tx_cplt_cnt;  /**< USART2 TC 发送完成中断累计次数 */
  uint32_t rx_evt_cnt;   /**< USART2 IDLE/RX 有效事件累计次数 */
  uint32_t err_irq_cnt;  /**< USART2 硬件错误中断累计次数 */
  uint32_t last_uart_er; /**< 最近一次 HAL_UART ErrorCode */
  int32_t last_ret;      /**< 最近一次 Bsp_Rs485Transceive 返回码 */
  uint16_t last_rx_len;  /**< 最近一次收到的原始字节长度 */
  uint8_t rx_pin_level;  /**< 当前 PD6 (USART2_RX) 引脚实时电平 */
  uint8_t last_rx[16];   /**< 最近一次收到的前 16 字节原始数据 */
} BspRs485Diag_t;

/**
 * @brief  读取 RS-485 中断统计与最近一次接收原始字节快照（内部使用临界区屏蔽同级中断防撕裂）
 * @param[out] out 诊断输出结构体指针（非空）
 * @warning 禁止在中断上下文调用
 */
void Bsp_Rs485GetDiag(BspRs485Diag_t *out);

/**
 * @brief  USART2 发送完全结束（TC）中断回调入口（由 Core/Src/usart.c 分发）
 * @note   严格在此处拉低 PD4 切回接收态并启动 ReceiveToIdle_DMA + 关 DMA_IT_HT
 */
void Bsp_Rs485OnTxCplt(void);

/**
 * @brief  USART2 接收空闲/完成事件中断回调入口（由 Core/Src/usart.c 分发）
 * @param[in] size 本次 DMA 接收到的字节数
 */
void Bsp_Rs485OnRxEvent(uint16_t size);

/**
 * @brief  USART2 硬件错误中断回调入口（由 Core/Src/usart.c 分发）
 */
void Bsp_Rs485OnError(void);

#endif /* BSP_RS485_H */
