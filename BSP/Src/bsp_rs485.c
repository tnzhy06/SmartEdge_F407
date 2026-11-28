/**
 * @file    bsp_rs485.c
 * @brief   RS-485 半双工通信板级驱动实现（USART2 DMA1 Stream5/6 + PD4 TC 方向切换）
 * @date    2026-11-28
 * @note    严格遵循 AGENTS.md 第 5 节红线：
 *          1. DMA 缓冲区全部置于默认 128 KB 主 RAM，严禁放 CCMRAM；
 *          2. 仅在 HAL_UART_TxCpltCallback (TC 中断) 中拉低 PD4，绝不在 TXE 切方向；
 *          3. 每次 HAL_UARTEx_ReceiveToIdle_DMA() 后立即关闭 DMA_IT_HT，防止 9 B 帧被腰斩
 */

#include "bsp_rs485.h"
#include "main.h"
#include "usart.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

/** @brief 发送与接收静态 DMA 缓冲（必须位于主 RAM 供 DMA1 Stream5/6 访问） */
static uint8_t s_tx_buf[BSP_RS485_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t s_rx_buf[BSP_RS485_BUF_SIZE] __attribute__((aligned(4)));

/** @brief 绑定等待 RS-485 事件的采集任务句柄 */
static osThreadId_t s_owner_task = NULL;

/** @brief 事务互斥防重入标志与 ISR/任务共享接收状态 */
static uint8_t s_busy = 0U;
static uint8_t s_rx_active = 0U;
static uint16_t s_rx_len = 0U;

/** @brief 底层通信诊断统计与最近接收样本（ISR 写入项加 volatile，读取/更新经临界区保护） */
static volatile uint32_t s_tx_cplt_cnt = 0U;
static volatile uint32_t s_rx_evt_cnt = 0U;
static volatile uint32_t s_err_irq_cnt = 0U;
static volatile uint32_t s_last_uart_err = 0U;
static int32_t s_last_ret = 0;
static uint16_t s_last_rx_len = 0U;
static uint8_t s_last_rx_sample[16];

/**
 * @brief  在极短临界区内原子刷新最近一次 RS-485 事务结果与原始接收样本
 * @param[in] ret    本次事务返回码
 * @param[in] rx     成功接收的数据首地址（失败时可为 NULL）
 * @param[in] rx_len 成功接收的字节数
 */
static void SaveDiagSnapshot(AppErr_t ret, const uint8_t *rx, uint16_t rx_len)
{
  uint8_t use_crit = (osKernelGetState() == osKernelRunning) ? 1U : 0U;

  if (use_crit != 0U)
  {
    taskENTER_CRITICAL();
  }
  s_last_ret = (int32_t)ret;
  if ((ret == APP_OK) && (rx != NULL) && (rx_len > 0U))
  {
    uint16_t snap_n = (rx_len > (uint16_t)sizeof(s_last_rx_sample))
                        ? (uint16_t)sizeof(s_last_rx_sample)
                        : rx_len;
    s_last_rx_len = rx_len;
    memcpy(s_last_rx_sample, rx, snap_n);
  }
  else
  {
    s_last_rx_len = 0U;
  }
  if (use_crit != 0U)
  {
    taskEXIT_CRITICAL();
  }
}

/**
 * @brief  约 50 us 方向建立延时（168 MHz 下确保 MAX3485E 驱动器完成总线使能稳定）
 */
static void Rs485TurnaroundDelayUs50(void)
{
  for (volatile uint32_t i = 0U; i < 2200U; i++)
  {
    __NOP();
  }
}

/**
 * @brief  清除 USART2 可能残留的硬件错误与空闲标志（防噪声干扰下次接收）
 */
static void ClearUart2Flags(void)
{
  __HAL_UART_CLEAR_PEFLAG(&huart2);
  __HAL_UART_CLEAR_FEFLAG(&huart2);
  __HAL_UART_CLEAR_NEFLAG(&huart2);
  __HAL_UART_CLEAR_OREFLAG(&huart2);
  __HAL_UART_CLEAR_IDLEFLAG(&huart2);
}

/**
 * @brief  启动 USART2 DMA 空闲接收并立即关闭半传输中断（红线防半包腰斩）
 * @return HAL_StatusTypeDef HAL 启动结果
 */
static HAL_StatusTypeDef StartRxToIdleDma(void)
{
  HAL_StatusTypeDef st;

  ClearUart2Flags();
  st = HAL_UARTEx_ReceiveToIdle_DMA(&huart2, s_rx_buf, (uint16_t)sizeof(s_rx_buf));
  if ((st == HAL_OK) && (huart2.hdmarx != NULL))
  {
    /* 关闭 DMA 半传输中断，防止 9 字节 KTH2-R 应答在收到一半时被提前截断 */
    __HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);
  }
  return st;
}

