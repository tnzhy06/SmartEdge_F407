/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_monitor.h"
#include "app_cli.h"
#include "app_hmi.h"
#include "app_control.h"
#include "app_fansim.h"
#include "app_sensor.h"
#include "bsp_can.h"
#include "bsp_uart_cli.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 384 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for CliTask */
osThreadId_t CliTaskHandle;
const osThreadAttr_t CliTask_attributes = {
  .name = "CliTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Definitions for HmiTask */
osThreadId_t HmiTaskHandle;
const osThreadAttr_t HmiTask_attributes = {
  .name = "HmiTask",
  .stack_size = 1536 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for ControlTask */
osThreadId_t ControlTaskHandle;
const osThreadAttr_t ControlTask_attributes = {
  .name = "ControlTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal2,
};
/* Definitions for FanSimTask */
osThreadId_t FanSimTaskHandle;
const osThreadAttr_t FanSimTask_attributes = {
  .name = "FanSimTask",
  .stack_size = 384 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 384 * 4,
  .priority = (osPriority_t) osPriorityNormal2,
};
/* Definitions for can1RxQueue */
osMessageQueueId_t can1RxQueueHandle;
const osMessageQueueAttr_t can1RxQueue_attributes = {
  .name = "can1RxQueue"
};
/* Definitions for can2RxQueue */
osMessageQueueId_t can2RxQueueHandle;
const osMessageQueueAttr_t can2RxQueue_attributes = {
  .name = "can2RxQueue"
};
/* Definitions for sysMutex */
osMutexId_t sysMutexHandle;
const osMutexAttr_t sysMutex_attributes = {
  .name = "sysMutex"
};
/* Definitions for lcdDmaSem */
osSemaphoreId_t lcdDmaSemHandle;
const osSemaphoreAttr_t lcdDmaSem_attributes = {
  .name = "lcdDmaSem"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartCliTask(void *argument);
void StartHmiTask(void *argument);
void StartControlTask(void *argument);
void StartFanSimTask(void *argument);
void StartSensorTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 4 */
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* 任务栈溢出钩子：立即阻塞打印溢出任务名并停机，防止静默踩坏内存 */
   (void)xTask;
   Bsp_CliEmergencyPuts("\r\n[FATAL] FreeRTOS Stack Overflow in task: ");
   Bsp_CliEmergencyPuts((const char *)pcTaskName);
   Bsp_CliEmergencyPuts("\r\n");
   __disable_irq();
   for (;;)
   {
   }
}
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
void vApplicationMallocFailedHook(void)
{
   /* FreeRTOS 堆分配失败钩子：立即打印致命错误并停机定位 */
   Bsp_CliEmergencyPuts("\r\n[FATAL] FreeRTOS pvPortMalloc Failed!\r\n");
   __disable_irq();
   for (;;)
   {
   }
}
/* USER CODE END 5 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */
  /* Create the mutex(es) */
  /* creation of sysMutex */
  sysMutexHandle = osMutexNew(&sysMutex_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of lcdDmaSem */
  lcdDmaSemHandle = osSemaphoreNew(1, 0, &lcdDmaSem_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of can1RxQueue */
  can1RxQueueHandle = osMessageQueueNew (16, sizeof(CanFrame_t), &can1RxQueue_attributes);

  /* creation of can2RxQueue */
  can2RxQueueHandle = osMessageQueueNew (8, sizeof(CanFrame_t), &can2RxQueue_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of CliTask */
  CliTaskHandle = osThreadNew(StartCliTask, NULL, &CliTask_attributes);

  /* creation of HmiTask */
  HmiTaskHandle = osThreadNew(StartHmiTask, NULL, &HmiTask_attributes);

  /* creation of ControlTask */
  ControlTaskHandle = osThreadNew(StartControlTask, NULL, &ControlTask_attributes);

  /* creation of FanSimTask */
  FanSimTaskHandle = osThreadNew(StartFanSimTask, NULL, &FanSimTask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  (void)argument;
  App_MonitorTask();
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_StartCliTask */
/**
* @brief Function implementing the CliTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartCliTask */
void StartCliTask(void *argument)
{
  /* USER CODE BEGIN StartCliTask */
  (void)argument;
  App_CliTask();
  /* USER CODE END StartCliTask */
}

/* USER CODE BEGIN Header_StartHmiTask */
/**
* @brief Function implementing the HmiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartHmiTask */
void StartHmiTask(void *argument)
{
  /* USER CODE BEGIN StartHmiTask */
  (void)argument;
  App_HmiTask();
  /* USER CODE END StartHmiTask */
}

/* USER CODE BEGIN Header_StartControlTask */
/**
* @brief Function implementing the ControlTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartControlTask */
void StartControlTask(void *argument)
{
  /* USER CODE BEGIN StartControlTask */
  (void)argument;
  App_ControlTask();
  /* USER CODE END StartControlTask */
}

/* USER CODE BEGIN Header_StartFanSimTask */
/**
* @brief Function implementing the FanSimTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartFanSimTask */
void StartFanSimTask(void *argument)
{
  /* USER CODE BEGIN StartFanSimTask */
  (void)argument;
  App_FanSimTask();
  /* USER CODE END StartFanSimTask */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */
  (void)argument;
  App_SensorTask();
  /* USER CODE END StartSensorTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

