/**
 * @file    bsp_st7789.c
 * @brief   2.8 英寸 ST7789 横屏（320x240）SPI2 + DMA 刷屏与 TIM12 背光板级驱动实现
 * @date    2026-11-27
 * @note    刷屏缓冲区须位于主 RAM，严禁放入 .ccmram；DMA1 Stream4 中断优先级为 8（>= 5）
 */

#include "bsp_st7789.h"
#include "main.h"
#include "spi.h"
#include "tim.h"
#include "cmsis_os.h"
#include <stddef.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的刷屏同步二值信号量（初始态 Depleted=0） */
extern osSemaphoreId_t lcdDmaSemHandle;

/** @brief 轮询写命令/小块数据超时时间（单位：ms） */
#define LCD_SPI_POLL_TIMEOUT_MS 100U

/** @brief 等待前一次 DMA 结束的最大超时时间（单位：ms） */
#define LCD_DMA_WAIT_TIMEOUT_MS 200U

/** @brief 静态全黑清屏行缓冲（主 RAM，320 像素 × 2 B = 640 B） */
static uint8_t s_clear_line_buf[BSP_LCD_WIDTH * 2U];

/** @brief 当前 DMA 刷屏忙标志与完成回调指针 */
static uint8_t s_dma_busy = 0U;
static BspLcdFlushDoneCb_t s_done_cb = NULL;

/**
 * @brief  兼容调度器启动前后的毫秒延时封装
 * @param[in] ms 延时毫秒数
 */
static void LcdDelayMs(uint32_t ms)
{
  if (osKernelGetState() == osKernelRunning)
  {
    (void)osDelay(ms);
  }
  else
  {
    HAL_Delay(ms);
  }
}

/**
 * @brief  向 ST7789 发送单字节命令（拉低 DC）
 * @param[in] cmd 命令码
 */
static void LcdWriteCmd(uint8_t cmd)
{
  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
  (void)HAL_SPI_Transmit(&hspi2, &cmd, 1U, LCD_SPI_POLL_TIMEOUT_MS);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/**
 * @brief  向 ST7789 轮询发送参数/数据字节序列（拉高 DC）
 * @param[in] data 数据指针
 * @param[in] len  数据字节数
 */
static void LcdWriteData(const uint8_t *data, uint16_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return;
  }

  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
  (void)HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, LCD_SPI_POLL_TIMEOUT_MS);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/**
 * @brief  配置 ST7789 显存写入窗口 [x1..x2, y1..y2] 并发送 RAMWR (0x2C)
 * @param[in] x1 起始列（0..319）
 * @param[in] y1 起始行（0..239）
 * @param[in] x2 结束列（x1..319）
 * @param[in] y2 结束行（y1..239）
 */
static void LcdSetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
  uint8_t col[4];
  uint8_t row[4];

  col[0] = (uint8_t)(x1 >> 8);
  col[1] = (uint8_t)(x1 & 0xFFU);
  col[2] = (uint8_t)(x2 >> 8);
  col[3] = (uint8_t)(x2 & 0xFFU);

  row[0] = (uint8_t)(y1 >> 8);
  row[1] = (uint8_t)(y1 & 0xFFU);
  row[2] = (uint8_t)(y2 >> 8);
  row[3] = (uint8_t)(y2 & 0xFFU);

  LcdWriteCmd(0x2AU); /* CASET: Column Address Set */
  LcdWriteData(col, 4U);

  LcdWriteCmd(0x2BU); /* RASET: Row Address Set */
  LcdWriteData(row, 4U);

  LcdWriteCmd(0x2CU); /* RAMWR: Memory Write */
}

/**
 * @brief  同步将整屏（320x240）填充为纯黑，消除上电随机 GRAM 噪点
 */
