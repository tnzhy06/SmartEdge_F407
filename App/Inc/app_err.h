/**
 * @file    app_err.h
 * @brief   SmartEdge_F407 全局统一错误码定义
 * @date    2026-11-26
 * @note    所有 BSP 与 App 公开接口统一返回 AppErr_t（0 为成功，负值为失败）
 */

#ifndef APP_ERR_H
#define APP_ERR_H

/**
 * @brief 全局统一模块返回码枚举
 */
typedef enum
{
  APP_OK          = 0,  /**< 执行成功 */
  APP_ERR_PARAM   = -1, /**< 参数越界或指针非法（对应 Modbus 异常码 0x03） */
  APP_ERR_PERM    = -2, /**< 当前模式或状态拒绝执行（对应 Modbus 异常码 0x04） */
  APP_ERR_ADDR    = -3, /**< 地址或寄存器不存在（对应 Modbus 异常码 0x02） */
  APP_ERR_TIMEOUT = -4, /**< 通信或同步等待超时 */
  APP_ERR_BUSY    = -5, /**< 外设或发送队列/邮箱忙 */
  APP_ERR_IO      = -6, /**< 硬件读写、校验或存储错误 */
  APP_ERR_NOT_RDY = -7  /**< 模块未初始化或设备未就绪 */
} AppErr_t;

#endif /* APP_ERR_H */
