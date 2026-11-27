/**
 * @file    ui_main.c
 * @brief   LVGL 公共顶栏、底栏三页导航框架与差量刷新分发实现
 * @date    2026-11-27
 * @note    横屏 320x240 布局：顶栏 28 px + 内容区 176 px + 底栏 36 px；纯英文 Montserrat 字体
 */

#include "app_ui.h"
#include "bsp_sys.h"
#include <stdio.h>
#include <string.h>

/** @brief 布局尺寸常量（像素） */
#define TOP_BAR_H 28
#define BOT_BAR_H 36
#define CONTENT_Y TOP_BAR_H
#define CONTENT_H (240 - TOP_BAR_H - BOT_BAR_H)

/** @brief 顶栏模式与外设状态小徽章控件句柄 */
static lv_obj_t *s_lbl_mode = NULL;
static lv_obj_t *s_badge_485 = NULL;
static lv_obj_t *s_badge_can = NULL;
static lv_obj_t *s_badge_eth = NULL;
static lv_obj_t *s_badge_tf = NULL;
static lv_obj_t *s_lbl_clock = NULL;

/** @brief 三个业务页面容器与底栏对应导航按钮 */
static lv_obj_t *s_pages[UI_PAGE_COUNT];
static lv_obj_t *s_nav_btns[UI_PAGE_COUNT];
static UiPageId_t s_active_page = UI_PAGE_OVERVIEW;

/** @brief 上次顶栏刷新缓存，用于差量比对减少无效重绘 */
static uint8_t s_top_inited = 0U;
static uint8_t s_prev_mode_tag = 0xFFU;
static uint16_t s_prev_fault = 0xFFFFU;
static uint8_t s_prev_sens_valid = 0xFFU;
static uint8_t s_prev_can_active = 0xFFU;
static uint8_t s_prev_eth = 0xFFU;
static uint8_t s_prev_tf = 0xFFU;
static uint32_t s_prev_unix = 0xFFFFFFFFUL;

/**
 * @brief  创建一个无边框、无滚动条的透明/定色基础容器
 * @param[in] parent 父对象
 * @param[in] x      X 坐标
 * @param[in] y      Y 坐标
 * @param[in] w      宽度
 * @param[in] h      高度
 * @param[in] bg_hex 背景色（RGB Hex）
 * @return lv_obj_t* 创建的容器句柄
 */
