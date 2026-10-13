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
/* 心跳超时门限：对端心跳周期 1s，3 倍时间收不到就判掉线 */
#define HB_TIMEOUT_MS   3000u
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
   *   深度8  = 最多缓存 8 条还没被取走的数据，满了再 xQueueSend 就会阻塞
   *   sizeof(s_serialData) = 4 字节 */
  xQueue = xQueueCreate(8, sizeof(s_serialData));

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
  /* 每个句柄都要查！栈和 TCB 都是从 configTOTAL_HEAP_SIZE(8KB) 里分的，
   * 堆不够时 osThreadNew 返回 NULL，任务会【静默地永远不运行】—— 连报错都没有，
   * 只表现为"别的功能全正常，偏偏它不工作"，极难往这上面想。
   * 建不起来就进 Error_Handler() 停机报信，比带着残缺的任务集往下跑安全。 */
  photoAdcTaskHandle = osThreadNew(photoAdcTask, NULL, &photoAdcTask_attributes);
  if (photoAdcTaskHandle == NULL) { Error_Handler(); }

  serialSendTaskHandle = osThreadNew(serialSendTask, NULL, &serialSendTask_attributes);
  if (serialSendTaskHandle == NULL) { Error_Handler(); }

  canRxHandle = osThreadNew(canRxTask, NULL, &canRxTask_attributes);
  if (canRxHandle == NULL) { Error_Handler(); }

  canHeartHandle = osThreadNew(canHeartTask, NULL, &canHeartTask_attributes);
  if (canHeartHandle == NULL) { Error_Handler(); }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/* USER CODE END Header_StartDefaultTask */

void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);   // 先把 LED 熄灭
  for(;;)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);   // 翻转LED
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

/**
 * @brief  心跳发送任务(A 节点)
 * @note   每 1 秒发一帧 0x701，载荷 = 魔数 0xA5 + 自增计数。
 *         计数是给接收方算"漏了几拍"用的(算法同 PROTO_SeqLost)。
 */
void canHeartTask(void *argument){
  uint8_t hb_cnt = 0;                       // 心跳计数，每发一帧 +1(0~255 自动回绕)
  uint8_t data[LEN_HEARTBEAT];              // 2 字节：魔数 + 计数
  for(;;){
    PROTO_PackHeartbeat(data, HB_MAGIC_A, hb_cnt++);
    CAN_Send(CAN_ID_A_HEARTBEAT, data, LEN_HEARTBEAT);   // 0x701，2 字节
    vTaskDelay(pdMS_TO_TICKS(1000));        // 1 秒一次
  }
}

/**
 * @brief  CAN 接收任务：心跳判活
 * @note   NULL
 */
void canRxTask(void *argument){
  CanFrame frame;
  uint32_t last_b_tick  = 0;    // 上次收到 B 心跳的时刻
  uint8_t  b_online  = 0;
  uint32_t last_gw_tick = 0;    // 上次收到网关心跳的时刻
  uint8_t  gw_online = 0;

  for(;;){
    /* 最多等 500ms：有帧就处理，没帧也要醒来查超时 */
    if (osMessageQueueGet(canRxQueue, &frame, NULL, pdMS_TO_TICKS(500)) == osOK)
    {
      /*紧急制动0x000*/
      if(frame.id ==CAN_ID_ESTOP && frame.len == LEN_ESTOP){
        if(frame.data[0] == ESTOP_ACTION_CLEAR){
          OLED_ShowString(3,11,"     ");
        }
        else{
          OLED_ShowString(3, 11,"ESTOP");
        }
      }

      /* 1. B心跳 0x702：ID、DLC、魔数三者都对，才算一个有效心跳。 */
      else if (frame.id == CAN_ID_B_HEARTBEAT && frame.len == LEN_HEARTBEAT
          && frame.data[0] == HB_MAGIC_B)
      {
        last_b_tick = osKernelGetTickCount();      // 记下"B 刚活过"
        if (!b_online)
        {
          b_online = 1;
          OLED_ShowString(4, 1, "b ON ");
        }
      }

      /* 2. 网关心跳 0x703 */
      else if (frame.id == CAN_ID_GW_HEARTBEAT && frame.len == LEN_HEARTBEAT
               && frame.data[0] == HB_MAGIC_GW)
      {
        last_gw_tick = osKernelGetTickCount();
        if (!gw_online)
        {
          gw_online = 1;
          OLED_ShowString(4, 7, "gw ON ");
        }
      }

      /* 3. 0x103 GW_STATUS 网关的状态
            *   B0     WiFi 状态(0=断开, 非0=已连)
            *   B1     MQTT 状态(0=断开, 非0=已连)
            *   B2-B3  本地缓存条数 0~65535(大端)
            *断网时数据先存本地，这个数字告诉别的节点"我积压了多少"。*/ 
      else if(frame.id == CAN_ID_GW_STATUS && frame.len == LEN_GW_STATUS){
        OLED_ShowNum(3, 1, frame.data[0], 1);//wifi_is_connected() ? 1 : 0
        OLED_ShowNum(3, 3, frame.data[1], 1);//mqtt_is_connected() ? 1 : 0
        OLED_ShowNum(3, 5, PROTO_GetU16(frame.data + 2), 5);
      }

    }
  /* ---- 超时判活 ----
    * 算的是"距上次心跳过了多久"，用无符号减法，tick 回绕也算得对。
    * 只判一次(靠 online 标志)，避免每轮都刷屏。 */
    if (b_online && (osKernelGetTickCount() - last_b_tick) > HB_TIMEOUT_MS)
    {
      b_online = 0;
      OLED_ShowString(4, 1, "b OFF");  
    }
    if (gw_online && (osKernelGetTickCount() - last_gw_tick) > HB_TIMEOUT_MS)
    {
      gw_online = 0;
      OLED_ShowString(4, 7, "gw OFF");
    }
  }
}


/* USER CODE END Application */
