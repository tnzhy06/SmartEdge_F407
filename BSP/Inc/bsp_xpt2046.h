/**
 * @file    bsp_xpt2046.h
 * @brief   XPT2046 电阻触摸屏软件 SPI 采样与 4 点仿射校准板级驱动接口
 * @date    2026-11-27
 * @note    使用独立 5 线 GPIO（PD8/10/11/12/13），TP_IRQ 为普通输入轮询（不启 EXTI 防抖动风暴）；
 *          仅依赖 HAL GPIO 与 app_err.h，不反向依赖 App 业务模块
 */

#ifndef BSP_XPT2046_H
#define BSP_XPT2046_H

#include <stdint.h>
#include "app_err.h"

/** @brief 4 点校准允许的最大验证点残差（像素，依据 SDD 第 4.6 节） */
#define BSP_TOUCH_CAL_MAX_ERR_PX 12.0f

/**
 * @brief  获取出厂默认横屏（320x240）仿射校准系数（6 个浮点 A..F）
 * @param[out] out_cal 输出 6 元素浮点系数数组（非空）
 */
void Bsp_TouchGetDefaultCal(float out_cal[6]);

/**
 * @brief  初始化 XPT2046 触摸驱动并加载仿射校准系数
 * @param[in] cal 6 元素仿射校准系数 [A, B, C, D, E, F]；传 NULL 时使用默认出厂参数
 */
void Bsp_TouchInit(const float cal[6]);

/**
 * @brief  在线更新当前生效的 6 系数仿射校准参数
 * @param[in] cal 6 元素仿射校准系数指针（非空）
 */
void Bsp_TouchSetCal(const float cal[6]);

/**
 * @brief  读取当前有效触摸按下时的滤波后原始 12-bit ADC 坐标
 * @param[out] raw_x 输出滤波后的原始 X 通道 ADC 值（0..4095，非空）
 * @param[out] raw_y 输出滤波后的原始 Y 通道 ADC 值（0..4095，非空）
 * @return uint8_t   1 表示有效按下，0 表示未按下或采样抖动被滤除
 */
uint8_t Bsp_TouchReadRaw(uint16_t *raw_x, uint16_t *raw_y);

/**
 * @brief  读取触摸点并经仿射变换映射为横屏像素坐标（0..319, 0..239）
 * @param[out] sx 输出屏幕 X 坐标（非空）
 * @param[out] sy 输出屏幕 Y 坐标（非空）
 * @return uint8_t 1 表示有效按下，0 表示松开
 */
uint8_t Bsp_TouchReadScreen(int16_t *sx, int16_t *sy);

/**
 * @brief  依据 4 点原始 ADC 样本与屏幕靶点坐标求解仿射校准系数并执行残差验证
 * @param[in]  adc_x   4 个靶点对应的原始 ADC X 坐标
 * @param[in]  adc_y   4 个靶点对应的原始 ADC Y 坐标
 * @param[in]  scr_x   4 个靶点对应的屏幕目标 X 坐标
 * @param[in]  scr_y   4 个靶点对应的屏幕目标 Y 坐标
 * @param[out] out_cal 输出求解通过的 6 系数仿射参数 [A, B, C, D, E, F]
 * @return AppErr_t    APP_OK 求解且第 4 点验证误差 <= 12 px；APP_ERR_PARAM 退化或残差超限
 */
AppErr_t Bsp_TouchSolveAffine4Pt(const uint16_t adc_x[4], const uint16_t adc_y[4],
                                 const int16_t scr_x[4], const int16_t scr_y[4],
                                 float out_cal[6]);

#endif /* BSP_XPT2046_H */
