/**
 * @file    app_cli.c
 * @brief   串口调试命令行任务（CliTask）M3 阶段实现
 * @date    2026-11-28
 * @note    依赖 bsp_uart_cli、bsp_sys、app_data、app_cfg、app_hmi、app_fansim 与 FreeRTOS 栈统计接口
 */

#include "app_cli.h"
#include "app_cfg.h"
#include "app_data.h"
#include "app_fansim.h"
#include "app_hmi.h"
#include "bsp_can.h"
#include "bsp_rs485.h"
#include "bsp_sys.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的任务句柄（供栈水位监控） */
extern osThreadId_t defaultTaskHandle;
extern osThreadId_t CliTaskHandle;
extern osThreadId_t HmiTaskHandle;
extern osThreadId_t ControlTaskHandle;
extern osThreadId_t FanSimTaskHandle;
extern osThreadId_t SensorTaskHandle;

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
 * @brief  处理 "help" 命令：打印 M3 阶段可用命令清单
 */
static void CmdHelp(void)
{
  Bsp_CliPrintf("Available commands (M3):\r\n");
  Bsp_CliPrintf("  help                              - Show this help\r\n");
  Bsp_CliPrintf("  status                            - Show sensor/CAN/HMI/CFG/RTOS status\r\n");
  Bsp_CliPrintf("  bus                               - Show CAN1/CAN2 & RS-485 hardware regs\r\n");
  Bsp_CliPrintf("  fault stall on|off                - Inject/clear CAN2 fan stall fault\r\n");
  Bsp_CliPrintf("  fault timeout on|off              - Inject/clear CAN2 cmd ACK timeout\r\n");
  Bsp_CliPrintf("  fault offline on|off              - Inject/clear CAN2 node offline fault\r\n");
  Bsp_CliPrintf("  fault clear                       - Clear all injected CAN faults\r\n");
  Bsp_CliPrintf("  rtc set YYYY-MM-DD HH:MM:SS       - Set RTC date & time\r\n");
  Bsp_CliPrintf("  bl <10..100>                      - Set LCD backlight brightness (%%)\r\n");
  Bsp_CliPrintf("  touch cal                         - Launch 4-point touch calibration\r\n");
  Bsp_CliPrintf("  cfg save                          - Flush config to Sector 11 now\r\n");
  Bsp_CliPrintf("  cfg reset                         - Restore default config & save\r\n");
  Bsp_CliPrintf("  reboot                            - Trigger software reset (SW)\r\n");
}

/**
 * @brief  处理 "status" 命令：打印复位原因、RTC、温湿度、CAN/控制状态、HMI 内存与 6 任务栈水位
 */
