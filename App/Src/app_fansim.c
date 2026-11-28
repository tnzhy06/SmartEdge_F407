/**
 * @file    app_fansim.c
 * @brief   CAN2 独立模拟风扇节点任务（FanSimTask）实现
 * @date    2026-11-28
 * @note    通过物理 CAN2 (SN65HVD230 #2) 接入同一条物理总线与 CAN1 交互：
 *          - 收到 0x101 立即唤醒并回复 0x201（延迟 < 1 ms）；
 *          - 每 100 ms 执行转速物理斜坡仿真（+200 / -400 RPM）；
 *          - 每 200 ms 上报 0x202 状态帧，每 500 ms 上报 0x701 心跳帧
 */

#include "app_fansim.h"
#include "app_can_proto.h"
#include "app_data.h"
#include "bsp_can.h"
#include "cmsis_os.h"
#include <stddef.h>

/** @brief CubeMX 在 Core/Src/freertos.c 中创建的 CAN2 接收队列句柄 */
extern osMessageQueueId_t can2RxQueueHandle;

/** @brief 物理斜坡仿真节拍与周期上报间隔（单位：ms） */
#define FANSIM_STEP_MS   100U
#define FANSIM_STAT_MS   200U
#define FANSIM_HB_MS     500U

/** @brief 转速斜坡步长（RPM / 100 ms） */
#define FANSIM_RAMP_UP_STEP   200U
#define FANSIM_RAMP_DOWN_STEP 400U

/** @brief 跨任务共享的故障注入位掩码（原子访问，无需互斥锁） */
static uint8_t s_inject_mask = 0U;

/**
 * @brief  根据当前运行使能、故障注入与目标转速推进一次 100 ms 物理斜坡仿真
 * @param[in]     enable     节点运行使能（0=停，1=启）
 * @param[in]     stalled    是否处于堵转注入态（1=堵转）
 * @param[in]     target_rpm 目标转速（RPM）
 * @param[in,out] base_rpm   当前平滑基线转速指针
 * @param[in,out] ripple_idx 稳态微小波动相位下标指针
 * @return uint16_t 本周期对外上报的模拟实测转速（RPM）
 */
static uint16_t StepFanPhysics(uint8_t enable, uint8_t stalled, uint16_t target_rpm,
                               uint16_t *base_rpm, uint8_t *ripple_idx)
{
  static const int8_t k_ripple[8] = {0, 3, 5, 2, 0, -3, -5, -2};
  uint16_t cur = *base_rpm;
  int32_t out_rpm;

  if ((stalled != 0U) || (enable == 0U))
  {
    /* 停机或堵转时以 -400 RPM / 100 ms 迅速制动归零 */
    cur = (cur > FANSIM_RAMP_DOWN_STEP) ? (uint16_t)(cur - FANSIM_RAMP_DOWN_STEP) : 0U;
    *base_rpm = cur;
    return cur;
  }

  /* 正常运行：以 +/-200 RPM / 100 ms 平滑跟踪目标转速 */
  if (cur < target_rpm)
  {
    uint16_t diff = (uint16_t)(target_rpm - cur);
    cur = (diff > FANSIM_RAMP_UP_STEP) ? (uint16_t)(cur + FANSIM_RAMP_UP_STEP) : target_rpm;
  }
  else if (cur > target_rpm)
  {
    uint16_t diff = (uint16_t)(cur - target_rpm);
    cur = (diff > FANSIM_RAMP_UP_STEP) ? (uint16_t)(cur - FANSIM_RAMP_UP_STEP) : target_rpm;
  }
  *base_rpm = cur;

  /* 到达稳态后叠加 +/-5 RPM 轻微波动使趋势曲线更贴近真实直流风扇反馈 */
  out_rpm = (int32_t)cur;
  if ((cur == target_rpm) && (cur >= 100U))
  {
    out_rpm += (int32_t)k_ripple[(*ripple_idx) & 0x07U];
    (*ripple_idx)++;
    if (out_rpm < 0)
    {
      out_rpm = 0;
    }
    else if (out_rpm > (int32_t)APP_FAN_RPM_MAX)
    {
      out_rpm = (int32_t)APP_FAN_RPM_MAX;
    }
  }
  return (uint16_t)out_rpm;
}

void AppFanSim_SetInjectMask(uint8_t mask)
{
  uint8_t clean = (uint8_t)(mask & SIM_INJ_ALL);
  __atomic_store_n(&s_inject_mask, clean, __ATOMIC_RELEASE);
}

uint8_t AppFanSim_GetInjectMask(void)
{
  return __atomic_load_n(&s_inject_mask, __ATOMIC_ACQUIRE);
}

