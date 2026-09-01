/**
 * @file    app_logs.c
 * @brief   Logs 日志查看应用：倒序浏览系统运行日志
 *
 * 数据源：sys_log 环形缓冲。展示系统关键事件——Boot OK、RTC 降级/校时、
 * 文件系统故障与自动重建、Paint 保存成败，每条带真实时间戳（HH:MM:SS）。
 * 演示思路：先看开机以来的完整日志，然后做一次 Paint 保存/删一个文件，
 * 再进来看新日志出现 → 日志系统"活着"。
 *
 * 交互：摇杆上下滚动（最新在顶，滚到最旧为止），SW 清空日志，K1 返回。
 * 不显示光标：纯查看界面，箭头没有意义。
 *
 * 刷新：EV_TICK 每秒重绘整列表。日志在任意埋点产生，UI 无法收到事件
 * 通知，只能周期性拉快照——每秒一次足够演示（相比 4K 擦除的秒级操作，
 * 人眼看不出延迟）。
 *
 * 布局：标题栏 0-24，提示行 29-41，列表 44 起，每行 18px 共 15 行
 *       （44 + 15*18 = 314 ≤ 319）。
 *       行内分栏：时间 x=8(56px) | 级别 x=72(30px) | 消息 x=110(100px)
 *       | 参数 x=216(24px)，参数为 0 时不显示。
 */
#include "app.h"
#include "app_logs.h"
#include "sys_log.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

#define LIST_Y0   44
#define ROW_H     18
#define ROWS      15          /* 可视行数：(320-44)/18 = 15.3 */

/* 消息字符串表：与 sys_log.h 的 log_msg_id_t 一一对应（最多 16 字符） */
static const char *const s_msg[LOG_MSG_N] = {
    "Boot OK",              /* LOG_BOOT_OK */
    "RTC LSE->LSI",         /* LOG_RTC_LSE */
    "Clock set",            /* LOG_RTC_SET */
    "LCD init fail",        /* LOG_LCD_FAIL */
    "Login fail",           /* LOG_LOGIN_FAIL */
    "Login locked",         /* LOG_LOGIN_LOCK */
    "FS mount fail",        /* LOG_FS_MOUNT */
    "FS rebuilt",           /* LOG_FS_REBUILD */
    "FS demo upgraded",     /* LOG_FS_UPGRADE */
    "FS write fail",        /* LOG_FS_WRITE */
    "FS delete fail",       /* LOG_FS_DEL */
    "File deleted",         /* LOG_FS_DELETE */
    "FS id exhausted",      /* LOG_FS_FULL */
    "File created",         /* LOG_FILE_CREATED */
    "Sens set",             /* LOG_SET_SENS */
    "Cursor set",           /* LOG_SET_CSIZE */
    "Brightness set",       /* LOG_SET_BRIGHT */
    "Volume set",           /* LOG_SET_VOL */
    "Screen time set",      /* LOG_SET_STIME */
    "Input lost",           /* LOG_INPUT_LOST */
    "Input back",           /* LOG_INPUT_RECOVER */
    "Screen off",           /* LOG_SCREEN_OFF */
    "Screen on",            /* LOG_SCREEN_ON */
    "Paint saved",          /* LOG_PAINT_OK */
    "Paint save fail",      /* LOG_PAINT_FAIL */
    "Config corrupt",       /* LOG_CFG_CORRUPT */
    "Image corrupt",        /* LOG_IMG_CORRUPT */
    "Watchdog reset",       /* LOG_WDG_RESET */
    "Task hung",            /* LOG_TASK_HUNG */
    "Task recovered",       /* LOG_TASK_RECOVER */
    "OTA pkg OK",           /* LOG_OTA_DOWNLOAD */
    "OTA applied",          /* LOG_OTA_APPLIED */
    "OTA rollback",         /* LOG_OTA_ROLLBACK */
};

static uint8_t s_scroll = 0;    /* 滚动偏移：0 = 最新在顶 */

static uint16_t level_color(uint8_t level)
{
    if (level >= LOG_LV_ERR)  return ATK_MD0280_RED;
    if (level == LOG_LV_WARN) return ATK_MD0280_YELLOW;
    return ATK_MD0280_BLUE;
}

/* 画一行：时间(灰) 级别(按级别着色) 消息(黑) 参数(灰) */
static void draw_row(uint16_t y, const log_entry_t *e)
{
    char buf[9];
    char lv[4];

    buf[0] = (char)('0' + e->hour / 10);
    buf[1] = (char)('0' + e->hour % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + e->min / 10);
    buf[4] = (char)('0' + e->min % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + e->sec / 10);
    buf[7] = (char)('0' + e->sec % 10);
    buf[8] = 0;
    atk_md0280_show_string(8, y, 56, 12, buf, ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    if (e->level >= LOG_LV_ERR)  { lv[0]='E'; lv[1]='R'; lv[2]='R'; }
    else if (e->level == LOG_LV_WARN) { lv[0]='W'; lv[1]='R'; lv[2]='N'; }
    else { lv[0]='I'; lv[1]='N'; lv[2]='F'; }
    lv[3] = 0;
    atk_md0280_show_string(72, y, 30, 12, lv, ATK_MD0280_LCD_FONT_12, level_color(e->level));

    atk_md0280_show_string(110, y, 100, 12, (char *)s_msg[e->msg_id],
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_BLACK);
    if (e->param != 0)
        atk_md0280_show_xnum(216, y, e->param, 3, ATK_MD0280_NUM_SHOW_NOZERO,
                             ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
}

/* 画列表区：最新在顶，从 s_scroll 开始向下排 */
static void draw_list(void)
{
    uint8_t n = sys_log_count();
    uint8_t i, vis;
    uint16_t y;

    atk_md0280_fill(0, LIST_Y0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    if (n == 0) {
        atk_md0280_show_string(100, 150, 120, 12, (char *)"No logs",
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
        return;
    }
    vis = (n < ROWS) ? n : ROWS;
    for (i = 0; i < vis; i++) {
        y = (uint16_t)(LIST_Y0 + i * ROW_H);
        draw_row(y, sys_log_get((uint8_t)(s_scroll + i)));
    }
}

void app_logs_open(void)
{
    s_scroll = 0;
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("Logs");
    atk_md0280_show_string(8, 29, 180, 12, (char *)"SW:Clear  Up/Down:Scroll",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    draw_list();
}

void app_logs_handle(input_event_t *ev)
{
    if (ev->type == EV_MOUSE_MOVE) {
        uint8_t n = sys_log_count();
        uint8_t max_scroll = (n > ROWS) ? (uint8_t)(n - ROWS) : 0;

        if (ev->dy > 0 && s_scroll < max_scroll) { s_scroll++; draw_list(); }
        if (ev->dy < 0 && s_scroll > 0)          { s_scroll--; draw_list(); }
    } else if (ev->type == EV_KEY_DOWN) {
        /* SW：清空日志。演示"清空后再操作产生新日志" */
        sys_log_clear();
        s_scroll = 0;
        draw_list();
    } else if (ev->type == EV_TICK) {
        draw_list();          /* 每秒拉一次快照：其他埋点可能刚写了新日志 */
    }
}
