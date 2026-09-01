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
    EV_BACK,             /* 返回键（K1 按下）：应用→桌面→锁屏，逐级回退 */
} input_event_type_t;

/* 输入事件
 * tick = 生产时刻时间戳（xTaskGetTickCount，1kHz）。
 * 用途：负载分析——消费者算出"入队→出队"的排队延迟；
 *       队列满丢弃时也能知道"积压了多久"（进阶④ 运行负载分析）。 */
typedef struct
{
    input_event_type_t type;
    int16_t dx;          /* X 位移（EV_MOUSE_MOVE 时有效） */
    int16_t dy;          /* Y 位移 */
    uint32_t tick;       /* 生产时刻（tick 数，生产者入队前填写） */
} input_event_t;

/* 全局事件队列（在 freertos.c 创建，容量 16） */
extern QueueHandle_t g_event_queue;

#endif
