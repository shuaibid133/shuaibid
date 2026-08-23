/**
 * @file    sys_stats.h
 * @brief   系统运行统计：跨任务共享的计数变量
 *
 * 并发说明：volatile uint32_t 字对齐访问在 CM3 上是单条指令、硬件原子，
 *           生产（input_task）独占写、消费（ui_task）只读，无需锁
 */
#ifndef __SYS_STATS_H
#define __SYS_STATS_H

#include <stdint.h>

extern volatile uint32_t g_stats_events;   /* 输入事件总数（input_task 每次产生事件 +1） */
extern volatile uint32_t g_stats_drops;    /* 队列满丢弃数（xQueueSend 失败 = 背压证据） */
extern volatile uint32_t g_stats_errors;   /* 系统级错误累计（RTC 降级 / 文件系统失败等） */
extern volatile uint32_t g_stats_sleeps;   /* 熄屏次数（desktop 空闲超时灭背光 +1，唤醒不计数） */

void sys_stats_reset(void);                /* 清零（开机时调用一次） */

#endif
