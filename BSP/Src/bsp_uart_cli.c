/**
 * @file    bsp_uart_cli.c
 * @brief   调试串口 USART1 DMA + IDLE 驱动实现
 * @date    2026-11-26
 * @note    所有 DMA 缓冲区均位于默认 128 KB 主 RAM，严禁放入 .ccmram
 */

#include "bsp_uart_cli.h"
#include "usart.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief 单次 DMA 接收临时缓冲区大小（字节） */
#define CLI_RX_DMA_SIZE 64U

/** @brief 软件接收环形缓冲区大小（必须为 2 的幂以便位掩码取模） */
#define CLI_RX_RING_SIZE 256U
#define CLI_RX_RING_MASK (CLI_RX_RING_SIZE - 1U)

/** @brief 单次 DMA 发送缓冲区大小（字节） */
#define CLI_TX_BUF_SIZE 256U

/** @brief 等待前一次发送完成的最大超时（单位：ms；256 B @ 115200 bps 约需 22 ms） */
#define CLI_TX_WAIT_MS 100U

/** @brief 紧急轮询发送单字节最大自旋次数（168 MHz 下约 5 ms，不依赖 HAL_GetTick） */
#define CLI_EMERG_SPIN_MAX 200000UL

/** @brief DMA 环形接收缓冲区（主 RAM，配 DMA2 Stream 2 Circular 模式零盲窗接收） */
static uint8_t s_rx_dma_buf[CLI_RX_DMA_SIZE];

/** @brief 上次已搬运的 DMA 缓冲区读指针（0..CLI_RX_DMA_SIZE-1）及 IDLE/TC 竞争消重标记 */
static uint16_t s_dma_last_pos = 0U;
static uint8_t s_tc_already_consumed = 0U;

/** @brief 软件接收环形缓冲区与头尾下标（ISR 写 head，任务读 tail，使用 Acquire/Release 原子访问） */
static uint8_t s_rx_ring[CLI_RX_RING_SIZE];
static uint16_t s_rx_head = 0U;
static uint16_t s_rx_tail = 0U;

/** @brief 静态 DMA 发送缓冲区与原子忙锁（直接承载格式化输出，避免在调用方任务栈分配 256 B） */
static uint8_t s_tx_buf[CLI_TX_BUF_SIZE];
static uint8_t s_tx_busy = 0U;

/** @brief 收到新数据后唤醒的任务句柄 */
static osThreadId_t s_rx_notify_task = NULL;

/** @brief 行装配临时状态（跨多次 ReadLine 保留未收完的半行及超长溢出标记） */
static char s_line_acc[128];
static uint16_t s_line_len = 0U;
static uint8_t s_line_overflow = 0U;

/**
 * @brief  重新挂载 USART1 DMA IDLE 接收并关闭半传输中断
 * @return AppErr_t APP_OK 成功；APP_ERR_IO 启动失败
 */
static AppErr_t StartRxDma(void)
{
  s_dma_last_pos = 0U;
  s_tc_already_consumed = 0U;

  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_dma_buf, CLI_RX_DMA_SIZE) != HAL_OK)
  {
    return APP_ERR_IO;
  }

  /* 关闭 DMA 半传输中断（遵循 AGENTS.md 红线 #5），环形模式下仅由 TC 与 IDLE 驱动搬运 */
  if (huart1.hdmarx != NULL)
  {
    __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
  }
  return APP_OK;
}

/**
 * @brief  争用静态发送缓冲区独占锁（等待前一次 DMA 发送完成）
 * @return AppErr_t APP_OK 获取成功；APP_ERR_TIMEOUT 等待超时
 */
static AppErr_t AcquireTxLock(void)
{
  uint32_t start_tick = HAL_GetTick();

  while (__atomic_exchange_n(&s_tx_busy, 1U, __ATOMIC_ACQUIRE) != 0U)
  {
    if ((HAL_GetTick() - start_tick) >= CLI_TX_WAIT_MS)
    {
      return APP_ERR_TIMEOUT;
    }
    if (osKernelGetState() == osKernelRunning)
    {
      osDelay(1U);
    }
  }
  return APP_OK;
}

/**
 * @brief  启动已填充好的静态发送缓冲区 s_tx_buf[0..len-1] 传输
 * @param[in] len 待发送字节数（1..CLI_TX_BUF_SIZE）
 * @return AppErr_t APP_OK 成功；APP_ERR_IO 硬件发送失败
 * @note   调用前必须已持有 AcquireTxLock()；若同步结束或启动失败则由本函数释放锁
 */
static AppErr_t LaunchTxBuf(uint16_t len)
{
  /* 调度器未启动前使用轮询发送，启动后走 DMA 异步发送释放 CPU */
  if (osKernelGetState() != osKernelRunning)
  {
    HAL_StatusTypeDef st = HAL_UART_Transmit(&huart1, s_tx_buf, len, CLI_TX_WAIT_MS);
    __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);
    return (st == HAL_OK) ? APP_OK : APP_ERR_IO;
  }

  if (HAL_UART_Transmit_DMA(&huart1, s_tx_buf, len) != HAL_OK)
  {
    __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);
    return APP_ERR_IO;
  }
  return APP_OK;
}

