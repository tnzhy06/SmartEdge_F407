/**
 * @file    ui_page_trend.c
 * @brief   历史趋势页（Trend）：基于 lv_chart 的温湿度与风扇转速曲线切换展示实现
 * @date    2026-11-27
 * @note    数据源来自 CCMRAM 中的 TrendRing_t（每 10 s 采样 1 点，图表展示最近 60 点滑动窗口）
 */

#include "app_ui.h"
#include <stdio.h>

/** @brief 图表同屏显示最近采样点数（60 点 x 10 s = 最近 10 分钟，点距清晰） */
#define TREND_VIEW_POINTS 60U

/** @brief 趋势图表显示模式 */
typedef enum
{
  TREND_MODE_TH  = 0U, /**< 温湿度双曲线（温度 0..60 °C + 湿度 0..100 %RH） */
  TREND_MODE_RPM = 1U  /**< 风扇转速单曲线（0..3000 RPM） */
} TrendViewMode_t;

/** @brief 页面控件句柄 */
static lv_obj_t *s_btn_th = NULL;
static lv_obj_t *s_btn_rpm = NULL;
static lv_obj_t *s_lbl_legend = NULL;
static lv_obj_t *s_lbl_y_top = NULL;
static lv_obj_t *s_lbl_y_mid = NULL;
static lv_obj_t *s_lbl_y_bot = NULL;
static lv_obj_t *s_chart = NULL;
static lv_chart_series_t *s_ser_temp = NULL;
static lv_chart_series_t *s_ser_humi = NULL;
static lv_chart_series_t *s_ser_rpm = NULL;

/** @brief 当前视图模式与上次同步签名（避免无新采样时重复刷图） */
static TrendViewMode_t s_view_mode = TREND_MODE_TH;
static uint8_t s_force_redraw = 1U;
static uint16_t s_last_write_idx = 0xFFFFU;
static uint16_t s_last_count = 0xFFFFU;

/** @brief 静态本地临时趋势副本（避免在 HmiTask 栈上放 724 B 大结构体） */
static TrendRing_t s_trend_copy;

/**
 * @brief  根据当前视图模式刷新按钮高亮、图例、纵轴刻度与曲线可见性
 */
static void ApplyViewModeStyle(void)
{
  if (s_view_mode == TREND_MODE_TH)
  {
    lv_obj_set_style_bg_color(s_btn_th, lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_bg_color(s_btn_rpm, lv_color_hex(UI_COLOR_BTN_IDLE), 0);
    lv_label_set_text(s_lbl_legend, "T(C) Cyan | RH(%) Grn");
    lv_label_set_text(s_lbl_y_top, "60");
    lv_label_set_text(s_lbl_y_mid, "30");
    lv_label_set_text(s_lbl_y_bot, "0");

    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 600);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_SECONDARY_Y, 0, 1000);
    lv_chart_hide_series(s_chart, s_ser_temp, false);
    lv_chart_hide_series(s_chart, s_ser_humi, false);
    lv_chart_hide_series(s_chart, s_ser_rpm, true);
  }
  else
  {
    lv_obj_set_style_bg_color(s_btn_th, lv_color_hex(UI_COLOR_BTN_IDLE), 0);
    lv_obj_set_style_bg_color(s_btn_rpm, lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
    lv_label_set_text(s_lbl_legend, "Fan Speed (RPM) Amber");
    lv_label_set_text(s_lbl_y_top, "3k");
    lv_label_set_text(s_lbl_y_mid, "1.5k");
    lv_label_set_text(s_lbl_y_bot, "0");

    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 3000);
    lv_chart_hide_series(s_chart, s_ser_temp, true);
    lv_chart_hide_series(s_chart, s_ser_humi, true);
    lv_chart_hide_series(s_chart, s_ser_rpm, false);
  }
  s_force_redraw = 1U;
}

/**
 * @brief  [T/H] 与 [RPM] 视图切换按钮回调
 * @param[in] e LVGL 点击事件指针（user_data 存放目标 TrendViewMode_t）
 */
static void OnTrendModeBtnClicked(lv_event_t *e)
{
  TrendViewMode_t next = (TrendViewMode_t)(uintptr_t)lv_event_get_user_data(e);

  if (next != s_view_mode)
  {
    s_view_mode = next;
    ApplyViewModeStyle();
    UiTrend_Update(NULL);
  }
}

