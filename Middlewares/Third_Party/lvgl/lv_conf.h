/**
 * @file    lv_conf.h
 * @brief   LVGL v9.6.0 项目定制配置文件（适配 SmartEdge_F407 STM32F407VGT6）
 * @date    2026-11-27
 * @note    48 KB 内部动态堆通过 AppHmi_GetLvglMemPool() 放置在 .ccmram（仅 CPU 访问）；
 *          默认颜色格式启用 LV_COLOR_FORMAT_RGB565_SWAPPED 直配 ST7789 大端 SPI DMA；
 *          纯英文界面，仅开启内置 Montserrat 12/14/16/24 字体，不引入中文字库。
 */

/* clang-format off */
#if 1 /* 启用本配置 */

#ifndef LV_CONF_H
#define LV_CONF_H

/*============================================================================
 * 内存管理与标准库
 *============================================================================*/

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING  LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_BUILTIN

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN
  /* LVGL 内部对象内存池：48 KB 放置于 CCMRAM（绝不用于 DMA） */
  #define LV_MEM_SIZE         (48U * 1024U)
  #define LV_MEM_ADR          0
  #define LV_MEM_POOL_INCLUDE "app_hmi.h"
  #define LV_MEM_POOL_ALLOC   AppHmi_GetLvglMemPool
#endif

#define LV_STDINT_INCLUDE   "stdint.h"
#define LV_STDDEF_INCLUDE   "stddef.h"
#define LV_STDBOOL_INCLUDE  "stdbool.h"
#define LV_INTTYPES_INCLUDE "inttypes.h"
#define LV_LIMITS_INCLUDE   "limits.h"
#define LV_STDARG_INCLUDE   "stdarg.h"

/*============================================================================
 * 操作系统适配
 *============================================================================*/

/* 遵循单线程铁律：所有 lv_* 调用严格限定在 HmiTask 内执行，无需开启 LVGL 内部 OS 线程 */
#define LV_USE_OS LV_OS_NONE

/*============================================================================
 * 渲染配置
 *============================================================================*/

/* 直接输出大端 RGB565，匹配 ST7789 SPI MSB-first 传输，零刷屏字节交换开销 */
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565_SWAPPED

#define LV_COLOR_MIX_ROUND_OFS  0
#define LV_DEF_REFR_PERIOD      20 /* 20 ms 周期（50 Hz 刷新与 XPT2046 触控扫描） */
#define LV_DPI_DEF              130
#define LV_DRAW_BUF_STRIDE_ALIGN 1
#define LV_DRAW_BUF_ALIGN       4

#define LV_USE_MATRIX           0
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (8U * 1024U)
#define LV_DRAW_LAYER_MAX_MEMORY      0

#define LV_USE_VECTOR_GRAPHIC   0
#define LV_USE_SNAPSHOT         0
#define LV_USE_THORVG           0

#define LV_USE_DRAW_SW          1
#if LV_USE_DRAW_SW
  #define LV_DRAW_SW_SUPPORT_RGB565                 1
  #define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED         1
  #define LV_DRAW_SW_SUPPORT_RGB565A8               0
  #define LV_DRAW_SW_SUPPORT_RGB888                 0
  #define LV_DRAW_SW_SUPPORT_XRGB8888               0
  #define LV_DRAW_SW_SUPPORT_ARGB8888               1
  #define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
  #define LV_DRAW_SW_SUPPORT_L8                     0
  #define LV_DRAW_SW_SUPPORT_AL88                   0
  #define LV_DRAW_SW_SUPPORT_A8                     1
  #define LV_DRAW_SW_SUPPORT_I1                     0

  #define LV_DRAW_SW_COMPLEX                        1
  #define LV_GRADIENT_MAX_STOPS                     2
  #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS          0
  #define LV_DRAW_SW_SHADOW_CACHE_SIZE              0
  #define LV_DRAW_SW_CIRCLE_CACHE_SIZE              4
  #define LV_USE_DRAW_SW_ASM                        LV_DRAW_SW_ASM_NONE
#endif

/* 禁用所有外部 GPU 后端 */
#define LV_USE_DRAW_VG_LITE   0
#define LV_USE_DRAW_DAVE2D    0
#define LV_USE_NEMA_GFX       0
#define LV_USE_DRAW_DMA2D     0
#define LV_USE_DRAW_OPENGLES  0
#define LV_USE_DRAW_NANOVG    0
#define LV_USE_DRAW_SDL       0
#define LV_USE_DRAW_G2D       0
#define LV_USE_DRAW_PXP       0
#define LV_USE_PPA            0
#define LV_USE_DRAW_EVE       0

/*============================================================================
 * 杂项功能与调试配置
 *============================================================================*/

#define LV_USE_OBJ_NAME       0
#define LV_USE_OBJ_ID         0
#define LV_USE_OBJ_PROPERTY   0
#define LV_USE_FLOAT          0
#define LV_USE_LOG            0

