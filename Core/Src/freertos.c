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
#include "stm32f103xb.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "semphr.h"
#include "stdio.h"
#include "string.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "adc_dma.h"               // ADC_DMA_GetValue()
#include "serial.h"                // Serial_SendData()
#include "interrupt.h"             // g_sample_enable 采样开关
#include "stm32f1xx_hal_gpio.h"    // HAL_GPIO_TogglePin()
#include <stdint.h>
#include "can.h"                   // CAN_Receive()
#include "protocol.h"
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
/* 串口发送队列：photoAdcTask 打包好的 s_serialData 塞进来，别的任务取走发出去 */
QueueHandle_t xQueue;

//采集的数据发送任务
osThreadId_t photoAdcTaskHandle;
const osThreadAttr_t photoAdcTask_attributes = {
  .name = "photoAdcTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,   // 周期采样+发送，硬时限→最高
};
//串口发送任务
osThreadId_t serialSendTaskHandle;
const osThreadAttr_t serialSendTask_attributes = {
  .name = "serialSendTask",
  .stack_size = 256 * 4,  
  .priority = (osPriority_t) osPriorityBelowNormal,
};
// CAN 接收队列：深度 8(能缓存8帧还没被显示)，每个元素是一整帧。
osThreadId_t canRxHandle;
const osThreadAttr_t canRxTask_attributes = {
  .name = "canRxTask",
  .stack_size = 256 * 4,
  .priority = (osPriority_t) osPriorityNormal,   // 平时阻塞在队列上，不占 CPU
};

//心跳包
osThreadId_t canHeartHandle;
const osThreadAttr_t canHeartTask_attributes = {
  .name = "canHeartTask",
  .stack_size = 256 * 4,  
  .priority = (osPriority_t) osPriorityLow7,
};



/* USER CODE END Variables */
/* Definitions for defaultTask */
/* 默认任务：LED 闪烁 */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void photoAdcTask(void *argument);
void serialSendTask(void *argument);
void canRxTask(void *argument);
void canHeartTask(void *argument);

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
   /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* xQueueCreate(队列深度, 每条数据多少字节)
   *   深度5  = 最多缓存 5 条还没被取走的数据，满了再 xQueueSend 就会阻塞
   *   sizeof(s_serialData) = 4 字节 */
  xQueue = xQueueCreate(5, sizeof(s_serialData));

  /* CAN 接收队列：深度 8(能缓存8帧还没被显示)，每个元素是一整帧。 */
  canRxQueue = osMessageQueueNew(8, sizeof(CanFrame), NULL);
  if (canRxQueue == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  photoAdcTaskHandle = osThreadNew(photoAdcTask, NULL, &photoAdcTask_attributes);
  serialSendTaskHandle = osThreadNew(serialSendTask, NULL, &serialSendTask_attributes);
  canRxHandle = osThreadNew(canRxTask, NULL, &canRxTask_attributes);
  canHeartHandle = osThreadNew(canHeartTask, NULL, &canHeartTask_attributes);
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
    osDelay(2000);
  }
  /* USER CODE END StartDefaultTask */
}


/* USER CODE BEGIN Application */
void photoAdcTask(void *argument){
  static uint8_t can_seq = 0;   // A_SENSOR 帧序列号，每发一帧 +1(0~255 自动回绕)
  for(;;){
    if(g_sample_enable == 1){
           s_serialData SerialData;
           SerialData.Infrared = ADC_DMA_GetValue(0);
           SerialData.Light    = ADC_DMA_GetValue(1);
           //xQueueSend(xQueue, &SerialData, 0);   // 数据塞到队列里让串口拿走发送

           uint8_t canData[5];
           PROTO_PackASensor(canData, SerialData.Infrared, SerialData.Light, can_seq++);
           CAN_Send(CAN_ID_A_SENSOR, canData, 5);     // 5字节：红外+光敏+序列号
    }
    vTaskDelay(pdMS_TO_TICKS(100));   // 100ms 周期：开着就采，关着就空等
  }
}

void serialSendTask(void *argument){
  s_serialData d;
  char buf[64];
  for(;;){
    if(xQueueReceive(xQueue, &d, portMAX_DELAY) == pdTRUE){
      int n = snprintf(buf, sizeof(buf), "t=%lu,IR=%u,LDR=%u\r\n",
                       (unsigned long)xTaskGetTickCount(), d.Infrared, d.Light);
      Serial_SendData((uint8_t *)buf, (uint16_t)n, 500);
      OLED_ShowNum(1, 1, d.Infrared, 5);
      OLED_ShowNum(2, 1, d.Light, 5);
    }
  }
}

void canHeartTask(void *argument){
  uint8_t hb_cnt = 0;                    // 心跳计数，每发一次 +1(0~255 自动回绕)
  uint8_t hb[LEN_HEARTBEAT];             // 2 字节：魔数 + 计数

  for(;;){
    PROTO_PackHeartbeat(hb, HB_MAGIC_A, hb_cnt++);
    CAN_Send(CAN_ID_A_HEARTBEAT, hb, LEN_HEARTBEAT);   // 0x701，2 字节
    vTaskDelay(pdMS_TO_TICKS(1000));     // 1 秒一次
  }
}


/**
 * @brief  CAN 接收显示任务
 * @note   死等队列：没帧就睡，来了帧立刻被唤醒，不空转也不漏帧。
 */
void canRxTask(void *argument){
  CanFrame frame;

  OLED_ShowString(1, 1, "ID:");
  OLED_ShowString(2, 1, "LEN:");
  OLED_ShowString(3, 1, "D:");

  for(;;){
    if (osMessageQueueGet(canRxQueue, &frame, NULL, osWaitForever) != osOK)
    {
      continue;
    }

    OLED_ShowHexNum(1, 5, frame.id, 3);    // 11位ID → 最多 3 位十六进制(0x7FF)
    OLED_ShowNum(2, 6, frame.len, 1);      // DLC 0~8，1 位就够

    for (uint8_t i = 0; i < 8; i++)
    {
      uint8_t Line   = (i < 4) ? 3 : 4;    // 8 字节分两行，每行 4 个
      uint8_t Column = 3 + (i % 4) * 2;

      if (i < frame.len)
      {
        OLED_ShowNum(Line, Column, frame.data[i], 2);
      }
      else
      {
        OLED_ShowString(Line, Column, "--");   // 超出本帧长度的字节占位
      }
    }
  }
}

/* USER CODE END Application */
