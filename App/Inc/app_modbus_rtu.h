/**
 * @file    app_modbus_rtu.h
 * @brief   轻量 Modbus RTU 主站组帧、CRC16 与 KTH2-R 温湿度应答解析接口
 * @date    2026-11-28
 * @note    纯算法无状态模块，严格对齐 SRS 4.2 节与 SDD 5.3 节 KTH2-R 寄存器协议
 */

#ifndef APP_MODBUS_RTU_H
#define APP_MODBUS_RTU_H

#include <stdint.h>
#include "app_err.h"

/** @brief KTH2-R 温湿度保持寄存器起始地址（0x0300 温度, 0x0301 湿度）与联读个数 */
#define KTH2_REG_TEMP_ADDR 0x0300U
#define KTH2_REG_READ_QTY  2U

/** @brief KTH2-R 物理量程合法边界（温度 -40.0..85.0 °C，湿度 0.0..100.0 %RH） */
#define KTH2_TEMP_MIN_X10 (-400)
#define KTH2_TEMP_MAX_X10 850
#define KTH2_HUMI_MAX_X10 1000U

/**
 * @brief  计算标准 Modbus RTU CRC16 校验值（初值 0xFFFF，反射多项式 0xA001）
 * @param[in] buf 输入字节流首地址（非空）
 * @param[in] len 参与计算的字节数
 * @return uint16_t 16 位 CRC 校验值（发送时低字节在前、高字节在后）
 */
uint16_t ModbusRtu_Crc16(const uint8_t *buf, uint16_t len);

/**
 * @brief  构造功能码 0x03（读保持寄存器）8 字节 Modbus RTU 请求帧
 * @param[in]  slave 从站地址（如 KTH2-R 默认 0x01）
 * @param[in]  reg   起始寄存器地址（如 0x0300）
 * @param[in]  qty   读取寄存器数量（如 2）
 * @param[out] out   输出 8 字节完整帧缓冲区（非空）
 * @return uint16_t  构造成功返回 8，out 为空返回 0
 */
uint16_t ModbusRtu_BuildRead03(uint8_t slave, uint16_t reg, uint16_t qty, uint8_t out[8]);

/**
 * @brief  校验并解析 KTH2-R 9 字节温湿度联读应答帧（含负温度补码与量程核验）
 * @param[in]  rx       收到的原始字节缓冲区（非空）
 * @param[in]  len      收到字节数（合法应答固定为 9）
 * @param[in]  slave    期望匹配的从站地址
 * @param[out] temp_x10 输出有符号温度（单位：0.1 °C，范围 -400..850）
 * @param[out] humi_x10 输出无符号湿度（单位：0.1 %RH，范围 0..1000）
 * @return AppErr_t     APP_OK 解析合法；APP_ERR_PARAM 指针为空；APP_ERR_IO 长度/地址/CRC/量程错误
 */
AppErr_t ModbusRtu_ParseKth2Resp(const uint8_t *rx, uint16_t len, uint8_t slave,
                                 int16_t *temp_x10, uint16_t *humi_x10);

#endif /* APP_MODBUS_RTU_H */