static void LcdClearBlack(void)
{
  memset(s_clear_line_buf, 0, sizeof(s_clear_line_buf));
  LcdSetWindow(0U, 0U, (uint16_t)(BSP_LCD_WIDTH - 1U), (uint16_t)(BSP_LCD_HEIGHT - 1U));

  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
  for (uint16_t y = 0U; y < BSP_LCD_HEIGHT; y++)
  {
    (void)HAL_SPI_Transmit(&hspi2, s_clear_line_buf, (uint16_t)sizeof(s_clear_line_buf),
                           LCD_SPI_POLL_TIMEOUT_MS);
  }
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/**
 * @brief  结束一次 DMA 刷屏会话（ISR 内调用：释放片选、清忙标志、释放信号量并回调 LVGL）
 */
static void FinishDmaSessionFromIsr(void)
{
  BspLcdFlushDoneCb_t cb = s_done_cb;

  /* 务必先拉高 LCD_CS 结束本次 SPI 帧，再释放忙标志并唤醒上层 */
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
  s_done_cb = NULL;
  __atomic_store_n(&s_dma_busy, 0U, __ATOMIC_RELEASE);

  if (lcdDmaSemHandle != NULL)
  {
    (void)osSemaphoreRelease(lcdDmaSemHandle);
  }

  if (cb != NULL)
  {
    cb();
  }
}

AppErr_t Bsp_LcdInit(void)
{
  /* ST7789P3 2.8" IPS 标准电源与伽马参数表 */
  static const uint8_t k_porctrl[] = {0x0CU, 0x0CU, 0x00U, 0x33U, 0x33U};
  static const uint8_t k_pwctrl1[] = {0xA4U, 0xA1U};
  static const uint8_t k_pvgam[] = {
    0xD0U, 0x04U, 0x0DU, 0x11U, 0x13U, 0x2BU, 0x3FU,
    0x54U, 0x4CU, 0x18U, 0x0DU, 0x0BU, 0x1FU, 0x23U
  };
  static const uint8_t k_nvgam[] = {
    0xD0U, 0x04U, 0x0CU, 0x11U, 0x13U, 0x2CU, 0x3FU,
    0x44U, 0x51U, 0x2FU, 0x1FU, 0x1FU, 0x20U, 0x23U
  };
  uint8_t val;

  __atomic_store_n(&s_dma_busy, 0U, __ATOMIC_RELEASE);
  s_done_cb = NULL;

  /* 1. 启动 TIM12_CH1 背光 PWM 并先置 0% 熄灭背光，防止复位期间出现白屏/花屏 */
  Bsp_LcdSetBacklight(0U);
  if (HAL_TIM_PWM_Start(&htim12, TIM_CHANNEL_1) != HAL_OK)
  {
    return APP_ERR_IO;
  }

  /* 2. 硬件复位脉冲（低电平 >= 10 us，此处留足 20 ms；释放后等待 120 ms 完成内部上电） */
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  LcdDelayMs(20U);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
  LcdDelayMs(120U);

  /* 3. 退出睡眠并配置横屏方向与 16-bit RGB565 像素格式 */
  LcdWriteCmd(0x11U); /* SLPOUT */
  LcdDelayMs(120U);

  LcdWriteCmd(0x36U); /* MADCTL */
  val = BSP_LCD_MADCTL_VAL;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0x3AU); /* COLMOD: 16-bit RGB565 */
  val = 0x55U;
  LcdWriteData(&val, 1U);

  /* 4. 面板驱动电压、帧率与伽马曲线配置 */
  LcdWriteCmd(0xB2U);
  LcdWriteData(k_porctrl, (uint16_t)sizeof(k_porctrl));

  LcdWriteCmd(0xB7U);
  val = 0x35U;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xBBU);
  val = 0x19U;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xC0U);
  val = 0x2CU;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xC2U);
  val = 0x01U;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xC3U);
  val = 0x12U;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xC4U);
  val = 0x20U;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xC6U); /* FRCTRL2: 60 Hz */
  val = 0x0FU;
  LcdWriteData(&val, 1U);

  LcdWriteCmd(0xD0U);
  LcdWriteData(k_pwctrl1, (uint16_t)sizeof(k_pwctrl1));

  LcdWriteCmd(0xE0U);
  LcdWriteData(k_pvgam, (uint16_t)sizeof(k_pvgam));

  LcdWriteCmd(0xE1U);
  LcdWriteData(k_nvgam, (uint16_t)sizeof(k_nvgam));

#if BSP_LCD_INVERT_COLOR
  LcdWriteCmd(0x21U); /* INVON */
