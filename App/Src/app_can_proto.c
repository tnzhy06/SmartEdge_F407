/**
 * @file    app_can_proto.c
 * @brief   双路 CAN 4 帧标准协议（0x101/0x201/0x202/0x701）编解码实现
 * @date    2026-11-28
 * @note    所有 16-bit 字段采用小端序，Byte 7 为 Byte 0..6 逐字节异或校验
 */

#include "app_can_proto.h"
#include <stddef.h>

/**
 * @brief  计算 CAN 载荷前 7 字节异或校验值
 * @param[in] data 至少 7 字节的载荷指针
 * @return uint8_t 异或校验结果
 */
static uint8_t CalcXor7(const uint8_t data[8])
{
  uint8_t x = 0U;

  for (uint8_t i = 0U; i < 7U; i++)
  {
    x ^= data[i];
  }
  return x;
}

/**
 * @brief  以小端序写入 16-bit 无符号整数
 * @param[out] dst 目标 2 字节首地址
 * @param[in]  val 待写入 16 位值
 */
static void PutLe16(uint8_t *dst, uint16_t val)
{
  dst[0] = (uint8_t)(val & 0xFFU);
  dst[1] = (uint8_t)(val >> 8U);
}

/**
 * @brief  以小端序读取 16-bit 无符号整数
 * @param[in] src 源 2 字节首地址
 * @return uint16_t 解码后的 16 位值
 */
static uint16_t GetLe16(const uint8_t *src)
{
  return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8U));
}

void CanProto_Pack101(const CanCmd101_t *in, uint8_t out[8])
{
  if ((in == NULL) || (out == NULL))
  {
    return;
  }

  out[0] = in->seq;
  out[1] = in->enable;
  PutLe16(&out[2], in->target_rpm);
  PutLe16(&out[4], in->mode);
  out[6] = 0x00U;
  out[7] = CalcXor7(out);
}

AppErr_t CanProto_Unpack101(const uint8_t in[8], CanCmd101_t *out)
{
  if ((in == NULL) || (out == NULL))
  {
    return APP_ERR_PARAM;
  }
  if (CalcXor7(in) != in[7])
  {
    return APP_ERR_IO;
  }

  out->seq = in[0];
  out->enable = in[1];
  out->target_rpm = GetLe16(&in[2]);
  out->mode = GetLe16(&in[4]);
  return APP_OK;
}

void CanProto_Pack201(const CanAck201_t *in, uint8_t out[8])
{
  if ((in == NULL) || (out == NULL))
  {
    return;
  }

  out[0] = in->echo_seq;
  out[1] = in->ack;
  PutLe16(&out[2], in->accepted_rpm);
  PutLe16(&out[4], in->state);
  out[6] = 0x00U;
  out[7] = CalcXor7(out);
}

AppErr_t CanProto_Unpack201(const uint8_t in[8], CanAck201_t *out)
{
  if ((in == NULL) || (out == NULL))
  {
    return APP_ERR_PARAM;
  }
  if (CalcXor7(in) != in[7])
  {
    return APP_ERR_IO;
  }

  out->echo_seq = in[0];
  out->ack = in[1];
  out->accepted_rpm = GetLe16(&in[2]);
  out->state = GetLe16(&in[4]);
  return APP_OK;
}

void CanProto_Pack202(const CanStat202_t *in, uint8_t out[8])
{
  if ((in == NULL) || (out == NULL))
  {
    return;
  }

  out[0] = in->node_state;
  out[1] = in->fault_bits;
  PutLe16(&out[2], in->actual_rpm);
  PutLe16(&out[4], in->target_rpm);
  out[6] = in->cnt;
  out[7] = CalcXor7(out);
}

AppErr_t CanProto_Unpack202(const uint8_t in[8], CanStat202_t *out)
{
  if ((in == NULL) || (out == NULL))
  {
    return APP_ERR_PARAM;
  }
  if (CalcXor7(in) != in[7])
  {
    return APP_ERR_IO;
  }

  out->node_state = in[0];
  out->fault_bits = in[1];
  out->actual_rpm = GetLe16(&in[2]);
  out->target_rpm = GetLe16(&in[4]);
  out->cnt = in[6];
  return APP_OK;
}

void CanProto_Pack701(const CanHb701_t *in, uint8_t out[8])
{
  if ((in == NULL) || (out == NULL))
  {
    return;
  }

  out[0] = in->hb_state;
  out[1] = in->alive_cnt;
  PutLe16(&out[2], in->uptime_s);
  PutLe16(&out[4], in->err_flags);
  out[6] = 0x00U;
  out[7] = CalcXor7(out);
}

AppErr_t CanProto_Unpack701(const uint8_t in[8], CanHb701_t *out)
{
  if ((in == NULL) || (out == NULL))
  {
    return APP_ERR_PARAM;
  }
  if (CalcXor7(in) != in[7])
  {
    return APP_ERR_IO;
  }

  out->hb_state = in[0];
  out->alive_cnt = in[1];
  out->uptime_s = GetLe16(&in[2]);
  out->err_flags = GetLe16(&in[4]);
  return APP_OK;
}
