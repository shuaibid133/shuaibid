/**
 * @file    ui_task.c
 * @brief   UI 任务实现：独占屏幕渲染（所有绘制都在本任务完成，避免多任务抢 LCD）
 *          事件循环：收到事件 → 移动事件先更新光标 → 全部交给桌面框架处理
 *          桌面框架（desktop.c）内部管理界面状态机（BOOT/LOGIN/DESKTOP）
 */
#include "ui_task.h"
#include "main.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "desktop.h"
#include "rtc_app.h"
#include "sys_cfg.h"
#include "sys_backlight.h"
#include "app_config.h"

void ui_task(void *argument)
{
    input_event_t ev;
    uint8_t ret = atk_md0280_init();   /* 0 = 屏幕初始化成功 */

    if (ret == 0)
    {
        atk_md0280_clear(ATK_MD0280_WHITE);
        rtc_app_init();                /* 时间源：内部 RTC（LSE，VBAT 保持） */
        sys_backlight_init();          /* 先点亮背光（默认亮度），再加载配置——
                                        * 即使配置读取异常，屏幕也可见（好诊断） */
        sys_cfg_load();                /* Flash → g_sys_cfg：灵敏度/亮度等在登录前生效 */
        sys_backlight_set(g_sys_cfg.brightness);   /* 应用配置的亮度 */
        desktop_init();                /* BOOT 2秒 → LOGIN，内部完成光标接管 */
    }

    for (;;)
    {
        if (xQueueReceive(g_event_queue, &ev, portMAX_DELAY) == pdPASS)
        {
            /* 移动事件：先更新光标，再通知桌面框架（键盘高亮等） */
            if (ev.type == EV_MOUSE_MOVE)
            {
                cursor_move(ev.dx, ev.dy);
            }
            desktop_handle_event(&ev);
        }
    }
}
