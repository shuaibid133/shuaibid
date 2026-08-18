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
#include "cmsis_os.h"

#define JOY_PERIOD_MS   15     /* 采样周期 */

void input_task(void *argument)
{
    input_event_t ev;

    for (;;)
    {
        if (joystick_scan(&ev))       /* dev 层扫描：位移/SW/K0 任一产生事件 */
        {
            g_stats_events++;         /* 统计：产生事件 +1 */
            if (xQueueSend(g_event_queue, &ev, 0) != pdPASS)
            {
                g_stats_drops++;      /* 统计：队列满（背压）丢弃 +1 */
            }
        }
        osDelay(JOY_PERIOD_MS);
    }
}
