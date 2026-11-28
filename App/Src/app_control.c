/**
 * @file    app_control.c
 * @brief   核心温控状态机与 CAN1 控制主站任务（ControlTask）实现
 * @date    2026-11-28
 * @note    严格对齐 SRS FR-CTRL-001~004 / FR-CAN-001~005 与 SDD 6.3 节：
 *          - 自动滞回 + 纯整数线性调速（T_start 起 1200..3000 RPM，死区维持 1000 RPM，变化 >= 50 RPM 防抖）；
 *          - 传感器断线自动切入 FAN_FAILSAFE（1500 RPM）；
 *          - 精准区分并自动清除三类 CAN 故障：STALL / CMD_TIMEOUT / NODE_OFFLINE
 */

#include "app_control.h"
#include "app_can_proto.h"
#include "app_data.h"
#include "bsp_can.h"
#include "bsp_uart_cli.h"
#include "cmsis_os.h"
#include <stddef.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的 ControlTask 句柄与 CAN1 接收队列句柄 */
extern osThreadId_t ControlTaskHandle;
extern osMessageQueueId_t can1RxQueueHandle;

/** @brief 唤醒 ControlTask 即刻重算控制律的线程标志位 */
#define CTRL_FLAG_UPDATE (1UL << 0)

/** @brief 控制周期、CAN 保活与三类故障超时阈值（单位：ms，严格对齐 SDD 6.3 节） */
#define CTRL_LOOP_TICK_MS       100U
#define CTRL_CAN_KEEPALIVE_MS   500U
#define CTRL_CAN_ACK_TIMEOUT_MS 500U
#define CTRL_CAN_HB_TIMEOUT_MS  1500U
#define CTRL_CAN_STALL_TIME_MS  1500U

/** @brief 自动/降级调速与堵转检测 RPM 基准参数 */
#define CTRL_AUTO_DEADBAND_RPM  1000U
#define CTRL_AUTO_BASE_RPM      1200U
#define CTRL_FAILSAFE_RPM       1500U
#define CTRL_AUTO_RPM_JITTER    50U
#define CTRL_CAN_STALL_RPM_MIN  200U

/** @brief 跨任务通知复位 CAN 超时计时基准的原子标志（Sector 11 擦写后置位） */
static uint8_t s_reset_timers_req = 0U;

/** @brief ControlTask 独享静态状态快照（移出任务栈节省栈空间） */
static AppState_t s_ctrl_snap;

/**
 * @brief  纯整数计算自动运行期目标转速（零浮点误差，严格对齐 SDD 6.3 节）
 * @param[in] t_x10       当前温度（单位：0.1 °C）
 * @param[in] t_start_x10 自动启动阈值（单位：0.1 °C）
 * @return uint16_t       自动目标转速（死区 1000 RPM；超温线性 1200..3000 RPM）
 */
static uint16_t CalcAutoRpm(int16_t t_x10, int16_t t_start_x10)
{
  int32_t delta;
  int32_t rpm;

  if (t_x10 < t_start_x10)
  {
    /* 滞回死区（T_stop < T < T_start）保持最低 1000 RPM 防止临界启停抖动 */
    return CTRL_AUTO_DEADBAND_RPM;
  }

  /* T_start 起 1200 RPM，每升高 +1.0 °C (10) 增加 180 RPM，+10.0 °C (100) 达满速 3000 RPM */
  delta = (int32_t)t_x10 - (int32_t)t_start_x10;
  rpm = (int32_t)CTRL_AUTO_BASE_RPM + ((delta * 1800) / 100);
  if (rpm > (int32_t)APP_FAN_RPM_MAX)
  {
    rpm = (int32_t)APP_FAN_RPM_MAX;
  }
  return (uint16_t)rpm;
}

/**
 * @brief  推进通风控制状态机并计算本周期期望的风扇启停、目标转速与 CAN 模式码
 * @param[in]     snap       当前系统状态快照
 * @param[in,out] ctrl_state 期望内部控制状态指针（FAN_STOP/AUTO_RUN/MAN_RUN/FAILSAFE）
 * @param[out]    want_en    输出期望启停（0/1）
 * @param[out]    want_rpm   输出期望目标转速（RPM）
 * @param[out]    cmd_mode   输出 0x101 控制模式字段（CAN_CMD_MODE_*）
 */
