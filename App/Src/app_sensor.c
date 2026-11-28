/**
 * @file    app_sensor.c
 * @brief   RS-485 温湿度采集任务（SensorTask）实现
 * @date    2026-11-28
 * @note    严格遵循 SRS FR-SENS-001~004 与 SDD 6.2 节：
 *          - 每 1000 ms 发送 01 03 03 00 00 02 C4 4F，单次应答超时 500 ms；
 *          - 连续 3 次失败判定传感器断线，置位 FAULT_SENSOR_OFFLINE 并通知 ControlTask 切入安全降级；
 *          - 恢复收到 1 帧有效数据即自动清除故障并恢复正常温控
 */

#include "app_sensor.h"
#include "app_cfg.h"
#include "app_control.h"
#include "app_data.h"
#include "app_modbus_rtu.h"
#include "bsp_rs485.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"

/** @brief 采集轮询周期与单次应答等待超时（单位：ms） */
#define SENS_POLL_PERIOD_MS  1000U
#define SENS_RX_TIMEOUT_MS   500U

/** @brief 判定传感器断线的连续失败次数阈值 */
#define SENS_OFFLINE_FAIL_CNT 3U

/** @brief 静态配置副本与收发帧缓冲（移出 1.5 KB 任务栈，保持充足栈裕量） */
static AppConfig_t s_sens_cfg;
static uint8_t s_tx_frame[8];
static uint8_t s_rx_frame[32];

void App_SensorTask(void)
{
  uint8_t slave_id = 0x01U;
  uint8_t fail_cnt = 0U;
  AppErr_t err;

  AppCfg_GetCurrent(&s_sens_cfg);
  if (s_sens_cfg.rs485_slave_id != 0U)
  {
    slave_id = s_sens_cfg.rs485_slave_id;
  }

  err = Bsp_Rs485Init(osThreadGetId());
  if (err != APP_OK)
  {
    Bsp_CliPrintf("[SENS] ERR: RS-485 init failed (%d)\r\n", (int)err);
  }

  /* 稍作延时让 ControlTask/CliTask 完成启动横幅初始化后再发出首帧采集 */
  osDelay(200U);

  for (;;)
  {
    uint32_t loop_start = osKernelGetTickCount();
    uint16_t rx_len = 0U;
    int16_t temp_x10 = 0;
    uint16_t humi_x10 = 0U;
    uint32_t elapsed;

    (void)ModbusRtu_BuildRead03(slave_id, KTH2_REG_TEMP_ADDR, KTH2_REG_READ_QTY, s_tx_frame);
    err = Bsp_Rs485Transceive(s_tx_frame, (uint16_t)sizeof(s_tx_frame),
                              s_rx_frame, (uint16_t)sizeof(s_rx_frame),
                              &rx_len, SENS_RX_TIMEOUT_MS);

    if ((err == APP_OK) &&
        (ModbusRtu_ParseKth2Resp(s_rx_frame, rx_len, slave_id, &temp_x10, &humi_x10) == APP_OK))
    {
      fail_cnt = 0U;
      AppData_UpdateSensor(temp_x10, humi_x10, 1U);

      if (AppData_UpdateFaultBit(FAULT_SENSOR_OFFLINE, 0U) != 0U)
      {
        int16_t abs_t = (temp_x10 < 0) ? (int16_t)(-temp_x10) : temp_x10;
        Bsp_CliPrintf("[SENS] KTH2-R online (T=%s%d.%d C, RH=%u.%u %%)\r\n",
                      (temp_x10 < 0) ? "-" : "",
                      (int)(abs_t / 10), (int)(abs_t % 10),
                      (unsigned int)(humi_x10 / 10U), (unsigned int)(humi_x10 % 10U));
      }
      AppControl_NotifyUpdate();
    }
    else
    {
      AppData_IncRs485Err();
      if (fail_cnt < SENS_OFFLINE_FAIL_CNT)
      {
        fail_cnt++;
      }

      if (fail_cnt >= SENS_OFFLINE_FAIL_CNT)
      {
        /* 先置位 FAULT_SENSOR_OFFLINE 再清 sensor_valid，避免 ControlTask 在双锁空隙误判为冷开机待机 */
        if (AppData_UpdateFaultBit(FAULT_SENSOR_OFFLINE, 1U) != 0U)
        {
          Bsp_CliPrintf("[SENS] FAULT: KTH2-R offline (3 consecutive failures)\r\n");
        }
        AppData_UpdateSensor(0, 0U, 0U);
        AppControl_NotifyUpdate();
      }
    }

    /* 精确补齐 1000 ms 轮询周期，不因正常响应耗时或 500 ms 超时导致周期漂移 */
    elapsed = osKernelGetTickCount() - loop_start;
    if (elapsed < SENS_POLL_PERIOD_MS)
    {
      osDelay(SENS_POLL_PERIOD_MS - elapsed);
    }
    else
    {
      osDelay(10U);
    }
  }
}
