/**
 * @file    app_data.h
 * @brief   全局系统共享状态中心（g_sys）、控制参数校验与历史趋势缓存接口定义
 * @date    2026-11-27
 * @note    通过 CubeMX 创建的 sysMutex 保护跨任务并发访问；严格短持锁（< 20 us）
 */

#ifndef APP_DATA_H
#define APP_DATA_H

#include <stdint.h>
#include "bsp_sys.h"
#include "app_cfg.h"
#include "app_err.h"

/** @brief 历史趋势缓存最大采样点数（每 10 s 采 1 点，覆盖近 20 分钟） */
#define TREND_MAX_POINTS 120U

/** @brief 手动目标转速合法上下限（单位：RPM，严格对齐 SRS FR-CTRL-003） */
#define APP_FAN_RPM_MIN 800U
#define APP_FAN_RPM_MAX 3000U

/** @brief 自动温控阈值合法上下限与最小滞回死区（单位：0.1 °C，严格对齐 SRS FR-CTRL-004） */
#define APP_TEMP_THRESH_MIN_X10 0
#define APP_TEMP_THRESH_MAX_X10 700
#define APP_TEMP_HYST_MIN_X10   10

/** @brief 屏幕背光亮度合法范围（单位：%） */
#define APP_BACKLIGHT_MIN_PCT 10U
#define APP_BACKLIGHT_MAX_PCT 100U

/** @brief 综合故障位掩码枚举（与 SRS 4.4 节 Modbus 0x0006 严格一致） */
typedef enum
{
  FAULT_NONE             = 0x0000U,   /**< 无活动故障 */
  FAULT_SENSOR_OFFLINE   = (1U << 0), /**< bit0: 温湿度传感器断线或连续 3 次失效 */
  FAULT_CAN_STALL        = (1U << 1), /**< bit1: CAN 风扇堵转 */
  FAULT_CAN_CMD_TIMEOUT  = (1U << 2), /**< bit2: CAN 控制命令应答超时 */
  FAULT_CAN_NODE_OFFLINE = (1U << 3), /**< bit3: CAN 模拟风扇节点心跳失联 */
  FAULT_TF_ERROR         = (1U << 4)  /**< bit4: TF 卡未插、挂载或写入失败 */
} FaultBit_t;

/** @brief 通风控制模式枚举 */
typedef enum
{
  CTRL_MODE_AUTO   = 0U, /**< 自动滞回 + 温差线性调速模式 */
  CTRL_MODE_MANUAL = 1U  /**< 手动启停与转速设定模式 */
} CtrlMode_t;

/** @brief 风扇运行状态枚举 */
typedef enum
{
  FAN_STOP     = 0U, /**< 正常停机 */
  FAN_AUTO_RUN = 1U, /**< 自动温控运行 */
  FAN_MAN_RUN  = 2U, /**< 手动控制运行 */
  FAN_FAILSAFE = 3U, /**< 传感器失效安全降级通风（1500 RPM） */
  FAN_FAULT    = 4U  /**< CAN 故障告警态 */
} FanState_t;

/** @brief 看门狗核心任务心跳监督位掩码 */
#define HB_BIT_CONTROL (1U << 0) /**< ControlTask 心跳位 */
#define HB_BIT_FANSIM  (1U << 1) /**< FanSimTask 心跳位 */
#define HB_BIT_SENSOR  (1U << 2) /**< SensorTask 心跳位 */
#define HB_BIT_NET     (1U << 3) /**< NetTask 心跳位 */
#define HB_BIT_HMI     (1U << 4) /**< HmiTask 心跳位 */
#define HB_MASK_ALL    (0x1FU)   /**< 纳入 IWDG 硬复位监督的 5 任务掩码 */
#define HB_BIT_MONITOR (1U << 5) /**< defaultTask 卡死注入测试位 */

/** @brief 全局系统运行状态快照结构体 */
typedef struct
{
  int16_t temp_x10;           /**< 当前环境温度（单位：0.1 °C，有符号） */
  uint16_t humi_x10;          /**< 当前相对湿度（单位：0.1 %RH） */
  uint8_t sensor_valid;       /**< 传感器数据有效标志（1=有效，0=失效） */
  uint8_t ctrl_mode;          /**< 当前控制模式（CtrlMode_t） */
  uint8_t fan_state;          /**< 当前风扇状态（FanState_t） */
  uint8_t manual_run;         /**< 手动模式启停请求（0=停，1=启） */
  uint16_t manual_target_rpm; /**< 手动目标转速（范围 800..3000 RPM） */
  uint16_t active_target_rpm; /**< 当前生效下发目标转速（RPM） */
  uint16_t actual_rpm;        /**< CAN2 反馈模拟实测转速（RPM） */
  int16_t temp_start_x10;     /**< 自动启动温度阈值（默认 300 = 30.0 °C） */
  int16_t temp_stop_x10;      /**< 自动停止温度阈值（默认 270 = 27.0 °C） */
  uint16_t fault_mask;        /**< 综合故障位掩码（FaultBit_t 按位或） */
  uint16_t reset_reason;      /**< 上次复位原因（ResetReason_t） */
  uint16_t hung_task_mask;    /**< IWDG 复位前记录的卡死任务位图 */
  uint8_t eth_link_up;        /**< 以太网链路连通标志（1=已连通） */
  uint8_t sntp_synced;        /**< SNTP 已校时标志（1=已校时） */
  uint8_t tf_ready;           /**< TF 卡挂载就绪标志（1=就绪） */
  uint8_t backlight_pct;      /**< 屏幕背光占空比（10..100 %） */
  uint32_t ipv4_addr;         /**< 当前 IPv4 地址（网络字节序） */
  uint32_t uptime_sec;        /**< 系统累计运行时长（单位：s） */
  uint32_t rtc_unix_ts;       /**< 当前本地 RTC Unix 时间戳（单位：s） */
  uint32_t rs485_err_cnt;     /**< RS-485 累计通信错误计数 */
  uint32_t can_err_cnt;       /**< CAN 累计异常计数 */
} AppState_t;

