/**
 * @file    bsp_st7789.h
 * @brief   2.8 英寸 ST7789 横屏（320x240）SPI2 + DMA 刷屏与 TIM12 背光板级驱动接口
 * @date    2026-11-27
 * @note    依赖 CubeMX 生成的 hspi2、DMA1 Stream4、htim12、lcdDmaSemHandle 及 LCD_CS/RST/DC 引脚；
 *          不反向依赖 App 业务模块与 LVGL 头文件（通过完成回调解耦）
 */

#ifndef BSP_ST7789_H
#define BSP_ST7789_H

#include <stdint.h>
#include "app_err.h"

/** @brief 屏幕横屏水平分辨率（像素） */
#define BSP_LCD_WIDTH 320U

/** @brief 屏幕横屏垂直分辨率（像素） */
#define BSP_LCD_HEIGHT 240U

/**
 * @brief ST7789 横屏扫描方向寄存器 MADCTL (0x36) 配置值
 * @note  0x70 = MX(0x40)|MV(0x20)|ML(0x10) 标准横屏 RGB；
 *        若实物方向旋转 180° 可改 0xA0，若红蓝反色可将 bit3 (0x08 BGR) 置 1
 */
#define BSP_LCD_MADCTL_VAL 0x70U

/**
 * @brief ST7789 是否启用硬件反显（0x21 INVON / 0x20 INVOFF）
 * @note  本实物 2.8 寸 ST7789P3 模组实测为非反显模式（0x20 INVOFF），设为 0U
 */
#define BSP_LCD_INVERT_COLOR 0U

/** @brief DMA 刷屏完成回调函数类型（在 DMA1 Stream4 中断上下文中调用） */
typedef void (*BspLcdFlushDoneCb_t)(void);

/**
 * @brief  初始化 ST7789 显示控制器、清屏为纯黑并启动 TIM12 背光 PWM（初始占空比 0%）
 * @return AppErr_t APP_OK 初始化成功；APP_ERR_IO 外设启动失败
 * @note   建议在首帧 LVGL 内容刷新完成后再调用 Bsp_LcdSetBacklight() 点亮背光，防止上电花屏闪烁
 */
AppErr_t Bsp_LcdInit(void);

/**
 * @brief  设置屏幕背光亮度占空比（联动 TIM12_CH1 2 kHz PWM）
 * @param[in] pct 背光百分比（0..100，0 为熄灭，超 100 自动限幅至 100）
 */
void Bsp_LcdSetBacklight(uint8_t pct);

/**
 * @brief  等待当前进行中的 SPI2 DMA 刷屏传输结束（超时时自动中止 SPI2 并复位忙状态）
 * @param[in] timeout_ms 最长等待时间（单位：ms）
 * @return AppErr_t APP_OK 已空闲；APP_ERR_PERM 中断上下文非法调用；APP_ERR_TIMEOUT 超时并已自恢复
 * @warning 禁止在中断上下文调用
 */
AppErr_t Bsp_LcdWaitIdle(uint32_t timeout_ms);

/**
 * @brief  向指定矩形区域发起 SPI2 DMA 异步刷屏传输（像素格式：大端 RGB565）
 * @param[in] x1       区域左上角 X 坐标（0..319）
 * @param[in] y1       区域左上角 Y 坐标（0..239）
 * @param[in] x2       区域右下角 X 坐标（x1..319）
 * @param[in] y2       区域右下角 Y 坐标（y1..239）
 * @param[in] pixels   大端 RGB565 像素数据首地址（必须位于 128 KB 主 RAM，严禁在 CCMRAM）
 * @param[in] byte_len 待发送总字节数（必须等于 (x2-x1+1)*(y2-y1+1)*2 且 <= 65535）
 * @param[in] done_cb  DMA 发送完成后的回调函数（仅在返回 APP_OK 时由 ISR 触发，可为 NULL）
 * @return AppErr_t    APP_OK 已成功发起 DMA；APP_ERR_PARAM 参数非法；
 *                     APP_ERR_PERM 中断上下文非法调用；APP_ERR_TIMEOUT 前次传输超时；APP_ERR_IO 发送失败
 * @note    契约保证：当且仅当返回 APP_OK 时才会异步触发 done_cb；返回任意错误码均不会调用 done_cb
 * @warning 禁止在中断上下文调用；pixels 缓冲区必须在 done_cb 触发前保持有效且不被修改
 */
AppErr_t Bsp_LcdFlushDma(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                         const uint8_t *pixels, uint32_t byte_len,
                         BspLcdFlushDoneCb_t done_cb);

/**
 * @brief  SPI2 DMA 发送完成中断分发回调（由 Core/Src/spi.c HAL_SPI_TxCpltCallback 调用）
 */
void Bsp_LcdOnSpiTxCplt(void);

/**
 * @brief  SPI2 DMA 传输错误中断分发回调（由 Core/Src/spi.c HAL_SPI_ErrorCallback 调用）
 */
void Bsp_LcdOnSpiError(void);

#endif /* BSP_ST7789_H */