void UiTrend_Create(lv_obj_t *parent)
{
  lv_obj_t *lbl;

  s_view_mode = TREND_MODE_TH;
  s_force_redraw = 1U;
  s_last_write_idx = 0xFFFFU;
  s_last_count = 0xFFFFU;

  /* 1. 顶部模式切换按钮与图例 */
  s_btn_th = lv_button_create(parent);
  lv_obj_set_pos(s_btn_th, 4, 3);
  lv_obj_set_size(s_btn_th, 54, 24);
  lv_obj_set_style_radius(s_btn_th, 4, 0);
  lv_obj_set_style_shadow_width(s_btn_th, 0, 0);
  lv_obj_set_style_pad_all(s_btn_th, 0, 0);
  lv_obj_add_event_cb(s_btn_th, OnTrendModeBtnClicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)TREND_MODE_TH);
  lbl = lv_label_create(s_btn_th);
  lv_label_set_text(lbl, "T / H");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl);

  s_btn_rpm = lv_button_create(parent);
  lv_obj_set_pos(s_btn_rpm, 62, 3);
  lv_obj_set_size(s_btn_rpm, 52, 24);
  lv_obj_set_style_radius(s_btn_rpm, 4, 0);
  lv_obj_set_style_shadow_width(s_btn_rpm, 0, 0);
  lv_obj_set_style_pad_all(s_btn_rpm, 0, 0);
  lv_obj_add_event_cb(s_btn_rpm, OnTrendModeBtnClicked, LV_EVENT_CLICKED,
                      (void *)(uintptr_t)TREND_MODE_RPM);
  lbl = lv_label_create(s_btn_rpm);
  lv_label_set_text(lbl, "RPM");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl);

  s_lbl_legend = lv_label_create(parent);
  lv_obj_set_style_text_font(s_lbl_legend, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_legend, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_align(s_lbl_legend, LV_ALIGN_TOP_RIGHT, -6, 8);

  /* 2. 左侧 Y 轴三段量程刻度文字 */
  s_lbl_y_top = lv_label_create(parent);
  lv_obj_set_style_text_font(s_lbl_y_top, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_y_top, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_y_top, 4, 34);

  s_lbl_y_mid = lv_label_create(parent);
  lv_obj_set_style_text_font(s_lbl_y_mid, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_y_mid, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_y_mid, 4, 94);

  s_lbl_y_bot = lv_label_create(parent);
  lv_obj_set_style_text_font(s_lbl_y_bot, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_y_bot, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_y_bot, 4, 154);

  /* 3. 主折线图控件 */
  s_chart = lv_chart_create(parent);
  lv_obj_set_pos(s_chart, 36, 32);
  lv_obj_set_size(s_chart, 280, 138);
  lv_obj_set_style_bg_color(s_chart, lv_color_hex(UI_COLOR_BG_BAR), 0);
  lv_obj_set_style_border_color(s_chart, lv_color_hex(UI_COLOR_BORDER), 0);
  lv_obj_set_style_border_width(s_chart, 1, 0);
  lv_obj_set_style_line_color(s_chart, lv_color_hex(0x283548UL), LV_PART_MAIN);
  lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR); /* 隐藏数据圆点使折线更干净 */
  lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(s_chart, 4, 0);

  lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(s_chart, TREND_VIEW_POINTS);
  lv_chart_set_div_line_count(s_chart, 3, 5);

  s_ser_temp = lv_chart_add_series(s_chart, lv_color_hex(UI_COLOR_TEMP_CYAN),
                                   LV_CHART_AXIS_PRIMARY_Y);
  s_ser_humi = lv_chart_add_series(s_chart, lv_color_hex(UI_COLOR_HUMI_GREEN),
                                   LV_CHART_AXIS_SECONDARY_Y);
  s_ser_rpm = lv_chart_add_series(s_chart, lv_color_hex(UI_COLOR_RPM_AMBER),
                                  LV_CHART_AXIS_PRIMARY_Y);

  lv_chart_set_all_values(s_chart, s_ser_temp, LV_CHART_POINT_NONE);
  lv_chart_set_all_values(s_chart, s_ser_humi, LV_CHART_POINT_NONE);
  lv_chart_set_all_values(s_chart, s_ser_rpm, LV_CHART_POINT_NONE);

  ApplyViewModeStyle();
}

void UiTrend_Update(const AppState_t *snap)
{
  uint16_t valid_n;
  uint16_t start_ring_idx;

  (void)snap;
  AppData_GetTrendCopy(&s_trend_copy);

  if ((s_force_redraw == 0U) &&
      (s_trend_copy.write_idx == s_last_write_idx) &&
      (s_trend_copy.count == s_last_count))
  {
    return;
  }

  s_force_redraw = 0U;
  s_last_write_idx = s_trend_copy.write_idx;
  s_last_count = s_trend_copy.count;

  valid_n = (s_trend_copy.count > TREND_VIEW_POINTS) ? TREND_VIEW_POINTS : s_trend_copy.count;

  /* 按时间先后从左到右填充最近 valid_n 个采样点，其余右侧空位保持 NONE */
  start_ring_idx = (uint16_t)((s_trend_copy.write_idx + TREND_MAX_POINTS - valid_n) %
                              TREND_MAX_POINTS);

  for (uint16_t i = 0U; i < TREND_VIEW_POINTS; i++)
  {
    if (i < valid_n)
    {
      uint16_t idx = (uint16_t)((start_ring_idx + i) % TREND_MAX_POINTS);
      lv_chart_set_series_value_by_id(s_chart, s_ser_temp, i,
                                      (int32_t)s_trend_copy.temp_x10[idx]);
      lv_chart_set_series_value_by_id(s_chart, s_ser_humi, i,
                                      (int32_t)s_trend_copy.humi_x10[idx]);
      lv_chart_set_series_value_by_id(s_chart, s_ser_rpm, i,
                                      (int32_t)s_trend_copy.rpm[idx]);
    }
    else
    {
      lv_chart_set_series_value_by_id(s_chart, s_ser_temp, i, LV_CHART_POINT_NONE);
      lv_chart_set_series_value_by_id(s_chart, s_ser_humi, i, LV_CHART_POINT_NONE);
      lv_chart_set_series_value_by_id(s_chart, s_ser_rpm, i, LV_CHART_POINT_NONE);
    }
  }

  lv_chart_refresh(s_chart);
}
