/**
 * @file    bsp_can.c
 * @brief   STM32F407 双路物理 CAN（CAN1 + CAN2）滤波器配置、非阻塞发送与中断入队实现
 * @date    2026-11-28
 * @note    严格遵循 AGENTS.md 第 5 节红线 3 与红线 8：
 *          - 先使能 CAN1 时钟并在 CAN1 上设置 SlaveStartFilterBank = 14；
 *          - 发送前检查空闲邮箱防卡死；
 *          - ISR 内仅调用 osMessageQueuePut(..., 0, 0)，绝不调用 Mutex 或阻塞 API
 */

#include "bsp_can.h"
#include "can.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的双路 CAN 接收消息队列句柄 */
extern osMessageQueueId_t can1RxQueueHandle;
extern osMessageQueueId_t can2RxQueueHandle;

/** @brief 16-bit IDLIST 滤波器单槽编码宏：StdID[10:0] 位于 bit[15:5]，RTR=0(数据帧)，IDE=0(标准帧) */
#define CAN_FILTER_STD16(id) ((uint32_t)(((uint32_t)(id) & 0x7FFUL) << 5U))

/** @brief 项目固定 CAN 标准帧 ID（与 SDD 5.5 节一致，用于精确硬件滤波） */
#define BSP_CAN_FILTER_ID_101 0x101U
#define BSP_CAN_FILTER_ID_201 0x201U
#define BSP_CAN_FILTER_ID_202 0x202U
#define BSP_CAN_FILTER_ID_701 0x701U

/** @brief CAN 双通道初始化就绪标志与诊断统计计数（ISR 写入项加 volatile） */
static uint8_t s_can_ready = 0U;
static volatile uint32_t s_rx_drop_cnt = 0U;
static volatile uint32_t s_can1_tx_ok = 0U;
static volatile uint32_t s_can1_tx_err = 0U;
static volatile uint32_t s_can1_rx_irq = 0U;
static volatile uint32_t s_can2_tx_ok = 0U;
static volatile uint32_t s_can2_tx_err = 0U;
static volatile uint32_t s_can2_rx_irq = 0U;

/**
 * @brief  向指定 CAN 实例发送 1 帧 8 字节标准数据帧（发送前检查空闲邮箱防阻塞）
 * @param[in] hcan   HAL CAN 句柄指针（&hcan1 或 &hcan2）
 * @param[in] std_id 11-bit 标准帧 ID
 * @param[in] data   8 字节数据载荷指针
 * @return AppErr_t  APP_OK 成功；APP_ERR_PARAM/NOT_RDY/BUSY/IO 对应失败原因
 */
static AppErr_t CanSendStd8(CAN_HandleTypeDef *hcan, uint32_t std_id, const uint8_t data[8])
{
  CAN_TxHeaderTypeDef hdr;
  uint32_t tx_mailbox = 0U;

  if ((hcan == NULL) || (data == NULL) || (std_id > 0x7FFUL))
  {
    return APP_ERR_PARAM;
  }
  if (__atomic_load_n(&s_can_ready, __ATOMIC_ACQUIRE) == 0U)
  {
    return APP_ERR_NOT_RDY;
  }

  /* 红线 8：发送前必须先确认存在空闲发送邮箱，避免总线断开无 ACK 时 3 邮箱占满卡死 */
  if (HAL_CAN_GetTxMailboxesFreeLevel(hcan) == 0U)
  {
    return APP_ERR_BUSY;
  }

  hdr.StdId = std_id;
  hdr.ExtId = 0UL;
  hdr.IDE = CAN_ID_STD;
  hdr.RTR = CAN_RTR_DATA;
  hdr.DLC = 8UL;
  hdr.TransmitGlobalTime = DISABLE;

  if (HAL_CAN_AddTxMessage(hcan, &hdr, data, &tx_mailbox) != HAL_OK)
  {
    return APP_ERR_IO;
  }
  return APP_OK;
}

/**
 * @brief  在 ISR 中排空指定硬件 RX FIFO 并将标准数据帧非阻塞投递到 FreeRTOS 队列
 * @param[in] hcan  触发中断的 CAN 句柄
 * @param[in] fifo  硬件接收 FIFO 编号（CAN_RX_FIFO0 或 CAN_RX_FIFO1）
 * @param[in] queue 目标 CMSIS-RTOS v2 消息队列句柄
 */
static void DrainRxFifoToQueueFromIsr(CAN_HandleTypeDef *hcan, uint32_t fifo,
                                      osMessageQueueId_t queue)
{
  CAN_RxHeaderTypeDef hdr;
  CanFrame_t frame;

  while (HAL_CAN_GetRxFifoFillLevel(hcan, fifo) > 0U)
  {
    memset(&frame, 0, sizeof(frame));
    if (HAL_CAN_GetRxMessage(hcan, fifo, &hdr, frame.data) != HAL_OK)
    {
      break;
    }

    if ((hdr.IDE != CAN_ID_STD) || (hdr.RTR != CAN_RTR_DATA))
    {
      continue;
    }

    frame.std_id = hdr.StdId;
    frame.dlc = (hdr.DLC > 8UL) ? 8U : (uint8_t)hdr.DLC;

    if (queue != NULL)
    {
      /* ISR 内强制使用 0 超时入队，队满仅丢弃计数绝不阻塞 */
      if (osMessageQueuePut(queue, &frame, 0U, 0U) != osOK)
      {
        s_rx_drop_cnt++;
      }
    }
  }
}