static void CmdStatus(void)
{
  AppState_t snap;
  AppHmiMemStats_t lv_mem;
  BspDateTime_t dt = {0};
  AppErr_t rtc_err;
  uint32_t mon_wm = 0U;
  uint32_t cli_wm = 0U;
  uint32_t hmi_wm = 0U;
  uint32_t ctrl_wm = 0U;
  uint32_t sim_wm = 0U;
  uint32_t sens_wm = 0U;
  int16_t abs_t;

  AppData_GetSnapshot(&snap);
  AppHmi_GetMemStats(&lv_mem);
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
  if (HmiTaskHandle != NULL)
  {
    hmi_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)HmiTaskHandle);
  }
  if (ControlTaskHandle != NULL)
  {
    ctrl_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)ControlTaskHandle);
  }
  if (FanSimTaskHandle != NULL)
  {
    sim_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)FanSimTaskHandle);
  }
  if (SensorTaskHandle != NULL)
  {
    sens_wm = (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)SensorTaskHandle);
  }

  abs_t = (snap.temp_x10 < 0) ? (int16_t)(-snap.temp_x10) : snap.temp_x10;

  Bsp_CliPrintf("---- SmartEdge_F407 Status (M3) ----\r\n");
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
  if (snap.sensor_valid != 0U)
  {
    Bsp_CliPrintf(" Sensor  : Valid=1 | T=%s%d.%d C | RH=%u.%u %% | RS485Err=%lu\r\n",
                  (snap.temp_x10 < 0) ? "-" : "",
                  (int)(abs_t / 10), (int)(abs_t % 10),
                  (unsigned int)(snap.humi_x10 / 10U), (unsigned int)(snap.humi_x10 % 10U),
                  (unsigned long)snap.rs485_err_cnt);
  }
  else
  {
    Bsp_CliPrintf(" Sensor  : Valid=0 | T=--.- C | RH=--.- %% | RS485Err=%lu\r\n",
                  (unsigned long)snap.rs485_err_cnt);
  }
  Bsp_CliPrintf(" Ctrl    : Mode=%s | FanSt=%u | ManRun=%u | Tgt=%u | Act=%u RPM\r\n",
                (snap.ctrl_mode == CTRL_MODE_MANUAL) ? "MAN" : "AUTO",
                (unsigned int)snap.fan_state,
                (unsigned int)snap.manual_run,
                (unsigned int)snap.active_target_rpm,
                (unsigned int)snap.actual_rpm);
  Bsp_CliPrintf(" CAN     : Online=%u | Fault=0x%04X | CanErr=%lu | Inj=0x%02X\r\n",
                (unsigned int)snap.can_online,
                (unsigned int)snap.fault_mask,
                (unsigned long)snap.can_err_cnt,
                (unsigned int)AppFanSim_GetInjectMask());
  Bsp_CliPrintf(" Config  : Src=%s | T_ON=%d.%d C | T_OFF=%d.%d C | BL=%u%% | Cal=%u\r\n",
                (AppCfg_IsLoadedFromFlash() != 0U) ? "Sector11" : "DEFAULT",
                (int)(snap.temp_start_x10 / 10), (int)(snap.temp_start_x10 % 10),
                (int)(snap.temp_stop_x10 / 10), (int)(snap.temp_stop_x10 % 10),
                (unsigned int)snap.backlight_pct,
                (unsigned int)AppCfg_IsTouchCalibrated());
  Bsp_CliPrintf(" GPIO    : LED2=%u | KEY(PA0)=%u | FLASH_CS(PA4)=%u\r\n",
                (unsigned int)Bsp_LedIsOn(),
                (unsigned int)Bsp_KeyIsPressed(),
                (unsigned int)Bsp_FlashGuardIsHigh());
  Bsp_CliPrintf(" LVGLMem : Used=%u%% | Free=%lu/%lu B | MaxUsed=%lu B (CCMRAM)\r\n",
                (unsigned int)lv_mem.used_pct,
                (unsigned long)lv_mem.free_bytes,
                (unsigned long)lv_mem.total_bytes,
                (unsigned long)lv_mem.max_used);
  Bsp_CliPrintf(" RTOS    : FreeHeap=%lu B (MinEver=%lu B)\r\n",
                (unsigned long)xPortGetFreeHeapSize(),
                (unsigned long)xPortGetMinimumEverFreeHeapSize());
  Bsp_CliPrintf(" StackWM : Mon=%lu | Cli=%lu | Hmi=%lu | Ctrl=%lu | Sim=%lu | Sens=%lu W\r\n",
                (unsigned long)mon_wm, (unsigned long)cli_wm, (unsigned long)hmi_wm,
                (unsigned long)ctrl_wm, (unsigned long)sim_wm, (unsigned long)sens_wm);
}

/**
 * @brief  处理 "bus" 命令：打印 CAN1/CAN2 硬件寄存器（TEC/REC/LEC/RX电平）与 RS-485 底层统计
 */
