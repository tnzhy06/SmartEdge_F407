/**
 * @file    bsp_flash.h
 * @brief   STM32F407 片内 Flash Sector 11 安全擦写板级驱动接口
 * @date    2026-11-27
 * @note    Sector 11 位于 0x080E0000（容量 128 KB），专用于掉电配置持久化存储；
 *          仅依赖 HAL Flash 驱动与 app_err.h，不反向依赖 App 业务模块
 */

#ifndef BSP_FLASH_H
#define BSP_FLASH_H

#include <stdint.h>
#include "app_err.h"

/** @brief STM32F407VGT6 Sector 11 起始地址（128 KB 独立配置扇区） */
#define BSP_FLASH_SECTOR11_ADDR 0x080E0000UL

/** @brief STM32F407VGT6 Sector 11 容量（字节） */
#define BSP_FLASH_SECTOR11_SIZE (128UL * 1024UL)

/** @brief STM32F407VGT6 片内 Flash 合法起始与结束地址 */
#define BSP_FLASH_BASE_ADDR 0x08000000UL
#define BSP_FLASH_END_ADDR  0x08100000UL

/**
 * @brief  从片内 Flash 任意合法区间读取字节流
 * @param[in]  addr 源 Flash 起始地址（必须位于 0x08000000..0x080FFFFF 内）
 * @param[out] buf  目标内存缓冲区指针（非空）
 * @param[in]  len  读取字节数（> 0，且 addr + len 不越界）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 地址或参数非法
 */
AppErr_t Bsp_FlashRead(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief  整扇区擦除 Sector 11 并按 32 位 Word 写入数据且执行回读校验
 * @param[in] data      待写入数据指针（非空）
 * @param[in] len_bytes 写入字节数（必须为 4 的整数倍且 4 <= len_bytes <= 128 KB）
 * @return AppErr_t     APP_OK 写入并校验通过；APP_ERR_PARAM 参数非法；
 *                      APP_ERR_PERM 中断上下文非法调用；APP_ERR_IO 擦写或校验失败
 * @warning STM32F407 擦除 128 KB Sector 11 耗时约 1.0~2.0 s，期间 CPU 从 Flash 取指暂停；
 *          禁止在中断上下文调用，禁止持 sysMutex 时调用；M5 开启 IWDG 后需在擦前/写后双次喂狗
 */
AppErr_t Bsp_FlashEraseWriteSector11(const void *data, uint32_t len_bytes);

#endif /* BSP_FLASH_H */
