/**
 * @file    bsp_xpt2046.c
 * @brief   XPT2046 电阻触摸屏软件 SPI 采样、5 点去极值滤波与 4 点仿射校准实现
 * @date    2026-11-27
 * @note    通过 PD8(CLK)、PD10(MOSI)、PD11(CS)、PD12(IRQ)、PD13(MISO) 独立通信，不争用硬件 SPI2
 */

#include "bsp_xpt2046.h"
#include "bsp_st7789.h"
#include "main.h"
#include "cmsis_os.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/** @brief XPT2046 差分 12-bit 采样控制字（PD1=0, PD0=0 保持 PENIRQ 中断使能） */
#define XPT2046_CMD_READ_X 0xD0U
#define XPT2046_CMD_READ_Y 0x90U

/** @brief 有效滤波采样点数（额外丢弃第 1 次切换瞬态样本） */
#define TOUCH_SAMPLE_CNT 5U

/** @brief 中间 3 点允许的最大离散跨度，用于剔除抬手/轻触飞点 */
#define TOUCH_MAX_SPREAD 100U

/** @brief 当前生效的 6 系数仿射校准参数：sx = A*x + B*y + C, sy = D*x + E*y + F */
static float s_cal[6];

/**
 * @brief  挂起 FreeRTOS 任务切换保护 24 字节校准矩阵原子读写（不关硬件中断，耗时 < 1 us）
 * @return int32_t 先前的内核锁状态；若调度器尚未运行返回 -1
 */
static int32_t TouchCalLock(void)
{
  if ((__get_IPSR() == 0U) && (osKernelGetState() == osKernelRunning))
  {
    return osKernelLock();
  }
  return -1;
}

/**
 * @brief  恢复 FreeRTOS 任务切换状态
 * @param[in] lock TouchCalLock() 返回的锁状态
 */
static void TouchCalUnlock(int32_t lock)
{
  if (lock >= 0)
  {
    (void)osKernelRestoreLock(lock);
  }
}

/**
 * @brief  约 0.5 us 短延时（168 MHz 下生成约 1 MHz 软件 SPI 时钟，兼顾速度与抗杜邦线干扰）
 */
static void TouchBitDelay(void)
{
  for (volatile uint32_t i = 0U; i < 24U; i++)
  {
    __NOP();
  }
}

/**
 * @brief  软件 SPI 发送 1 字节命令（MSB 先行，上升沿锁存）
 * @param[in] byte 待发送控制字节
 */
static void SoftSpiWriteByte(uint8_t byte)
{
  for (uint8_t i = 0U; i < 8U; i++)
  {
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(TP_MOSI_GPIO_Port, TP_MOSI_Pin,
                      ((byte & 0x80U) != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    TouchBitDelay();
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_SET);
    TouchBitDelay();
    byte <<= 1;
  }
  HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
}

/**
 * @brief  软件 SPI 读取 XPT2046 12-bit 转换结果（含 1 个忙等待时钟与 3 位尾零对齐）
 * @return uint16_t 12 位原始 ADC 值（0..4095）
 */
static uint16_t SoftSpiRead12Bit(void)
{
  uint16_t val = 0U;

  /* 1 个额外时钟周期供 XPT2046 完成 SAR 采样保持切换 */
  HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_SET);
  TouchBitDelay();
  HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
  TouchBitDelay();

  for (uint8_t i = 0U; i < 12U; i++)
  {
    val <<= 1;
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_SET);
    TouchBitDelay();
    if (HAL_GPIO_ReadPin(TP_MISO_GPIO_Port, TP_MISO_Pin) == GPIO_PIN_SET)
    {
      val |= 0x0001U;
    }
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
    TouchBitDelay();
  }

  /* 补齐剩余 4 个空时钟让芯片恢复低功耗并重新使能 PENIRQ */
  for (uint8_t i = 0U; i < 4U; i++)
  {
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_SET);
    TouchBitDelay();
    HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
    TouchBitDelay();
  }

  return val;
}

/**
 * @brief  执行一次单通道 XPT2046 12-bit ADC 采样
 * @param[in] cmd 控制字（XPT2046_CMD_READ_X 或 XPT2046_CMD_READ_Y）
 * @return uint16_t 原始 12-bit ADC 值
 */
static uint16_t ReadChannelOnce(uint8_t cmd)
{
  uint16_t result;

  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_RESET);
  TouchBitDelay();
  SoftSpiWriteByte(cmd);
  result = SoftSpiRead12Bit();
  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_SET);
  TouchBitDelay();

  return result;
}

/**
 * @brief  对 5 元素无符号短整型数组升序排列（简单插入排序）
 * @param[in,out] arr 长度为 5 的数组
 */
