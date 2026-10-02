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
#include "cmsis_os2.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "semphr.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "adc_dma.h"               // ADC_DMA_GetValue()
#include "serial.h"                // Serial_SendData()
#include "stm32f1xx_hal_gpio.h"    // HAL_GPIO_TogglePin()
#include <stdint.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct{
  uint16_t Infrared;   // 红外传感器 ch0
  uint16_t Light;      // 光敏传感器 ch1
}s_serialData;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* 二值信号量：创建后是 0，要等中断里 give 一次，任务才拿得到 */
SemaphoreHandle_t xBinarySem;

/* 串口发送队列：photoAdcTask 打包好的 s_serialData 塞进来，别的任务取走发出去 */
QueueHandle_t xQueue;

osThreadId_t photoAdcTaskHandle;
const osThreadAttr_t photoAdcTask_attributes = {
  .name = "photoAdcTask",
  .stack_size = 128 * 4,        // 单位是字节，不是字
  .priority = (osPriority_t) osPriorityNormal,
};

osThreadId_t serialSendTaskHandle;
const osThreadAttr_t serialSendTask_attributes = {
  .name = "serialSendTask",
  .stack_size = 128 * 4,        // 单位是字节，不是字
  .priority = (osPriority_t) osPriorityBelowNormal,
};

/* USER CODE END Variables */
/* Definitions for defaultTask */
/* 默认任务：LED 闪烁 */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void photoAdcTask(void *argument);
void serialSendTask(void *argument);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* 创建任务/信号量。这里只是"登记"，要等 osKernelStart() 之后才真正开跑 */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  xBinarySem = xSemaphoreCreateBinary();
   /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* xQueueCreate(队列深度, 每条数据多少字节)
   *   深度5  = 最多缓存 5 条还没被取走的数据，满了再 xQueueSend 就会阻塞
   *   sizeof(s_serialData) = 4 字节 */
  xQueue = xQueueCreate(5, sizeof(s_serialData));
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  photoAdcTaskHandle = osThreadNew(photoAdcTask, NULL, &photoAdcTask_attributes);
  serialSendTaskHandle = osThreadNew(serialSendTask, NULL, &serialSendTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/* USER CODE END Header_StartDefaultTask */

void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_15);   // 翻转LED
    osDelay(500);        // 睡500ms，主动让出CPU
  }
  /* USER CODE END StartDefaultTask */
}


/* USER CODE BEGIN Application */
void photoAdcTask(void *argument){
  for(;;){
    /* 没按键就阻塞在这不占 CPU；超时 10s 兜底采一次 */
    if(xSemaphoreTake(xBinarySem,10000)==pdTRUE){
           s_serialData SerialData;
           SerialData.Infrared = ADC_DMA_GetValue(0);
           SerialData.Light    = ADC_DMA_GetValue(1);
           xQueueSend(xQueue, &SerialData, 0);
           vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void serialSendTask(void *argument){
  s_serialData SerialData;
  for(;;){
    /* 阻塞等队列。portMAX_DELAY = 无限等，一直等到 photoAdcTask 塞数据进来。
     * 取到之后，SerialData 里就是那份数据的"副本"——
     * 所以 photoAdcTask 紧接着改它自己的变量也没关系。 */
    if(xQueueReceive(xQueue, &SerialData, portMAX_DELAY) == pdTRUE){
      /* 结构体两个 uint16 共4字节，按字节流发出去(小端序) */
      Serial_SendData((uint8_t *)&SerialData, sizeof(SerialData), 500);
      OLED_ShowNum(1, 1, SerialData.Infrared, 5);
      OLED_ShowNum(2, 1, SerialData.Light, 5);
    }
  }
}

/* USER CODE END Application */
