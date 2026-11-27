/**
 * @file    app_hmi.h
 * @brief   本地触控 HMI 任务（HmiTask）与 LVGL v9.6.0 CCMRAM 内存池接口定义
 * @date    2026-11-27
 * @note    严格遵守 LVGL 单线程红线：除 HmiTask 外严禁任何任务直接调用 lv_* 函数
 */

#ifndef APP_HMI_H
#define APP_HMI_H

#include <stddef.h>
#include <stdint.h>

/** @brief LVGL 内存池占用摘要结构体（供 CLI status 线程安全查询） */
typedef struct
{
  uint32_t total_bytes; /**< 内存池总容量（字节） */
  uint32_t free_bytes;  /**< 当前剩余空闲字节数 */
  uint32_t max_used;    /**< 历史峰值已用字节数 */
  uint8_t used_pct;     /**< 当前使用率（0..100 %） */
} AppHmiMemStats_t;

/**
 * @brief  向 LVGL 内置分配器提供位于 .ccmram 的 48 KB 内存池首地址
 * @param[in] size LVGL 请求的内存池字节数（<= 49152）
 * @return void* 8 字节对齐的 .ccmram 内存池首地址；若请求越界返回 NULL
 * @note   由 lv_conf.h 中 LV_MEM_POOL_ALLOC 宏在 lv_init() 内部自动调用
 */
void *AppHmi_GetLvglMemPool(size_t size);

/**
 * @brief  本地触控 HMI 任务主循环（由 Core/Src/freertos.c StartHmiTask 直接调用）
 * @note   本函数永不返回；负责 ST7789/XPT2046/LVGL 初始化、10 ms 节拍渲染与 250 ms 数据刷新
 */
void App_HmiTask(void);

/**
 * @brief  跨任务安全请求启动 4 点触摸校准向导（供 CLI "touch cal" 调用）
 * @note   仅置位原子请求标志，实际 LVGL 遮罩创建由 HmiTask 在下一次主循环中执行
 */
void AppHmi_RequestTouchCal(void);

/**
 * @brief  获取最近一次周期采集的 LVGL 内存池统计信息（供 CLI status 跨任务读取）
 * @param[out] out 输出统计结构体指针（非空）
 */
void AppHmi_GetMemStats(AppHmiMemStats_t *out);

#endif /* APP_HMI_H */
