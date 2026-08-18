/**
 * @file    event.h
 * @brief   输入事件定义 + 全局事件队列句柄
 *          生产-消费模型：输入任务生产事件，UI 任务消费事件
 */
#ifndef __EVENT_H
#define __EVENT_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

typedef enum
{
    EV_MOUSE_MOVE = 0,   /* 摇杆位移事件 */
    EV_KEY_DOWN,         /* 摇杆按键按下 */
    EV_KEY_UP,           /* 摇杆按键释放 */
    EV_TICK,             /* 1 秒时钟心跳（软件定时器产生，驱动状态栏时间） */
    EV_DEV_TOGGLE,       /* 输入设备开关取反（K0 按下） */
} input_event_type_t;

/* 输入事件 */
typedef struct
{
    input_event_type_t type;
    int16_t dx;          /* X 位移（EV_MOUSE_MOVE 时有效） */
    int16_t dy;          /* Y 位移 */
} input_event_t;

/* 全局事件队列（在 freertos.c 创建，容量 8） */
extern QueueHandle_t g_event_queue;

#endif
