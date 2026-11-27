/**
 * @file    ui_page_diag.c
 * @brief   系统诊断与设置页（Diag）及全屏 4 点电阻触摸校准向导实现
 * @date    2026-11-27
 * @note    支持查看复位原因、网络/RTC/TF/CAN/RS-485 详细诊断、温控阈值设置、背光滑块与 4 点校准
 */

#include "app_ui.h"
#include "app_cfg.h"
#include "bsp_sys.h"
#include "bsp_uart_cli.h"
#include "bsp_xpt2046.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>

/** @brief 4 点触摸校准状态机状态定义（严格对齐 SDD 第 6.5 节） */
typedef enum
{
  CAL_IDLE = 0U,         /**< 空闲（未激活遮罩） */
  CAL_WAIT_PRESS,        /**< 等待用户按下当前靶点并采集 3 次稳定样本 */
  CAL_WAIT_RELEASE,      /**< 等待用户完全松手，防止连点串扰下一个靶点 */
  CAL_SHOW_RESULT_OK,    /**< 校准成功展示提示（停留 1 s 自动关闭） */
  CAL_SHOW_RESULT_RETRY  /**< 残差超限提示重试（停留 1.2 s 回到靶点 0） */
} TouchCalState_t;

/** @brief 4 个屏幕校准靶点坐标（四角对称分布，距边 30 px 避免电阻屏边缘非线性） */
static const int16_t k_cal_scr_x[4] = {30, 290, 30, 290};
static const int16_t k_cal_scr_y[4] = {30, 30, 210, 210};

/** @brief 校准无操作超时自动退出时长（单位：ms） */
#define CAL_IDLE_TIMEOUT_MS 30000U

/** @brief 左侧诊断信息标签句柄 */
static lv_obj_t *s_lbl_rst = NULL;
static lv_obj_t *s_lbl_rtc = NULL;
static lv_obj_t *s_lbl_up = NULL;
static lv_obj_t *s_lbl_ip = NULL;
static lv_obj_t *s_lbl_net = NULL;
static lv_obj_t *s_lbl_err = NULL;
static lv_obj_t *s_lbl_can = NULL;

/** @brief 右侧设置区控件句柄 */
static lv_obj_t *s_lbl_ton = NULL;
static lv_obj_t *s_lbl_toff = NULL;
static lv_obj_t *s_lbl_tip = NULL;
static lv_obj_t *s_lbl_bl = NULL;
static lv_obj_t *s_slider_bl = NULL;

/** @brief 差量刷新缓存 */
static uint8_t s_diag_inited = 0U;
static AppState_t s_prev;

/** @brief 全屏 4 点触摸校准遮罩控件与状态变量 */
static lv_obj_t *s_cal_overlay = NULL;
static lv_obj_t *s_cal_cross_h = NULL;
static lv_obj_t *s_cal_cross_v = NULL;
static lv_obj_t *s_cal_msg = NULL;
static lv_obj_t *s_cal_sub = NULL;

static TouchCalState_t s_cal_state = CAL_IDLE;
static uint8_t s_cal_idx = 0U;
static uint8_t s_cal_press_cnt = 0U;
static uint8_t s_cal_rel_cnt = 0U;
static uint32_t s_cal_sum_x = 0U;
static uint32_t s_cal_sum_y = 0U;
static uint16_t s_cal_adc_x[4];
static uint16_t s_cal_adc_y[4];
static uint32_t s_cal_tick = 0U;

/**
 * @brief  创建标准圆角卡片容器
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
 * @brief  创建紧凑的阈值 +/- 微调按钮
 * @param[in] parent 父容器指针
 * @param[in] x      X 坐标
 * @param[in] y      Y 坐标
 * @param[in] text   按钮文本（"+" 或 "-"）
 * @param[in] cb     点击事件回调
 * @param[in] tag    回调区分标签（+/-1 对应 T_ON，+/-2 对应 T_OFF）
 * @return lv_obj_t* 按钮对象指针
 */
static lv_obj_t *CreateSmallBtn(lv_obj_t *parent, int32_t x, int32_t y,
                                const char *text, lv_event_cb_t cb, int32_t tag)
{
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_t *lbl = lv_label_create(btn);

  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, 30, 26);
  lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_BTN_DARK), 0);
  lv_obj_set_style_radius(btn, 4, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)tag);

  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(lbl);
  return btn;
}