static void CmdBusDiag(void)
{
  BspCanChDiag_t c1;
  BspCanChDiag_t c2;
  BspRs485Diag_t r485;

  Bsp_CanGetDiag(&c1, &c2);
  Bsp_Rs485GetDiag(&r485);

  Bsp_CliPrintf("---- Hardware Bus Diag (CAN & RS-485) ----\r\n");
  Bsp_CliPrintf(" CAN1 : ESR=0x%08lX (TEC=%lu REC=%lu LEC=%lu BOFF=%lu) RXPin=%lu MSR=0x%04lX TSR=0x%08lX\r\n",
                (unsigned long)c1.esr,
                (unsigned long)((c1.esr >> 16U) & 0xFFUL),
                (unsigned long)((c1.esr >> 24U) & 0xFFUL),
                (unsigned long)((c1.esr >> 4U) & 0x07UL),
                (unsigned long)((c1.esr >> 2U) & 0x01UL),
                (unsigned long)((c1.msr >> 11U) & 0x01UL),
                (unsigned long)(c1.msr & 0xFFFFUL),
                (unsigned long)c1.tsr);
  Bsp_CliPrintf("        TxOK=%lu TxErr=%lu RxIRQ=%lu Drop=%lu\r\n",
                (unsigned long)c1.tx_ok_cnt, (unsigned long)c1.tx_err_cnt,
                (unsigned long)c1.rx_irq_cnt, (unsigned long)Bsp_CanGetRxDropCount());
  Bsp_CliPrintf(" CAN2 : ESR=0x%08lX (TEC=%lu REC=%lu LEC=%lu BOFF=%lu) RXPin=%lu MSR=0x%04lX TSR=0x%08lX\r\n",
                (unsigned long)c2.esr,
                (unsigned long)((c2.esr >> 16U) & 0xFFUL),
                (unsigned long)((c2.esr >> 24U) & 0xFFUL),
                (unsigned long)((c2.esr >> 4U) & 0x07UL),
                (unsigned long)((c2.esr >> 2U) & 0x01UL),
                (unsigned long)((c2.msr >> 11U) & 0x01UL),
                (unsigned long)(c2.msr & 0xFFFFUL),
                (unsigned long)c2.tsr);
  Bsp_CliPrintf("        TxOK=%lu TxErr=%lu RxIRQ=%lu\r\n",
                (unsigned long)c2.tx_ok_cnt, (unsigned long)c2.tx_err_cnt,
                (unsigned long)c2.rx_irq_cnt);
  Bsp_CliPrintf(" RS485: TxTC=%lu RxEvt=%lu ErrIRQ=%lu UartErr=0x%lX Ret=%ld RXPin(PD6)=%u Len=%u\r\n",
                (unsigned long)r485.tx_cplt_cnt, (unsigned long)r485.rx_evt_cnt,
                (unsigned long)r485.err_irq_cnt, (unsigned long)r485.last_uart_er,
                (long)r485.last_ret, (unsigned int)r485.rx_pin_level,
                (unsigned int)r485.last_rx_len);
  if (r485.last_rx_len > 0U)
  {
    char hex_buf[64];
    size_t pos = 0U;
    uint16_t n = (r485.last_rx_len > 16U) ? 16U : r485.last_rx_len;

    for (uint16_t i = 0U; (i < n) && (pos < (sizeof(hex_buf) - 1U)); i++)
    {
      int wrote = snprintf(hex_buf + pos, sizeof(hex_buf) - pos, " %02X",
                           (unsigned int)r485.last_rx[i]);
      if (wrote > 0)
      {
        pos += (size_t)wrote;
      }
    }
    hex_buf[sizeof(hex_buf) - 1U] = '\0';
    Bsp_CliPrintf("        LastRX:%s\r\n", hex_buf);
  }
}

/**
 * @brief  处理 "fault ..." 命令：向 CAN2 模拟风扇节点注入或清除三类 CAN 故障
 * @param[in] arg "fault " 之后的参数子串
 */
