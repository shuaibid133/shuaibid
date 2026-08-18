/**
 * @file    app_monitor.c
 * @brief   System Monitor 应用：实时监控 RTOS 运行状态
 *
 * 内容（全部来自 FreeRTOS 内核 API，真实数据）：
 *   - 堆剩余：xPortGetFreeHeapSize()（字节）
 *   - 任务列表：uxTaskGetSystemState() 枚举全部任务，每行显示
 *     任务名 + 栈剩余高水位（usStackHighWaterMark，字×4=字节，任务
 *     栈最紧张时还剩多少，判断栈溢出风险）
 *
 * 刷新：复用 EV_TICK（1 秒心跳）——应用不需要自己的定时器，
 *       单写者原则保持（所有绘制仍在 ui_task 中执行）
 */
#include "app.h"
#include "app_monitor.h"
#include "cursor.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "FreeRTOS.h"
#include "task.h"

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* 局部重绘保护（与桌面框架同协议）：Monitor 不显示光标，
 * cursor_overlap 返回 0，保护零开销 */
static uint8_t redraw_protect_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (cursor_overlap(x0, y0, x1, y1)) {
        cursor_hide();
        return 1;
    }
    return 0;
}

static void redraw_protect_end(uint8_t hidden)
{
    if (hidden) cursor_show();
}

/* 刷新监控内容区（y≥28：标题栏以下整块重绘） */
static void draw_content(void)
{
    TaskStatus_t ts[8];
    uint32_t n, heap;
    uint8_t i, hid;
    uint16_t y;

    hid = redraw_protect_begin(0, 28, SCR_W - 1, SCR_H - 1);
    atk_md0280_fill(0, 28, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);

    /* 第 1 行：堆剩余 */
    heap = xPortGetFreeHeapSize();
    atk_md0280_show_string(8, 32, 48, 16, (char *)"Heap", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(60, 32, heap, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_string(124, 32, 16, 16, (char *)"B", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);

    /* 任务列表：名字 + 栈剩余水位（字节） */
    n = uxTaskGetSystemState(ts, 8, NULL);   /* 系统任务数（空闲/定时器服务 + 应用任务） */
    for (i = 0; i < n && i < 8; i++) {
        y = 54 + i * 20;
        atk_md0280_show_string(8, y, 120, 16, (char *)ts[i].pcTaskName,
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(120, y, 16, 16, (char *)"S", ATK_MD0280_LCD_FONT_16, ATK_MD0280_GRAY);
        atk_md0280_show_xnum(136, y, ts[i].usStackHighWaterMark * 4, 4,
                             ATK_MD0280_NUM_SHOW_NOZERO, ATK_MD0280_LCD_FONT_16, ATK_MD0280_GRAY);
    }

    redraw_protect_end(hid);
}

/* 进入应用：全屏自绘 + 立即刷一次数据（不显示光标：纯监控界面） */
void app_monitor_open(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("System Monitor");
    draw_content();
}

/* 事件分发：只要 EV_TICK 每秒刷新；摇杆/SW 事件忽略 */
void app_monitor_handle(input_event_t *ev)
{
    if (ev->type == EV_TICK) {
        draw_content();
    }
}
