/**
 * @file    bsp_sys.h
 * @brief   系统基础板级驱动接口（复位原因捕获、RTC/备份寄存器、板载 LED 与按键、PA4 隔离）
 * @date    2026-11-26
 * @note    依赖 CubeMX 生成的 hrtc、PWR/RCC/GPIO HAL 驱动；仅共享 app_err.h 公共返回码，不依赖 App 业务模块
 */

#ifndef BSP_SYS_H
#define BSP_SYS_H

#include <stdint.h>
#include "app_err.h"

/** @brief RTC 已校时/初始化备份魔数（存放于 RTC_BKP_DR0） */
#define BSP_RTC_INIT_MAGIC 0x32F2A55AUL

/** @brief IWDG 看门狗卡死任务掩码有效前缀（存放于 RTC_BKP_DR3 高 8 位） */
#define BSP_WDT_HUNG_PREFIX 0xA5000000UL

/** @brief 复位原因类型枚举（与 SRS 4.4 节 Modbus TCP 0x0007 保持一致） */
typedef enum
{
  RESET_REASON_UNKNOWN = 0U, /**< 未知复位原因 */
  RESET_REASON_POR_BOR = 1U, /**< 上电/掉电或欠压复位（POR/PDR/BOR） */
  RESET_REASON_PIN     = 2U, /**< 外部 NRST 引脚/按键复位 */
  RESET_REASON_IWDG    = 3U, /**< 独立看门狗硬件复位 */
  RESET_REASON_WWDG    = 4U, /**< 窗口看门狗复位 */
  RESET_REASON_SW      = 5U  /**< 软件复位（NVIC_SystemReset） */
} ResetReason_t;

/** @brief 简明日历时间结构体（二进制十进制格式，UTC+8 本地时间） */
typedef struct
{
  uint16_t year;  /**< 年份（2000..2099） */
  uint8_t month;  /**< 月份（1..12） */
  uint8_t day;    /**< 日期（1..31） */
  uint8_t hour;   /**< 小时（0..23） */
  uint8_t min;    /**< 分钟（0..59） */
  uint8_t sec;    /**< 秒钟（0..59） */
} BspDateTime_t;

/**
 * @brief  系统早期初始化（必须在 main.c 的 USER CODE BEGIN Init 中尽早调用）
 * @note   完成三项关键动作：
 *         1. 使能 PWR 备份域访问并捕获/清除 RCC->CSR 硬件复位原因；
 *         2. 若为 IWDG 复位则提取并清除 RTC_BKP_DR3 中的卡死任务位图；
 *         3. 抢先将 PA4（FLASH_CS_GUARD）配置为推挽高电平，隔离板载选贴 SPI Flash。
 */
void Bsp_SysEarlyInit(void);

/**
 * @brief  获取本次启动捕获的系统复位原因枚举
 * @return ResetReason_t 复位原因枚举值
 */
ResetReason_t Bsp_SysGetResetReason(void);

/**
 * @brief  获取本次 IWDG 复位前记录的卡死任务位图
 * @return uint16_t 卡死任务位图（非 IWDG 复位或无记录时返回 0）
 */
uint16_t Bsp_SysGetHungTaskMask(void);

/**
 * @brief  将复位原因枚举转换为英文可读字符串
 * @param[in] reason 复位原因枚举
 * @return const char* 英文描述字符串（如 "POR/BOR"、"NRST"、"IWDG"、"SW"）
 */
const char *Bsp_SysResetReasonStr(ResetReason_t reason);

/**
 * @brief  查询硬件 RTC 是否已完成过有效校时/设置
 * @return uint8_t 1 表示已初始化有效时间，0 表示尚未校时（默认时间）
 */
uint8_t Bsp_RtcIsInitialized(void);

/**
 * @brief  读取当前硬件 RTC 日历时间（本地 UTC+8）
 * @param[out] dt 输出日历结构体指针（非空）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 参数非法；APP_ERR_PERM 中断上下文非法调用；APP_ERR_IO 硬件读取失败
 * @note   内部严格按 HAL 要求先读 Time 再读 Date，并通过调度锁防跨任务并发撕裂
 * @warning 禁止在中断上下文调用
 */
AppErr_t Bsp_RtcGetDateTime(BspDateTime_t *dt);

/**
 * @brief  设置硬件 RTC 日历时间并写入 RTC_BKP_DR0 已初始化魔数
 * @param[in] dt 待设置的本地日历时间指针（年份范围 2000..2099）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 日期时间非法；APP_ERR_PERM 中断上下文非法调用；APP_ERR_IO 写入失败
 * @warning 禁止在中断上下文调用
 */
AppErr_t Bsp_RtcSetDateTime(const BspDateTime_t *dt);

/**
 * @brief  获取当前本地 RTC 对应的 Unix 秒级时间戳（以 1970-01-01 00:00:00 为基准）
 * @return uint32_t 本地时间对应的 Unix 时间戳（读取失败返回 0）
 */
uint32_t Bsp_RtcGetUnixTime(void);

/**
 * @brief  读取 RTC 备份寄存器（RTC_BKP_DR0..DR19）
 * @param[in] dr_idx 备份寄存器索引宏（如 RTC_BKP_DR0）
 * @return uint32_t 32 位寄存器值
 */
uint32_t Bsp_BkpRead(uint32_t dr_idx);

/**
 * @brief  写入 RTC 备份寄存器（RTC_BKP_DR0..DR19）
 * @param[in] dr_idx 备份寄存器索引宏
 * @param[in] val    待写入的 32 位数值
 */
void Bsp_BkpWrite(uint32_t dr_idx, uint32_t val);

/**
 * @brief  设置板载用户 LED2（PB2）亮灭状态
 * @param[in] on 非 0 点亮（高电平），0 熄灭（低电平）
 */
void Bsp_LedSet(uint8_t on);

/**
 * @brief  翻转板载用户 LED2（PB2）当前状态
 */
void Bsp_LedToggle(void);

/**
 * @brief  读取板载用户 LED2（PB2）当前输出状态
 * @return uint8_t 1 表示点亮，0 表示熄灭
 */
uint8_t Bsp_LedIsOn(void);

/**
 * @brief  读取板载用户按键 SW2（PA0）状态
 * @return uint8_t 1 表示按下（高电平），0 表示松开
 */
uint8_t Bsp_KeyIsPressed(void);

/**
 * @brief  读取 PA4（FLASH_CS_GUARD）当前电平状态
 * @return uint8_t 1 表示高电平（已安全屏蔽选贴 Flash），0 表示低电平
 */
uint8_t Bsp_FlashGuardIsHigh(void);

/**
 * @brief  触发 MCU 软件系统复位（重启后复位原因为 RESET_REASON_SW）
 */
void Bsp_SysSoftReset(void);

#endif /* BSP_SYS_H */
