/**
 * @file    app_cli.h
 * @brief   串口调试命令行任务（CliTask）接口定义
 * @date    2026-11-26
 * @note    M1 阶段提供 help、status、rtc set、reboot 等基础验证命令
 */

#ifndef APP_CLI_H
#define APP_CLI_H

/**
 * @brief  串口 CLI 任务主循环（由 freertos.c 的 StartCliTask 直接调用）
 * @note   本函数永不返回；通过线程标志阻塞等待 USART1 IDLE 中断唤醒
 */
void App_CliTask(void);

#endif /* APP_CLI_H */
