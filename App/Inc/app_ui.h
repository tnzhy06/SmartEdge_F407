/**
 * @file    app_ui.h
 * @brief   LVGL v9.6.0 纯英文三页面 UI 公共接口与配色规范定义
 * @date    2026-11-27
 * @note    本头文件内全部函数仅允许在 HmiTask 单线程上下文中调用
 */

#ifndef APP_UI_H
#define APP_UI_H

#include <stdint.h>
#include "lvgl.h"
#include "app_data.h"

/** @brief 统一工业深色主题配色表（Hex RGB） */
#define UI_COLOR_BG_MAIN     0x0F172AUL /**< 主屏幕深石板蓝背景 */
#define UI_COLOR_BG_BAR      0x1E293BUL /**< 顶栏/底栏/卡片背景 */
#define UI_COLOR_BORDER      0x334155UL /**< 卡片边框灰蓝 */
#define UI_COLOR_TEXT_MAIN   0xF1F5F9UL /**< 主文案亮白 */
#define UI_COLOR_TEXT_MUTED  0x94A3B8UL /**< 辅助文案灰白 */
#define UI_COLOR_ACCENT_BLUE 0x2563EBUL /**< 导航与操作主蓝色 */
#define UI_COLOR_TEMP_CYAN   0x38BDF8UL /**< 温度数值/曲线青色 */
#define UI_COLOR_HUMI_GREEN  0x34D399UL /**< 湿度数值/曲线翠绿 */
#define UI_COLOR_RPM_AMBER   0xF59E0BUL /**< 转速曲线/手动高亮琥珀色 */
#define UI_COLOR_OK_GREEN    0x16A34AUL /**< 正常在线状态绿 */
#define UI_COLOR_WARN_AMBER  0xD97706UL /**< 安全降级/提示黄 */
#define UI_COLOR_ERR_RED     0xDC2626UL /**< 故障告警红 */
#define UI_COLOR_BTN_IDLE    0x334155UL /**< 未选中按钮灰蓝 */
#define UI_COLOR_BTN_DARK    0x273549UL /**< 次级微调按钮深灰蓝 */

/** @brief 页面索引枚举 */
typedef enum
{
  UI_PAGE_OVERVIEW = 0U, /**< 运行总览页 */
  UI_PAGE_TREND    = 1U, /**< 历史趋势页 */
  UI_PAGE_DIAG     = 2U, /**< 系统诊断与设置页 */
  UI_PAGE_COUNT    = 3U  /**< 页面总数 */
} UiPageId_t;

/**
 * @brief  初始化公共顶栏、底栏导航及三个业务页面容器
 */
void UiMain_Init(void);

/**
 * @brief  差量刷新顶栏、当前活动页面与必要数据（每 250 ms 由 HmiTask 调用）
 * @param[in] snap 最新系统状态只读快照指针（非空）
 */
void UiMain_Update(const AppState_t *snap);

/**
 * @brief  在指定父容器内构建 Overview 运行总览页
 * @param[in] parent 主内容区页面容器指针
 */
void UiOverview_Create(lv_obj_t *parent);

/**
 * @brief  差量刷新 Overview 运行总览页控件
 * @param[in] snap 最新系统状态只读快照指针
 */
void UiOverview_Update(const AppState_t *snap);

/**
 * @brief  在指定父容器内构建 Trend 历史趋势页
 * @param[in] parent 主内容区页面容器指针
 */
void UiTrend_Create(lv_obj_t *parent);

/**
 * @brief  刷新 Trend 历史趋势页（仅在趋势环形缓存有新采样点时重绘曲线）
 * @param[in] snap 最新系统状态只读快照指针
 */
void UiTrend_Update(const AppState_t *snap);

/**
 * @brief  在指定父容器内构建 Diag 系统诊断与设置页
 * @param[in] parent 主内容区页面容器指针
 */
void UiDiag_Create(lv_obj_t *parent);

/**
 * @brief  差量刷新 Diag 系统诊断与设置页
 * @param[in] snap 最新系统状态只读快照指针
 */
void UiDiag_Update(const AppState_t *snap);

/**
 * @brief  启动全屏 4 点触摸校准向导遮罩
 * @note   仅由 HmiTask 调用（按钮点击或 CLI 请求转发）
 */
void UiDiag_StartTouchCal(void);

/**
 * @brief  查询当前是否正处于全屏 4 点触摸校准状态
 * @return uint8_t 1 表示正在校准（需拦截普通 LVGL 触控），0 表示正常界面态
 */
uint8_t UiDiag_IsTouchCalActive(void);

/**
 * @brief  驱动 4 点触摸校准状态机（校准激活期间由 HmiTask 每 10 ms 调用）
 */
void UiDiag_TouchCalTick(void);

#endif /* APP_UI_H */
