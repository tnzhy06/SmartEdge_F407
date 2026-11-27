/**
 * @file    app_hmi.c
 * @brief   本地触控 HMI 任务（HmiTask）与 LVGL v9.6.0 显示/输入移植实现
 * @date    2026-11-27
 * @note    内存红线严格落实：
 *          - g_lvgl_mem[48 KB] 放置于 .ccmram（纯 CPU 访问）；
 *          - s_lcd_buf1/2[2 x 12.5 KB = 25 KB] 放置于 128 KB 主 RAM（走 SPI2 DMA1 Stream4）
 */

#include "app_hmi.h"
#include "app_cfg.h"
#include "app_data.h"
#include "app_ui.h"
#include "bsp_st7789.h"
#include "bsp_uart_cli.h"
#include "bsp_xpt2046.h"
#include "cmsis_os.h"
#include "lvgl.h"
#include <stddef.h>
#include <string.h>

/** @brief LVGL 主循环节拍周期（单位：ms） */
#define HMI_LOOP_MS 10U

/** @brief 业务状态差量刷新周期（单位：ms，依据 SDD 第 6.5 节） */
#define HMI_DATA_REFRESH_MS 250U

/** @brief LVGL 内存池统计更新周期（单位：ms） */
#define HMI_MEM_STAT_MS 1000U

/** @brief 单块部分刷屏缓冲行数（320 * 20 * 2 = 12800 B = 12.5 KB，整除 240 高为 12 片） */
#define LCD_BUF_LINES 20U

/** @brief LVGL v9 内部对象内存池（48 KB，放置于 .ccmram，绝不参与任何 DMA） */
static uint8_t g_lvgl_mem[48U * 1024U] __attribute__((section(".ccmram"), aligned(8)));

_Static_assert(sizeof(g_lvgl_mem) == LV_MEM_SIZE, "g_lvgl_mem size must match LV_MEM_SIZE");

/** @brief 双部分刷屏像素缓冲（25 KB，位于默认 128 KB 主 RAM，专供 SPI2 DMA1 Stream4） */
static uint8_t s_lcd_buf1[BSP_LCD_WIDTH * LCD_BUF_LINES * 2U] __attribute__((aligned(4)));
static uint8_t s_lcd_buf2[BSP_LCD_WIDTH * LCD_BUF_LINES * 2U] __attribute__((aligned(4)));

/** @brief LVGL 显示对象句柄（供 DMA 完成回调通知 lv_display_flush_ready） */
static lv_display_t *s_disp = NULL;

/** @brief 单次等待上一片 SPI2 DMA 刷屏完成的最长超时（单位：ms） */
#define HMI_LCD_DMA_TIMEOUT_MS 200U

/** @brief 跨任务发起触摸校准的原子请求标志 */
static uint8_t s_cal_req = 0U;

/** @brief 最近一次周期采集的 LVGL 内存池统计副本 */
static AppHmiMemStats_t s_mem_stats;

/** @brief HmiTask 单线程独享的配置与状态快照（移出任务栈，给 LVGL 深层渲染留足栈裕量） */
static AppConfig_t s_hmi_cfg;
static AppState_t s_hmi_snap;

/**
 * @brief  DMA1 Stream4 刷屏完成 ISR 回调：通知 LVGL 释放当前帧缓冲
 */
static void OnLcdFlushDoneIsr(void)
{
  if (s_disp != NULL)
  {
    lv_display_flush_ready(s_disp);
  }
}

/**
 * @brief  LVGL 显示刷屏回调（RGB565_SWAPPED 格式，直接零拷贝走 SPI2 DMA）
 * @param[in] disp   LVGL 显示对象句柄
 * @param[in] area   待刷新屏幕矩形区域
 * @param[in] px_map 已按大端 RGB565 排布好的像素首地址
 */
static void LvglFlushCb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
  uint32_t w;
  uint32_t h;
  uint32_t bytes;

  if ((area == NULL) || (px_map == NULL) || (area->x2 < area->x1) || (area->y2 < area->y1))
  {
    lv_display_flush_ready(disp);
    return;
  }

  w = (uint32_t)(area->x2 - area->x1 + 1);
  h = (uint32_t)(area->y2 - area->y1 + 1);
  bytes = w * h * 2UL;

  /* 仅在 Bsp_LcdFlushDma 返回 APP_OK 时才由 ISR 触发 OnLcdFlushDoneIsr；
     任意同步失败返回均在此统一释放 LVGL 刷屏标志，杜绝 while(disp->flushing) 死锁 */
  if (Bsp_LcdFlushDma((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2,
                      px_map, bytes, OnLcdFlushDoneIsr) != APP_OK)
  {
    lv_display_flush_ready(disp);
  }
}

/**
 * @brief  LVGL 等待刷屏完成回调：阻塞等待 lcdDmaSem 让出 CPU，并具备 200 ms 超时自恢复兜底
 * @param[in] disp LVGL 显示对象句柄
 */
static void LvglFlushWaitCb(lv_display_t *disp)
{
  (void)disp;
  (void)Bsp_LcdWaitIdle(HMI_LCD_DMA_TIMEOUT_MS);
}

/**
 * @brief  LVGL 触摸输入轮询回调（每 20 ms 由 LVGL 输入定时器调用）
 * @param[in]  indev 输入设备对象句柄
 * @param[out] data  填充触摸状态与屏幕坐标
 */
