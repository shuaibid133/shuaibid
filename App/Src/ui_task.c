/**
 * @file    ui_task.c
 * @brief   UI 任务实现：独占屏幕渲染（所有绘制都在本任务完成，避免多任务抢 LCD），
 *          消费事件队列：移动事件 → cursor_move
 *          后续桌面/文件/画图等应用都挂在本任务上
 */
#include "ui_task.h"
#include "main.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "event.h"

void ui_task(void *argument)
{
    input_event_t ev;
    uint8_t ret = atk_md0280_init();   /* 0 = 屏幕初始化成功 */

    if (ret == 0)
    {
        atk_md0280_clear(ATK_MD0280_WHITE);
        cursor_init(120, 160);         /* 光标初始位置：屏幕中央 */
        cursor_show();
    }

    for (;;)
    {
        if (xQueueReceive(g_event_queue, &ev, portMAX_DELAY) == pdPASS)
        {
            switch (ev.type)
            {
            case EV_MOUSE_MOVE:
                cursor_move(ev.dx, ev.dy);
                break;
            case EV_KEY_DOWN:
            case EV_KEY_UP:
            default:
                break;   /* 按键事件：桌面框架阶段再处理 */
            }
        }
    }
}
