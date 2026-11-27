/**
 * @file    ui_page_overview.c
 * @brief   运行总览页（Overview）：温湿度、告警状态与风扇自动/手动控制界面实现
 * @date    2026-11-27
 * @note    温度单位严格使用内置支持的 "°C"（UTF-8 \xC2\xB0C），禁用 CJK 字符 ℃
 */

#include "app_ui.h"
#include <stdio.h>
#include <string.h>

/** @brief 左侧环境监测卡片控件 */
static lv_obj_t *s_lbl_temp = NULL;
static lv_obj_t *s_lbl_humi = NULL;
static lv_obj_t *s_box_alarm = NULL;
static lv_obj_t *s_lbl_alarm = NULL;

/** @brief 右侧风扇控制卡片控件 */
static lv_obj_t *s_lbl_fan_state = NULL;
static lv_obj_t *s_lbl_rpm_info = NULL;
static lv_obj_t *s_btn_mode = NULL;
static lv_obj_t *s_lbl_btn_mode = NULL;
static lv_obj_t *s_btn_run = NULL;
static lv_obj_t *s_lbl_btn_run = NULL;
static lv_obj_t *s_slider_rpm = NULL;
static lv_obj_t *s_btn_rpm_dec = NULL;
static lv_obj_t *s_btn_rpm_inc = NULL;
static lv_obj_t *s_lbl_man_rpm = NULL;

/** @brief 上次界面缓存用于差量更新 */
static uint8_t s_inited = 0U;
static AppState_t s_prev;

/**
 * @brief  创建标准圆角信息卡片容器
 * @param[in] parent 父容器指针
 * @param[in] x      左上角 X 坐标
 * @param[in] y      左上角 Y 坐标
 * @param[in] w      卡片宽度（像素）
 * @param[in] h      卡片高度（像素）
 * @return lv_obj_t* 创建完成的卡片对象指针
 */
static lv_obj_t *CreateCard(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_BG_BAR), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_BORDER), 0);
  lv_obj_set_style_radius(card, 6, 0);
  lv_obj_set_style_pad_all(card, 6, 0);
  lv_obj_set_scrollable(card, false);
  return card;
}

/**
 * @brief  将风扇运行状态枚举转为英文短文本
 * @param[in] st 风扇状态枚举值（FanState_t）
 * @return const char* 英文短标签字符串
 */
static const char *FanStateStr(uint8_t st)
{
  switch (st)
  {
    case FAN_AUTO_RUN:
      return "AUTO RUN";
    case FAN_MAN_RUN:
      return "MAN RUN";
    case FAN_FAILSAFE:
      return "FAILSAFE";
    case FAN_FAULT:
      return "FAULT";
    default:
      return "STOP";
  }
}

/**
 * @brief  根据全局故障位掩码生成清晰的英文告警摘要与状态色
 * @param[in]  mask    综合故障位掩码（FaultBit_t 按位或）
 * @param[out] out     告警文案输出缓冲区
 * @param[in]  max_len 输出缓冲区容量（字节）
 * @param[out] bg_hex  告警卡片对应背景色（RGB Hex）
 */
static void FormatAlarmText(uint16_t mask, char *out, size_t max_len, uint32_t *bg_hex)
{
  if (mask == FAULT_NONE)
  {
    (void)snprintf(out, max_len, "STATUS: OK");
    *bg_hex = UI_COLOR_OK_GREEN;
    return;
  }

  *bg_hex = UI_COLOR_ERR_RED;

  /* 优先完整显示单故障名称；多故障并发时使用紧凑串联，确保三类 CAN 故障始终可区分 */
  if (mask == FAULT_SENSOR_OFFLINE)
  {
    (void)snprintf(out, max_len, "ERR: SENSOR OFF");
  }
  else if (mask == FAULT_CAN_STALL)
  {
    (void)snprintf(out, max_len, "ERR: CAN STALL");
  }
  else if (mask == FAULT_CAN_CMD_TIMEOUT)
  {
    (void)snprintf(out, max_len, "ERR: CMD TMO");
  }
  else if (mask == FAULT_CAN_NODE_OFFLINE)
  {
    (void)snprintf(out, max_len, "ERR: NODE OFF");
  }
  else if (mask == FAULT_TF_ERROR)
  {
    (void)snprintf(out, max_len, "WARN: TF CARD");
    *bg_hex = UI_COLOR_WARN_AMBER;
  }
  else
  {
    size_t pos = (size_t)snprintf(out, max_len, "ERR:");
    if (((mask & FAULT_SENSOR_OFFLINE) != 0U) && (pos < max_len))
    {
      pos += (size_t)snprintf(out + pos, max_len - pos, " 485");
    }
    if (((mask & FAULT_CAN_STALL) != 0U) && (pos < max_len))
    {
      pos += (size_t)snprintf(out + pos, max_len - pos, " STL");
    }
    if (((mask & FAULT_CAN_CMD_TIMEOUT) != 0U) && (pos < max_len))
    {
      pos += (size_t)snprintf(out + pos, max_len - pos, " TMO");
    }
    if (((mask & FAULT_CAN_NODE_OFFLINE) != 0U) && (pos < max_len))
    {
      pos += (size_t)snprintf(out + pos, max_len - pos, " OFF");
    }
    if (((mask & FAULT_TF_ERROR) != 0U) && (pos < max_len))
    {
      (void)snprintf(out + pos, max_len - pos, " TF");
    }
  }
}

