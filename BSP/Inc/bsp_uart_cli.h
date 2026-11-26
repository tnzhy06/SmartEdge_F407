/**
 * @file    bsp_uart_cli.h
 * @brief   调试串口 USART1 板级驱动接口（基于 DMA2 Stream2/7 + IDLE 接收与异步发送）
 * @date    2026-11-26
 * @note    依赖 CubeMX 生成的 huart1、DMA2 Stream2/7 与 CMSIS-RTOS v2；仅共享 app_err.h 公共返回码，不依赖 App 业务模块
 */

#ifndef BSP_UART_CLI_H
#define BSP_UART_CLI_H

#include <stdint.h>
#include "cmsis_os.h"
#include "app_err.h"

/** @brief 通知 CLI 任务有新接收字节到达的线程标志位 */
#define BSP_CLI_FLAG_RX 0x0001UL

/**
 * @brief  初始化调试串口接收缓冲并启动 USART1 DMA IDLE 接收
 * @param[in] rx_notify_task 收到串口数据后需要唤醒的任务句柄（可为 NULL）
 * @return AppErr_t APP_OK 成功；APP_ERR_IO 启动 DMA 接收失败
 * @note   本函数会自动关闭 DMA 半传输中断（DMA_IT_HT），避免接收帧被提前拆分
 */
AppErr_t Bsp_CliInit(osThreadId_t rx_notify_task);

/**
 * @brief  从内部环形接收缓冲区提取一行以 '\r' 或 '\n' 结尾的文本命令
 * @param[out] out     输出行字符串缓冲区（保证以 '\0' 结尾，不含行尾回车换行）
 * @param[in]  max_len 输出缓冲区最大容量（至少为 2）
 * @return uint16_t 实际提取到的命令字符数；若当前尚无完整一行则返回 0
 * @note   空行会被自动跳过；支持退格键（'\b' 或 0x7F）基础行编辑
 */
uint16_t Bsp_CliReadLine(char *out, uint16_t max_len);

/**
 * @brief  通过 USART1 DMA 发送原始字节流
 * @param[in] data 待发送数据指针（非空）
 * @param[in] len  发送字节数（1..256）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 参数非法；APP_ERR_TIMEOUT 等待忙超时；APP_ERR_IO 发送失败
 * @warning 禁止在中断上下文调用，禁止持 sysMutex 时调用
 */
AppErr_t Bsp_CliWrite(const uint8_t *data, uint16_t len);

/**
 * @brief  格式化输出调试字符串至 USART1（最大单次输出 255 字节）
 * @param[in] fmt printf 风格格式串（非空）
 * @warning 禁止在中断上下文调用，禁止持 sysMutex 时调用
 */
void Bsp_CliPrintf(const char *fmt, ...);

/**
 * @brief  紧急纯寄存器轮询输出字符串（专供栈溢出/内存不足/断言等关中断异常钩子使用）
 * @param[in] str 以 '\0' 结尾的常量字符串
 * @note   直接操作 USART1->SR/DR 并带循环计数上限，不依赖 DMA、RTOS 或 HAL_GetTick()
 */
void Bsp_CliEmergencyPuts(const char *str);

/**
 * @brief  USART1 空闲/接收完成中断分发回调（由 Core/Src/usart.c HAL 回调调用）
 * @param[in] size 本次回调时 DMA 环形缓冲区写入位置（1..64）
 */
void Bsp_CliOnRxEvent(uint16_t size);

/**
 * @brief  USART1 发送完成中断分发回调（由 Core/Src/usart.c HAL 回调调用）
 */
void Bsp_CliOnTxCplt(void);

/**
 * @brief  USART1 错误中断分发回调（清除溢出/帧错误并自动重启 DMA 接收）
 */
void Bsp_CliOnError(void);

#endif /* BSP_UART_CLI_H */