AppErr_t Bsp_CliInit(osThreadId_t rx_notify_task)
{
  s_rx_notify_task = rx_notify_task;
  __atomic_store_n(&s_rx_head, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&s_rx_tail, 0U, __ATOMIC_RELEASE);
  s_line_len = 0U;
  s_line_overflow = 0U;
  __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);

  return StartRxDma();
}

uint16_t Bsp_CliReadLine(char *out, uint16_t max_len)
{
  uint16_t head;
  uint16_t tail;

  if ((out == NULL) || (max_len < 2U))
  {
    return 0U;
  }

  head = __atomic_load_n(&s_rx_head, __ATOMIC_ACQUIRE);
  tail = __atomic_load_n(&s_rx_tail, __ATOMIC_RELAXED);

  while (tail != head)
  {
    uint8_t ch = s_rx_ring[tail];
    tail = (uint16_t)((tail + 1U) & CLI_RX_RING_MASK);
    __atomic_store_n(&s_rx_tail, tail, __ATOMIC_RELEASE);

    if ((ch == '\r') || (ch == '\n'))
    {
      if (s_line_overflow != 0U)
      {
        /* 超长命令直接整行丢弃，避免将截断前缀误当成有效命令执行 */
        s_line_len = 0U;
        s_line_overflow = 0U;
        continue;
      }
      if (s_line_len > 0U)
      {
        uint16_t copy_len = s_line_len;
        if (copy_len >= max_len)
        {
          copy_len = (uint16_t)(max_len - 1U);
        }
        memcpy(out, s_line_acc, copy_len);
        out[copy_len] = '\0';
        s_line_len = 0U;
        return copy_len;
      }
      /* 连续的 \r\n 或空回车直接忽略 */
      continue;
    }

    if (s_line_overflow != 0U)
    {
      continue;
    }

    if ((ch == '\b') || (ch == 0x7FU))
    {
      if (s_line_len > 0U)
      {
        s_line_len--;
      }
      continue;
    }

    /* 仅接收可打印 ASCII 字符，过滤非法控制字节 */
    if ((ch >= 0x20U) && (ch <= 0x7EU))
    {
      if (s_line_len < (uint16_t)(sizeof(s_line_acc) - 1U))
      {
        s_line_acc[s_line_len++] = (char)ch;
      }
      else
      {
        s_line_overflow = 1U;
      }
    }
  }

  return 0U;
}

AppErr_t Bsp_CliWrite(const uint8_t *data, uint16_t len)
{
  AppErr_t err;

  if ((data == NULL) || (len == 0U) || (len > CLI_TX_BUF_SIZE))
  {
    return APP_ERR_PARAM;
  }

  err = AcquireTxLock();
  if (err != APP_OK)
  {
    return err;
  }

  memcpy(s_tx_buf, data, len);
  return LaunchTxBuf(len);
}

void Bsp_CliPrintf(const char *fmt, ...)
{
  va_list args;
  int n;

  if (fmt == NULL)
  {
    return;
  }

  /* 先独占静态 s_tx_buf 再直接格式化写入，彻底避免在调用方任务栈上分配 256 B 缓冲 */
  if (AcquireTxLock() != APP_OK)
  {
    return;
  }

  va_start(args, fmt);
  n = vsnprintf((char *)s_tx_buf, sizeof(s_tx_buf), fmt, args);
  va_end(args);

  if (n <= 0)
  {
    __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);
    return;
  }
  if (n >= (int)sizeof(s_tx_buf))
  {
    n = (int)(sizeof(s_tx_buf) - 1U);
  }

  (void)LaunchTxBuf((uint16_t)n);
}

void Bsp_CliEmergencyPuts(const char *str)
{
  uint32_t guard;

  if (str == NULL)
  {
    return;
  }

  /* 异常钩子可能处于关中断（PRIMASK=1）态，禁止依赖 HAL_GetTick()；直接切回寄存器纯轮询 */
  CLEAR_BIT(USART1->CR3, USART_CR3_DMAT);
  CLEAR_BIT(USART1->CR1, USART_CR1_TXEIE | USART_CR1_TCIE);
  __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);

  if ((USART1->CR1 & (USART_CR1_UE | USART_CR1_TE)) != (USART_CR1_UE | USART_CR1_TE))
  {
    return;
  }

  while (*str != '\0')
  {
    guard = CLI_EMERG_SPIN_MAX;
    while (((USART1->SR & USART_SR_TXE) == 0U) && (guard > 0U))
    {
      guard--;
    }
    if (guard == 0U)
    {
      return;
    }
    USART1->DR = (uint8_t)(*str);
    str++;
  }

  /* 有界等待最后一字节完全移出停止位 */
  guard = CLI_EMERG_SPIN_MAX;
  while (((USART1->SR & USART_SR_TC) == 0U) && (guard > 0U))
  {
    guard--;
  }
}

