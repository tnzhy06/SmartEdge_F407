/**
 * @file    app_modbus_rtu.c
 * @brief   轻量 Modbus RTU 主站组帧、CRC16 与 KTH2-R 温湿度应答解析实现
 * @date    2026-11-28
 * @note    验证示例（SRS 4.2）：
 *          - 请求：01 03 03 00 00 02 C4 4F
 *          - 应答：01 03 04 01 02 02 88 5B 09 -> 25.8 °C / 64.8 %RH
 */

#include "app_modbus_rtu.h"
#include <stddef.h>

uint16_t ModbusRtu_Crc16(const uint8_t *buf, uint16_t len)
{
  uint16_t crc = 0xFFFFU;

  if (buf == NULL)
  {
    return 0U;
  }

  for (uint16_t i = 0U; i < len; i++)
  {
    crc ^= (uint16_t)buf[i];
    for (uint8_t bit = 0U; bit < 8U; bit++)
    {
      if ((crc & 0x0001U) != 0U)
      {
        crc = (uint16_t)((crc >> 1U) ^ 0xA001U);
      }
      else
      {
        crc >>= 1U;
      }
    }
  }
  return crc;
}

uint16_t ModbusRtu_BuildRead03(uint8_t slave, uint16_t reg, uint16_t qty, uint8_t out[8])
{
  uint16_t crc;

  if (out == NULL)
  {
    return 0U;
  }

  out[0] = slave;
  out[1] = 0x03U; /* Read Holding Registers */
  out[2] = (uint8_t)(reg >> 8U);
  out[3] = (uint8_t)(reg & 0xFFU);
  out[4] = (uint8_t)(qty >> 8U);
  out[5] = (uint8_t)(qty & 0xFFU);

  /* Modbus RTU 规范规定 CRC16 字节序为小端（低字节在前、高字节在后） */
  crc = ModbusRtu_Crc16(out, 6U);
  out[6] = (uint8_t)(crc & 0xFFU);
  out[7] = (uint8_t)(crc >> 8U);
  return 8U;
}

AppErr_t ModbusRtu_ParseKth2Resp(const uint8_t *rx, uint16_t len, uint8_t slave,
                                 int16_t *temp_x10, uint16_t *humi_x10)
{
  uint16_t calc_crc;
  uint16_t recv_crc;
  uint16_t raw_t;
  int16_t t;
  uint16_t h;

  if ((rx == NULL) || (temp_x10 == NULL) || (humi_x10 == NULL))
  {
    return APP_ERR_PARAM;
  }

  /* 固定 9 字节：Addr(1) + Func 0x03(1) + ByteCnt 0x04(1) + Temp(2) + Humi(2) + CRC(2) */
  if ((len != 9U) || (rx[0] != slave) || (rx[1] != 0x03U) || (rx[2] != 0x04U))
  {
    return APP_ERR_IO;
  }

  calc_crc = ModbusRtu_Crc16(rx, 7U);
  recv_crc = (uint16_t)((uint16_t)rx[7] | ((uint16_t)rx[8] << 8U));
  if (calc_crc != recv_crc)
  {
    return APP_ERR_IO;
  }

  /* 温度采用 16 位大端有符号二进制补码（支持负温，如 0xFF27 -> -21.7 °C） */
  raw_t = (uint16_t)(((uint16_t)rx[3] << 8U) | (uint16_t)rx[4]);
  t = (int16_t)raw_t;
  h = (uint16_t)(((uint16_t)rx[5] << 8U) | (uint16_t)rx[6]);

  /* 按 SRS FR-SENS-002 核验物理量程合法性，剔除传感器内部异常码或总线错码 */
  if ((t < KTH2_TEMP_MIN_X10) || (t > KTH2_TEMP_MAX_X10) || (h > KTH2_HUMI_MAX_X10))
  {
    return APP_ERR_IO;
  }

  *temp_x10 = t;
  *humi_x10 = h;
  return APP_OK;
}
