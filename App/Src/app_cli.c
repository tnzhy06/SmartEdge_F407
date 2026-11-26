/**
 * @file    app_cli.c
 * @brief   串口调试命令行任务（CliTask）M1 阶段实现
 * @date    2026-11-26
 * @note    依赖 bsp_uart_cli、bsp_sys、app_data 与 FreeRTOS 任务栈统计接口
 */

#include "app_cli.h"
#include "app_data.h"
#include "bsp_sys.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的任务句柄（供栈水位监控） */
extern osThreadId_t defaultTaskHandle;
extern osThreadId_t CliTaskHandle;

/** @brief 单行命令最大缓存长度（字节） */
#define CLI_LINE_MAX 128U

/**
 * @brief  将定长十进制数字子串解析为无符号整数（避免引入重型 sscanf）
 * @param[in]  s   起始字符指针
 * @param[in]  len 连续数字字符长度（1..4）
 * @param[out] out 输出解析结果
 * @return uint8_t 1 解析成功，0 含非数字字符
 */
static uint8_t ParseDigits(const char *s, uint8_t len, uint16_t *out)
{
  uint16_t val = 0U;

  if ((s == NULL) || (out == NULL) || (len == 0U))
  {
    return 0U;
  }

  for (uint8_t i = 0U; i < len; i++)
  {
    if ((s[i] < '0') || (s[i] > '9'))
    {
      return 0U;
    }
    val = (uint16_t)((val * 10U) + (uint16_t)(s[i] - '0'));
  }

  *out = val;
  return 1U;
}

/**
 * @brief  处理 "help" 命令：打印 M1 阶段可用命令清单
 */
static void CmdHelp(void)
{
  Bsp_CliPrintf("Available commands (M1):\r\n");
  Bsp_CliPrintf("  help                              - Show this help\r\n");
  Bsp_CliPrintf("  status                            - Show reset/RTC/GPIO/RTOS status\r\n");
  Bsp_CliPrintf("  rtc set YYYY-MM-DD HH:MM:SS       - Set RTC date & time\r\n");
  Bsp_CliPrintf("  reboot                            - Trigger software reset (SW)\r\n");
}

/**
 * @brief  处理 "status" 命令：打印复位原因、RTC、引脚状态与 FreeRTOS 堆/栈水位
 */
static void CmdStatus(void)
{
  AppState_t snap;
  BspDateTime_t dt = {0};
  AppErr_t rtc_err;
  uint32_t mon_wm = 0U;
  uint32_t cli_wm = 0U;

  AppData_GetSnapshot(&snap);
  rtc_err = Bsp_RtcGetDateTime(&dt);

  /* 读取任务剩余最小栈空间（单位：Words，乘 4 为 Bytes），便于验证栈预算是否充裕 */
  if (defaultTaskHandle != NULL)
  {
    mon_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)defaultTaskHandle);
  }
  if (CliTaskHandle != NULL)
  {
    cli_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)CliTaskHandle);
  }

  Bsp_CliPrintf("---- SmartEdge_F407 Status (M1) ----\r\n");
  Bsp_CliPrintf(" Reset   : %s (code=%u, hung=0x%02X)\r\n",
                Bsp_SysResetReasonStr((ResetReason_t)snap.reset_reason),
                (unsigned int)snap.reset_reason,
                (unsigned int)snap.hung_task_mask);
  Bsp_CliPrintf(" Uptime  : %lu s\r\n", (unsigned long)snap.uptime_sec);
  if (rtc_err == APP_OK)
  {
    Bsp_CliPrintf(" RTC     : %04u-%02u-%02u %02u:%02u:%02u (%s, unix=%lu)\r\n",
                  (unsigned int)dt.year, (unsigned int)dt.month, (unsigned int)dt.day,
                  (unsigned int)dt.hour, (unsigned int)dt.min, (unsigned int)dt.sec,
                  (Bsp_RtcIsInitialized() != 0U) ? "CALIBRATED" : "DEFAULT",
                  (unsigned long)Bsp_RtcGetUnixTime());
  }
  else
  {
    Bsp_CliPrintf(" RTC     : READ_ERR (%d)\r\n", (int)rtc_err);
  }
  Bsp_CliPrintf(" GPIO    : LED2=%u | KEY(PA0)=%u | FLASH_CS(PA4)=%u\r\n",
                (unsigned int)Bsp_LedIsOn(),
                (unsigned int)Bsp_KeyIsPressed(),
                (unsigned int)Bsp_FlashGuardIsHigh());
  Bsp_CliPrintf(" RTOS    : FreeHeap=%lu B (MinEver=%lu B)\r\n",
                (unsigned long)xPortGetFreeHeapSize(),
                (unsigned long)xPortGetMinimumEverFreeHeapSize());
  Bsp_CliPrintf(" StackWM : defaultTask=%lu W (%lu B) | CliTask=%lu W (%lu B)\r\n",
                (unsigned long)mon_wm, (unsigned long)(mon_wm * 4U),
                (unsigned long)cli_wm, (unsigned long)(cli_wm * 4U));
}