AppErr_t Bsp_Rs485Init(osThreadId_t owner_task)
{
  if (owner_task == NULL)
  {
    return APP_ERR_PARAM;
  }

  s_owner_task = owner_task;
  __atomic_store_n(&s_busy, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_len, 0U, __ATOMIC_RELEASE);

  /* 空闲态强制保持低电平接收模式，释放 RS-485 差分总线 */
  HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
  (void)HAL_UART_Abort(&huart2);
  ClearUart2Flags();
  return APP_OK;
}

AppErr_t Bsp_Rs485Transceive(const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_max,
                             uint16_t *rx_len, uint32_t timeout_ms)
{
  uint32_t flags;
  uint16_t got;
  AppErr_t ret = APP_OK;

  if ((tx == NULL) || (rx == NULL) || (rx_len == NULL) ||
      (tx_len == 0U) || (tx_len > BSP_RS485_BUF_SIZE) ||
      (rx_max == 0U) || (timeout_ms == 0U))
  {
    return APP_ERR_PARAM;
  }
  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }
  if (s_owner_task == NULL)
  {
    return APP_ERR_NOT_RDY;
  }
  if (osThreadGetId() != s_owner_task)
  {
    return APP_ERR_PERM;
  }
  if (__atomic_exchange_n(&s_busy, 1U, __ATOMIC_ACQUIRE) != 0U)
  {
    return APP_ERR_BUSY;
  }

  *rx_len = 0U;

  /* 1. 清理前次可能遗留的线程标志与串口状态 */
  (void)osThreadFlagsClear(BSP_RS485_FLAG_RX_DONE | BSP_RS485_FLAG_ERR);
  __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_len, 0U, __ATOMIC_RELEASE);
  (void)HAL_UART_Abort(&huart2);
  ClearUart2Flags();

  /* 2. 拷贝载荷到主 RAM DMA 发送缓冲，拉高 PD4 并等待收发器稳定后启动 TX DMA */
  memcpy(s_tx_buf, tx, tx_len);
  HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_SET);
  Rs485TurnaroundDelayUs50();

  if (HAL_UART_Transmit_DMA(&huart2, s_tx_buf, tx_len) != HAL_OK)
  {
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
    SaveDiagSnapshot(APP_ERR_IO, NULL, 0U);
    __atomic_store_n(&s_busy, 0U, __ATOMIC_RELEASE);
    return APP_ERR_IO;
  }

  /* 3. 阻塞等待 TC 切接收后的 IDLE 应答或超时（让出 CPU，绝不持 sysMutex） */
  flags = osThreadFlagsWait(BSP_RS485_FLAG_RX_DONE | BSP_RS485_FLAG_ERR,
                            osFlagsWaitAny, timeout_ms);

  if ((flags & 0x80000000UL) != 0UL)
  {
    /* 超时或内核错误：先中止外设/DMA 关闭中断，再清状态并拉低 PD4，杜绝竞态重入 */
    (void)HAL_UART_Abort(&huart2);
    __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
    ClearUart2Flags();
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
    ret = APP_ERR_TIMEOUT;
  }
  else if ((flags & BSP_RS485_FLAG_ERR) != 0UL)
  {
    (void)HAL_UART_Abort(&huart2);
    __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
    ClearUart2Flags();
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
    ret = APP_ERR_IO;
  }
  else
  {
    HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
    got = __atomic_load_n(&s_rx_len, __ATOMIC_ACQUIRE);
    if ((got == 0U) || (got > rx_max))
    {
      ret = APP_ERR_IO;
    }
    else
    {
      memcpy(rx, s_rx_buf, got);
      *rx_len = got;
      ret = APP_OK;
    }
  }

  SaveDiagSnapshot(ret, rx, *rx_len);

  __atomic_store_n(&s_busy, 0U, __ATOMIC_RELEASE);
  return ret;
}