/**
 * @brief  切换自动/手动控制模式按钮事件回调
 * @param[in] e LVGL 点击事件指针
 */
static void OnModeToggleClicked(lv_event_t *e)
{
  AppState_t snap;
  uint8_t next_mode;

  (void)e;
  AppData_GetSnapshot(&snap);
  next_mode = (snap.ctrl_mode == CTRL_MODE_AUTO) ? CTRL_MODE_MANUAL : CTRL_MODE_AUTO;
  if (AppCtrl_SetMode(next_mode) == APP_OK)
  {
    AppData_GetSnapshot(&snap);
    /* 同步刷新顶栏模式徽章与总览页按钮，避免顶栏滞后最多 250 ms */
    UiMain_Update(&snap);
  }
}

/**
 * @brief  手动启停按钮事件回调
 * @param[in] e LVGL 点击事件指针
 */
static void OnRunToggleClicked(lv_event_t *e)
{
  AppState_t snap;
  uint8_t next_run;

  (void)e;
  AppData_GetSnapshot(&snap);
  if (snap.ctrl_mode != CTRL_MODE_MANUAL)
  {
    return;
  }
  next_run = (snap.manual_run == 0U) ? 1U : 0U;
  if (AppCtrl_SetManualRun(next_run, 0U) == APP_OK)
  {
    AppData_GetSnapshot(&snap);
    UiMain_Update(&snap);
  }
}

/**
 * @brief  手动转速滑块拖动变化回调（步进对齐到 50 RPM）
 * @param[in] e LVGL 数值变更事件指针
 */
static void OnRpmSliderChanged(lv_event_t *e)
{
  lv_obj_t *slider = lv_event_get_target_obj(e);
  int32_t raw = lv_slider_get_value(slider);
  uint16_t rpm = (uint16_t)(((raw + 25) / 50) * 50);

  if (rpm < APP_FAN_RPM_MIN)
  {
    rpm = APP_FAN_RPM_MIN;
  }
  else if (rpm > APP_FAN_RPM_MAX)
  {
    rpm = APP_FAN_RPM_MAX;
  }

  if (AppCtrl_SetManualRpm(rpm) == APP_OK)
  {
    char buf[12];
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned int)rpm);
    lv_label_set_text(s_lbl_man_rpm, buf);
  }
}

/**
 * @brief  手动转速 -100 / +100 微调按钮事件回调
 * @param[in] e LVGL 点击事件指针（user_data 携带步进增量 -100 或 +100）
 */
static void OnRpmStepClicked(lv_event_t *e)
{
  int32_t delta = (int32_t)(intptr_t)lv_event_get_user_data(e);
  AppState_t snap;
  int32_t next_rpm;

  AppData_GetSnapshot(&snap);
  if (snap.ctrl_mode != CTRL_MODE_MANUAL)
  {
    return;
  }

  next_rpm = (int32_t)snap.manual_target_rpm + delta;
  if (next_rpm < (int32_t)APP_FAN_RPM_MIN)
  {
    next_rpm = (int32_t)APP_FAN_RPM_MIN;
  }
  else if (next_rpm > (int32_t)APP_FAN_RPM_MAX)
  {
    next_rpm = (int32_t)APP_FAN_RPM_MAX;
  }

  if (AppCtrl_SetManualRpm((uint16_t)next_rpm) == APP_OK)
  {
    AppData_GetSnapshot(&snap);
    UiOverview_Update(&snap);
  }
}

/**
 * @brief  根据是否手动模式统一启用或置灰禁用手动控制控件
 * @param[in] enabled 1 启用手动控件，0 置灰禁用
 */