static void EvaluateVentState(const AppState_t *snap, uint8_t *ctrl_state,
                              uint8_t *want_en, uint16_t *want_rpm, uint16_t *cmd_mode)
{
  uint8_t st = *ctrl_state;

  if (snap->ctrl_mode == CTRL_MODE_MANUAL)
  {
    *cmd_mode = CAN_CMD_MODE_MANUAL;
    if (snap->manual_run != 0U)
    {
      st = FAN_MAN_RUN;
      *want_en = 1U;
      *want_rpm = snap->manual_target_rpm;
    }
    else
    {
      st = FAN_STOP;
      *want_en = 0U;
      *want_rpm = 0U;
    }
    *ctrl_state = st;
    return;
  }

  /* 自动模式：仅在正式触发 FAULT_SENSOR_OFFLINE 后切入安全降级，避免冷开机首秒误闪 FAILSAFE */
  if ((snap->fault_mask & FAULT_SENSOR_OFFLINE) != 0U)
  {
    *ctrl_state = FAN_FAILSAFE;
    *want_en = 1U;
    *want_rpm = CTRL_FAILSAFE_RPM;
    *cmd_mode = CAN_CMD_MODE_FAILSAFE;
    return;
  }

  if (snap->sensor_valid == 0U)
  {
    /* 冷启动尚未收到首帧传感器数据也未达 3 次断线判定，保持平稳待机 */
    *ctrl_state = FAN_STOP;
    *want_en = 0U;
    *want_rpm = 0U;
    *cmd_mode = CAN_CMD_MODE_AUTO;
    return;
  }

  /* 传感器数据有效：按滞回曲线跃迁 */
  if (st == FAN_FAILSAFE)
  {
    st = (snap->temp_x10 > snap->temp_stop_x10) ? FAN_AUTO_RUN : FAN_STOP;
  }
  else if (st == FAN_AUTO_RUN)
  {
    if (snap->temp_x10 <= snap->temp_stop_x10)
    {
      st = FAN_STOP;
    }
  }
  else
  {
    /* 从 FAN_STOP 或手动切换回自动 */
    st = (snap->temp_x10 >= snap->temp_start_x10) ? FAN_AUTO_RUN : FAN_STOP;
  }

  *ctrl_state = st;
  *cmd_mode = CAN_CMD_MODE_AUTO;
  if (st == FAN_AUTO_RUN)
  {
    *want_en = 1U;
    *want_rpm = CalcAutoRpm(snap->temp_x10, snap->temp_start_x10);
  }
  else
  {
    *want_en = 0U;
    *want_rpm = 0U;
  }
}

void AppControl_NotifyUpdate(void)
{
  if ((osKernelGetState() == osKernelRunning) && (ControlTaskHandle != NULL))
  {
    (void)osThreadFlagsSet(ControlTaskHandle, CTRL_FLAG_UPDATE);
  }
}

void AppControl_MarkCanTimerReset(void)
{
  __atomic_store_n(&s_reset_timers_req, 1U, __ATOMIC_RELEASE);
}

void AppControl_ResetCanTimers(void)
{
  AppControl_MarkCanTimerReset();
  AppControl_NotifyUpdate();
}

