/**
 * @file    sys_stats.c
 * @brief   系统运行统计实现
 *
 * 统计源：
 *   - g_stats_events / g_stats_drops：input_task 事件生产处埋点
 *     （发送前 +1，xQueueSend 返回 pdFAIL 说明队列满 → 丢弃 +1）
 *   - g_stats_errors：各模块系统级异常埋点
 *     （rtc_app LSE 起振失败降级 LSI；文件系统挂载失败等）
 */
#include "sys_stats.h"

volatile uint32_t g_stats_events = 0;
volatile uint32_t g_stats_drops  = 0;
volatile uint32_t g_stats_errors = 0;
volatile uint32_t g_stats_sleeps = 0;
volatile uint32_t g_stats_q_peak  = 0;
volatile uint32_t g_stats_dly_max = 0;
volatile uint32_t g_stats_dly_sum = 0;
volatile uint32_t g_stats_dly_cnt = 0;
volatile uint32_t g_stats_merged  = 0;

void sys_stats_reset(void)
{
    g_stats_events = 0;
    g_stats_drops  = 0;
    g_stats_errors = 0;
    g_stats_sleeps = 0;
    g_stats_q_peak  = 0;
    g_stats_dly_max = 0;
    g_stats_dly_sum = 0;
    g_stats_dly_cnt = 0;
    g_stats_merged  = 0;
}