static void Sort5(uint16_t arr[TOUCH_SAMPLE_CNT])
{
  for (uint8_t i = 1U; i < TOUCH_SAMPLE_CNT; i++)
  {
    uint16_t key = arr[i];
    uint8_t j = i;
    while ((j > 0U) && (arr[j - 1U] > key))
    {
      arr[j] = arr[j - 1U];
      j--;
    }
    arr[j] = key;
  }
}

void Bsp_TouchGetDefaultCal(float out_cal[6])
{
  if (out_cal == NULL)
  {
    return;
  }

  /* 本实物 2.8 寸横屏（MADCTL=0x70）出厂经验初值，首次上电可通过 4 点校准精确替换并存 Sector 11 */
  out_cal[0] = 0.0f;
  out_cal[1] = 0.088f;
  out_cal[2] = -18.0f;
  out_cal[3] = 0.066f;
  out_cal[4] = 0.0f;
  out_cal[5] = -16.0f;
}

void Bsp_TouchInit(const float cal[6])
{
  float init_cal[6];
  int32_t lock;

  HAL_GPIO_WritePin(TP_CS_GPIO_Port, TP_CS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(TP_CLK_GPIO_Port, TP_CLK_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TP_MOSI_GPIO_Port, TP_MOSI_Pin, GPIO_PIN_RESET);

  if (cal != NULL)
  {
    memcpy(init_cal, cal, sizeof(init_cal));
  }
  else
  {
    Bsp_TouchGetDefaultCal(init_cal);
  }

  lock = TouchCalLock();
  memcpy(s_cal, init_cal, sizeof(s_cal));
  TouchCalUnlock(lock);
}

void Bsp_TouchSetCal(const float cal[6])
{
  int32_t lock;

  if (cal == NULL)
  {
    return;
  }

  /* 防止 CliTask (cfg reset) 写校准矩阵时被高优先级 HmiTask 抢占读到半更新数据 */
  lock = TouchCalLock();
  memcpy(s_cal, cal, sizeof(s_cal));
  TouchCalUnlock(lock);
}

uint8_t Bsp_TouchReadRaw(uint16_t *raw_x, uint16_t *raw_y)
{
  uint16_t xs[TOUCH_SAMPLE_CNT];
  uint16_t ys[TOUCH_SAMPLE_CNT];

  if ((raw_x == NULL) || (raw_y == NULL))
  {
    return 0U;
  }

  /* 仅当硬件 PENIRQ 拉低时才启动软件 SPI 采样，空闲期零总线开销 */
  if (HAL_GPIO_ReadPin(TP_IRQ_GPIO_Port, TP_IRQ_Pin) != GPIO_PIN_RESET)
  {
    return 0U;
  }

  /* 先空采 1 组丢弃，消除电阻膜刚接触或通道切换瞬间的建立误差 */
  (void)ReadChannelOnce(XPT2046_CMD_READ_X);
  (void)ReadChannelOnce(XPT2046_CMD_READ_Y);

  for (uint8_t i = 0U; i < TOUCH_SAMPLE_CNT; i++)
  {
    xs[i] = ReadChannelOnce(XPT2046_CMD_READ_X);
    ys[i] = ReadChannelOnce(XPT2046_CMD_READ_Y);
  }

  /* 采样结束后复核 PENIRQ，若中途已松手则整组丢弃防止抬手飞点 */
  if (HAL_GPIO_ReadPin(TP_IRQ_GPIO_Port, TP_IRQ_Pin) != GPIO_PIN_RESET)
  {
    return 0U;
  }

  Sort5(xs);
  Sort5(ys);

  /* 检查中间 3 点离散度，过滤接触不良导致的跳变噪声 */
  if (((uint16_t)(xs[3] - xs[1]) > TOUCH_MAX_SPREAD) ||
      ((uint16_t)(ys[3] - ys[1]) > TOUCH_MAX_SPREAD))
  {
    return 0U;
  }

  *raw_x = (uint16_t)(((uint32_t)xs[1] + (uint32_t)xs[2] + (uint32_t)xs[3]) / 3U);
  *raw_y = (uint16_t)(((uint32_t)ys[1] + (uint32_t)ys[2] + (uint32_t)ys[3]) / 3U);
  return 1U;
}

uint8_t Bsp_TouchReadScreen(int16_t *sx, int16_t *sy)
{
  uint16_t rx = 0U;
  uint16_t ry = 0U;
  float cal[6];
  int32_t lock;
  float fx;
  float fy;
  int32_t ix;
  int32_t iy;

  if ((sx == NULL) || (sy == NULL))
  {
    return 0U;
  }

  if (Bsp_TouchReadRaw(&rx, &ry) == 0U)
  {
    return 0U;
  }

  /* 原子复制 24 B 校准矩阵到本地栈后再计算，杜绝跨任务修改造成的系数撕裂 */
  lock = TouchCalLock();
  memcpy(cal, s_cal, sizeof(cal));
  TouchCalUnlock(lock);

  fx = (cal[0] * (float)rx) + (cal[1] * (float)ry) + cal[2];
  fy = (cal[3] * (float)rx) + (cal[4] * (float)ry) + cal[5];

  ix = (int32_t)((fx >= 0.0f) ? (fx + 0.5f) : (fx - 0.5f));
  iy = (int32_t)((fy >= 0.0f) ? (fy + 0.5f) : (fy - 0.5f));

  if (ix < 0)
  {
    ix = 0;
  }
  else if (ix >= (int32_t)BSP_LCD_WIDTH)
  {
    ix = (int32_t)BSP_LCD_WIDTH - 1;
  }

  if (iy < 0)
  {
    iy = 0;
  }
  else if (iy >= (int32_t)BSP_LCD_HEIGHT)
  {
    iy = (int32_t)BSP_LCD_HEIGHT - 1;
  }

  *sx = (int16_t)ix;
  *sy = (int16_t)iy;
  return 1U;
}

AppErr_t Bsp_TouchSolveAffine4Pt(const uint16_t adc_x[4], const uint16_t adc_y[4],
                                 const int16_t scr_x[4], const int16_t scr_y[4],
                                 float out_cal[6])
{
  float x0;
  float y0;
  float x1;
  float y1;
  float x2;
  float y2;
  float det;
  float sx0;
  float sx1;
  float sx2;
  float sy0;
  float sy1;
  float sy2;
  float a;
  float b;
  float c;
  float d;
  float e;
  float f;
  float pred_x;
  float pred_y;

  if ((adc_x == NULL) || (adc_y == NULL) || (scr_x == NULL) ||
      (scr_y == NULL) || (out_cal == NULL))
  {
    return APP_ERR_PARAM;
  }

  /* 用前 3 个不共线靶点构造 3x3 线性方程组并用克莱姆法则直接求解 A..F */
  x0 = (float)adc_x[0];
  y0 = (float)adc_y[0];
  x1 = (float)adc_x[1];
  y1 = (float)adc_y[1];
  x2 = (float)adc_x[2];
  y2 = (float)adc_y[2];

  det = (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2);
  if (fabsf(det) < 1000.0f)
  {
    /* 判别式过小说明 3 点近似共线或用户重复点在同一位置，拒绝解算 */
    return APP_ERR_PARAM;
  }

  sx0 = (float)scr_x[0];
  sx1 = (float)scr_x[1];
  sx2 = (float)scr_x[2];
  sy0 = (float)scr_y[0];
  sy1 = (float)scr_y[1];
  sy2 = (float)scr_y[2];

  a = ((sx0 - sx2) * (y1 - y2) - (sx1 - sx2) * (y0 - y2)) / det;
  b = ((x0 - x2) * (sx1 - sx2) - (x1 - x2) * (sx0 - sx2)) / det;
  c = (y0 * (x2 * sx1 - x1 * sx2) + y1 * (x0 * sx2 - x2 * sx0) +
       y2 * (x1 * sx0 - x0 * sx1)) / det;

  d = ((sy0 - sy2) * (y1 - y2) - (sy1 - sy2) * (y0 - y2)) / det;
  e = ((x0 - x2) * (sy1 - sy2) - (x1 - x2) * (sy0 - sy2)) / det;
  f = (y0 * (x2 * sy1 - x1 * sy2) + y1 * (x0 * sy2 - x2 * sy0) +
       y2 * (x1 * sy0 - x0 * sy1)) / det;

  /* 用第 4 个独立验证点检验线性映射残差，防止误点导致校准失效 */
  pred_x = (a * (float)adc_x[3]) + (b * (float)adc_y[3]) + c;
  pred_y = (d * (float)adc_x[3]) + (e * (float)adc_y[3]) + f;

  if ((fabsf(pred_x - (float)scr_x[3]) > BSP_TOUCH_CAL_MAX_ERR_PX) ||
      (fabsf(pred_y - (float)scr_y[3]) > BSP_TOUCH_CAL_MAX_ERR_PX))
  {
    return APP_ERR_PARAM;
  }

  out_cal[0] = a;
  out_cal[1] = b;
  out_cal[2] = c;
  out_cal[3] = d;
  out_cal[4] = e;
  out_cal[5] = f;
  return APP_OK;
}