static lv_obj_t *CreateFlatBox(lv_obj_t *parent, int32_t x, int32_t y,
                               int32_t w, int32_t h, uint32_t bg_hex)
{
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_set_pos(box, x, y);
  lv_obj_set_size(box, w, h);
  lv_obj_set_style_bg_color(box, lv_color_hex(bg_hex), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_radius(box, 0, 0);
  lv_obj_set_style_pad_all(box, 0, 0);
  lv_obj_set_scrollable(box, false);
  return box;
}

/**
 * @brief  在顶栏创建一颗紧凑圆角状态徽章（含居中文字）
 * @param[in] parent 顶栏容器
 * @param[in] x      X 坐标
 * @param[in] w      宽度
 * @param[in] text   英文缩写文本
 * @return lv_obj_t* 徽章容器句柄
 */
static lv_obj_t *CreateStatusBadge(lv_obj_t *parent, int32_t x, int32_t w, const char *text)
{
  lv_obj_t *badge = lv_obj_create(parent);
  lv_obj_t *lbl;

  lv_obj_set_pos(badge, x, 4);
  lv_obj_set_size(badge, w, 20);
  lv_obj_set_style_bg_color(badge, lv_color_hex(UI_COLOR_BTN_IDLE), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_radius(badge, 4, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_set_scrollable(badge, false);

  lbl = lv_label_create(badge);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_center(lbl);

  return badge;
}

/**
 * @brief  切换当前可见页面并更新底栏导航按钮高亮样式
 * @param[in] target 目标页面 ID
 */
static void SwitchPage(UiPageId_t target)
{
  if (target >= UI_PAGE_COUNT)
  {
    return;
  }

  s_active_page = target;
  for (uint8_t i = 0U; i < (uint8_t)UI_PAGE_COUNT; i++)
  {
    if (i == (uint8_t)target)
    {
      lv_obj_set_hidden(s_pages[i], false);
      lv_obj_set_style_bg_color(s_nav_btns[i], lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
    }
    else
    {
      lv_obj_set_hidden(s_pages[i], true);
      lv_obj_set_style_bg_color(s_nav_btns[i], lv_color_hex(UI_COLOR_BTN_IDLE), 0);
    }
  }
  /* 切页时立刻按当前快照刷新目标页面，消除首次进入 Diag/Trend 时最多 250 ms 的占位文本闪现 */
  if (s_top_inited != 0U)
  {
    AppState_t snap;
    AppData_GetSnapshot(&snap);
    UiMain_Update(&snap);
  }
}

/**
 * @brief  底栏导航按钮点击回调
 * @param[in] e LVGL 事件指针（user_data 存放目标页面索引）
 */
static void OnNavBtnClicked(lv_event_t *e)
{
  uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);

  if (idx < (uintptr_t)UI_PAGE_COUNT)
  {
    SwitchPage((UiPageId_t)idx);
  }
}

/**
 * @brief  差量刷新顶栏模式徽章、4 个通信状态小标与实时时钟
 * @param[in] snap 最新系统快照
 */
static void UpdateTopBar(const AppState_t *snap)
{
  uint8_t mode_tag;

  /* 0=AUTO, 1=MAN, 2=SAFE（安全降级优先显示） */
  if (snap->fan_state == FAN_FAILSAFE)
  {
    mode_tag = 2U;
  }
  else if (snap->ctrl_mode == CTRL_MODE_MANUAL)
  {
    mode_tag = 1U;
  }
  else
  {
    mode_tag = 0U;
  }

  if ((s_top_inited == 0U) || (mode_tag != s_prev_mode_tag))
  {
    lv_obj_t *mode_box = lv_obj_get_parent(s_lbl_mode);
    s_prev_mode_tag = mode_tag;

    if (mode_tag == 2U)
    {
      lv_label_set_text(s_lbl_mode, "SAFE");
      lv_obj_set_style_bg_color(mode_box, lv_color_hex(UI_COLOR_WARN_AMBER), 0);
    }
    else if (mode_tag == 1U)
    {
      lv_label_set_text(s_lbl_mode, "MAN");
      lv_obj_set_style_bg_color(mode_box, lv_color_hex(UI_COLOR_RPM_AMBER), 0);
    }
    else
    {
      lv_label_set_text(s_lbl_mode, "AUTO");
      lv_obj_set_style_bg_color(mode_box, lv_color_hex(UI_COLOR_ACCENT_BLUE), 0);
    }
  }

  uint8_t can_active = (snap->actual_rpm > 0U) ? 1U : 0U;

  if ((s_top_inited == 0U) || (snap->fault_mask != s_prev_fault) ||
      (snap->sensor_valid != s_prev_sens_valid) || (can_active != s_prev_can_active) ||
      (snap->eth_link_up != s_prev_eth) || (snap->tf_ready != s_prev_tf))
  {
    uint32_t c_485;
    uint32_t c_can;
    uint32_t c_eth;
    uint32_t c_tf;
    uint16_t can_faults =
      (uint16_t)(FAULT_CAN_STALL | FAULT_CAN_CMD_TIMEOUT | FAULT_CAN_NODE_OFFLINE);

    s_prev_fault = snap->fault_mask;
    s_prev_sens_valid = snap->sensor_valid;
    s_prev_can_active = can_active;
    s_prev_eth = snap->eth_link_up;
    s_prev_tf = snap->tf_ready;

    if ((snap->fault_mask & FAULT_SENSOR_OFFLINE) != 0U)
    {
      c_485 = UI_COLOR_ERR_RED;
    }
    else if (snap->sensor_valid != 0U)
    {
      c_485 = UI_COLOR_OK_GREEN;
    }
    else
    {
      c_485 = UI_COLOR_BTN_IDLE; /* M2 未接 RS-485 采集中，显示中性灰 */
    }

    /* 仅在无 CAN 故障且收到真实转速反馈时将 CAN 徽章标绿，避免 M2 手动点 RUN 误标在线 */
    if ((snap->fault_mask & can_faults) != 0U)
    {
      c_can = UI_COLOR_ERR_RED;
    }
    else if (can_active != 0U)
    {
      c_can = UI_COLOR_OK_GREEN;
    }
    else
    {
      c_can = UI_COLOR_BTN_IDLE;
    }

    c_eth = (snap->eth_link_up != 0U) ? UI_COLOR_OK_GREEN : UI_COLOR_BTN_IDLE;

    if ((snap->fault_mask & FAULT_TF_ERROR) != 0U)
    {
      c_tf = UI_COLOR_ERR_RED;
    }
    else
    {
      c_tf = (snap->tf_ready != 0U) ? UI_COLOR_OK_GREEN : UI_COLOR_BTN_IDLE;
    }

    lv_obj_set_style_bg_color(s_badge_485, lv_color_hex(c_485), 0);
    lv_obj_set_style_bg_color(s_badge_can, lv_color_hex(c_can), 0);
    lv_obj_set_style_bg_color(s_badge_eth, lv_color_hex(c_eth), 0);
    lv_obj_set_style_bg_color(s_badge_tf, lv_color_hex(c_tf), 0);
  }

  if ((s_top_inited == 0U) || (snap->rtc_unix_ts != s_prev_unix))
  {
    BspDateTime_t dt = {0};
    char buf[12];

    s_prev_unix = snap->rtc_unix_ts;
    if (Bsp_RtcGetDateTime(&dt) == APP_OK)
    {
      (void)snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                     (unsigned int)dt.hour, (unsigned int)dt.min, (unsigned int)dt.sec);
    }
    else
    {
      (void)snprintf(buf, sizeof(buf), "--:--:--");
    }
    lv_label_set_text(s_lbl_clock, buf);
  }

  s_top_inited = 1U;
}

void UiMain_Init(void)
{
  static const char *k_nav_titles[UI_PAGE_COUNT] = {"Overview", "Trend", "Diag"};
  static const int32_t k_nav_x[UI_PAGE_COUNT] = {4, 109, 215};
  static const int32_t k_nav_w[UI_PAGE_COUNT] = {101, 102, 101};

  lv_obj_t *scr = lv_screen_active();
  lv_obj_t *top_bar;
  lv_obj_t *bot_bar;
  lv_obj_t *mode_badge;

  s_top_inited = 0U;

  lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG_MAIN), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);

  /* 1. 公共顶栏（y=0..27） */
  top_bar = CreateFlatBox(scr, 0, 0, 320, TOP_BAR_H, UI_COLOR_BG_BAR);
  lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(top_bar, 1, 0);
  lv_obj_set_style_border_color(top_bar, lv_color_hex(UI_COLOR_BORDER), 0);

  mode_badge = CreateStatusBadge(top_bar, 4, 48, "AUTO");
  s_lbl_mode = lv_obj_get_child(mode_badge, 0);

  s_badge_485 = CreateStatusBadge(top_bar, 58, 36, "485");
  s_badge_can = CreateStatusBadge(top_bar, 98, 38, "CAN");
  s_badge_eth = CreateStatusBadge(top_bar, 140, 38, "ETH");
  s_badge_tf = CreateStatusBadge(top_bar, 182, 28, "TF");

  s_lbl_clock = lv_label_create(top_bar);
  lv_label_set_text(s_lbl_clock, "00:00:00");
  lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lbl_clock, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
  lv_obj_align(s_lbl_clock, LV_ALIGN_RIGHT_MID, -6, 0);

  /* 2. 中部三页独立内容容器（y=28..203，切页仅切换可见性零堆碎片） */
  for (uint8_t i = 0U; i < (uint8_t)UI_PAGE_COUNT; i++)
  {
    s_pages[i] = CreateFlatBox(scr, 0, CONTENT_Y, 320, CONTENT_H, UI_COLOR_BG_MAIN);
  }

  UiOverview_Create(s_pages[UI_PAGE_OVERVIEW]);
  UiTrend_Create(s_pages[UI_PAGE_TREND]);
  UiDiag_Create(s_pages[UI_PAGE_DIAG]);

  /* 3. 公共底栏导航（y=204..239） */
  bot_bar = CreateFlatBox(scr, 0, 240 - BOT_BAR_H, 320, BOT_BAR_H, UI_COLOR_BG_BAR);
  lv_obj_set_style_border_side(bot_bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(bot_bar, 1, 0);
  lv_obj_set_style_border_color(bot_bar, lv_color_hex(UI_COLOR_BORDER), 0);

  for (uint8_t i = 0U; i < (uint8_t)UI_PAGE_COUNT; i++)
  {
    lv_obj_t *btn = lv_button_create(bot_bar);
    lv_obj_t *lbl = lv_label_create(btn);

    lv_obj_set_pos(btn, k_nav_x[i], 3);
    lv_obj_set_size(btn, k_nav_w[i], 30);
    lv_obj_set_style_radius(btn, 5, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, OnNavBtnClicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    lv_label_set_text(lbl, k_nav_titles[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_COLOR_TEXT_MAIN), 0);
    lv_obj_center(lbl);

    s_nav_btns[i] = btn;
  }

  SwitchPage(UI_PAGE_OVERVIEW);
}

void UiMain_Update(const AppState_t *snap)
{
  if (snap == NULL)
  {
    return;
  }

  UpdateTopBar(snap);

  /* 始终仅按需刷新当前展示页，减轻无谓的 CPU 渲染开销 */
  if (s_active_page == UI_PAGE_OVERVIEW)
  {
    UiOverview_Update(snap);
  }
  else if (s_active_page == UI_PAGE_TREND)
  {
    UiTrend_Update(snap);
  }
  else
  {
    UiDiag_Update(snap);
  }
}