/**
 * @brief  将 IWDG 卡死任务位图格式化为简短英文任务名（如 "CTRL"）
 * @param[in] mask 卡死任务位图（HB_BIT_* 组合）
 * @return const char* 英文缩写任务名
 */
static const char *HungTaskShortStr(uint16_t mask)
{
  if ((mask & HB_BIT_CONTROL) != 0U)
  {
    return "CTRL";
  }
  if ((mask & HB_BIT_FANSIM) != 0U)
  {
    return "SIM";
  }
  if ((mask & HB_BIT_SENSOR) != 0U)
  {
    return "SENS";
  }
  if ((mask & HB_BIT_NET) != 0U)
  {
    return "NET";
  }
  if ((mask & HB_BIT_HMI) != 0U)
  {
    return "HMI";
  }
  if ((mask & HB_BIT_MONITOR) != 0U)
  {
    return "MON";
  }
  return "?";
}

/**
 * @brief  将十字靶标移动至指定校准点序号（0..3）并更新提示文案
 * @param[in] idx 靶点序号（0..3）
 */
static void MoveCalTarget(uint8_t idx)
{
  char buf[40];
  int32_t cx = (int32_t)k_cal_scr_x[idx];
  int32_t cy = (int32_t)k_cal_scr_y[idx];

  lv_obj_set_hidden(s_cal_cross_h, false);
  lv_obj_set_hidden(s_cal_cross_v, false);
  lv_obj_set_pos(s_cal_cross_h, cx - 12, cy - 1);
  lv_obj_set_pos(s_cal_cross_v, cx - 1, cy - 12);

  (void)snprintf(buf, sizeof(buf), "Touch Crosshair (%u/4)", (unsigned int)(idx + 1U));
  lv_label_set_text(s_cal_msg, buf);
  lv_obj_set_style_text_color(s_cal_msg, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_label_set_text(s_cal_sub, "Press & hold target | USER_KEY to cancel");
}

/**
 * @brief  销毁全屏校准遮罩并恢复普通界面态
 */
static void CloseTouchCalOverlay(void)
{
  if (s_cal_overlay != NULL)
  {
    lv_obj_delete(s_cal_overlay);
    s_cal_overlay = NULL;
    s_cal_cross_h = NULL;
    s_cal_cross_v = NULL;
    s_cal_msg = NULL;
    s_cal_sub = NULL;
  }
  s_cal_state = CAL_IDLE;
}

/**
 * @brief  温控阈值 T_ON / T_OFF 微调按钮回调（步进 0.5 °C，带实时合法性提示）
 * @param[in] e LVGL 点击事件指针（user_data：+1/-1 调 T_ON，+2/-2 调 T_OFF）
 */
static void OnThreshBtnClicked(lv_event_t *e)
{
  int32_t tag = (int32_t)(intptr_t)lv_event_get_user_data(e);
  AppState_t snap;
  int16_t next_start;
  int16_t next_stop;
  AppErr_t err;

  AppData_GetSnapshot(&snap);
  next_start = snap.temp_start_x10;
  next_stop = snap.temp_stop_x10;

  if (tag == 1)
  {
    next_start = (int16_t)(next_start + 5);
  }
  else if (tag == -1)
  {
    next_start = (int16_t)(next_start - 5);
  }
  else if (tag == 2)
  {
    next_stop = (int16_t)(next_stop + 5);
  }
  else if (tag == -2)
  {
    next_stop = (int16_t)(next_stop - 5);
  }

  err = AppCtrl_SetThresholds(next_start, next_stop);
  if (err == APP_OK)
  {
    lv_label_set_text(s_lbl_tip, "Saved in 2s (Flash)");
    lv_obj_set_style_text_color(s_lbl_tip, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
    AppData_GetSnapshot(&snap);
    UiDiag_Update(&snap);
  }
  else
  {
    lv_label_set_text(s_lbl_tip, "Err: 0..70C, dT>=1");
    lv_obj_set_style_text_color(s_lbl_tip, lv_color_hex(UI_COLOR_ERR_RED), 0);
  }
}

/**
 * @brief  背光亮度滑块拖动回调（实时调节 TIM12 PWM 并触发 2 s 防抖落盘）
 * @param[in] e LVGL 数值变更事件指针
 */
static void OnBacklightSliderChanged(lv_event_t *e)
{
  lv_obj_t *slider = lv_event_get_target_obj(e);
  int32_t pct = lv_slider_get_value(slider);
  char buf[16];

  if (AppCtrl_SetBacklight((uint8_t)pct) == APP_OK)
  {
    (void)snprintf(buf, sizeof(buf), "BL: %ld%%", (long)pct);
    lv_label_set_text(s_lbl_bl, buf);
  }
}

/**
 * @brief  [Touch Cal] 按钮点击回调
 * @param[in] e LVGL 点击事件指针
 */
static void OnTouchCalBtnClicked(lv_event_t *e)
{
  (void)e;
  UiDiag_StartTouchCal();
}

void UiDiag_Create(lv_obj_t *parent)
{
  lv_obj_t *left_card;
  lv_obj_t *right_card;
  lv_obj_t *lbl;
  lv_obj_t *btn_cal;

  s_diag_inited = 0U;

  /* ==================== 左侧：系统诊断信息卡片 ==================== */
  left_card = CreateCard(parent, 4, 4, 152, 168);

  s_lbl_rst = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_rst, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_rst, lv_color_hex(UI_COLOR_WARN_AMBER), 0);
  lv_obj_set_pos(s_lbl_rst, 0, 2);
  lv_label_set_text(s_lbl_rst, "RST: --");

  s_lbl_rtc = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_rtc, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_rtc, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_set_pos(s_lbl_rtc, 0, 24);
  lv_label_set_text(s_lbl_rtc, "DATE: ----");

  s_lbl_up = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_up, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_up, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_up, 0, 46);
  lv_label_set_text(s_lbl_up, "UP: 0s");

  s_lbl_ip = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_ip, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_ip, lv_color_hex(UI_COLOR_TEMP_CYAN), 0);
  lv_obj_set_pos(s_lbl_ip, 0, 68);
  lv_label_set_text(s_lbl_ip, "IP: 192.168.1.100");

  s_lbl_net = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_net, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_net, 0, 90);
  lv_label_set_text(s_lbl_net, "ETH:DN NTP:WAIT");

  s_lbl_err = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_err, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_err, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_err, 0, 112);
  lv_label_set_text(s_lbl_err, "TF:NO E485:0 C:0");

  s_lbl_can = lv_label_create(left_card);
  lv_obj_set_style_text_font(s_lbl_can, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_can, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
  lv_obj_set_pos(s_lbl_can, 0, 134);
  lv_label_set_text(s_lbl_can, "CAN: OK");

  /* ==================== 右侧：阈值、背光与触摸校准设置卡片 ==================== */
  right_card = CreateCard(parent, 160, 4, 156, 168);

  /* T_ON 启动阈值设置行 */
  lbl = lv_label_create(right_card);
  lv_label_set_text(lbl, "ON");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(lbl, 0, 6);

  (void)CreateSmallBtn(right_card, 32, 0, "-", OnThreshBtnClicked, -1);
  s_lbl_ton = lv_label_create(right_card);
  lv_label_set_text(s_lbl_ton, "30.0");
  lv_obj_set_style_text_font(s_lbl_ton, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_ton, lv_color_hex(UI_COLOR_TEMP_CYAN), 0);
  lv_obj_set_pos(s_lbl_ton, 68, 5);
  (void)CreateSmallBtn(right_card, 110, 0, "+", OnThreshBtnClicked, 1);

  /* T_OFF 停止阈值设置行 */
  lbl = lv_label_create(right_card);
  lv_label_set_text(lbl, "OFF");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(lbl, 0, 38);

  (void)CreateSmallBtn(right_card, 32, 32, "-", OnThreshBtnClicked, -2);
  s_lbl_toff = lv_label_create(right_card);
  lv_label_set_text(s_lbl_toff, "27.0");
  lv_obj_set_style_text_font(s_lbl_toff, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_toff, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
  lv_obj_set_pos(s_lbl_toff, 68, 37);
  (void)CreateSmallBtn(right_card, 110, 32, "+", OnThreshBtnClicked, 2);

  /* 阈值合法性即时提示 */
  s_lbl_tip = lv_label_create(right_card);
  lv_label_set_text(s_lbl_tip, "Step 0.5C, dT>=1.0");
  lv_obj_set_style_text_font(s_lbl_tip, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_tip, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_set_pos(s_lbl_tip, 0, 64);

  /* 背光亮度滑块 */
  s_lbl_bl = lv_label_create(right_card);
  lv_label_set_text(s_lbl_bl, "BL: 80%");
  lv_obj_set_style_text_font(s_lbl_bl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lbl_bl, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_set_pos(s_lbl_bl, 0, 84);

  s_slider_bl = lv_slider_create(right_card);
  lv_obj_set_pos(s_slider_bl, 4, 104);
  lv_obj_set_size(s_slider_bl, 134, 10);
  lv_slider_set_range(s_slider_bl, (int32_t)APP_BACKLIGHT_MIN_PCT, (int32_t)APP_BACKLIGHT_MAX_PCT);
  lv_slider_set_value(s_slider_bl, 80, LV_ANIM_OFF);
  lv_obj_add_event_cb(s_slider_bl, OnBacklightSliderChanged, LV_EVENT_VALUE_CHANGED, NULL);

  /* 4 点触摸校准入口按钮 */
  btn_cal = lv_button_create(right_card);
  lv_obj_set_pos(btn_cal, 0, 126);
  lv_obj_set_size(btn_cal, 142, 28);
  lv_obj_set_style_bg_color(btn_cal, lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
  lv_obj_set_style_radius(btn_cal, 5, 0);
  lv_obj_set_style_shadow_width(btn_cal, 0, 0);
  lv_obj_set_style_pad_all(btn_cal, 0, 0);
  lv_obj_add_event_cb(btn_cal, OnTouchCalBtnClicked, LV_EVENT_CLICKED, NULL);

  lbl = lv_label_create(btn_cal);
  lv_label_set_text(lbl, "Touch Cal");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(lbl);
}

void UiDiag_Update(const AppState_t *snap)
{
  char buf[40];

  if (snap == NULL)
  {
    return;
  }

  /* 1. 上次复位原因（仅初始化刷新一次，或发生变化时刷新） */
  if ((s_diag_inited == 0U) || (snap->reset_reason != s_prev.reset_reason) ||
      (snap->hung_task_mask != s_prev.hung_task_mask))
  {
    if ((snap->reset_reason == RESET_REASON_IWDG) && (snap->hung_task_mask != 0U))
    {
      (void)snprintf(buf, sizeof(buf), "RST: IWDG(%s)", HungTaskShortStr(snap->hung_task_mask));
    }
    else
    {
      (void)snprintf(buf, sizeof(buf), "RST: %s",
                     Bsp_SysResetReasonStr((ResetReason_t)snap->reset_reason));
    }
    lv_label_set_text(s_lbl_rst, buf);
  }

  /* 2. RTC 日期与运行时长 */
  if ((s_diag_inited == 0U) || (snap->uptime_sec != s_prev.uptime_sec) ||
      (snap->rtc_unix_ts != s_prev.rtc_unix_ts))
  {
    BspDateTime_t dt = {0};
    if (Bsp_RtcGetDateTime(&dt) == APP_OK)
    {
      (void)snprintf(buf, sizeof(buf), "RTC: %04u-%02u-%02u",
                     (unsigned int)dt.year, (unsigned int)dt.month, (unsigned int)dt.day);
    }
    else
    {
      (void)snprintf(buf, sizeof(buf), "RTC: ERR");
    }
    lv_label_set_text(s_lbl_rtc, buf);

    (void)snprintf(buf, sizeof(buf), "UP:%lus CFG:%s",
                   (unsigned long)snap->uptime_sec,
                   (AppCfg_IsLoadedFromFlash() != 0U) ? "FLS" : "DEF");
    lv_label_set_text(s_lbl_up, buf);
  }

  /* 3. IP 与网络/SNTP 状态 */
  if ((s_diag_inited == 0U) || (snap->ipv4_addr != s_prev.ipv4_addr))
  {
    uint32_t ip = snap->ipv4_addr;
    (void)snprintf(buf, sizeof(buf), "IP:%u.%u.%u.%u",
                   (unsigned int)(ip & 0xFFU),
                   (unsigned int)((ip >> 8) & 0xFFU),
                   (unsigned int)((ip >> 16) & 0xFFU),
                   (unsigned int)((ip >> 24) & 0xFFU));
    lv_label_set_text(s_lbl_ip, buf);
  }

  if ((s_diag_inited == 0U) || (snap->eth_link_up != s_prev.eth_link_up) ||
      (snap->sntp_synced != s_prev.sntp_synced))
  {
    (void)snprintf(buf, sizeof(buf), "ETH:%s NTP:%s",
                   (snap->eth_link_up != 0U) ? "UP" : "DN",
                   (snap->sntp_synced != 0U) ? "OK" : "WAIT");
    lv_label_set_text(s_lbl_net, buf);
  }

  /* 4. TF 与通信错误计数 */
  if ((s_diag_inited == 0U) || (snap->tf_ready != s_prev.tf_ready) ||
      (snap->rs485_err_cnt != s_prev.rs485_err_cnt) ||
      (snap->can_err_cnt != s_prev.can_err_cnt))
  {
    (void)snprintf(buf, sizeof(buf), "TF:%s 4:%lu C:%lu",
                   (snap->tf_ready != 0U) ? "OK" : "NO",
                   (unsigned long)snap->rs485_err_cnt,
                   (unsigned long)snap->can_err_cnt);
    lv_label_set_text(s_lbl_err, buf);
  }

  /* 5. 三类 CAN 故障细分显示 */
  if ((s_diag_inited == 0U) || (snap->fault_mask != s_prev.fault_mask))
  {
    uint16_t can_m =
      (uint16_t)(snap->fault_mask &
                 (FAULT_CAN_STALL | FAULT_CAN_CMD_TIMEOUT | FAULT_CAN_NODE_OFFLINE));
    if (can_m == 0U)
    {
      lv_label_set_text(s_lbl_can, "CAN: OK");
      lv_obj_set_style_text_color(s_lbl_can, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
    }
    else
    {
      size_t p = (size_t)snprintf(buf, sizeof(buf), "CAN:");
      if (((can_m & FAULT_CAN_STALL) != 0U) && (p < sizeof(buf)))
      {
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, " STL");
      }
      if (((can_m & FAULT_CAN_CMD_TIMEOUT) != 0U) && (p < sizeof(buf)))
      {
        p += (size_t)snprintf(buf + p, sizeof(buf) - p, " TMO");
      }
      if (((can_m & FAULT_CAN_NODE_OFFLINE) != 0U) && (p < sizeof(buf)))
      {
        (void)snprintf(buf + p, sizeof(buf) - p, " OFF");
      }
      lv_label_set_text(s_lbl_can, buf);
      lv_obj_set_style_text_color(s_lbl_can, lv_color_hex(UI_COLOR_ERR_RED), 0);
    }
  }

  /* 6. 温控阈值与背光滑块 */
  if ((s_diag_inited == 0U) || (snap->temp_start_x10 != s_prev.temp_start_x10))
  {
    (void)snprintf(buf, sizeof(buf), "%d.%d",
                   (int)(snap->temp_start_x10 / 10), (int)(snap->temp_start_x10 % 10));
    lv_label_set_text(s_lbl_ton, buf);
  }

  if ((s_diag_inited == 0U) || (snap->temp_stop_x10 != s_prev.temp_stop_x10))
  {
    (void)snprintf(buf, sizeof(buf), "%d.%d",
                   (int)(snap->temp_stop_x10 / 10), (int)(snap->temp_stop_x10 % 10));
    lv_label_set_text(s_lbl_toff, buf);
  }

  if ((s_diag_inited == 0U) || (snap->backlight_pct != s_prev.backlight_pct))
  {
    if (!lv_obj_has_state(s_slider_bl, LV_STATE_PRESSED))
    {
      lv_slider_set_value(s_slider_bl, (int32_t)snap->backlight_pct, LV_ANIM_OFF);
    }
    (void)snprintf(buf, sizeof(buf), "BL: %u%%", (unsigned int)snap->backlight_pct);
    lv_label_set_text(s_lbl_bl, buf);
  }

  s_prev = *snap;
  s_diag_inited = 1U;
}

void UiDiag_StartTouchCal(void)
{
  if (s_cal_state != CAL_IDLE)
  {
    return;
  }

  /* 在 lv_layer_top() 创建全屏不透明校准遮罩，屏蔽底层页面交互 */
  s_cal_overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_pos(s_cal_overlay, 0, 0);
  lv_obj_set_size(s_cal_overlay, 320, 240);
  lv_obj_set_style_bg_color(s_cal_overlay, lv_color_hex(0x020617UL), 0);
  lv_obj_set_style_bg_opa(s_cal_overlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_cal_overlay, 0, 0);
  lv_obj_set_style_radius(s_cal_overlay, 0, 0);
  lv_obj_set_style_pad_all(s_cal_overlay, 0, 0);
  lv_obj_set_scrollable(s_cal_overlay, false);

  /* 高对比黄色十字靶标（横竖各 25x3 px 矩形条） */
  s_cal_cross_h = lv_obj_create(s_cal_overlay);
  lv_obj_set_size(s_cal_cross_h, 25, 3);
  lv_obj_set_style_bg_color(s_cal_cross_h, lv_color_hex(0xFACC15UL), 0);
  lv_obj_set_style_bg_opa(s_cal_cross_h, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_cal_cross_h, 0, 0);
  lv_obj_set_style_radius(s_cal_cross_h, 0, 0);
  lv_obj_set_style_pad_all(s_cal_cross_h, 0, 0);

  s_cal_cross_v = lv_obj_create(s_cal_overlay);
  lv_obj_set_size(s_cal_cross_v, 3, 25);
  lv_obj_set_style_bg_color(s_cal_cross_v, lv_color_hex(0xFACC15UL), 0);
  lv_obj_set_style_bg_opa(s_cal_cross_v, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_cal_cross_v, 0, 0);
  lv_obj_set_style_radius(s_cal_cross_v, 0, 0);
  lv_obj_set_style_pad_all(s_cal_cross_v, 0, 0);

  s_cal_msg = lv_label_create(s_cal_overlay);
  lv_obj_set_style_text_font(s_cal_msg, &lv_font_montserrat_16, 0);
  lv_obj_align(s_cal_msg, LV_ALIGN_CENTER, 0, -10);

  s_cal_sub = lv_label_create(s_cal_overlay);
  lv_obj_set_style_text_font(s_cal_sub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_cal_sub, lv_color_hex(UI_COLOR_TEXT_MUTED), 0);
  lv_obj_align(s_cal_sub, LV_ALIGN_CENTER, 0, 16);

  s_cal_idx = 0U;
  s_cal_press_cnt = 0U;
  s_cal_rel_cnt = 0U;
  s_cal_sum_x = 0U;
  s_cal_sum_y = 0U;
  s_cal_tick = osKernelGetTickCount();
  /* 先等触发校准的手指松开，再开始采集第 1 点 */
  s_cal_state = CAL_WAIT_RELEASE;
  MoveCalTarget(0U);
}

uint8_t UiDiag_IsTouchCalActive(void)
{
  return (s_cal_state != CAL_IDLE) ? 1U : 0U;
}

void UiDiag_TouchCalTick(void)
{
  uint16_t rx = 0U;
  uint16_t ry = 0U;
  uint8_t pressed;
  uint32_t now = osKernelGetTickCount();

  if (s_cal_state == CAL_IDLE)
  {
    return;
  }

  if (s_cal_state == CAL_SHOW_RESULT_OK)
  {
    /* 成功提示态：满 1 s 或按下 USER_KEY 提前关闭，不误报 cancelled 日志 */
    if (((now - s_cal_tick) >= 1000U) || (Bsp_KeyIsPressed() != 0U))
    {
      CloseTouchCalOverlay();
    }
    return;
  }

  /* 其余采集/重试态允许按板载 USER_KEY (PA0) 或 30 s 无操作取消校准，防止卡死在遮罩页 */
  if ((Bsp_KeyIsPressed() != 0U) || ((now - s_cal_tick) >= CAL_IDLE_TIMEOUT_MS))
  {
    Bsp_CliPrintf("[HMI] Touch calibration cancelled.\r\n");
    CloseTouchCalOverlay();
    return;
  }

  if (s_cal_state == CAL_SHOW_RESULT_RETRY)
  {
    if ((now - s_cal_tick) >= 1200U)
    {
      s_cal_idx = 0U;
      s_cal_press_cnt = 0U;
      s_cal_rel_cnt = 0U;
      s_cal_sum_x = 0U;
      s_cal_sum_y = 0U;
      s_cal_tick = now;
      /* 重试前先等手指完全松开，防止停留在屏幕上的手指误采靶点 1 */
      s_cal_state = CAL_WAIT_RELEASE;
      MoveCalTarget(0U);
    }
    return;
  }

  pressed = Bsp_TouchReadRaw(&rx, &ry);

  if (s_cal_state == CAL_WAIT_RELEASE)
  {
    if (pressed == 0U)
    {
      s_cal_rel_cnt++;
      if (s_cal_rel_cnt >= 5U)
      {
        s_cal_rel_cnt = 0U;
        s_cal_press_cnt = 0U;
        s_cal_sum_x = 0U;
        s_cal_sum_y = 0U;
        s_cal_state = CAL_WAIT_PRESS;
        MoveCalTarget(s_cal_idx);
      }
    }
    else
    {
      s_cal_rel_cnt = 0U;
    }
    return;
  }

  /* CAL_WAIT_PRESS：连续采集 3 帧有效原始坐标求均值 */
  if (pressed != 0U)
  {
    s_cal_tick = now;
    s_cal_sum_x += (uint32_t)rx;
    s_cal_sum_y += (uint32_t)ry;
    s_cal_press_cnt++;

    if (s_cal_press_cnt >= 3U)
    {
      s_cal_adc_x[s_cal_idx] = (uint16_t)(s_cal_sum_x / 3U);
      s_cal_adc_y[s_cal_idx] = (uint16_t)(s_cal_sum_y / 3U);
      Bsp_CliPrintf("[HMI] Cal Pt%u raw=(%u, %u)\r\n",
                    (unsigned int)(s_cal_idx + 1U),
                    (unsigned int)s_cal_adc_x[s_cal_idx],
                    (unsigned int)s_cal_adc_y[s_cal_idx]);

      s_cal_idx++;
      if (s_cal_idx < 4U)
      {
        s_cal_rel_cnt = 0U;
        s_cal_state = CAL_WAIT_RELEASE;
        lv_label_set_text(s_cal_sub, "Release finger...");
      }
      else
      {
        float solved[6];

        lv_obj_set_hidden(s_cal_cross_h, true);
        lv_obj_set_hidden(s_cal_cross_v, true);
        s_cal_tick = now;

        if (Bsp_TouchSolveAffine4Pt(s_cal_adc_x, s_cal_adc_y,
                                    k_cal_scr_x, k_cal_scr_y, solved) == APP_OK)
        {
          Bsp_TouchSetCal(solved);
          AppCfg_UpdateTouchCal(solved);
          lv_label_set_text(s_cal_msg, "Calibration OK!");
          lv_obj_set_style_text_color(s_cal_msg, lv_color_hex(UI_COLOR_HUMI_GREEN), 0);
          lv_label_set_text(s_cal_sub, "Saving to Sector 11...");
          Bsp_CliPrintf("[HMI] Touch calibration passed & queued for Flash save.\r\n");
          s_cal_state = CAL_SHOW_RESULT_OK;
        }
        else
        {
          lv_label_set_text(s_cal_msg, "Cal Failed! Retry...");
          lv_obj_set_style_text_color(s_cal_msg, lv_color_hex(UI_COLOR_ERR_RED), 0);
          lv_label_set_text(s_cal_sub, "4th point residual > 12 px");
          Bsp_CliPrintf("[HMI] Touch calibration failed (residual > 12 px), retrying.\r\n");
          s_cal_state = CAL_SHOW_RESULT_RETRY;
        }
      }
    }
  }
  else
  {
    s_cal_press_cnt = 0U;
    s_cal_sum_x = 0U;
    s_cal_sum_y = 0U;
  }
}