#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_USE_SYSMON         0
#define LV_USE_PROFILER       0
#define LV_USE_MONKEY         0
#define LV_USE_GRIDNAV        0
#define LV_USE_FRAGMENT       0
#define LV_USE_IMGFONT        0
#define LV_USE_OBSERVER       0
#define LV_USE_IME_PINYIN     0
#define LV_USE_FILE_EXPLORER  0
#define LV_USE_TRANSLATION    0

/*============================================================================
 * 主题与布局
 *============================================================================*/

#define LV_USE_THEME_DEFAULT 1
#if LV_USE_THEME_DEFAULT
  #define LV_THEME_DEFAULT_DARK            1 /* 深色工业风格 */
  #define LV_THEME_DEFAULT_GROW            0
  #define LV_THEME_DEFAULT_TRANSITION_TIME 50
#endif
#define LV_USE_THEME_SIMPLE  0
#define LV_USE_THEME_MONO    0

#define LV_USE_FLEX          1
#define LV_USE_GRID          0

/*============================================================================
 * 图像解码器（纯矢量控件界面，关闭外部图库节省 Flash）
 *============================================================================*/

#define LV_CACHE_DEF_SIZE              0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT  0
#define LV_USE_RLE                     0
#define LV_USE_LZ4                     0
#define LV_BIN_DECODER_RAM_LOAD        0
#define LV_USE_LODEPNG                 0
#define LV_USE_LIBPNG                  0
#define LV_USE_BMP                     0
#define LV_USE_TJPGD                   0
#define LV_USE_LIBJPEG_TURBO           0
#define LV_USE_LIBWEBP                 0
#define LV_USE_SVG                     0

/*============================================================================
 * 文本与内置英文字体（严格按 AGENTS.md 仅启用 Montserrat 12/14/16/24）
 *============================================================================*/

#define LV_TXT_ENC                  LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS          " ,.;:-_)]}"
#define LV_TXT_LINE_BREAK_LONG_LEN  0
#define LV_TXT_COLOR_CMD            "#"
#define LV_USE_BIDI                 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  1
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  0
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  0
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  0
#define LV_FONT_MONTSERRAT_28_COMPRESSED       0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW       0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK      0
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK      0
#define LV_FONT_UNSCII_8                       0
#define LV_FONT_UNSCII_16                      0

#define LV_FONT_DEFAULT         LV_FONT_DEFAULT_MONTSERRAT_14
#define LV_FONT_FMT_TXT_LARGE   0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_USE_FONT_MANAGER     0
#define LV_USE_FREETYPE         0
#define LV_USE_TINY_TTF         0

/*============================================================================
 * 控件裁剪（仅保留本项目 HMI 实际使用的核心控件，控制 Flash 占用）
 *============================================================================*/

#define LV_WIDGETS_HAS_DEFAULT_VALUE 1

#define LV_USE_3DTEXTURE     0
#define LV_USE_ANIMIMG       0
#define LV_USE_ARC           0
#define LV_USE_ARCLABEL      0
#define LV_USE_BAR           1 /* Slider 依赖 Bar */
#define LV_USE_BARCODE       0
#define LV_USE_BUTTON        1
#define LV_USE_BUTTONMATRIX  0
#define LV_USE_CALENDAR      0
#define LV_USE_CANVAS        0
#define LV_USE_CHART         1
#define LV_USE_CHECKBOX      0
#define LV_USE_DROPDOWN      0
#define LV_USE_FFMPEG        0
#define LV_USE_GIF           0
#define LV_USE_GLTF          0
#define LV_USE_GSTREAMER     0
#define LV_USE_IMAGE         0
#define LV_USE_IMAGEBUTTON   0
#define LV_USE_KEYBOARD      0
#define LV_USE_LABEL         1
#define LV_USE_LED           0
#define LV_USE_LINE          0
#define LV_USE_LIST          0
#define LV_USE_MENU          0
#define LV_USE_MSGBOX        0
#define LV_USE_QRCODE        0
#define LV_USE_RLOTTIE       0
#define LV_USE_ROLLER        0
#define LV_USE_SCALE         0
#define LV_USE_SLIDER        1
#define LV_USE_SPAN          0
#define LV_USE_SPINBOX       0
#define LV_USE_SPINNER       0
#define LV_USE_SWITCH        0
#define LV_USE_TEXTAREA      0
#define LV_USE_TABLE         0
#define LV_USE_TABVIEW       0
#define LV_USE_TILEVIEW      0
#define LV_USE_WIN           0

/*============================================================================
 * 关闭内置驱动与示例/演示
 *============================================================================*/

#define LV_USE_GENERIC_MIPI 0
#define LV_USE_ST7789       0
#define LV_BUILD_EXAMPLES   0
#define LV_BUILD_DEMOS      0

#endif /* LV_CONF_H */
#endif /* 启用本配置 */
