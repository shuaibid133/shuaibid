/**
 * @file    input_task.c
 * @brief   输入任务实现：15ms 周期轮询 dev 层（joystick），事件进队列
 *
 * 分层：硬件采样/消抖/设备开关全在 App/Dev/joystick.c（dev 层），
 *       本任务只做"扫描 → 发队列"，是生产侧唯一入口
 */
#include "input_task.h"
#include "event.h"
#include "joystick.h"
#include "sys_stats.h"
#include "sys_wdg.h"
#include "sys_watch.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

#define JOY_PERIOD_MS   15      /* 采样周期 */

/* 进阶②演示 A：1 = input_task 在开机 10 秒后死循环 → 喂狗链断 → IWDG 2s
 * 硬件复位 → 重启后再次 10 秒正常 → 循环。先正常启动（屏幕亮、进桌面）
 * 再卡死，演示"正常 → 卡死 → 自动重启"全过程（演示完改回 0） */
#define WDG_DEMO_INPUT_HANG       0
#define WDG_DEMO_INPUT_HANG_AT_MS 10000   /* 开机 10s 后开始死循环 */

void input_task(void *argument)
{
    input_event_t ev, victim;

    for (;;)
    {
        /* 演示 A：10s 后才死循环（卡在喂狗前 → IWDG 2s 复位）。
         * 若从启动就死循环，ui_task 永远轮不到执行，屏幕永远不亮，
         * 演示不出"正常→卡死→重启"的完整过程 */
        if (WDG_DEMO_INPUT_HANG &&
            xTaskGetTickCount() >= WDG_DEMO_INPUT_HANG_AT_MS) {
            while (1) { }
        }

        /* 进阶②：本任务是喂狗者 + 健康哨兵。
         * 喂狗：IWDG 2s 超时，15ms 喂一次余量巨大；
         * 打卡 + 检查：所有任务心跳超时未更新 → 记日志 */
        sys_wdg_feed();
        sys_watch_beat(WATCH_INPUT);
        sys_watch_check();

        if (joystick_scan(&ev))       /* dev 层扫描：位移/SW/K0 任一产生事件 */
        {
            g_stats_events++;         /* 统计：产生事件 +1 */
            ev.tick = xTaskGetTickCount();   /* 生产时刻戳：负载分析量"排队延迟"用 */
            if (xQueueSend(g_event_queue, &ev, 0) != pdPASS)
            {
                if (ev.type != EV_MOUSE_MOVE)
                {
                    /* 进阶① 优先级策略：队列满 + 新事件不可丢（按键/心跳/
                     * 回退）→ 挤掉队首的移动事件让位。移动是增量可合并，
                     * 少一条只少动一点；按键是一次性操作，丢了操作就没了。
                     * 挤出的 victim 不是移动事件则放回队首（不破坏顺序） */
                    if (xQueueReceive(g_event_queue, &victim, 0) == pdPASS)
                    {
                        if (victim.type == EV_MOUSE_MOVE)
                        {
                            g_stats_merged++;   /* 被挤掉的移动视作合并掉 */
                            xQueueSend(g_event_queue, &ev, 0);   /* 已腾出空间 */
                        }
                        else
                        {
                            xQueueSendToFront(g_event_queue, &victim, 0);
                            g_stats_drops++;
                        }
                    }
                    else
                    {
                        g_stats_drops++;
                    }
                }
                else
                {
                    g_stats_drops++;   /* 移动事件满队列：直接丢（可容忍） */
                }
            }
        }
        osDelay(JOY_PERIOD_MS);
    }
}