/**
 * @brief  处理 "rtc set YYYY-MM-DD HH:MM:SS" 命令
 * @param[in] arg 日期时间参数串（精确长度 19 字符，如 "2026-11-26 22:40:00"）
 */
static void CmdRtcSet(const char *arg)
{
  BspDateTime_t dt;
  uint16_t y = 0U;
  uint16_t mo = 0U;
  uint16_t d = 0U;
  uint16_t h = 0U;
  uint16_t mi = 0U;
  uint16_t s = 0U;

  if ((arg == NULL) || (strlen(arg) != 19U) ||
      (arg[4] != '-') || (arg[7] != '-') || (arg[10] != ' ') ||
      (arg[13] != ':') || (arg[16] != ':'))
  {
    Bsp_CliPrintf("Usage: rtc set YYYY-MM-DD HH:MM:SS\r\n");
    return;
  }

  if ((ParseDigits(&arg[0], 4U, &y) == 0U) ||
      (ParseDigits(&arg[5], 2U, &mo) == 0U) ||
      (ParseDigits(&arg[8], 2U, &d) == 0U) ||
      (ParseDigits(&arg[11], 2U, &h) == 0U) ||
      (ParseDigits(&arg[14], 2U, &mi) == 0U) ||
      (ParseDigits(&arg[17], 2U, &s) == 0U))
  {
    Bsp_CliPrintf("Error: invalid digits in datetime.\r\n");
    return;
  }

  dt.year = y;
  dt.month = (uint8_t)mo;
  dt.day = (uint8_t)d;
  dt.hour = (uint8_t)h;
  dt.min = (uint8_t)mi;
  dt.sec = (uint8_t)s;

  if (Bsp_RtcSetDateTime(&dt) == APP_OK)
  {
    /* 立即同步 g_sys.rtc_unix_ts，避免 1 s 内查 status 时出现旧时间戳 */
    AppData_UpdateRtcTime(Bsp_RtcGetUnixTime());
    Bsp_CliPrintf("RTC updated to %04u-%02u-%02u %02u:%02u:%02u.\r\n",
                  (unsigned int)dt.year, (unsigned int)dt.month, (unsigned int)dt.day,
                  (unsigned int)dt.hour, (unsigned int)dt.min, (unsigned int)dt.sec);
  }
  else
  {
    Bsp_CliPrintf("Error: datetime out of valid range.\r\n");
  }
}

/**
 * @brief  解析并分发单行串口命令（自动裁剪首尾空白字符）
 * @param[in,out] line 已去除首尾换行符的可修改命令字符串
 */
static void DispatchLine(char *line)
{
  size_t len;

  /* 跳过前导空格/制表符 */
  while ((*line == ' ') || (*line == '\t'))
  {
    line++;
  }

  /* 裁剪行尾空格/制表符，避免 "status " 这类输入被误判为未知命令 */
  len = strlen(line);
  while ((len > 0U) && ((line[len - 1U] == ' ') || (line[len - 1U] == '\t')))
  {
    line[--len] = '\0';
  }
  if (*line == '\0')
  {
    return;
  }

  if (strcmp(line, "help") == 0)
  {
    CmdHelp();
  }
  else if (strcmp(line, "status") == 0)
  {
    CmdStatus();
  }
  else if (strncmp(line, "rtc set ", 8U) == 0)
  {
    CmdRtcSet(line + 8U);
  }
  else if (strcmp(line, "reboot") == 0)
  {
    Bsp_CliPrintf("Rebooting via NVIC_SystemReset()...\r\n");
    /* 留出 50 ms 给 USART1 DMA 发送完提示字符串再复位 */
    osDelay(50U);
    Bsp_SysSoftReset();
  }
  else
  {
    Bsp_CliPrintf("Unknown command: '%s' (type 'help')\r\n", line);
  }
}

void App_CliTask(void)
{
  char line[CLI_LINE_MAX];
  AppErr_t err;

  /* 传入当前任务句柄，待 USART1 IDLE 中断到来时唤醒本任务 */
  err = Bsp_CliInit(osThreadGetId());
  if (err != APP_OK)
  {
    Bsp_CliPrintf("[CLI] ERR: USART1 RX DMA init failed (%d)\r\n", (int)err);
  }

  for (;;)
  {
    /* 无串口输入时彻底挂起让出 CPU，零轮询功耗 */
    (void)osThreadFlagsWait(BSP_CLI_FLAG_RX, osFlagsWaitAny, osWaitForever);

    while (Bsp_CliReadLine(line, (uint16_t)sizeof(line)) > 0U)
    {
      DispatchLine(line);
      Bsp_CliPrintf("> ");
    }
  }
}
