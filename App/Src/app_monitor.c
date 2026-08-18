/**
 * @file    app_monitor.c
 * @brief   System Monitor 应用：实时监控 RTOS 运行状态
 *
 * 内容（全部来自 FreeRTOS 内核 API / 统计埋点，真实数据）：
 *   - Up：系统运行时间（xTaskGetTickCount，1kHz tick → 时:分:秒）
 *   - Heap：堆剩余（xPortGetFreeHeapSize）
 *   - Ev/Dr/Er：输入事件总数 / 队列满丢弃 / 系统错误累计（sys_stats 埋点）
 *   - 任务列表：名字 + 状态 + 栈剩余高水位（usStackHighWaterMark×4=字节）。
 *     状态字母沿用 vTaskList 惯例：X=运行 R=就绪 B=阻塞 S=挂起 D=删除
 *
 * 刷新：复用 EV_TICK（1 秒心跳）——应用不需要自己的定时器，
 *       单写者原则保持（所有绘制仍在 ui_task 中执行）
 */
#include "app.h"
#include "app_monitor.h"
#include "cursor.h"
#include "sys_stats.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "FreeRTOS.h"
#include "task.h"

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* 局部重绘保护（与桌面框架同协议） */
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

/* 任务状态 → 单字符（vTaskList 输出惯例） */
static char task_state_char(eTaskState st)
{
    switch (st) {
    case eRunning:   return 'X';   /* 正在执行 */
    case eReady:     return 'R';   /* 就绪，等待调度 */
    case eBlocked:   return 'B';   /* 阻塞（等事件/延时，不耗 CPU） */
    case eSuspended: return 'S';   /* 挂起 */
    case eDeleted:   return 'D';   /* 已删除 */
    default:         return '?';
    }
}

/* 运行时间行：手工拼 "H:MM:SS" 字符串，避开 sprintf 重库。
 * tick 1kHz → 秒 = tick/1000；uint32 计数约 49.7 天后回绕（演示期无碍） */
static void draw_uptime(uint16_t y)
{
    uint32_t sec = xTaskGetTickCount() / 1000;
    uint16_t h = sec / 3600;
    uint16_t m = (sec % 3600) / 60;
    uint16_t s = sec % 60;
    char buf[12];
    uint8_t i = 0;

    if (h >= 1000) buf[i++] = '0' + (h / 1000) % 10;
    if (h >= 100)  buf[i++] = '0' + (h / 100) % 10;
    if (h >= 10)   buf[i++] = '0' + (h / 10) % 10;
    buf[i++] = '0' + h % 10;
    buf[i++] = ':';
    buf[i++] = '0' + m / 10;
    buf[i++] = '0' + m % 10;
    buf[i++] = ':';
    buf[i++] = '0' + s / 10;
    buf[i++] = '0' + s % 10;
    buf[i] = '\0';

    atk_md0280_show_string(8, y, 28, 16, (char *)"Up", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_string(32, y, 120, 16, buf, ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
}

/* 刷新监控内容区（y≥28：标题栏以下整块重绘） */
static void draw_content(void)
{
    static TaskStatus_t ts[8];   /* 静态区：TaskStatus_t 约 40B×8=320B，
                                  * ui_task 栈只有 1KB，放栈上会压垮调用链 */
    uint32_t n, heap;
    uint8_t i, hid;
    uint16_t y;

    hid = redraw_protect_begin(0, 28, SCR_W - 1, SCR_H - 1);
    atk_md0280_fill(0, 28, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);

    /* 运行时间 */
    draw_uptime(30);

    /* 堆剩余 */
    heap = xPortGetFreeHeapSize();
    atk_md0280_show_string(8, 46, 48, 16, (char *)"Heap", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(60, 46, heap, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_string(124, 46, 16, 16, (char *)"B", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);

    /* 事件/丢弃/错误计数 */
    atk_md0280_show_string(8, 62, 24, 16, (char *)"Ev", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(32, 62, g_stats_events, 7, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_string(92, 62, 24, 16, (char *)"Dr", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(116, 62, g_stats_drops, 4, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_string(152, 62, 24, 16, (char *)"Er", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(176, 62, g_stats_errors, 4, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);

    /* 任务列表列头 */
    atk_md0280_show_string(8, 82, 40, 12, (char *)"Task", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    atk_md0280_show_string(108, 83, 20, 12, (char *)"St", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    atk_md0280_show_string(136, 83, 70, 12, (char *)"Stack(B)", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    /* 任务行：名字 + 状态 + 栈剩余水位（字节） */
    n = uxTaskGetSystemState(ts, 8, NULL);   /* 系统任务数（空闲/定时器服务 + 应用任务） */
    for (i = 0; i < n && i < 8; i++) {
        char stc = task_state_char(ts[i].eCurrentState);

        y = 100 + i * 20;
        atk_md0280_show_string(8, y, 100, 16, (char *)ts[i].pcTaskName,
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_char(108, y, stc, ATK_MD0280_LCD_FONT_16,
                             (stc == 'X') ? ATK_MD0280_BLUE : ATK_MD0280_BLACK);
        atk_md0280_show_xnum(136, y, ts[i].usStackHighWaterMark * 4, 4,
                             ATK_MD0280_NUM_SHOW_NOZERO, ATK_MD0280_LCD_FONT_16, ATK_MD0280_GRAY);
    }

    redraw_protect_end(hid);
}

/* 进入应用：全屏自绘 + 立即刷一次数据。
 * 光标显示在内容区下方空白处（可自由移动，监控界面不参与交互） */
void app_monitor_open(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("System Monitor");
    draw_content();
    cursor_init(120, 260);      /* 屏内安全位：外框 (117,257)~(171,316) 不越界 */
    cursor_show();
}

/* 事件分发：只要 EV_TICK 每秒刷新；摇杆/SW 事件忽略 */
void app_monitor_handle(input_event_t *ev)
{
    if (ev->type == EV_TICK) {
        draw_content();
    }
}