static void CmdFault(const char *arg)
{
  uint8_t mask;

  if (arg == NULL)
  {
    Bsp_CliPrintf("Usage: fault <stall|timeout|offline> <on|off> | fault clear\r\n");
    return;
  }

  if (strcmp(arg, "clear") == 0)
  {
    AppFanSim_SetInjectMask(0U);
    Bsp_CliPrintf("All CAN fault injections cleared (mask=0x00).\r\n");
    return;
  }

  mask = AppFanSim_GetInjectMask();
  if (strcmp(arg, "stall on") == 0)
  {
    mask |= SIM_INJ_STALL;
  }
  else if (strcmp(arg, "stall off") == 0)
  {
    mask = (uint8_t)(mask & (uint8_t)(~SIM_INJ_STALL));
  }
  else if (strcmp(arg, "timeout on") == 0)
  {
    mask |= SIM_INJ_TIMEOUT;
  }
  else if (strcmp(arg, "timeout off") == 0)
  {
    mask = (uint8_t)(mask & (uint8_t)(~SIM_INJ_TIMEOUT));
  }
  else if (strcmp(arg, "offline on") == 0)
  {
    mask |= SIM_INJ_OFFLINE;
  }
  else if (strcmp(arg, "offline off") == 0)
  {
    mask = (uint8_t)(mask & (uint8_t)(~SIM_INJ_OFFLINE));
  }
  else
  {
    Bsp_CliPrintf("Usage: fault <stall|timeout|offline> <on|off> | fault clear\r\n");
    return;
  }

  AppFanSim_SetInjectMask(mask);
  Bsp_CliPrintf("CAN2 fault inject mask updated: 0x%02X (stall=%u timeout=%u offline=%u)\r\n",
                (unsigned int)mask,
                ((mask & SIM_INJ_STALL) != 0U) ? 1U : 0U,
                ((mask & SIM_INJ_TIMEOUT) != 0U) ? 1U : 0U,
                ((mask & SIM_INJ_OFFLINE) != 0U) ? 1U : 0U);
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
 * @brief  处理 "bl <10..100>" 命令：设置屏幕背光亮度并标记配置保存
 * @param[in] arg 亮度数字参数字符串
 */
static void CmdBacklight(const char *arg)
{
  size_t len;
  uint16_t pct = 0U;

  if (arg == NULL)
  {
    Bsp_CliPrintf("Usage: bl <10..100>\r\n");
    return;
  }

  len = strlen(arg);
  if ((len < 1U) || (len > 3U) || (ParseDigits(arg, (uint8_t)len, &pct) == 0U))
  {
    Bsp_CliPrintf("Usage: bl <10..100>\r\n");
    return;
  }

  /* 必须在缩窄强转 uint8_t 之前完成上界检查，防止如 bl 300 截断为 44 绕过合法性校验 */
  if ((pct < APP_BACKLIGHT_MIN_PCT) || (pct > APP_BACKLIGHT_MAX_PCT))
  {
    Bsp_CliPrintf("Error: backlight must be in 10..100.\r\n");
    return;
  }

  if (AppCtrl_SetBacklight((uint8_t)pct) == APP_OK)
  {
    Bsp_CliPrintf("Backlight set to %u%% (auto-save in 2s).\r\n", (unsigned int)pct);
  }
  else
  {
    Bsp_CliPrintf("Error: backlight must be in 10..100.\r\n");
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
  else if (strcmp(line, "bus") == 0)
  {
    CmdBusDiag();
  }
  else if (strncmp(line, "fault ", 6U) == 0)
  {
    CmdFault(line + 6U);
  }
  else if (strncmp(line, "rtc set ", 8U) == 0)
  {
    CmdRtcSet(line + 8U);
  }
  else if (strncmp(line, "bl ", 3U) == 0)
  {
    CmdBacklight(line + 3U);
  }
  else if (strcmp(line, "touch cal") == 0)
  {
    AppHmi_RequestTouchCal();
    Bsp_CliPrintf("4-point touch calibration requested.\r\n");
  }
  else if (strcmp(line, "cfg save") == 0)
  {
    if (AppCfg_FlushNow() == APP_OK)
    {
      Bsp_CliPrintf("Config flushed to Sector 11.\r\n");
    }
    else
    {
      Bsp_CliPrintf("Error: failed to save config to Sector 11.\r\n");
    }
  }
  else if (strcmp(line, "cfg reset") == 0)
  {
    AppConfig_t def;

    /* 先同步重置 s_cfg、触摸矩阵、g_sys 与背光硬件，再立即落盘，避免写盘期间/失败产生半重置分裂 */
    AppCfg_ApplyDefaultMemory(&def);
    AppData_ApplyConfig(&def);
    if (AppCfg_FlushNow() == APP_OK)
    {
      Bsp_CliPrintf("Config restored to defaults and saved.\r\n");
    }
    else
    {
      Bsp_CliPrintf("Config restored in RAM; Sector 11 save will retry in 2s.\r\n");
    }
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