#else
  LcdWriteCmd(0x20U); /* INVOFF */
#endif
  LcdWriteCmd(0x13U); /* NORON */

  /* 5. 清空整屏显存后再开启显示输出 */
  LcdClearBlack();
  LcdWriteCmd(0x29U); /* DISPON */
  LcdDelayMs(20U);

  return APP_OK;
}

void Bsp_LcdSetBacklight(uint8_t pct)
{
  uint32_t ccr;

  if (pct > 100U)
  {
    pct = 100U;
  }

  /* TIM12 ARR=999（2 kHz）：0% -> 0，100% -> 1000（恒高），中间每 1% 步进 10 */
  ccr = (uint32_t)pct * 10UL;
  __HAL_TIM_SET_COMPARE(&htim12, TIM_CHANNEL_1, ccr);
}

AppErr_t Bsp_LcdWaitIdle(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }

  while (__atomic_load_n(&s_dma_busy, __ATOMIC_ACQUIRE) != 0U)
  {
    uint32_t elapsed = HAL_GetTick() - start;
    if (elapsed >= timeout_ms)
    {
      /* 超时自恢复：强行中止异常挂起的 SPI2 DMA、释放片选并清除忙标志，避免驱动永久锁死 */
      (void)HAL_SPI_Abort(&hspi2);
      HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
      s_done_cb = NULL;
      __atomic_store_n(&s_dma_busy, 0U, __ATOMIC_RELEASE);
      return APP_ERR_TIMEOUT;
    }

    if ((osKernelGetState() == osKernelRunning) && (lcdDmaSemHandle != NULL))
    {
      (void)osSemaphoreAcquire(lcdDmaSemHandle, timeout_ms - elapsed);
    }
  }
  return APP_OK;
}

AppErr_t Bsp_LcdFlushDma(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                         const uint8_t *pixels, uint32_t byte_len,
                         BspLcdFlushDoneCb_t done_cb)
{
  uint32_t expect_bytes;

  if ((pixels == NULL) || (x1 > x2) || (y1 > y2) ||
      (x2 >= BSP_LCD_WIDTH) || (y2 >= BSP_LCD_HEIGHT))
  {
    return APP_ERR_PARAM;
  }

  expect_bytes = (uint32_t)(x2 - x1 + 1U) * (uint32_t)(y2 - y1 + 1U) * 2UL;
  if ((byte_len == 0UL) || (byte_len != expect_bytes) || (byte_len > 65535UL))
  {
    return APP_ERR_PARAM;
  }
  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }

  /* 防护：若上一次 DMA 尚未释放总线，阻塞等待其结束（正常双缓冲下不会阻塞） */
  if (Bsp_LcdWaitIdle(LCD_DMA_WAIT_TIMEOUT_MS) != APP_OK)
  {
    return APP_ERR_TIMEOUT;
  }

  /* 清空可能残留的历史信号量计数，保证本次等待与本次 DMA 一一对应 */
  if ((osKernelGetState() == osKernelRunning) && (lcdDmaSemHandle != NULL))
  {
    while (osSemaphoreAcquire(lcdDmaSemHandle, 0U) == osOK)
    {
    }
  }

  s_done_cb = done_cb;
  __atomic_store_n(&s_dma_busy, 1U, __ATOMIC_RELEASE);

  /* 先轮询设置本次刷屏窗口，再保持 CS=LOW、DC=HIGH 启动 SPI2 TX DMA 连续发送像素流 */
  LcdSetWindow(x1, y1, x2, y2);
  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);

  if (HAL_SPI_Transmit_DMA(&hspi2, (uint8_t *)pixels, (uint16_t)byte_len) != HAL_OK)
  {
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    s_done_cb = NULL;
    __atomic_store_n(&s_dma_busy, 0U, __ATOMIC_RELEASE);
    /* 统一由调用方依据非 APP_OK 返回值收尾，避免失败路径双调或漏调 done_cb */
    return APP_ERR_IO;
  }

  return APP_OK;
}

void Bsp_LcdOnSpiTxCplt(void)
{
  FinishDmaSessionFromIsr();
}

void Bsp_LcdOnSpiError(void)
{
  FinishDmaSessionFromIsr();
}