/** @brief 历史趋势环形缓存（存放于 .ccmram，仅 CPU 访问，绝不参与 DMA） */
typedef struct
{
  int16_t temp_x10[TREND_MAX_POINTS];  /**< 历史温度采样数组（0.1 °C） */
  uint16_t humi_x10[TREND_MAX_POINTS]; /**< 历史湿度采样数组（0.1 %RH） */
  uint16_t rpm[TREND_MAX_POINTS];      /**< 历史风扇转速采样数组（RPM） */
  uint16_t write_idx;                  /**< 下一个写入下标（0..119） */
  uint16_t count;                      /**< 当前有效采样点数（0..120） */
} TrendRing_t;

/**
 * @brief  初始化全局系统状态结构体与 CCMRAM 趋势缓存
 * @param[in] cfg       掉电配置结构体指针（可为 NULL，NULL 时采用安全默认值）
 * @param[in] rst       本次启动捕获的复位原因
 * @param[in] hung_mask 若为 IWDG 复位时的卡死任务位图
 * @note   应在调度器启动前（main.c USER CODE BEGIN 2）调用一次
 */
void AppData_Init(const AppConfig_t *cfg, ResetReason_t rst, uint16_t hung_mask);

/**
 * @brief  将配置结构体中的可调运行参数重新应用到全局状态（供 cfg reset 使用）
 * @param[in] cfg 有效配置指针（非空）
 */
void AppData_ApplyConfig(const AppConfig_t *cfg);

/**
 * @brief  线程安全地获取全局系统状态的完整只读副本
 * @param[out] out 输出快照指针（非空）
 * @warning 禁止在中断上下文中调用
 */
void AppData_GetSnapshot(AppState_t *out);

/**
 * @brief  更新系统运行秒数与当前 RTC 时间戳（由 defaultTask 每秒调用）
 * @param[in] uptime_sec  系统运行秒数
 * @param[in] rtc_unix_ts 当前本地 RTC Unix 时间戳
 */
void AppData_UpdateSystemTick(uint32_t uptime_sec, uint32_t rtc_unix_ts);

/**
 * @brief  立即同步全局状态中的 RTC 时间戳（供 CLI rtc set 或 SNTP 校时后调用）
 * @param[in] rtc_unix_ts 最新的本地 RTC Unix 时间戳
 */
void AppData_UpdateRtcTime(uint32_t rtc_unix_ts);

/**
 * @brief  向 CCMRAM 历史趋势环形数组追加一个采样点（由 defaultTask 每 10 s 调用）
 * @param[in] temp_x10 当前温度（0.1 °C）
 * @param[in] humi_x10 当前湿度（0.1 %RH）
 * @param[in] rpm      当前风扇转速（RPM）
 */
void AppData_PushTrendPoint(int16_t temp_x10, uint16_t humi_x10, uint16_t rpm);

/**
 * @brief  线程安全地复制完整历史趋势缓存供 Trend 页面绘制
 * @param[out] out 输出趋势结构体指针（非空）
 */
void AppData_GetTrendCopy(TrendRing_t *out);

/**
 * @brief  受校验地切换通风控制模式（自动 / 手动）
 * @param[in] mode 目标模式（CTRL_MODE_AUTO=0 / CTRL_MODE_MANUAL=1）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 模式非法
 */
AppErr_t AppCtrl_SetMode(uint8_t mode);

/**
 * @brief  受校验地设置手动模式风扇启停命令
 * @param[in] run         0=停止，1=启动
 * @param[in] from_modbus 1=来自 Modbus TCP 上位机（自动模式下拒绝并返回 APP_ERR_PERM）
 * @return AppErr_t       APP_OK 成功；APP_ERR_PARAM 参数非法；APP_ERR_PERM 自动模式拒绝
 */
AppErr_t AppCtrl_SetManualRun(uint8_t run, uint8_t from_modbus);

/**
 * @brief  受校验地设置手动目标转速（800..3000 RPM）并触发配置防抖保存
 * @param[in] rpm 目标转速（单位：RPM）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 转速越界
 */
AppErr_t AppCtrl_SetManualRpm(uint16_t rpm);

/**
 * @brief  受校验地设置自动启停温度阈值并触发配置防抖保存
 * @param[in] start_x10 自动启动阈值（0.1 °C，范围 0..700）
 * @param[in] stop_x10  自动停止阈值（0.1 °C，范围 0..700，且 start_x10 - stop_x10 >= 10）
 * @return AppErr_t     APP_OK 成功；APP_ERR_PARAM 越界或回差不足 1.0 °C
 */
AppErr_t AppCtrl_SetThresholds(int16_t start_x10, int16_t stop_x10);

/**
 * @brief  受校验地设置屏幕背光亮度（10..100 %），实时生效并触发配置防抖保存
 * @param[in] pct 目标背光占空比（10..100）
 * @return AppErr_t APP_OK 成功；APP_ERR_PARAM 越界
 */
AppErr_t AppCtrl_SetBacklight(uint8_t pct);

#endif /* APP_DATA_H */