void App_FanSimTask(void)
{
  uint8_t sim_enable = 0U;
  uint16_t sim_target_rpm = 0U;
  uint16_t sim_base_rpm = 0U;
  uint16_t sim_actual_rpm = 0U;
  uint8_t ripple_idx = 0U;
  uint8_t stat_cnt = 0U;
  uint8_t hb_cnt = 0U;
  uint32_t last_step_tick;
  uint32_t last_stat_tick;
  uint32_t last_hb_tick;

  /* 等待 ControlTask 完成 CAN1/CAN2 共享滤波器配置并启动物理总线 */
  while (Bsp_CanIsReady() == 0U)
  {
    osDelay(10U);
  }

  last_step_tick = osKernelGetTickCount();
  last_stat_tick = last_step_tick;
  last_hb_tick = last_step_tick;

  for (;;)
  {
    CanFrame_t rx_frame;
    uint32_t now = osKernelGetTickCount();
    uint32_t elapsed = now - last_step_tick;
    uint32_t wait_ms = (elapsed < FANSIM_STEP_MS) ? (FANSIM_STEP_MS - elapsed) : 0U;
    uint8_t inj = __atomic_load_n(&s_inject_mask, __ATOMIC_ACQUIRE);

    /* 1. 阻塞等待 0x101 控制帧（超时补齐至 100 ms 节拍），收到命令立即回复 0x201 */
    if ((can2RxQueueHandle != NULL) &&
        (osMessageQueueGet(can2RxQueueHandle, &rx_frame, NULL, wait_ms) == osOK))
    {
      do
      {
        CanCmd101_t cmd;
        if ((rx_frame.std_id == CAN_ID_CTRL_CMD) && (rx_frame.dlc == 8U) &&
            (CanProto_Unpack101(rx_frame.data, &cmd) == APP_OK))
        {
          sim_enable = (cmd.enable != 0U) ? 1U : 0U;
          sim_target_rpm = (sim_enable != 0U) ? cmd.target_rpm : 0U;
          if (sim_target_rpm > APP_FAN_RPM_MAX)
          {
            sim_target_rpm = APP_FAN_RPM_MAX;
          }

          /* 仅当未注入超时或失联故障时，立即经 CAN2 回发 0x201 命令应答帧 */
          inj = __atomic_load_n(&s_inject_mask, __ATOMIC_ACQUIRE);
          if ((inj & (SIM_INJ_TIMEOUT | SIM_INJ_OFFLINE)) == 0U)
          {
            CanAck201_t ack;
            uint8_t tx_raw[8];
            uint8_t stalled = ((inj & SIM_INJ_STALL) != 0U) ? 1U : 0U;

            ack.echo_seq = cmd.seq;
            ack.ack = (stalled != 0U) ? CAN_ACK_STALL : CAN_ACK_OK;
            ack.accepted_rpm = sim_target_rpm;
            if (stalled != 0U)
            {
              ack.state = CAN_NODE_STATE_STALL;
            }
            else
            {
              ack.state = (sim_enable != 0U) ? CAN_NODE_STATE_RUN : CAN_NODE_STATE_STOP;
            }
            CanProto_Pack201(&ack, tx_raw);
            (void)Bsp_Can2Send(CAN_ID_CMD_ACK, tx_raw);
          }
        }
      } while (osMessageQueueGet(can2RxQueueHandle, &rx_frame, NULL, 0U) == osOK);
    }

    now = osKernelGetTickCount();
    inj = __atomic_load_n(&s_inject_mask, __ATOMIC_ACQUIRE);

    /* 2. 每 100 ms 推进一次转速斜坡仿真 */
    if ((now - last_step_tick) >= FANSIM_STEP_MS)
    {
      uint8_t stalled = ((inj & SIM_INJ_STALL) != 0U) ? 1U : 0U;
      last_step_tick = now;
      sim_actual_rpm = StepFanPhysics(sim_enable, stalled, sim_target_rpm,
                                      &sim_base_rpm, &ripple_idx);
    }

    /* 3. 节点在线（未注入失联）时按 200 ms 上报 0x202 状态帧，按 500 ms 上报 0x701 心跳帧 */
    if ((inj & SIM_INJ_OFFLINE) == 0U)
    {
      if ((now - last_stat_tick) >= FANSIM_STAT_MS)
      {
        CanStat202_t stat;
        uint8_t tx_raw[8];
        uint8_t stalled = ((inj & SIM_INJ_STALL) != 0U) ? 1U : 0U;

        last_stat_tick = now;
        if (stalled != 0U)
        {
          stat.node_state = CAN_NODE_STATE_STALL;
          stat.fault_bits = CAN_NODE_FAULT_STALL;
        }
        else
        {
          stat.node_state = (sim_enable != 0U) ? CAN_NODE_STATE_RUN : CAN_NODE_STATE_STOP;
          stat.fault_bits = 0U;
        }
        stat.actual_rpm = sim_actual_rpm;
        stat.target_rpm = sim_target_rpm;
        stat.cnt = stat_cnt++;

        CanProto_Pack202(&stat, tx_raw);
        (void)Bsp_Can2Send(CAN_ID_FAN_STATUS, tx_raw);
      }

      if ((now - last_hb_tick) >= FANSIM_HB_MS)
      {
        CanHb701_t hb;
        uint8_t tx_raw[8];

        last_hb_tick = now;
        hb.hb_state = CAN_HB_STATE_ONLINE;
        hb.alive_cnt = hb_cnt++;
        hb.uptime_s = (uint16_t)((now / 1000UL) & 0xFFFFUL);
        hb.err_flags = (uint16_t)inj;

        CanProto_Pack701(&hb, tx_raw);
        (void)Bsp_Can2Send(CAN_ID_FAN_HB, tx_raw);
      }
    }
  }
}