void Bsp_Rs485GetDiag(BspRs485Diag_t *out)
{
  uint8_t use_crit;

  if ((out == NULL) || (__get_IPSR() != 0U))
  {
    return;
  }

  /* 使用临界区（屏蔽优先级 >= 5 中断，含 USART2=7，不影响 TIM6=1）保证全部诊断字段一致 */
  use_crit = (osKernelGetState() == osKernelRunning) ? 1U : 0U;
  if (use_crit != 0U)
  {
    taskENTER_CRITICAL();
  }
  out->tx_cplt_cnt = s_tx_cplt_cnt;
  out->rx_evt_cnt = s_rx_evt_cnt;
  out->err_irq_cnt = s_err_irq_cnt;
  out->last_uart_er = s_last_uart_err;
  out->last_ret = s_last_ret;
  out->last_rx_len = s_last_rx_len;
  out->rx_pin_level = (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_6) == GPIO_PIN_SET) ? 1U : 0U;
  memcpy(out->last_rx, s_last_rx_sample, sizeof(out->last_rx));
  if (use_crit != 0U)
  {
    taskEXIT_CRITICAL();
  }
}

void Bsp_Rs485OnTxCplt(void)
{
  s_tx_cplt_cnt++;

  /* 必须在 TC（发送完全结束）中断中才拉低 PD4，避免最后一字节停止位被截断 */
  HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);

  __atomic_store_n(&s_rx_len, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_active, 1U, __ATOMIC_RELEASE);

  if (StartRxToIdleDma() != HAL_OK)
  {
    __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
    if (s_owner_task != NULL)
    {
      (void)osThreadFlagsSet(s_owner_task, BSP_RS485_FLAG_ERR);
    }
  }
}

void Bsp_Rs485OnRxEvent(uint16_t size)
{
  if (__atomic_load_n(&s_rx_active, __ATOMIC_ACQUIRE) == 0U)
  {
    return;
  }

  if (size == 0U)
  {
    /* 方向切换瞬间的空 IDLE 脉冲忽略并重新挂载接收，避免过早结束等待 */
    if (StartRxToIdleDma() != HAL_OK)
    {
      __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
      if (s_owner_task != NULL)
      {
        (void)osThreadFlagsSet(s_owner_task, BSP_RS485_FLAG_ERR);
      }
    }
    return;
  }

  if (size > BSP_RS485_BUF_SIZE)
  {
    size = BSP_RS485_BUF_SIZE;
  }

  s_rx_evt_cnt++;
  __atomic_store_n(&s_rx_len, size, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);

  if (s_owner_task != NULL)
  {
    (void)osThreadFlagsSet(s_owner_task, BSP_RS485_FLAG_RX_DONE);
  }
}

void Bsp_Rs485OnError(void)
{
  s_err_irq_cnt++;
  s_last_uart_err = huart2.ErrorCode;
  HAL_GPIO_WritePin(RS485_DE_RE_GPIO_Port, RS485_DE_RE_Pin, GPIO_PIN_RESET);
  __atomic_store_n(&s_rx_active, 0U, __ATOMIC_RELEASE);
  ClearUart2Flags();

  if (s_owner_task != NULL)
  {
    (void)osThreadFlagsSet(s_owner_task, BSP_RS485_FLAG_ERR);
  }
}