/**
 * @brief  将 DMA 缓冲区间 [start, end) 搬运至软件环形接收缓冲（ISR 内调用）
 * @param[in] start 起始下标（含）
 * @param[in] end   结束下标（不含，<= CLI_RX_DMA_SIZE）
 * @return uint16_t 实际搬运字节数
 */
static uint16_t PushDmaSliceToRing(uint16_t start, uint16_t end)
{
  uint16_t i;
  uint16_t pushed = 0U;
  uint16_t head = __atomic_load_n(&s_rx_head, __ATOMIC_RELAXED);
  uint16_t tail = __atomic_load_n(&s_rx_tail, __ATOMIC_ACQUIRE);

  for (i = start; i < end; i++)
  {
    uint16_t next = (uint16_t)((head + 1U) & CLI_RX_RING_MASK);
    if (next != tail)
    {
      s_rx_ring[head] = s_rx_dma_buf[i];
      head = next;
      pushed++;
    }
  }
  __atomic_store_n(&s_rx_head, head, __ATOMIC_RELEASE);
  return pushed;
}

void Bsp_CliOnRxEvent(uint16_t size)
{
  uint16_t pushed = 0U;
  uint8_t tc_pending = 0U;

  if (size > CLI_RX_DMA_SIZE)
  {
    size = CLI_RX_DMA_SIZE;
  }

  /* DMA_CIRCULAR 模式持续运行无需重装，按 RxEventType 与 s_dma_last_pos 精确计算增量区间 */
  if (huart1.RxEventType == HAL_UART_RXEVENT_TC)
  {
    /* 若先触发的 IDLE 已提前搬运了本次回绕末尾段，则跳过此次延迟到达的 TC 避免重复 */
    if (s_tc_already_consumed != 0U)
    {
      s_tc_already_consumed = 0U;
      return;
    }
    pushed = PushDmaSliceToRing(s_dma_last_pos, CLI_RX_DMA_SIZE);
    s_dma_last_pos = 0U;
  }
  else
  {
    /* IDLE 事件：HAL 在 NDTR==CLI_RX_DMA_SIZE 时也会传入 size==CLI_RX_DMA_SIZE */
    if (huart1.hdmarx != NULL)
    {
      tc_pending =
        (__HAL_DMA_GET_FLAG(huart1.hdmarx, __HAL_DMA_GET_TC_FLAG_INDEX(huart1.hdmarx)) != RESET)
          ? 1U
          : 0U;
    }

    if (size == CLI_RX_DMA_SIZE)
    {
      /* 仅当 s_dma_last_pos > 0 说明刚填满末尾且 IDLE 抢先于 TC；若为 0 则已由 TC 消费或无数据 */
      if (s_dma_last_pos > 0U)
      {
        pushed = PushDmaSliceToRing(s_dma_last_pos, CLI_RX_DMA_SIZE);
        s_dma_last_pos = 0U;
        if (tc_pending != 0U)
        {
          s_tc_already_consumed = 1U;
        }
      }
    }
    else if (size > s_dma_last_pos)
    {
      pushed = PushDmaSliceToRing(s_dma_last_pos, size);
      s_dma_last_pos = size;
    }
    else if (size < s_dma_last_pos)
    {
      /* DMA 已发生回绕且 IDLE 先于 TC 运行：先搬尾部 [last..64)，再搬头部 [0..size) */
      pushed = PushDmaSliceToRing(s_dma_last_pos, CLI_RX_DMA_SIZE);
      pushed = (uint16_t)(pushed + PushDmaSliceToRing(0U, size));
      s_dma_last_pos = size;
      if (tc_pending != 0U)
      {
        s_tc_already_consumed = 1U;
      }
    }
  }

  if ((pushed > 0U) && (s_rx_notify_task != NULL))
  {
    (void)osThreadFlagsSet(s_rx_notify_task, BSP_CLI_FLAG_RX);
  }
}

void Bsp_CliOnTxCplt(void)
{
  __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);
}

void Bsp_CliOnError(void)
{
  /* 仅当确为 TX DMA 错误导致 HAL 终止发送时才释放 s_tx_busy，
     防止 RX 噪声/帧错误落在任务执行 vsnprintf（gState==READY 但已持锁）期间误解锁 */
  if (((huart1.ErrorCode & HAL_UART_ERROR_DMA) != 0U) &&
      (huart1.hdmatx != NULL) &&
      (huart1.hdmatx->ErrorCode != HAL_DMA_ERROR_NONE) &&
      (huart1.gState == HAL_UART_STATE_READY))
  {
    huart1.hdmatx->ErrorCode = HAL_DMA_ERROR_NONE;
    __atomic_store_n(&s_tx_busy, 0U, __ATOMIC_RELEASE);
  }

  /* 禁止在 ISR 中调用阻塞型 HAL_UART_AbortReceive；仅在 HAL 已结束接收态时重装 RX DMA */
  if (huart1.RxState == HAL_UART_STATE_READY)
  {
    (void)StartRxDma();
  }
}
