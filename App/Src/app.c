/**
 * @file    app.c
 * @brief   应用注册表实现：桌面框架与应用之间的"中间层"
 *
 * 概念：注册表 = 应用信息表。桌面代码不认识 Files/Paint/... 具体是什么，
 *       只认"表里的第几项"：查表 → 调 open() 让它出现 → 把事件转发给它的
 *       handle()。应用自己管自己（画自己的界面、处理自己的事件）。
 *
 * 当前状态：Monitor 是第一个真实实现（验证"打开→操作→返回"链路），
 *           其余应用暂为占位 stub（Coming Soon），逐个实现后替换
 *           注册表对应行即可，框架代码零改动。
 */
#include "app.h"
#include "app_monitor.h"
#include "cursor.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* ---------- 占位应用：未实现的功能先显示 Coming Soon ---------- */
static void app_stub_handle(input_event_t *ev)
{
    (void)ev;   /* 占位：不响应任何事件 */
}

static void app_stub_open(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("MINI OS");
    atk_md0280_show_string((SCR_W - 11 * 12) / 2, 150, 240, 24,
                           (char *)"Coming Soon", ATK_MD0280_LCD_FONT_24, ATK_MD0280_GRAY);
    atk_md0280_show_string((SCR_W - 20 * 8) / 2, 196, 240, 16,
                           (char *)"Press K1 to go back", ATK_MD0280_LCD_FONT_16, ATK_MD0280_GRAY);
    cursor_init(120, 250);      /* 屏内安全位：外框 (117,247)~(171,306) 不越界 */
    cursor_show();
}

/* ---------- 应用注册表 ----------
 * 颜色：图标色块（RGB565）；桌面图标/hover 遍历此表绘制 */
const app_t g_apps[] = {
    {"Files",    ATK_MD0280_BLUE,     app_stub_open,    app_stub_handle},
    {"Paint",    ATK_MD0280_GREEN,    app_stub_open,    app_stub_handle},
    {"Music",    ATK_MD0280_MAGENTA,  app_stub_open,    app_stub_handle},
    {"Settings", ATK_MD0280_YELLOW,   app_stub_open,    app_stub_handle},
    {"Logs",     ATK_MD0280_CYAN,     app_stub_open,    app_stub_handle},
    {"Monitor",  ATK_MD0280_GRAY,     app_monitor_open, app_monitor_handle},
};

const uint8_t g_app_count = sizeof(g_apps) / sizeof(g_apps[0]);

/* ---------- 应用公共标题栏 ----------
 * 蓝条：左侧应用名，右侧 "K1:Back" 返回提示（返回入口永远可见） */
void app_draw_title(const char *name)
{
    atk_md0280_fill(0, 0, SCR_W - 1, 24, ATK_MD0280_BLUE);
    atk_md0280_show_string(8, 5, 150, 16, (char *)name,
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
    atk_md0280_show_string(160, 5, 80, 16, (char *)"K1:Back",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
}
