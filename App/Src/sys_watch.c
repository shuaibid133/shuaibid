/**
 * @file    sys_watch.c
 * @brief   任务心跳监控实现（进阶②）
 *
 * 并发：g_watch_beat 由各自任务写（单写者），哨兵（input_task）只读；
 * 32 位对齐读写硬件原子，无锁。
 */
#include "sys_watch.h"
#include "sys_log.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint32_t g_watch_beat[WATCH_N] = {0};
static uint8_t s_hung[WATCH_N] = {0};        /* 去抖：已报过卡死，恢复前不重复报 */

/* 各任务卡死判定阈值（tick=1ms）。非 const：⑥ 伪 OTA 的长阻塞操作
 * （Flash 擦写 2s/扇区）需要临时豁免窗口，避免哨兵误报卡死 */
static uint32_t s_watch_ms[WATCH_N] = {
    1000,   /* WATCH_INPUT：15ms 周期，60 拍没打卡 = 死 */
    3000,   /* WATCH_MUSIC：事件驱动，音符最长 1s + 间隙 */
    2000,   /* WATCH_UI：EV_TICK 每秒必有 */
};

void sys_watch_beat(uint8_t slot)
{
    if (slot < WATCH_N) g_watch_beat[slot] = xTaskGetTickCount();
}

void sys_watch_set_interval(uint8_t slot, uint32_t ms)
{
    if (slot < WATCH_N && ms > 0) s_watch_ms[slot] = ms;
}

/* 哨兵检查：超时未打卡 → 记 ERR 日志（只报一次）；心跳恢复 → 记恢复日志。
 * uint32 无符号减法天然处理 tick 回绕 */
void sys_watch_check(void)
{
    uint8_t i;
    uint32_t now = xTaskGetTickCount();

    for (i = 0; i < WATCH_N; i++) {
        if (g_watch_beat[i] == 0) continue;   /* 从未打卡 = 任务还没启动，跳过
                                               * （否则开机瞬间全任务误报卡死） */
        if (now - g_watch_beat[i] > s_watch_ms[i]) {
            if (!s_hung[i]) {
                s_hung[i] = 1;
                sys_log_add(LOG_LV_ERR, LOG_TASK_HUNG, i);
            }
        } else if (s_hung[i]) {
            s_hung[i] = 0;
            sys_log_add(LOG_LV_WARN, LOG_TASK_RECOVER, i);
        }
    }
}
