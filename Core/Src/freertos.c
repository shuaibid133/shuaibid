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
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "app_config.h"
#include "event.h"
#include "input_task.h"
#include "ui_task.h"
#include "desktop.h"
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
QueueHandle_t g_event_queue;
osThreadId_t inputTaskHandle;
osThreadId_t uiTaskHandle;
const osThreadAttr_t inputTask_attributes = {
  .name = "inputTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
const osThreadAttr_t uiTask_attributes = {
  .name = "uiTask",
  /* 512 words x 4B = 2KB: FATFS calls (f_open/f_readdir/f_mkfs) run in this
     task's stack; 1KB left too little headroom */
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal,
};
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void tick_timer_cb(void *argument);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* 1s periodic software timer: callback emits EV_TICK into the event queue,
     driving the status bar clock. The callback runs in the timer daemon task
     and must NOT block - a queue send is the only thing it does. */
  osTimerId_t tickTimer = osTimerNew(tick_timer_cb, osTimerPeriodic, NULL, NULL);
  osTimerStart(tickTimer, 1000);
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* event queue: capacity 16, input task produces, UI task consumes.
     Sized generously: while Files formats the flash (a few seconds of UI
     stall) K1-press events must survive, not get dropped behind EV_TICKs. */
  g_event_queue = xQueueCreate(16, sizeof(input_event_t));
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* input task: joystick sampling -> event queue (top priority, 15ms period) */
  inputTaskHandle = osThreadNew(input_task, NULL, &inputTask_attributes);
  /* UI task: sole owner of LCD rendering, consumes event queue */
  uiTaskHandle = osThreadNew(ui_task, NULL, &uiTask_attributes);
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
  /* LCD init and cursor rendering moved to UI task; this task only toggles LED */
  /* Infinite loop */
  for(;;)
  {
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* Heap-exhaustion watchdog: heap_4 calls this when any RTOS allocation fails.
   Freeze here so a dead allocation is VISIBLE (LED stops blinking) instead of
   a silently missing task (2026-08-18: uiTask failed to start, screen black). */
void vApplicationMallocFailedHook(void)
{
    for (;;) { }
}

/* 1s tick timer callback: producer of EV_TICK, must not block.
   UI task is the only consumer - single-writer rendering stays intact. */
static void tick_timer_cb(void *argument)
{
    input_event_t ev = { .type = EV_TICK, .dx = 0, .dy = 0 };

    xQueueSend(g_event_queue, &ev, 0);
}

/* USER CODE END Application */