static void LvglTouchReadCb(lv_indev_t *indev, lv_indev_data_t *data)
{
  static int16_t s_last_x = 0;
  static int16_t s_last_y = 0;
  int16_t sx = 0;
  int16_t sy = 0;

  (void)indev;

  /* 全屏 4 点触摸校准期间拦截普通控件事件，防止靶点点击穿透到底层按钮 */
  if (UiDiag_IsTouchCalActive() != 0U)
  {
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    return;
  }

  if (Bsp_TouchReadScreen(&sx, &sy) != 0U)
  {
    s_last_x = sx;
    s_last_y = sy;
    data->point.x = sx;
    data->point.y = sy;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

/**
 * @brief  为 LVGL 提供 1 ms 系统滴答时基（绑定 FreeRTOS osKernelGetTickCount）
 * @return uint32_t 系统运行毫秒计数
 */
static uint32_t LvglTickCb(void)
{
  return osKernelGetTickCount();
}

/**
 * @brief  刷新 LVGL 内存池统计快照（仅在 HmiTask 内调用，用短调度锁防止 CliTask 读到撕裂数据）
 */
static void UpdateMemStats(void)
{
  lv_mem_monitor_t mon;
  AppHmiMemStats_t tmp;
  int32_t lock = -1;

  lv_mem_monitor(&mon);
  tmp.total_bytes = (uint32_t)mon.total_size;
  tmp.free_bytes = (uint32_t)mon.free_size;
  tmp.max_used = (uint32_t)mon.max_used;
  tmp.used_pct = mon.used_pct;

  if (osKernelGetState() == osKernelRunning)
  {
    lock = osKernelLock();
  }
  s_mem_stats = tmp;
  if (lock >= 0)
  {
    (void)osKernelRestoreLock(lock);
  }
}

void *AppHmi_GetLvglMemPool(size_t size)
{
  if (size > sizeof(g_lvgl_mem))
  {
    return NULL;
  }
  /* 默认启动代码不初始化 .ccmram，此处在交给 LVGL TLSF 前显式清零 */
  memset(g_lvgl_mem, 0, sizeof(g_lvgl_mem));
  return (void *)g_lvgl_mem;
}

void AppHmi_RequestTouchCal(void)
{
  __atomic_store_n(&s_cal_req, 1U, __ATOMIC_RELEASE);
}

void AppHmi_GetMemStats(AppHmiMemStats_t *out)
{
  int32_t lock = -1;

  if (out == NULL)
  {
    return;
  }

  /* 与 UpdateMemStats() 配套使用极短调度锁，防止低优先级 CliTask 读取时被抢占撕裂 */
  if (osKernelGetState() == osKernelRunning)
  {
    lock = osKernelLock();
  }
  *out = s_mem_stats;
  if (lock >= 0)
  {
    (void)osKernelRestoreLock(lock);
  }
}

void App_HmiTask(void)
{
  lv_indev_t *indev;
  uint32_t last_refresh_tick = 0U;
  uint32_t last_mem_tick = 0U;

  /* 1. 加载掉电保存的触摸校准参数并初始化硬件触摸与 LCD 控制器（此时背光仍为 0%） */
  AppCfg_GetCurrent(&s_hmi_cfg);
  Bsp_TouchInit(s_hmi_cfg.touch_cal);

  if (Bsp_LcdInit() != APP_OK)
  {
    Bsp_CliPrintf("[HMI] ERR: ST7789 init failed!\r\n");
  }

  /* 2. 初始化 LVGL v9.6.0，绑定 1 ms 滴答、320x240 大端双缓冲与 XPT2046 输入设备 */
  lv_init();
  lv_tick_set_cb(LvglTickCb);

  s_disp = lv_display_create((int32_t)BSP_LCD_WIDTH, (int32_t)BSP_LCD_HEIGHT);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(s_disp, s_lcd_buf1, s_lcd_buf2,
                         (uint32_t)sizeof(s_lcd_buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_disp, LvglFlushCb);
  lv_display_set_flush_wait_cb(s_disp, LvglFlushWaitCb);

  indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, LvglTouchReadCb);

  /* 3. 构建三页面界面并同步一次初始数据 */
  UiMain_Init();
  AppData_GetSnapshot(&s_hmi_snap);
  UiMain_Update(&s_hmi_snap);

  /* 4. 先渲染并 DMA 刷完首帧内容，再按配置占空比点亮背光，彻底杜绝开机花屏闪一下 */
  (void)lv_timer_handler();
  (void)Bsp_LcdWaitIdle(200U);
  Bsp_LcdSetBacklight(s_hmi_snap.backlight_pct);

  UpdateMemStats();
  last_refresh_tick = osKernelGetTickCount();
  last_mem_tick = last_refresh_tick;

  for (;;)
  {
    uint32_t now = osKernelGetTickCount();

    /* 响应来自串口 CLI "touch cal" 的跨任务校准请求 */
    if (__atomic_exchange_n(&s_cal_req, 0U, __ATOMIC_ACQ_REL) != 0U)
    {
      UiDiag_StartTouchCal();
    }

    /* 校准激活期驱动 4 点采集状态机 */
    if (UiDiag_IsTouchCalActive() != 0U)
    {
      UiDiag_TouchCalTick();
    }

    /* 调度 LVGL 渲染与输入轮询定时器 */
    (void)lv_timer_handler();

    /* 每 250 ms 拉取一次状态快照执行差量刷新 */
    if ((now - last_refresh_tick) >= HMI_DATA_REFRESH_MS)
    {
      last_refresh_tick = now;
      AppData_GetSnapshot(&s_hmi_snap);
      UiMain_Update(&s_hmi_snap);
    }

    /* 每 1000 ms 更新一次 LVGL 内存池占用统计供 CLI status 观测 */
    if ((now - last_mem_tick) >= HMI_MEM_STAT_MS)
    {
      last_mem_tick = now;
      UpdateMemStats();
    }

    osDelay(HMI_LOOP_MS);
  }
}
