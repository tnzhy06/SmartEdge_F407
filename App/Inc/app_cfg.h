/**
 * @file    app_cfg.h
 * @brief   Flash Sector 11 掉电持久化配置管理模块接口定义
 * @date    2026-11-27
 * @note    严格对齐 SDD 第 3.2 节 128 字节固定布局（Magic + IEEE CRC32 + 2 s 防抖写盘）
 */

#ifndef APP_CFG_H
#define APP_CFG_H

#include <stdint.h>
#include "app_err.h"

/** @brief 掉电配置魔数 "SECF" */
#define APP_CFG_MAGIC 0x53454346UL

/** @brief 掉电配置结构体版本号 */
#define APP_CFG_VERSION 0x0001U

/** @brief 配置标志位定义 */
#define APP_CFG_FLAG_DHCP      (1U << 0) /**< bit0: 0=静态 IP, 1=DHCP */
#define APP_CFG_FLAG_TOUCH_CAL (1U << 1) /**< bit1: 1=已完成有效 4 点触摸校准 */

/** @brief 配置变更防抖等待时长（单位：ms，合并连续参数微调减少 Flash 擦写次数） */
#define APP_CFG_SAVE_DEBOUNCE_MS 2000U

/**
 * @brief 掉电持久化配置结构体（严格 128 字节，4 字节对齐，存放于 Sector 11）
 */
typedef struct __attribute__((aligned(4)))
{
  uint32_t magic;          /**< [0..3]   固定魔数 APP_CFG_MAGIC (0x53454346) */
  uint16_t version;        /**< [4..5]   结构体版本号 APP_CFG_VERSION */
  uint16_t flags;          /**< [6..7]   标志位（DHCP / 触摸校准有效） */
  int16_t temp_start_x10;  /**< [8..9]   自动启动温度阈值（0.1 °C，默认 300） */
  int16_t temp_stop_x10;   /**< [10..11] 自动停止温度阈值（0.1 °C，默认 270） */
  uint16_t manual_rpm_def; /**< [12..13] 手动默认目标转速（800..3000 RPM，默认 1500） */
  uint8_t backlight_pct;   /**< [14]     背光亮度（10..100 %，默认 80） */
  uint8_t rs485_slave_id;  /**< [15]     KTH2-R Modbus 从站地址（默认 0x01） */
  uint32_t static_ip;      /**< [16..19] 静态 IPv4（网络字节序，默认 192.168.1.100） */
  uint32_t static_mask;    /**< [20..23] 子网掩码（默认 255.255.255.0） */
  uint32_t static_gw;      /**< [24..27] 默认网关（默认 192.168.1.1） */
  uint32_t ntp_ip;         /**< [28..31] 可选局域网 NTP IP（0 表示默认公网池） */
  float touch_cal[6];      /**< [32..55] XPT2046 4 点仿射校准系数 A..F */
  uint8_t reserved[68];    /**< [56..123] 预留扩展字节（填 0） */
  uint32_t crc32;          /**< [124..127] 前 124 字节标准 IEEE CRC32 校验值 */
} AppConfig_t;

_Static_assert(sizeof(AppConfig_t) == 128U, "AppConfig_t must be 128 bytes");

/**
 * @brief  装载安全出厂默认配置至目标结构体
 * @param[out] cfg 输出配置结构体指针（非空）
 */
void AppCfg_LoadDefault(AppConfig_t *cfg);

/**
 * @brief  开机初始化配置模块：尝试从 Flash Sector 11 加载并校验，失败则安全回退默认值
 * @return uint8_t 1 表示成功加载 Flash 已有配置，0 表示首次上电或校验失败已回退默认值
 */
uint8_t AppCfg_Init(void);

/**
 * @brief  读取当前生效的配置副本（线程安全）
 * @param[out] out 输出配置副本指针（非空）
 */
void AppCfg_GetCurrent(AppConfig_t *out);

/**
 * @brief  查询开机是否成功从 Flash Sector 11 加载了有效配置
 * @return uint8_t 1 表示来自 Flash，0 表示本次使用出厂默认值
 */
uint8_t AppCfg_IsLoadedFromFlash(void);

/**
 * @brief  查询当前配置是否已完成有效的 4 点触摸校准（轻量只读，避免在任务栈拷贝 128 B 结构体）
 * @return uint8_t 1 表示已校准，0 表示尚未校准（使用出厂默认参数）
 */
uint8_t AppCfg_IsTouchCalibrated(void);

/**
 * @brief  更新运行期同步的配置字段（温控阈值、手动目标转速与背光）并触发 2 s 防抖保存
 * @param[in] start_x10      自动启动温度阈值（0.1 °C）
 * @param[in] stop_x10       自动停止温度阈值（0.1 °C）
 * @param[in] manual_rpm_def 手动目标转速（RPM）
 * @param[in] backlight_pct  背光百分比（10..100）
 */
void AppCfg_SyncRuntimeParams(int16_t start_x10, int16_t stop_x10,
                              uint16_t manual_rpm_def, uint8_t backlight_pct);

/**
 * @brief  更新 4 点触摸校准系数、置位已校准标志并触发 2 s 防抖保存
 * @param[in] cal 6 元素仿射校准系数（非空）
 */
void AppCfg_UpdateTouchCal(const float cal[6]);

/**
 * @brief  标记配置已脏并重置 2 s 防抖计时器
 */
void AppCfg_MarkDirty(void);

/**
 * @brief  轮询检查是否到达 2 s 防抖保存时机并落盘至 Sector 11（由 defaultTask 每秒调用）
 * @return AppErr_t APP_OK 无需保存或保存成功；APP_ERR_BUSY 正有其他写盘在进行；APP_ERR_IO Flash 写入失败
 * @warning 本函数在需要写盘时会阻塞 1~2 s，仅允许由 defaultTask 调用且严禁持 sysMutex
 */
AppErr_t AppCfg_PollSave(void);

/**
 * @brief  立即将当前配置刷盘至 Sector 11（供 CLI cfg save / cfg reset 或后续 OTA 升级前调用）
 * @return AppErr_t APP_OK 保存成功；APP_ERR_BUSY 等待前次写盘超时；APP_ERR_IO Flash 写入失败
 * @note   若当前已有并发写盘在进行，最多等待 2500 ms 让出调度后再执行刷盘
 */
AppErr_t AppCfg_FlushNow(void);

/**
 * @brief  将内存配置缓存与触摸驱动矩阵原子恢复为出厂默认值并置位脏标志（不阻塞擦写 Flash）
 * @param[out] out_def 输出恢复后的默认配置副本（可为 NULL，非空时供调用方立刻同步 g_sys）
 * @note   调用方应先紧接着调用 AppData_ApplyConfig() 同步运行态，再调用 AppCfg_FlushNow() 落盘，
 *         从根源避免擦写 Flash 期间或失败时出现 s_cfg 与 g_sys 状态分裂
 */
void AppCfg_ApplyDefaultMemory(AppConfig_t *out_def);

/**
 * @brief  将全部配置恢复为出厂默认值并立即写入 Sector 11
 * @return AppErr_t APP_OK 恢复并落盘成功；APP_ERR_BUSY 等待写盘超时；APP_ERR_IO Flash 写入失败
 */
AppErr_t AppCfg_ResetToDefault(void);

#endif /* APP_CFG_H */