void App_ControlTask(void)
{
  uint8_t ctrl_state = FAN_STOP;
  uint8_t tx_inited = 0U;
  uint8_t tx_seq = 0U;
  uint8_t sent_en = 0U;
  uint16_t sent_rpm = 0U;
  uint16_t sent_mode = CAN_CMD_MODE_AUTO;
  uint8_t prev_want_en = 0U;

  CanCmd101_t pending_cmd = {0};
  uint8_t cmd_waiting = 0U;
  uint8_t cmd_retries = 0U;
  uint32_t cmd_tx_tick;
  uint32_t last_cmd_tick;
  uint32_t last_hb_tick;
  uint32_t run_start_tick;

  uint8_t can_online = 0U;
  uint16_t actual_rpm = 0U;
  uint8_t node_stall_flag = 0U;
  uint8_t prev_node_stall = 0U;

  AppErr_t err = Bsp_CanInit();
  if (err != APP_OK)
  {
    Bsp_CliPrintf("[CTRL] ERR: CAN1/CAN2 init failed (%d)\r\n", (int)err);
  }

  last_hb_tick = osKernelGetTickCount();
  cmd_tx_tick = last_hb_tick;
  last_cmd_tick = last_hb_tick;
  run_start_tick = last_hb_tick;

  for (;;)
  {
    CanFrame_t rx_frame;
    uint32_t now;
    uint8_t want_en = 0U;
    uint16_t want_rpm = 0U;
    uint16_t cmd_mode = CAN_CMD_MODE_AUTO;
    uint8_t node_offline;
    uint16_t rpm_diff;
    uint8_t need_send;
    uint8_t keepalive_due;
    uint8_t show_state;
    uint16_t can_fault_mask;

    /* 100 ms 常规巡检节拍，收到控制参数或传感器变更事件时立即唤醒（延迟 < 1 ms） */
    (void)osThreadFlagsWait(CTRL_FLAG_UPDATE, osFlagsWaitAny, CTRL_LOOP_TICK_MS);
    now = osKernelGetTickCount();

    /* 红线 10：擦写 Sector 11 结束后重置所有 CAN 超时基准，抵消 1~2 s 取指挂起 */
    if (__atomic_exchange_n(&s_reset_timers_req, 0U, __ATOMIC_ACQ_REL) != 0U)
    {
      last_hb_tick = now;
      cmd_tx_tick = now;
      run_start_tick = now;
    }

    /* 1. 非阻塞排空 CAN1 FIFO0 接收队列中的所有反馈帧 */
    while ((can1RxQueueHandle != NULL) &&
           (osMessageQueueGet(can1RxQueueHandle, &rx_frame, NULL, 0U) == osOK))
    {
      if (rx_frame.dlc != 8U)
      {
        AppData_IncCanErr();
        continue;
      }

      if (rx_frame.std_id == CAN_ID_CMD_ACK)
      {
        CanAck201_t ack;
        if (CanProto_Unpack201(rx_frame.data, &ack) == APP_OK)
        {
          if ((cmd_waiting != 0U) && (ack.echo_seq == pending_cmd.seq))
          {
            cmd_waiting = 0U;
            cmd_retries = 0U;
            if (AppData_UpdateFaultBit(FAULT_CAN_CMD_TIMEOUT, 0U) != 0U)
            {
              Bsp_CliPrintf("[CTRL] CAN1 cmd ACK recovered (seq=%u)\r\n",
                            (unsigned int)ack.echo_seq);
            }
          }
          if (ack.ack == CAN_ACK_STALL)
          {
            node_stall_flag = 1U;
          }
        }
        else
        {
          AppData_IncCanErr();
        }
      }
      else if (rx_frame.std_id == CAN_ID_FAN_STATUS)
      {
        CanStat202_t stat;
        if (CanProto_Unpack202(rx_frame.data, &stat) == APP_OK)
        {
          actual_rpm = stat.actual_rpm;
          node_stall_flag = ((stat.fault_bits & CAN_NODE_FAULT_STALL) != 0U) ? 1U : 0U;
          if ((prev_node_stall != 0U) && (node_stall_flag == 0U))
          {
            /* 堵转解除后给予电机重新加速宽限窗口，防止转速尚未爬过 200 RPM 时反复抖动 */
            run_start_tick = now;
          }
          prev_node_stall = node_stall_flag;
        }
        else
        {
          AppData_IncCanErr();
        }
      }
      else if (rx_frame.std_id == CAN_ID_FAN_HB)
      {
        CanHb701_t hb;
        if ((CanProto_Unpack701(rx_frame.data, &hb) == APP_OK) &&
            (hb.hb_state == CAN_HB_STATE_ONLINE))
        {
          last_hb_tick = now;
          if (can_online == 0U)
          {
            /* 节点首次上线或失联恢复时刷新堵转宽限计时基准，给予完整 1500 ms 启动加速窗口 */
            run_start_tick = now;
          }
          can_online = 1U;
          if (AppData_UpdateFaultBit(FAULT_CAN_NODE_OFFLINE, 0U) != 0U)
          {
            Bsp_CliPrintf("[CTRL] CAN2 node online (heartbeat recovered)\r\n");
          }
        }
        else
        {
          AppData_IncCanErr();
        }
      }
    }

    /* 2. 读取最新业务快照并运行通风控制状态机 */
    AppData_GetSnapshot(&s_ctrl_snap);
    EvaluateVentState(&s_ctrl_snap, &ctrl_state, &want_en, &want_rpm, &cmd_mode);

    if ((want_en == 0U) || (prev_want_en == 0U) || (can_online == 0U))
    {
      run_start_tick = now;
    }
    prev_want_en = want_en;

    /* 3. 三类 CAN 故障独立判定（先判心跳失联，再判命令应答超时与堵转） */
    node_offline = ((now - last_hb_tick) >= CTRL_CAN_HB_TIMEOUT_MS) ? 1U : 0U;
    if (node_offline != 0U)
    {
      can_online = 0U;
      actual_rpm = 0U;
      node_stall_flag = 0U;
      prev_node_stall = 0U;
      cmd_waiting = 0U;
      cmd_retries = 0U;
      run_start_tick = now;

      if (AppData_UpdateFaultBit(FAULT_CAN_NODE_OFFLINE, 1U) != 0U)
      {
        AppData_IncCanErr();
        Bsp_CliPrintf("[CTRL] FAULT: CAN2 node offline (HB timeout > 1500 ms)\r\n");
      }
      /* 节点整体失联时收敛从属故障位，确保界面清晰聚焦根因 ERR: NODE OFF */
      (void)AppData_UpdateFaultBit(FAULT_CAN_CMD_TIMEOUT, 0U);
      (void)AppData_UpdateFaultBit(FAULT_CAN_STALL, 0U);
    }
    else
    {
      /* 3.1 命令应答超时判定：500 ms 未收匹配 0x201 重试原 seq 一次，再超 500 ms 报故障 */
      if ((cmd_waiting != 0U) && ((now - cmd_tx_tick) >= CTRL_CAN_ACK_TIMEOUT_MS))
      {
        if (cmd_retries == 0U)
        {
          uint8_t retry_raw[8];
          CanProto_Pack101(&pending_cmd, retry_raw);
          (void)Bsp_Can1Send(CAN_ID_CTRL_CMD, retry_raw);
          cmd_retries = 1U;
          cmd_tx_tick = now;
          last_cmd_tick = now;
          AppData_IncCanErr();
        }
        else
        {
          cmd_waiting = 0U;
          cmd_retries = 0U;
          AppData_IncCanErr();
          if (AppData_UpdateFaultBit(FAULT_CAN_CMD_TIMEOUT, 1U) != 0U)
          {
            Bsp_CliPrintf("[CTRL] FAULT: CAN1 cmd ACK timeout (seq=%u)\r\n",
                          (unsigned int)pending_cmd.seq);
          }
        }
      }

      /* 3.2 风扇堵转判定：节点自报堵转位，或运行超过 1500 ms 且反馈转速仍 < 200 RPM */
      {
        uint8_t rpm_stalled = ((want_en != 0U) &&
                               ((now - run_start_tick) >= CTRL_CAN_STALL_TIME_MS) &&
                               (actual_rpm < CTRL_CAN_STALL_RPM_MIN))
                                ? 1U
                                : 0U;
        uint8_t is_stall =
          ((can_online != 0U) && ((node_stall_flag != 0U) || (rpm_stalled != 0U))) ? 1U : 0U;

        if (AppData_UpdateFaultBit(FAULT_CAN_STALL, is_stall) != 0U)
        {
          if (is_stall != 0U)
          {
            AppData_IncCanErr();
            Bsp_CliPrintf("[CTRL] FAULT: CAN fan stall detected (rpm=%u)\r\n",
                          (unsigned int)actual_rpm);
          }
          else
          {
            Bsp_CliPrintf("[CTRL] CAN fan stall cleared\r\n");
          }
        }
      }
    }

    /* 4. 变更即发 + 500 ms 保活下发 0x101（自动模式转速变化 >= 50 RPM 防抖；等待 ACK 期间不插队保活） */
    rpm_diff = (want_rpm >= sent_rpm) ? (uint16_t)(want_rpm - sent_rpm)
                                      : (uint16_t)(sent_rpm - want_rpm);
    need_send = 0U;
    if ((tx_inited == 0U) || (want_en != sent_en) || (cmd_mode != sent_mode))
    {
      need_send = 1U;
    }
    else if (want_en != 0U)
    {
      if ((cmd_mode == CAN_CMD_MODE_MANUAL) && (want_rpm != sent_rpm))
      {
        need_send = 1U;
      }
      else if (rpm_diff >= CTRL_AUTO_RPM_JITTER)
      {
        need_send = 1U;
      }
    }

    keepalive_due = ((cmd_waiting == 0U) && ((now - last_cmd_tick) >= CTRL_CAN_KEEPALIVE_MS))
                      ? 1U
                      : 0U;

    if ((need_send != 0U) || (keepalive_due != 0U))
    {
      uint8_t tx_raw[8];

      tx_seq = (tx_seq == 255U) ? 1U : (uint8_t)(tx_seq + 1U);
      pending_cmd.seq = tx_seq;
      pending_cmd.enable = want_en;
      pending_cmd.target_rpm = want_rpm;
      pending_cmd.mode = cmd_mode;

      CanProto_Pack101(&pending_cmd, tx_raw);
      if (Bsp_Can1Send(CAN_ID_CTRL_CMD, tx_raw) != APP_OK)
      {
        AppData_IncCanErr();
      }

      tx_inited = 1U;
      sent_en = want_en;
      sent_rpm = want_rpm;
      sent_mode = cmd_mode;
      last_cmd_tick = now;
      cmd_tx_tick = now;
      cmd_retries = 0U;
      cmd_waiting = (node_offline == 0U) ? 1U : 0U;
    }

    /* 5. 将权威反馈转速、实际下发目标转速与展示状态同步至全局数据中心 */
    AppData_GetSnapshot(&s_ctrl_snap);
    can_fault_mask =
      (uint16_t)(s_ctrl_snap.fault_mask &
                 (FAULT_CAN_STALL | FAULT_CAN_CMD_TIMEOUT | FAULT_CAN_NODE_OFFLINE));
    show_state = (can_fault_mask != 0U) ? FAN_FAULT : ctrl_state;
    AppData_UpdateCanFeedback(actual_rpm, sent_rpm, show_state, can_online);
  }
}