static void SetManualControlsEnabled(uint8_t enabled)
{
  lv_obj_t *ctrls[4] = {s_btn_run, s_slider_rpm, s_btn_rpm_dec, s_btn_rpm_inc};

  for (uint8_t i = 0U; i < 4U; i++)
  {
    if (enabled != 0U)
    {
      lv_obj_remove_state(ctrls[i], LV_STATE_DISABLED);
      lv_obj_set_style_opa(ctrls[i], LV_OPA_COVER, 0);
    }
    else
    {
      lv_obj_add_state(ctrls[i], LV_STATE_DISABLED);
      lv_obj_set_style_opa(ctrls[i], LV_OPA_50, 0);
    }
  }
}

void UiOverview_Create(lv_obj_t *parent)
{
  lv_obj_t *left_card;
  lv_obj_t *right_card;
  lv_obj_t *lbl_title;
  lv_obj_t *lbl;

  s_inited = 0U;

  /* ==================== 左侧：环境温湿度与告警卡片 ==================== */
  left_card = CreateCard(parent, 4, 4, 152, 168);

  lbl_title = lv_label_create(left_card);
  lv_label_set_text(lbl_title, "ENVIRONMENT");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(lbl_title, 2, 0);

  s_lbl_temp = lv_label_create(left_card);
  lv_label_set_text(s_lbl_temp, "--.- \xC2\xB0" "C");
  lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(UI_COLOR_TEMP_CYAN), 0);
  lv_obj_set_pos(s_lbl_temp, 2, 26);

  s_lbl_humi = lv_label_create(left_card);
  lv_label_set_text(s_lbl_humi, "RH: --.- %");
  lv_obj_set_style_text_font(s_lbl_humi, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_lbl_humi, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
  lv_obj_set_pos(s_lbl_humi, 2, 66);

  s_box_alarm = lv_obj_create(left_card);
  lv_obj_set_pos(s_box_alarm, 0, 108);
  lv_obj_set_size(s_box_alarm, 138, 44);
  lv_obj_set_style_bg_color(s_box_alarm, lv_color_hex(UI_COLOR_OK_GREEN), 0);
  lv_obj_set_style_bg_opa(s_box_alarm, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_box_alarm, 0, 0);
  lv_obj_set_style_radius(s_box_alarm, 5, 0);
  lv_obj_set_style_pad_all(s_box_alarm, 4, 0);
  lv_obj_set_scrollable(s_box_alarm, false);

  s_lbl_alarm = lv_label_create(s_box_alarm);
  lv_label_set_text(s_lbl_alarm, "STATUS: OK");
  lv_obj_set_width(s_lbl_alarm, 130);
  lv_label_set_long_mode(s_lbl_alarm, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(s_lbl_alarm, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_lbl_alarm, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_alarm, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_center(s_lbl_alarm);

  /* ==================== 右侧：风扇状态与手动调速卡片 ==================== */
  right_card = CreateCard(parent, 160, 4, 156, 168);

  lbl_title = lv_label_create(right_card);
  lv_label_set_text(lbl_title, "FAN:");
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl_title, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(lbl_title, 2, 0);

  s_lbl_fan_state = lv_label_create(right_card);
  lv_label_set_text(s_lbl_fan_state, "STOP");
  lv_obj_set_style_text_font(s_lbl_fan_state, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_fan_state, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_set_pos(s_lbl_fan_state, 38, 0);

  s_lbl_rpm_info = lv_label_create(right_card);
  lv_label_set_text(s_lbl_rpm_info, "A:0  T:0 RPM");
  lv_obj_set_style_text_font(s_lbl_rpm_info, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_rpm_info, lv_color_hex(UI_COLOR_RPM_AMBER), 0);
  lv_obj_set_pos(s_lbl_rpm_info, 2, 20);

  /* 模式切换按钮 */
  s_btn_mode = lv_button_create(right_card);
  lv_obj_set_pos(s_btn_mode, 0, 46);
  lv_obj_set_size(s_btn_mode, 68, 30);
  lv_obj_set_style_bg_color(s_btn_mode, lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
  lv_obj_set_style_radius(s_btn_mode, 5, 0);
  lv_obj_set_style_shadow_width(s_btn_mode, 0, 0);
  lv_obj_set_style_pad_all(s_btn_mode, 0, 0);
  lv_obj_add_event_cb(s_btn_mode, OnModeToggleClicked, LV_EVENT_CLICKED, NULL);

  s_lbl_btn_mode = lv_label_create(s_btn_mode);
  lv_label_set_text(s_lbl_btn_mode, "AUTO");
  lv_obj_set_style_text_font(s_lbl_btn_mode, &lv_font_montserrat_12, 0);
  lv_obj_center(s_lbl_btn_mode);

  /* 手动启停按钮 */
  s_btn_run = lv_button_create(right_card);
  lv_obj_set_pos(s_btn_run, 74, 46);
  lv_obj_set_size(s_btn_run, 68, 30);
  lv_obj_set_style_bg_color(s_btn_run, lv_color_hex(UI_COLOR_BTN_IDLE), 0);
  lv_obj_set_style_radius(s_btn_run, 5, 0);
  lv_obj_set_style_shadow_width(s_btn_run, 0, 0);
  lv_obj_set_style_pad_all(s_btn_run, 0, 0);
  lv_obj_add_event_cb(s_btn_run, OnRunToggleClicked, LV_EVENT_CLICKED, NULL);

  s_lbl_btn_run = lv_label_create(s_btn_run);
  lv_label_set_text(s_lbl_btn_run, "STOP");
  lv_obj_set_style_text_font(s_lbl_btn_run, &lv_font_montserrat_12, 0);
  lv_obj_center(s_lbl_btn_run);

  /* 手动目标转速滑块（800..3000 RPM） */
  s_slider_rpm = lv_slider_create(right_card);
  lv_obj_set_pos(s_slider_rpm, 6, 92);
  lv_obj_set_size(s_slider_rpm, 130, 10);
  lv_slider_set_range(s_slider_rpm, (int32_t)APP_FAN_RPM_MIN, (int32_t)APP_FAN_RPM_MAX);
  lv_slider_set_value(s_slider_rpm, 1500, LV_ANIM_OFF);
  lv_obj_add_event_cb(s_slider_rpm, OnRpmSliderChanged, LV_EVENT_VALUE_CHANGED, NULL);

  /* 转速 -100 / 数值 / +100 微调行 */
  s_btn_rpm_dec = lv_button_create(right_card);
  lv_obj_set_pos(s_btn_rpm_dec, 0, 120);
  lv_obj_set_size(s_btn_rpm_dec, 44, 30);
  lv_obj_set_style_bg_color(s_btn_rpm_dec, lv_color_hex(UI_COLOR_BTN_DARK), 0);
  lv_obj_set_style_radius(s_btn_rpm_dec, 5, 0);
  lv_obj_set_style_shadow_width(s_btn_rpm_dec, 0, 0);
  lv_obj_set_style_pad_all(s_btn_rpm_dec, 0, 0);
  lv_obj_add_event_cb(s_btn_rpm_dec, OnRpmStepClicked, LV_EVENT_CLICKED, (void *)(intptr_t)(-100));
  lbl = lv_label_create(s_btn_rpm_dec);
  lv_label_set_text(lbl, "-100");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl);

  s_lbl_man_rpm = lv_label_create(right_card);
  lv_label_set_text(s_lbl_man_rpm, "1500");
  lv_obj_set_style_text_font(s_lbl_man_rpm, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_man_rpm, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_set_pos(s_lbl_man_rpm, 52, 127);

  s_btn_rpm_inc = lv_button_create(right_card);
  lv_obj_set_pos(s_btn_rpm_inc, 98, 120);
  lv_obj_set_size(s_btn_rpm_inc, 44, 30);
  lv_obj_set_style_bg_color(s_btn_rpm_inc, lv_color_hex(UI_COLOR_BTN_DARK), 0);
  lv_obj_set_style_radius(s_btn_rpm_inc, 5, 0);
  lv_obj_set_style_shadow_width(s_btn_rpm_inc, 0, 0);
  lv_obj_set_style_pad_all(s_btn_rpm_inc, 0, 0);
  lv_obj_add_event_cb(s_btn_rpm_inc, OnRpmStepClicked, LV_EVENT_CLICKED, (void *)(intptr_t)(100));
  lbl = lv_label_create(s_btn_rpm_inc);
  lv_label_set_text(lbl, "+100");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl);

  SetManualControlsEnabled(0U);
}

void UiOverview_Update(const AppState_t *snap)
{
  char buf[32];

  if (snap == NULL)
  {
    return;
  }

  /* 1. 温湿度数值区（失效时显示 --.- 并变告警色，防止误导显示陈旧正常值） */
  if ((s_inited == 0U) || (snap->sensor_valid != s_prev.sensor_valid) ||
      (snap->temp_x10 != s_prev.temp_x10) || (snap->humi_x10 != s_prev.humi_x10))
  {
    if (snap->sensor_valid != 0U)
    {
      int16_t abs_t = (snap->temp_x10 < 0) ? (int16_t)(-snap->temp_x10) : snap->temp_x10;
      (void)snprintf(buf, sizeof(buf), "%s%d.%d \xC2\xB0" "C",
                     (snap->temp_x10 < 0) ? "-" : "",
                     (int)(abs_t / 10), (int)(abs_t % 10));
      lv_label_set_text(s_lbl_temp, buf);
      lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(UI_COLOR_TEMP_CYAN), 0);

      (void)snprintf(buf, sizeof(buf), "RH: %u.%u %%",
                     (unsigned int)(snap->humi_x10 / 10U),
                     (unsigned int)(snap->humi_x10 % 10U));
      lv_label_set_text(s_lbl_humi, buf);
      lv_obj_set_style_text_color(s_lbl_humi, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
    }
    else
    {
      lv_label_set_text(s_lbl_temp, "--.- \xC2\xB0" "C");
      lv_obj_set_style_text_color(s_lbl_temp, lv_color_hex(UI_COLOR_WARN_AMBER), 0);
      lv_label_set_text(s_lbl_humi, "RH: --.- %");
      lv_obj_set_style_text_color(s_lbl_humi, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
    }
  }

  /* 2. 告警栏 */
  if ((s_inited == 0U) || (snap->fault_mask != s_prev.fault_mask))
  {
    uint32_t alarm_bg = UI_COLOR_OK_GREEN;
    FormatAlarmText(snap->fault_mask, buf, sizeof(buf), &alarm_bg);
    lv_label_set_text(s_lbl_alarm, buf);
    lv_obj_set_style_bg_color(s_box_alarm, lv_color_hex(alarm_bg), 0);
  }

  /* 3. 风扇状态与转速读数 */
  if ((s_inited == 0U) || (snap->fan_state != s_prev.fan_state))
  {
    uint32_t st_color = UI_COLOR_TEXT_MAIN;
    if (snap->fan_state == FAN_FAULT)
    {
      st_color = UI_COLOR_ERR_RED;
    }
    else if (snap->fan_state == FAN_FAILSAFE)
    {
      st_color = UI_COLOR_WARN_AMBER;
    }
    else if ((snap->fan_state == FAN_AUTO_RUN) || (snap->fan_state == FAN_MAN_RUN))
    {
      st_color = UI_COLOR_OK_GREEN;
    }
    lv_label_set_text(s_lbl_fan_state, FanStateStr(snap->fan_state));
    lv_obj_set_style_text_color(s_lbl_fan_state, lv_color_hex(st_color), 0);
  }

  if ((s_inited == 0U) || (snap->actual_rpm != s_prev.actual_rpm) ||
      (snap->active_target_rpm != s_prev.active_target_rpm))
  {
    (void)snprintf(buf, sizeof(buf), "A:%u T:%u",
                   (unsigned int)snap->actual_rpm,
                   (unsigned int)snap->active_target_rpm);
    lv_label_set_text(s_lbl_rpm_info, buf);
  }

  /* 4. 控制模式与手动启停按钮状态 */
  if ((s_inited == 0U) || (snap->ctrl_mode != s_prev.ctrl_mode) ||
      (snap->manual_run != s_prev.manual_run))
  {
    uint8_t is_man = (snap->ctrl_mode == CTRL_MODE_MANUAL) ? 1U : 0U;

    lv_label_set_text(s_lbl_btn_mode, (is_man != 0U) ? "MAN" : "AUTO");
    lv_obj_set_style_bg_color(s_btn_mode,
                              lv_color_hex((is_man != 0U) ? UI_COLOR_RPM_AMBER
                                                          : UI_COLOR_ACCENT_BLUE),
                              0);

    lv_label_set_text(s_lbl_btn_run, (snap->manual_run != 0U) ? "RUN" : "STOP");
    lv_obj_set_style_bg_color(s_btn_run,
                              lv_color_hex((snap->manual_run != 0U) ? UI_COLOR_OK_GREEN
                                                                    : UI_COLOR_BTN_IDLE),
                              0);
    SetManualControlsEnabled(is_man);
  }

  /* 5. 手动目标转速（用户正按住滑块拖动时跳过同步，防止指针弹回） */
  if ((s_inited == 0U) || (snap->manual_target_rpm != s_prev.manual_target_rpm))
  {
    if (!lv_obj_has_state(s_slider_rpm, LV_STATE_PRESSED))
    {
      lv_slider_set_value(s_slider_rpm, (int32_t)snap->manual_target_rpm, LV_ANIM_OFF);
    }
    (void)snprintf(buf, sizeof(buf), "%u", (unsigned int)snap->manual_target_rpm);
    lv_label_set_text(s_lbl_man_rpm, buf);
  }

  s_prev = *snap;
  s_inited = 1U;
}