AppErr_t Bsp_CanInit(void)
{
  CAN_FilterTypeDef filter;

  if (__atomic_load_n(&s_can_ready, __ATOMIC_ACQUIRE) != 0U)
  {
    return APP_OK;
  }

  /* 红线 8：STM32F407 CAN2 共享 CAN1 的 28 组滤波器，操作滤波器前确保 CAN1/CAN2 时钟均已开启 */
  __HAL_RCC_CAN1_CLK_ENABLE();
  __HAL_RCC_CAN2_CLK_ENABLE();

  /* 1. 配置 CAN1 Bank 0（16-bit IDLIST）：精确接收 0x201/0x202/0x701 导向 FIFO0，空槽复用 0x701 */
  memset(&filter, 0, sizeof(filter));
  filter.FilterBank = 0U;
  filter.FilterMode = CAN_FILTERMODE_IDLIST;
  filter.FilterScale = CAN_FILTERSCALE_16BIT;
  filter.FilterIdHigh = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_201);
  filter.FilterIdLow = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_202);
  filter.FilterMaskIdHigh = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_701);
  filter.FilterMaskIdLow = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_701);
  filter.FilterFIFOAssignment = CAN_FILTER_FIFO0;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;

  if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
  {
    return APP_ERR_IO;
  }

  /* 2. 配置 CAN2 Bank 14（通过主控制器 &hcan1 写入共享滤波器组）：精确接收 0x101 导向 FIFO1 */
  memset(&filter, 0, sizeof(filter));
  filter.FilterBank = 14U;
  filter.FilterMode = CAN_FILTERMODE_IDLIST;
  filter.FilterScale = CAN_FILTERSCALE_16BIT;
  filter.FilterIdHigh = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_101);
  filter.FilterIdLow = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_101);
  filter.FilterMaskIdHigh = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_101);
  filter.FilterMaskIdLow = CAN_FILTER_STD16(BSP_CAN_FILTER_ID_101);
  filter.FilterFIFOAssignment = CAN_FILTER_FIFO1;
  filter.FilterActivation = ENABLE;
  filter.SlaveStartFilterBank = 14U;

  if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
  {
    return APP_ERR_IO;
  }

  /* 3. 启动 CAN1/CAN2 进入 Normal 通信态并使能 FIFO0/FIFO1 挂起中断 */
  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    return APP_ERR_IO;
  }
  if (HAL_CAN_Start(&hcan2) != HAL_OK)
  {
    return APP_ERR_IO;
  }
  if (HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
  {
    return APP_ERR_IO;
  }
  if (HAL_CAN_ActivateNotification(&hcan2, CAN_IT_RX_FIFO1_MSG_PENDING) != HAL_OK)
  {
    return APP_ERR_IO;
  }

  __atomic_store_n(&s_can_ready, 1U, __ATOMIC_RELEASE);
  return APP_OK;
}

uint8_t Bsp_CanIsReady(void)
{
  return __atomic_load_n(&s_can_ready, __ATOMIC_ACQUIRE);
}

AppErr_t Bsp_Can1Send(uint32_t std_id, const uint8_t data[8])
{
  AppErr_t err = CanSendStd8(&hcan1, std_id, data);
  if (err == APP_OK)
  {
    s_can1_tx_ok++;
  }
  else
  {
    s_can1_tx_err++;
  }
  return err;
}

AppErr_t Bsp_Can2Send(uint32_t std_id, const uint8_t data[8])
{
  AppErr_t err = CanSendStd8(&hcan2, std_id, data);
  if (err == APP_OK)
  {
    s_can2_tx_ok++;
  }
  else
  {
    s_can2_tx_err++;
  }
  return err;
}

uint32_t Bsp_CanGetRxDropCount(void)
{
  return s_rx_drop_cnt;
}

void Bsp_CanGetDiag(BspCanChDiag_t *can1_out, BspCanChDiag_t *can2_out)
{
  uint8_t use_crit;

  if (__get_IPSR() != 0U)
  {
    return;
  }

  /* 极短临界区（屏蔽 CAN1/CAN2 RX 中断=6，不影响 TIM6=1）保证寄存器与统计计数一致 */
  use_crit = (osKernelGetState() == osKernelRunning) ? 1U : 0U;
  if (use_crit != 0U)
  {
    taskENTER_CRITICAL();
  }
  if (can1_out != NULL)
  {
    can1_out->esr = hcan1.Instance->ESR;
    can1_out->msr = hcan1.Instance->MSR;
    can1_out->tsr = hcan1.Instance->TSR;
    can1_out->tx_ok_cnt = s_can1_tx_ok;
    can1_out->tx_err_cnt = s_can1_tx_err;
    can1_out->rx_irq_cnt = s_can1_rx_irq;
  }
  if (can2_out != NULL)
  {
    can2_out->esr = hcan2.Instance->ESR;
    can2_out->msr = hcan2.Instance->MSR;
    can2_out->tsr = hcan2.Instance->TSR;
    can2_out->tx_ok_cnt = s_can2_tx_ok;
    can2_out->tx_err_cnt = s_can2_tx_err;
    can2_out->rx_irq_cnt = s_can2_rx_irq;
  }
  if (use_crit != 0U)
  {
    taskEXIT_CRITICAL();
  }
}

void Bsp_CanOnRxFifo0Pending(void)
{
  s_can1_rx_irq++;
  DrainRxFifoToQueueFromIsr(&hcan1, CAN_RX_FIFO0, can1RxQueueHandle);
}

void Bsp_CanOnRxFifo1Pending(void)
{
  s_can2_rx_irq++;
  DrainRxFifoToQueueFromIsr(&hcan2, CAN_RX_FIFO1, can2RxQueueHandle);
}
