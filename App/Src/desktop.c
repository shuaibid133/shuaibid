/**
 * @file    desktop.c
 * @brief   桌面框架：上电启动界面 → 密码登录 → 桌面主界面
 *
 * 架构：由 ui_task 逐事件调用（ui_task 只做事件分发，本文件是桌面框架）
 *   - 状态机：UI_BOOT → UI_CLOCK_SET（RTC 失效时）→ UI_LOGIN → UI_DESKTOP
 *     （桌面态内嵌应用前台态：s_cur_app 非空时事件全部转发给注册表中的应用，
 *     K1 返回桌面；UI_PWD_SET 改密码由 Settings 进入，完成后回桌面）
 *   - 校时界面：精英板 VBAT 无电池，断电 RTC 丢失——每次开机自动进
 *     Set Clock 输入真实时间（复用登录数字键盘），K1 可跳过
 *   - 密码 4 位（默认 1234，Settings 可改并落盘 Flash），错误提示，
 *     连续错误进入锁定（倒计时，输入全部无效）
 *   - 数字键盘交互：摇杆移光标选格（蓝框高亮），SW 按下输入
 *   - K1 逐级回退：应用 → 桌面（锁屏） → 登录页
 *   - 桌面图标来自应用注册表（app.c）：hover 白框高亮，SW 打开
 *
 * 关键点：
 *   - 屏幕 240×320 竖屏（ATK-MD0280 默认方向）
 *   - 界面切换 = 全屏重绘；动态变化（高亮/圆点/提示）= 局部重绘，避免卡顿
 *   - 锁定倒计时用 osDelay 阻塞实现（UI 任务独占渲染，锁定期只画倒计时）
 *   - 切换界面时 xQueueReset 清掉积压事件，防止旧事件污染新界面
 */
#include "desktop.h"
#include "app.h"
#include "main.h"
#include <string.h>
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "event.h"
#include "joystick.h"
#include "rtc_app.h"
#include "sys_stats.h"
#include "sys_backlight.h"
#include "app_config.h"
#include "sys_cfg.h"
#include "cmsis_os.h"
#include "sys_log.h"

/* ---------- 登录参数 ---------- */
/* 密码本体在 g_sys_cfg.pwd：默认 "1234"（sys_cfg 出厂默认值），Settings 应用
 * 可改，改完立即落盘 Flash、重启保留。登录/锁屏验证读它，这里只剩防锁参数 */
#define LOGIN_MAX_FAIL      3        /* 连续错误 N 次锁定 */
#define LOGIN_LOCK_S        30       /* 锁定秒数 */

/* ---------- 屏幕几何（240×320 竖屏） ---------- */
#define SCR_W               ATK_MD0280_LCD_WIDTH
#define SCR_H               ATK_MD0280_LCD_HEIGHT

/* 键盘区：3 列 × 4 行，格子 60×45 */
#define KEY_COLS            3
#define KEY_ROWS            4
#define KEY_W               60
#define KEY_H               45
#define KEY_X0              30
#define KEY_Y0              120

/* 桌面图标区：3 列 × 2 行，圆角色块 68×60 + 白符号（名字在色块下方） */
#define ICON_COLS           3
#define ICON_W              68
#define ICON_H              60
#define ICON_X0             18
#define ICON_Y0             60
#define ICON_GAP_X          73
#define ICON_GAP_Y          80
#define ICON_RAD            8        /* 图标圆角半径（扁平化圆角色块） */

/* ---------- 颜色（RGB565） ---------- */
#define CLR_KEY_HOV_BG      0x7DFC    /* 高亮格浅蓝底 */
#define CLR_ERR             ATK_MD0280_RED
/* 浅蓝科技风：主体底色（桌面/登录/校时共用）。凡是"擦成背景色"的地方
 * 都用 CLR_BG——若用纯白擦，图标/键盘重绘时会在淡蓝底上留白块洞 */
#define CLR_BG              0xC73F    /* 桌面主体底色（淡蓝，图标区） */
#define CLR_LOGIN_BG        0x9E3D    /* 登录/校时底色：比桌面深一档的蓝（键盘白卡更突出） */
#define CLR_WATERMARK       0x855B    /* 底部水印灰蓝 */
#define CLR_HINT            0x4208    /* 次要提示灰字（淡蓝底上 0x7BEF 会隐形，用深灰） */

/* ---------- 状态机 ---------- */
typedef enum {
    UI_BOOT,        /* 启动界面（停留 2 秒） */
    UI_CLOCK_SET,   /* 校时界面（RTC 失效时自动进入，输入真实时间） */
    UI_LOGIN,       /* 密码登录 */
    UI_PWD_SET,     /* 改密码（Settings 进入：输两遍，一致才生效） */
    UI_DESKTOP,     /* 桌面主界面 */
} ui_state_t;

static ui_state_t s_state = UI_BOOT;
static char  s_pwd[4];          /* 已输入密码（登录/改密码共用输入缓冲） */
static uint8_t s_pwd_len = 0;
static uint8_t s_fail_cnt = 0;  /* 连续错误计数 */
static uint8_t s_pwd_phase = 0; /* 改密码阶段：0=输新密码，1=再输一遍确认 */
static char  s_pwd_verify[4];   /* 改密码第一遍暂存（第二遍逐位比对） */
static int8_t  s_hover = 4;     /* 键盘高亮格索引 0~11 */
static const app_t *s_cur_app = NULL;  /* 非空 = 应用前台态（注册表指针） */
static int8_t  s_hover_icon = -1;      /* 桌面 hover 图标索引（-1 = 无） */
static uint8_t s_clock_from_app = 0;   /* 框架内跳转界面来源：1=应用（Settings）
                                        * 进入（校时/改密码），完成后回桌面；
                                        * 0=开机自动进入校时，完成后回登录 */
static uint8_t  s_sleeping = 0;        /* 熄屏态：背光灭，任一输入事件唤醒 */
static uint16_t s_idle_sec = 0;        /* 空闲累计秒数（EV_TICK 递增，输入重置） */

/* 校时输入：8 位数字 = MM-DD HH:MM（月/日/时/分，年取编译年份） */
static char s_clk[8];           /* 已输入数字字符 */
static uint8_t s_clk_len = 0;

/* 键盘内容：'*' = 占位空格，'<' = 退格 */
static const char s_key[KEY_ROWS][KEY_COLS] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','<'},
};

/* 桌面图标：来自应用注册表 g_apps（app.c）——框架不重复维护图标数组，
 * 图标绘制/命中/打开全部遍历注册表，加应用 = 表里加一行 */
/* 注意：atk_md0280 库的 show_string 只支持 ASCII（见库源码），界面文字统一用英文 */

/* ---------- 局部绘制 ---------- */

/* 局部重绘保护：重绘矩形与光标外框相交时，先隐藏光标（写回已存背景，光标从
 * 屏幕消失），重绘完成后再画回。否则 fill 只盖住光标一部分，"截断光标"会被
 * cursor_show 当背景存进缓冲，下次移动恢复时固化成残影（数字框边的幽灵）
 * 返回 1 表示隐藏过（结束时要画回）；不相交则原样直画，零开销 */
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

/* 画一个键盘格：idx=0~11；hover=1 高亮（浅蓝底+蓝框），0 普通（白底+灰框） */
static void draw_key_cell(uint8_t idx, uint8_t hover)
{
    uint8_t row = idx / KEY_COLS, col = idx % KEY_COLS;
    uint16_t x0 = KEY_X0 + col * KEY_W;
    uint16_t y0 = KEY_Y0 + row * KEY_H;
    char c;

    atk_md0280_fill(x0, y0, x0 + KEY_W - 1, y0 + KEY_H - 1,
                    hover ? CLR_KEY_HOV_BG : ATK_MD0280_WHITE);
    atk_md0280_draw_rect(x0, y0, x0 + KEY_W - 1, y0 + KEY_H - 1,
                         hover ? ATK_MD0280_BLUE : ATK_MD0280_GRAY);

    c = s_key[row][col];
    if (c == '*') {
        /* 占位格：画个灰色小方块装饰 */
        atk_md0280_fill(x0 + KEY_W / 2 - 3, y0 + KEY_H / 2 - 3,
                        x0 + KEY_W / 2 + 3, y0 + KEY_H / 2 + 3, ATK_MD0280_GRAY);
    } else if (c == '<') {
        /* 退格：12 号 "<" */
        atk_md0280_show_string(x0 + (KEY_W - 6) / 2, y0 + (KEY_H - 12) / 2,
                               60, 12, (char *)"<", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    } else {
        /* 数字：24 号黑色，格内居中 */
        atk_md0280_show_char(x0 + (KEY_W - 12) / 2, y0 + (KEY_H - 24) / 2,
                             c, ATK_MD0280_LCD_FONT_24, ATK_MD0280_BLACK);
    }
}

/* ---------- 图标绘制工具（桌面扁平化图标用） ---------- */

/* 实心圆（Bresenham 行扫描法，纯整数运算，每行一次 fill）。
 * 画整圆：y 从 0..r 求每行半宽 s，对称画上下两行 */
static void fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint16_t color)
{
    int16_t y;
    int16_t s = (int16_t)r;
    uint32_t rr = (uint32_t)r * r;

    for (y = 0; y <= (int16_t)r; y++) {
        while ((int32_t)s * s + (int32_t)y * y > (int32_t)rr) s--;
        atk_md0280_fill(cx - s, cy - y, cx + s, cy - y, color);
        if (y) atk_md0280_fill(cx - s, cy + y, cx + s, cy + y, color);
    }
}

/* 实心圆角矩形：中间横带 + 上下两条 + 四角补实心圆，无缝隙 */
static void fill_round_rect(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                            uint16_t r, uint16_t color)
{
    atk_md0280_fill(x0, y0 + r, x1, y1 - r, color);         /* 中间横带 */
    atk_md0280_fill(x0 + r, y0, x1 - r, y0 + r - 1, color); /* 上带 */
    atk_md0280_fill(x0 + r, y1 - r + 1, x1 - r, y1, color); /* 下带 */
    fill_circle(x0 + r, y0 + r, r, color);                  /* 四个角 */
    fill_circle(x1 - r, y0 + r, r, color);
    fill_circle(x0 + r, y1 - r, r, color);
    fill_circle(x1 - r, y1 - r, r, color);
}

/* ---------- 图标符号（扁平化：彩色圆角底 + 白色图形符号） ----------
 * 每个应用一个符号函数，绘制范围限定在色块内（x..x+ICON_W, y..y+ICON_H）。
 * 图形比例对标 Material/Feather 图标语言（2~4px 笔画、块内居中）。
 * 只在此处画符号，不涉及坐标/命中/hover 逻辑。 */

/* Files：文件夹（柄 + 主体） */
static void sym_files(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 14, y + 13, x + 26, y + 15, ATK_MD0280_WHITE);   /* 柄 */
    atk_md0280_fill(x + 11, y + 16, x + 57, y + 45, ATK_MD0280_WHITE);   /* 主体 */
}

/* Paint：画刷（柄 + 金属箍 + 宽刷头 + 刷毛尖） */
static void sym_paint(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 31, y + 14, x + 37, y + 32, ATK_MD0280_WHITE);   /* 柄 */
    atk_md0280_fill(x + 28, y + 32, x + 40, y + 36, ATK_MD0280_WHITE);   /* 箍 */
    atk_md0280_fill(x + 20, y + 36, x + 46, y + 42, ATK_MD0280_WHITE);   /* 宽刷头 */
    atk_md0280_fill(x + 23, y + 42, x + 25, y + 45, ATK_MD0280_WHITE);   /* 刷毛尖 */
    atk_md0280_fill(x + 31, y + 42, x + 33, y + 45, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 39, y + 42, x + 41, y + 45, ATK_MD0280_WHITE);
}

/* Music：双八分音符（双杆 + 双横梁 + 两个符头圆点） */
static void sym_music(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 23, y + 10, x + 25, y + 36, ATK_MD0280_WHITE);   /* 左杆 */
    atk_md0280_fill(x + 41, y + 10, x + 43, y + 36, ATK_MD0280_WHITE);   /* 右杆 */
    atk_md0280_fill(x + 23, y + 10, x + 43, y + 12, ATK_MD0280_WHITE);   /* 双横梁 */
    atk_md0280_fill(x + 23, y + 14, x + 43, y + 16, ATK_MD0280_WHITE);
    fill_circle(x + 24, y + 39, 4, ATK_MD0280_WHITE);                    /* 符头 */
    fill_circle(x + 42, y + 39, 4, ATK_MD0280_WHITE);
}

/* Settings：三档调节滑块（黄底用黑色符号，白/黄会看不清） */
static void sym_settings(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 6, y + 12, x + 60, y + 15, ATK_MD0280_BLACK);    /* 滑轨 */
    fill_circle(x + 46, y + 13, 6, ATK_MD0280_BLACK);                    /* 圆钮 */
    atk_md0280_fill(x + 6, y + 26, x + 48, y + 29, ATK_MD0280_BLACK);
    fill_circle(x + 20, y + 27, 6, ATK_MD0280_BLACK);
    atk_md0280_fill(x + 6, y + 40, x + 56, y + 43, ATK_MD0280_BLACK);
    fill_circle(x + 36, y + 41, 6, ATK_MD0280_BLACK);
}

/* Logs：要点列表（圆点 + 长短不一的横线，模拟日志行） */
static void sym_logs(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 8, y + 10, x + 10, y + 12, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 14, y + 10, x + 52, y + 12, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 8, y + 22, x + 10, y + 24, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 14, y + 22, x + 36, y + 24, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 8, y + 34, x + 10, y + 36, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 14, y + 34, x + 56, y + 36, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 8, y + 46, x + 10, y + 48, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 14, y + 46, x + 30, y + 48, ATK_MD0280_WHITE);
}

/* Monitor：柱状图（基线 + 三根高度递增的柱） */
static void sym_monitor(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 4, y + 44, x + 60, y + 46, ATK_MD0280_WHITE);    /* 基线 */
    atk_md0280_fill(x + 10, y + 30, x + 20, y + 44, ATK_MD0280_WHITE);   /* 三根柱 */
    atk_md0280_fill(x + 26, y + 20, x + 36, y + 44, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 42, y + 10, x + 52, y + 44, ATK_MD0280_WHITE);
}

/* OTA：下载（竖杆 + 箭头三角 + 底部托盘线） */
static void sym_ota(uint16_t x, uint16_t y)
{
    atk_md0280_fill(x + 31, y + 12, x + 35, y + 28, ATK_MD0280_WHITE);   /* 箭杆 */
    atk_md0280_fill(x + 22, y + 32, x + 46, y + 35, ATK_MD0280_WHITE);   /* 箭头三角 */
    atk_md0280_fill(x + 28, y + 28, x + 40, y + 32, ATK_MD0280_WHITE);
    atk_md0280_fill(x + 6, y + 42, x + 58, y + 44, ATK_MD0280_WHITE);    /* 托盘 */
}

/* 按注册表名字分发到符号函数（不依赖注册表顺序，加应用只需补分支） */
static void draw_icon_sym(uint8_t idx, uint16_t x, uint16_t y)
{
    const char *n = g_apps[idx].name;

    if (strcmp(n, "Files") == 0) {
        sym_files(x, y);
    } else if (strcmp(n, "Paint") == 0) {
        sym_paint(x, y);
    } else if (strcmp(n, "Music") == 0) {
        sym_music(x, y);
    } else if (strcmp(n, "Settings") == 0) {
        sym_settings(x, y);
    } else if (strcmp(n, "Logs") == 0) {
        sym_logs(x, y);
    } else if (strcmp(n, "Monitor") == 0) {
        sym_monitor(x, y);
    } else if (strcmp(n, "OTA") == 0) {
        sym_ota(x, y);
    }
}

/* 桌面图标：彩色圆角底 + 白符号 + 底部名字。
 * hover = 图标外圈白框衬灰描边（白框在白底上看不见，灰描边让选中感浮现）；
 * 普通 = 色块外 1px 灰环。
 * 擦除区随之扩到 x-3/y-3：hover 灰描边切回普通时不残留 */
/* 565 三分量各减半 = 色块变暗（hover 反馈）。不做外框/白线高亮：
 * 外框越出色块 1~3px，会切掉上一行图标的文字底边（行距 80 恰好只够
 * 放下文字 y+64..y+80），且直角框贴圆角色块视觉脏。
 * 变暗 = 整块重画、永不越界。 */
#define CLR_DARKEN(c)  ((((c) & 0xF800) >> 1) | (((c) & 0x07E0) >> 1) | (((c) & 0x001F) >> 1))

static void draw_icon(uint8_t idx, uint8_t hover)
{
    uint16_t x = ICON_X0 + (idx % ICON_COLS) * ICON_GAP_X;
    uint16_t y = ICON_Y0 + (idx / ICON_COLS) * ICON_GAP_Y;

    /* 整块重画，范围严格限在色块界内（不碰上行文字/邻格）：
     * hover 暗色块 ↔ 普通原色块，圆角色块"全盖"绘制无残留，无需预擦 */
    fill_round_rect(x, y, x + ICON_W, y + ICON_H, ICON_RAD,
                    hover ? CLR_DARKEN(g_apps[idx].color) : g_apps[idx].color);
    draw_icon_sym(idx, x, y);
    /* 名字在色块下方，块宽内居中（16 号，字宽 8px） */
    atk_md0280_show_string(x + (ICON_W - strlen(g_apps[idx].name) * 8) / 2,
                           y + ICON_H + 4, ICON_W, 16,
                           (char *)g_apps[idx].name,
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
}

/* 密码框 4 个圆点：已输入 = 蓝色实心，未输入 = 灰色空心 */
static void draw_pwd_dots(void)
{
    uint8_t i, hid;
    uint16_t x;

    hid = redraw_protect_begin(60, 52, 60 + 3 * 30 + 6, 58);   /* 圆点区域 */
    for (i = 0; i < 4; i++) {
        x = 60 + i * 30;
        if (i < s_pwd_len) {
            atk_md0280_fill(x, 52, x + 6, 58, ATK_MD0280_BLUE);
        } else {
            /* 先整块擦白再画 1px 灰框：直接画框的话，之前"蓝色实心"
             * 的中间 5×5 会残留成蓝块（擦除不彻底，容易误看成还有输入） */
            atk_md0280_fill(x, 52, x + 6, 58, CLR_LOGIN_BG);
            atk_md0280_draw_rect(x, 52, x + 6, 58, ATK_MD0280_GRAY);
        }
    }
    redraw_protect_end(hid);
}

/* 提示行（y=21~47 整行重绘：上界 21 避让顶部 JS 状态，47 避让 y=52 起的密码圆点）。
 * 文字按 16 号字宽 8px 动态居中，避免长文案（Joystick OFF - Press K0）溢出 */
static void draw_hint(const char *str, uint16_t color)
{
    uint8_t hid = redraw_protect_begin(0, 21, SCR_W - 1, 47);
    uint16_t x = (SCR_W - strlen(str) * 8) / 2;

    atk_md0280_fill(0, 21, SCR_W - 1, 47, CLR_LOGIN_BG);  /* 先擦后写 */
    atk_md0280_show_string(x, 24, 240, 16, (char *)str, ATK_MD0280_LCD_FONT_16, color);
    redraw_protect_end(hid);
}

/* 锁定提示：显示剩余秒数，每秒刷新 */
static void draw_lock_hint(uint8_t sec)
{
    uint8_t hid = redraw_protect_begin(0, 21, SCR_W - 1, 47);

    atk_md0280_fill(0, 21, SCR_W - 1, 47, CLR_LOGIN_BG);
    atk_md0280_show_string(40, 24, 100, 16, (char *)"Locked ", ATK_MD0280_LCD_FONT_16, CLR_ERR);
    atk_md0280_show_xnum(104, 24, sec, 2, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, CLR_ERR);
    atk_md0280_show_string(128, 24, 40, 16, (char *)"s", ATK_MD0280_LCD_FONT_16, CLR_ERR);
    redraw_protect_end(hid);
}

/* 输入设备状态：LOGIN 白底 / DESKTOP 蓝条，同一位置（x 100 起，y 4~20）。
 * "JS:ON" 绿 / "JS:OFF" 红——设备未连接时红色常驻，恢复连接变绿 */
static void draw_js_status(void)
{
    uint16_t bg = (s_state == UI_DESKTOP) ? ATK_MD0280_BLUE : CLR_LOGIN_BG;
    uint8_t hid = redraw_protect_begin(100, 4, 164, 20);

    atk_md0280_fill(100, 4, 164, 20, bg);
    atk_md0280_show_string(100, 5, 56, 16,
                           (char *)(g_js_on ? "JS:ON " : "JS:OFF"),
                           ATK_MD0280_LCD_FONT_16,
                           g_js_on ? ATK_MD0280_GREEN : CLR_ERR);
    redraw_protect_end(hid);
}

/* 状态栏时间（DESKTOP 蓝条右侧，y 4~20）：EV_TICK 每秒重画。
 * 手工拼 "HH:MM" 字符串，避免引入 sprintf 重库 */
static void draw_clock(void)
{
    uint8_t h, m;
    char buf[6];
    uint8_t hid;

    rtc_app_get_time(&h, &m);
    buf[0] = '0' + h / 10;
    buf[1] = '0' + h % 10;
    buf[2] = ':';
    buf[3] = '0' + m / 10;
    buf[4] = '0' + m % 10;
    buf[5] = '\0';

    hid = redraw_protect_begin(192, 4, 231, 20);
    atk_md0280_fill(192, 4, 231, 20, ATK_MD0280_BLUE);
    atk_md0280_show_string(192, 5, 40, 16, buf, ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
    redraw_protect_end(hid);
}

/* 光标像素坐标 → 键盘格索引；不在键盘区返回 0xFF */
static uint8_t hit_key(uint16_t x, uint16_t y)
{
    uint8_t row, col;

    if (x < KEY_X0 || x >= KEY_X0 + KEY_COLS * KEY_W) return 0xFF;
    if (y < KEY_Y0 || y >= KEY_Y0 + KEY_ROWS * KEY_H) return 0xFF;
    col = (x - KEY_X0) / KEY_W;
    row = (y - KEY_Y0) / KEY_H;
    return row * KEY_COLS + col;
}

/* 光标坐标 → 注册表图标索引；不在任何图标色块内返回 0xFF */
static uint8_t hit_icon(uint16_t x, uint16_t y)
{
    uint8_t i;
    uint16_t ix, iy;

    for (i = 0; i < g_app_count; i++) {
        ix = ICON_X0 + (i % ICON_COLS) * ICON_GAP_X;
        iy = ICON_Y0 + (i / ICON_COLS) * ICON_GAP_Y;
        if (x >= ix && x <= ix + ICON_W && y >= iy && y <= iy + ICON_H) return i;
    }
    return 0xFF;
}

/* ---------- 界面 ---------- */

/* 版本号格式化：V%d.0（1-99，无 sprintf 重库手动拼） */
static void fmt_ver(char *buf, uint8_t v)
{
    uint8_t i = 0;

    buf[i++] = 'V';
    if (v >= 10) buf[i++] = (char)('0' + v / 10);
    buf[i++] = (char)('0' + v % 10);
    buf[i++] = '.';
    buf[i++] = '0';
    buf[i] = 0;
}

/* 进度条推进（boot 阶段每档调一次）：条形单调增长 + 下方居中百分比。
 * 芯区 x 62..176（满 115px），文本带 y 208..220 先清后写；
 * 纯演示节奏（desktop_init 以 200ms/档推进），不挂钩真实初始化 */
static void boot_progress(uint8_t pct)
{
    uint8_t x1, nd;
    uint16_t tx;

    if (pct > 100) pct = 100;
    x1 = (uint8_t)(61 + 115u * pct / 100u);
    if (x1 >= 62) atk_md0280_fill(62, 197, x1, 203, ATK_MD0280_WHITE);

    /* "N%" / "NN%" / "100%"：数字 xnum 左对齐 + % 字符紧随，按位数定起始 x */
    nd = (pct >= 100) ? 3 : (pct >= 10 ? 2 : 1);
    tx = (uint16_t)((SCR_W - (nd + 1) * 6) / 2);
    atk_md0280_fill(96, 208, 144, 220, ATK_MD0280_BLUE);   /* 清文本带 */
    atk_md0280_show_xnum(tx, 208, pct, 3, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_12, ATK_MD0280_WHITE);
    atk_md0280_show_char((uint16_t)(tx + nd * 6), 208, '%',
                         ATK_MD0280_LCD_FONT_12, ATK_MD0280_WHITE);
}

/* BOOT：蓝底 + 四窗格 logo + 项目名 + 进度条 + 系统版本
 * （OTA 升级后重启即显示新版本——"升级生效"的第一个可见证据，
 * 与 OTA 应用里的 Current 呼应） */
static void draw_boot(void)
{
    char ver[8];

    fmt_ver(ver, g_sys_cfg.version);
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_BLUE);

    /* 品牌 logo：四窗格（窗口意象，呼应桌面），每格一个 4px 方点 */
    atk_md0280_draw_rect(96, 68, 144, 116, ATK_MD0280_WHITE);
    atk_md0280_fill(96, 90, 144, 94, ATK_MD0280_WHITE);    /* 横分隔 */
    atk_md0280_fill(118, 68, 122, 116, ATK_MD0280_WHITE);  /* 竖分隔 */
    atk_md0280_fill(104, 76, 108, 80, ATK_MD0280_WHITE);   /* 四格点 */
    atk_md0280_fill(132, 76, 136, 80, ATK_MD0280_WHITE);
    atk_md0280_fill(104, 102, 108, 106, ATK_MD0280_WHITE);
    atk_md0280_fill(132, 102, 136, 106, ATK_MD0280_WHITE);

    atk_md0280_show_string((SCR_W - 7 * 16) / 2, 140, 160, 32, (char *)"KazepOS",
                           ATK_MD0280_LCD_FONT_32, ATK_MD0280_WHITE);

    /* 进度条外框（芯由 boot_progress 推进） */
    atk_md0280_draw_rect(60, 196, 180, 204, ATK_MD0280_WHITE);
    boot_progress(0);
    atk_md0280_show_string((SCR_W - 4 * 6) / 2, 228, 60, 12, ver,
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_WHITE);
}

/* ---------- 校时界面（Set Clock） ----------
 * 用途：RTC 断电失效（无 VBAT 电池）时每次开机自动进入，输入真实时间。
 * 布局复用登录键盘：顶部提示 + 输入显示行 + 数字键盘
 * 输入 8 位：MM-DD HH:MM；满 8 位校验（月 1-12 日 1-31 时 0-23 分 0-59），
 * 合法 → 写入 RTC（年份取编译年份）+ BKP 魔数 → 进登录；
 * 非法 → 清空重输；K1 = 跳过校时（时间保持编译时刻） */

/* 输入行：MM-DD HH:MM，未输的位显示 '_'（16 号字，居中 y=52 行） */
static void draw_clk_input(void)
{
    uint8_t i, hid;
    char line[12];

    for (i = 0; i < 8; i++) line[i] = (i < s_clk_len) ? s_clk[i] : '_';
    line[8] = 0;
    /* 8 字符 → "XX-XX XX:XX"：12 个字符位（含分隔符） */
    {
        char full[12];
        full[0] = line[0]; full[1] = line[1]; full[2] = '-';
        full[3] = line[2]; full[4] = line[3]; full[5] = ' ';
        full[6] = line[4]; full[7] = line[5]; full[8] = ':';
        full[9] = line[6]; full[10] = line[7]; full[11] = 0;
        hid = redraw_protect_begin(30, 52, 210, 68);
        /* 擦除条带须盖满 16 号字形全高（52..67）——只擦 6px 会残留旧字
         * 下半截：退格时"8 只删上半"就是这么来的 */
        atk_md0280_fill(30, 52, 210, 68, CLR_LOGIN_BG);
        atk_md0280_show_string((SCR_W - 12 * 8) / 2, 52, 160, 16, full,
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLUE);
        redraw_protect_end(hid);
    }
}

/* 进入校时界面：清屏 → 提示 → 输入行 → 键盘 → 光标定位数字 5 */
static void enter_clock_set(void)
{
    uint8_t i;

    cursor_hide();   /* 对称于 launch_app：切换前先藏掉旧界面光标，保证 visible
                      * 状态与屏幕一致——否则 fill 全屏盖掉光标后状态失真，
                      * 后续重绘保护会误触发 hide→restore（旧背景写回屏幕成
                      * 残影）→show（光标画回旧位置），最终双光标 */
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, CLR_LOGIN_BG);
    s_state = UI_CLOCK_SET;
    s_clk_len = 0;
    s_hover = 4;
    for (i = 0; i < 8; i++) s_clk[i] = 0;

    draw_js_status();
    draw_hint(g_js_on ? "Set Clock (MM-DD HH:MM)" : "Joystick OFF - Press K0",
              g_js_on ? CLR_HINT : CLR_ERR);
    draw_clk_input();
    atk_md0280_show_string(24, 78, 200, 12, (char *)"K1: skip  Enter: SW",
                           ATK_MD0280_LCD_FONT_12, CLR_HINT);
    for (i = 0; i < KEY_COLS * KEY_ROWS; i++) draw_key_cell(i, 0);
    draw_key_cell(s_hover, 1);

    cursor_init(KEY_X0 + KEY_W / 2 + KEY_W, KEY_Y0 + KEY_H / 2 + KEY_H);
    cursor_show();
}

/* 进入登录界面：清屏 → 标题 → 密码框 → 键盘 → 光标定位数字 5 */
/* 垂直渐变：y0(顶) 到 y1(底) 从 c0 平滑过渡到 c1，逐行一色。
 * 纯整数逐通道插值，不引浮点库；顶栏深蓝 → 淡蓝主体 */
static void draw_grad_v(uint16_t y0, uint16_t y1, uint16_t c0, uint16_t c1)
{
    int32_t r0, g0, b0, r1, g1, b1, n;
    int16_t yy;
    uint16_t r, g, b;

    if (y1 <= y0) return;
    r0 = (c0 >> 11) & 0x1F;  g0 = (c0 >> 5) & 0x3F;  b0 = c0 & 0x1F;
    r1 = (c1 >> 11) & 0x1F;  g1 = (c1 >> 5) & 0x3F;  b1 = c1 & 0x1F;
    n = y1 - y0;
    for (yy = 0; yy <= n; yy++) {
        r = (uint16_t)(r0 + (r1 - r0) * yy / n);
        g = (uint16_t)(g0 + (g1 - g0) * yy / n);
        b = (uint16_t)(b0 + (b1 - b0) * yy / n);
        atk_md0280_fill(0, y0 + yy, SCR_W - 1, y0 + yy, (uint16_t)((r << 11) | (g << 5) | b));
    }
}

/* 桌面壁纸装饰（进入桌面时画一次，纯绘制无交互）：
 * - 顶栏下 25..54：深蓝 → 淡蓝渐变，与蓝色顶栏无缝衔接
 *   （图标行从 y57 起，渐变止于 54，不与图标擦除区相交）
 * - 底部 302..319：中央 KazepOS 水印 + 左右细线
 *   （第三行图标名字到 y300 止，水印在其下） */
static void draw_desktop_deco(void)
{
    draw_grad_v(25, 54, ATK_MD0280_BLUE, CLR_BG);
    atk_md0280_show_string((SCR_W - 36) / 2, 302, 80, 12, (char *)"KazepOS",
                           ATK_MD0280_LCD_FONT_12, CLR_WATERMARK);
    atk_md0280_fill(18, 307, 82, 308, CLR_WATERMARK);
    atk_md0280_fill(158, 307, 222, 308, CLR_WATERMARK);
}

static void enter_login(void)
{
    uint8_t i;

    cursor_hide();   /* 同 enter_clock_set：切换前先同步光标状态（锁屏等路径
                      * 从可见光标界面进来，不隐藏会留残影/双光标） */
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, CLR_LOGIN_BG);
    s_state = UI_LOGIN;
    s_pwd_len = 0;
    s_hover = 4;

    /* 设备默认未连接：提示先按 K0 打开摇杆 */
    draw_hint(g_js_on ? "Enter Password" : "Joystick OFF - Press K0",
              g_js_on ? CLR_HINT : CLR_ERR);
    draw_js_status();
    draw_pwd_dots();
    atk_md0280_show_string(24, 96, 200, 12, (char *)"Move: Joystick  OK: SW",
                           ATK_MD0280_LCD_FONT_12, CLR_HINT);
    for (i = 0; i < KEY_COLS * KEY_ROWS; i++) draw_key_cell(i, 0);
    draw_key_cell(s_hover, 1);                       /* 初始高亮数字 5 */

    /* 光标定位到数字 5 所在格中心 */
    cursor_init(KEY_X0 + KEY_W / 2 + KEY_W, KEY_Y0 + KEY_H / 2 + KEY_H);
    cursor_show();
}

/* ---------- 改密码界面（UI_PWD_SET，Settings 入口） ----------
 * 复用登录的键盘/圆点布局：输 4 位新密码 → 自动切"再输一遍确认"，
 * 两遍一致立即落盘回桌面；不一致提示重来。K1 = 取消（回桌面） */

/* 提示行按当前阶段重画：阶段 0 "New Password"，阶段 1 "Confirm PWD"；
 * JS 未开时两阶段都显示引导（与登录界面同一逻辑） */
static void pwd_set_hint(void)
{
    draw_hint(g_js_on ? (s_pwd_phase == 0 ? "New Password" : "Confirm PWD")
                      : "Joystick OFF - Press K0",
              g_js_on ? CLR_HINT : CLR_ERR);
}

static void enter_pwd_set(void)
{
    uint8_t i;

    cursor_hide();   /* 同 enter_login：切换前先同步光标状态 */
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, CLR_LOGIN_BG);
    s_state = UI_PWD_SET;
    s_pwd_phase = 0;
    s_pwd_len = 0;
    s_hover = 4;

    draw_js_status();
    pwd_set_hint();
    draw_pwd_dots();
    atk_md0280_show_string(24, 96, 200, 12, (char *)"Move: Joystick  OK: SW",
                           ATK_MD0280_LCD_FONT_12, CLR_HINT);
    for (i = 0; i < KEY_COLS * KEY_ROWS; i++) draw_key_cell(i, 0);
    draw_key_cell(s_hover, 1);                       /* 初始高亮数字 5 */

    /* 光标定位到数字 5 所在格中心 */
    cursor_init(KEY_X0 + KEY_W / 2 + KEY_W, KEY_Y0 + KEY_H / 2 + KEY_H);
    cursor_show();
}

/* 进入桌面：清屏 → 状态栏 → 2×2 图标 → 光标定位第一个图标 */
static void enter_desktop(void)
{
    uint8_t i;

    cursor_hide();   /* 同 enter_clock_set：从应用返回时先把应用界面光标藏掉
                      * （Monitor 等可见光标应用实测出过双光标残影） */
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, CLR_BG);
    s_state = UI_DESKTOP;

    /* 顶部状态栏：KazepOS | JS 状态 | 时间 */
    atk_md0280_fill(0, 0, SCR_W - 1, 24, ATK_MD0280_BLUE);
    atk_md0280_show_string(8, 5, 100, 16, (char *)"KazepOS",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
    draw_js_status();
    draw_clock();

    /* 壁纸装饰：顶栏下蓝→淡蓝渐变 + 底部水印（纯装饰，避开图标区） */
    draw_desktop_deco();

    /* 图标网格：遍历注册表绘制；光标定位第 1 个图标中心，初始即高亮 */
    s_cur_app = NULL;                    /* 回到桌面态（可能从应用返回） */
    s_hover_icon = 0;
    for (i = 0; i < g_app_count; i++) {
        draw_icon(i, i == (uint8_t)s_hover_icon);
    }

    cursor_init(ICON_X0 + ICON_W / 2, ICON_Y0 + ICON_H / 2);
    cursor_show();
}

/* 打开应用：先把光标藏好（已存背景写回屏幕，界面干净），再让应用自己全屏
 * 绘制。进入时光标是"隐藏且状态干净"的，应用自行决定是否显示光标
 * （Monitor 纯显示不显示，Files/Paint 交互类显示） */
static void launch_app(uint8_t idx)
{
    cursor_hide();
    s_cur_app = &g_apps[idx];
    s_cur_app->open();
}

/* 连续错误锁定：倒计时期间事件全部无效，结束后清队列回 LOGIN */
static void lock_screen(void)
{
    uint8_t sec;

    for (sec = LOGIN_LOCK_S; sec > 0; sec--) {
        draw_lock_hint(sec);
        osDelay(1000);
    }
    xQueueReset(g_event_queue);    /* 丢弃锁定期积压的摇杆事件 */
    s_fail_cnt = 0;
    enter_login();
}

/* ---------- 对外接口 ---------- */

/* 校时结束出口：从应用（Settings）进入 → 回桌面（内部重绘桌面）；
 * 开机自动进入 → 回登录页（须在 enter_desktop 定义之后，C90） */
static void clock_set_done(void)
{
    if (s_clock_from_app) {
        s_clock_from_app = 0;
        enter_desktop();
    } else {
        enter_login();
    }
}

/* 上电初始化：BOOT 界面 2 秒（进度条 10 档 × 200ms 推进）→
 * RTC 失效则先进校时，否则直接登录（在 ui_task 初始化时调用一次） */
void desktop_init(void)
{
    uint8_t pct;

    draw_boot();
    for (pct = 10; pct <= 100; pct += 10) {
        boot_progress(pct);
        osDelay(200);
    }
    xQueueReset(g_event_queue);  /* 丢弃启动期间积压的摇杆事件 */
    if (rtc_app_needs_setup()) {
        enter_clock_set();       /* 断电过/首次 → 先设真实时间 */
    } else {
        enter_login();
    }
}

/* Settings 的校时入口：切到校时界面（当前应用保持打开，
 * s_cur_app 在完成后由 enter_desktop 清空回桌面） */
void desktop_enter_clock_set(void)
{
    s_clock_from_app = 1;        /* 标记来源：完成后回桌面 */
    enter_clock_set();
}

/* Settings 的改密码入口：切到改密码界面（复用 s_clock_from_app：
 * 完成后 clock_set_done 回桌面，Settings 顺带关闭） */
void desktop_enter_pwd_set(void)
{
    s_clock_from_app = 1;
    enter_pwd_set();
}

/* ui_task 读（LED 慢闪档判定）：熄屏管理都在本任务内（desktop_handle_event
 * 同线程调用），直接读状态即可 */
uint8_t desktop_is_sleeping(void)
{
    return s_sleeping;
}

/* 事件分发：按当前状态决定事件含义 */
void desktop_handle_event(input_event_t *ev)
{
    /* ---- 熄屏管理（所有状态共享）：空闲超时灭背光，任一输入唤醒 ---- */
    if (ev->type == EV_TICK) {
        if (s_idle_sec < 300) s_idle_sec++;
        if (!s_sleeping && g_sys_cfg.screen_time > 0
         && s_idle_sec >= g_sys_cfg.screen_time) {
            sys_backlight_set(0);        /* 灭背光（屏幕内容保持，省电） */
            s_sleeping = 1;
            g_stats_sleeps++;            /* 统计：熄屏次数（日志模块消费） */
            sys_log_add(LOG_LV_WARN, LOG_SCREEN_OFF, 0);   /* 日志：熄屏 */
        }
    } else if (s_sleeping) {
        sys_backlight_set(g_sys_cfg.brightness);   /* 任一输入 → 恢复亮度 */
        s_sleeping = 0;
        s_idle_sec = 0;
        sys_log_add(LOG_LV_INFO, LOG_SCREEN_ON, 0);    /* 日志：唤醒 */
    } else {
        s_idle_sec = 0;
    }

    switch (s_state) {
    case UI_BOOT:
        break;   /* 防御：BOOT 阶段由 desktop_init 阻塞渡过，不会到达这里 */

    case UI_CLOCK_SET: {
        /* 键盘交互与登录完全一致：移光标高亮 → SW 输入数字/退格 */
        uint16_t cx, cy;
        cursor_get_pos(&cx, &cy);

        if (ev->type == EV_DEV_TOGGLE) {
            /* K0 设备开关：红/绿切换 + 提示行同步（同登录界面） */
            draw_js_status();
            draw_hint(g_js_on ? "Set Clock (MM-DD HH:MM)" : "Joystick OFF - Press K0",
                      g_js_on ? ATK_MD0280_GRAY : CLR_ERR);
        } else if (ev->type == EV_MOUSE_MOVE) {
            uint8_t idx = hit_key(cx, cy);
            if (idx != 0xFF && idx != s_hover) {
                uint8_t hid;
                uint16_t ox, oy, nx, ny, rx0, ry0, rx1, ry1;

                ox = KEY_X0 + (s_hover % KEY_COLS) * KEY_W;
                oy = KEY_Y0 + (s_hover / KEY_COLS) * KEY_H;
                nx = KEY_X0 + (idx % KEY_COLS) * KEY_W;
                ny = KEY_Y0 + (idx / KEY_COLS) * KEY_H;
                rx0 = (ox < nx) ? ox : nx;
                ry0 = (oy < ny) ? oy : ny;
                rx1 = ((ox > nx) ? ox : nx) + KEY_W - 1;
                ry1 = ((oy > ny) ? oy : ny) + KEY_H - 1;

                hid = redraw_protect_begin(rx0, ry0, rx1, ry1);
                draw_key_cell(s_hover, 0);
                s_hover = idx;
                draw_key_cell(idx, 1);
                redraw_protect_end(hid);
            }
        } else if (ev->type == EV_BACK) {
            /* K1 = 跳过校时（时间保持编译时刻，进登录/回桌面） */
            clock_set_done();
        } else if (ev->type == EV_KEY_DOWN) {
            uint8_t idx, i;
            char c;

            idx = hit_key(cx, cy);
            if (idx == 0xFF) break;
            c = s_key[idx / KEY_COLS][idx % KEY_COLS];

            if (c == '<') {                  /* 退格 */
                if (s_clk_len > 0) { s_clk_len--; draw_clk_input(); }
            } else if (c != '*') {           /* 数字 */
                if (s_clk_len < 8) {
                    s_clk[s_clk_len++] = c;
                    draw_clk_input();
                }
                if (s_clk_len == 8) {        /* 满 8 位：MM-DD HH:MM 校验 */
                    uint8_t mon  = (uint8_t)((s_clk[0] - '0') * 10 + (s_clk[1] - '0'));
                    uint8_t day  = (uint8_t)((s_clk[2] - '0') * 10 + (s_clk[3] - '0'));
                    uint8_t hour = (uint8_t)((s_clk[4] - '0') * 10 + (s_clk[5] - '0'));
                    uint8_t min  = (uint8_t)((s_clk[6] - '0') * 10 + (s_clk[7] - '0'));

                    if (mon >= 1 && mon <= 12 && day >= 1 && day <= 31
                     && hour <= 23 && min <= 59) {
                        rtc_app_set_datetime(mon, day, hour, min);
                        clock_set_done();    /* 校时完成 → 登录 / 回桌面 */
                    } else {
                        for (i = 0; i < 8; i++) s_clk[i] = 0;
                        s_clk_len = 0;
                        draw_clk_input();
                        draw_hint("Invalid! Re-enter", CLR_ERR);
                    }
                }
            }
        }
        break;
    }

    case UI_LOGIN: {
        uint16_t cx, cy;
        cursor_get_pos(&cx, &cy);

        if (ev->type == EV_DEV_TOGGLE) {
            /* 设备开关变化（K0）：状态从红 OFF 变绿 ON（或反向），
             * 提示行同步切换——"设备插上"才能输密码 */
            draw_js_status();
            draw_hint(g_js_on ? "Enter Password" : "Joystick OFF - Press K0",
                      g_js_on ? ATK_MD0280_GRAY : CLR_ERR);
        } else if (ev->type == EV_MOUSE_MOVE) {
            /* 光标已由 ui_task 移动，这里只更新键盘高亮（重绘旧格+新格） */
            uint8_t idx = hit_key(cx, cy);
            if (idx != 0xFF && idx != s_hover) {
                uint8_t hid;
                uint16_t ox, oy, nx, ny, rx0, ry0, rx1, ry1;

                /* 重绘区域 = 旧格 ∪ 新格（并集矩形），先保护再重绘：
                 * 光标外框(54×60)比格子(60×45)高，fill 只盖得住光标中间，
                 * 不先隐藏就会存到"截断光标"，下次恢复固化成框边残影 */
                ox = KEY_X0 + (s_hover % KEY_COLS) * KEY_W;
                oy = KEY_Y0 + (s_hover / KEY_COLS) * KEY_H;
                nx = KEY_X0 + (idx % KEY_COLS) * KEY_W;
                ny = KEY_Y0 + (idx / KEY_COLS) * KEY_H;
                rx0 = (ox < nx) ? ox : nx;
                ry0 = (oy < ny) ? oy : ny;
                rx1 = ((ox > nx) ? ox : nx) + KEY_W - 1;
                ry1 = ((oy > ny) ? oy : ny) + KEY_H - 1;

                hid = redraw_protect_begin(rx0, ry0, rx1, ry1);
                draw_key_cell(s_hover, 0);
                s_hover = idx;
                draw_key_cell(idx, 1);
                redraw_protect_end(hid);   /* 重绘完成后再把光标画回最上层 */
            }
        } else if (ev->type == EV_BACK) {
            /* K1 = 清空已输密码（"取消输入"） */
            if (s_pwd_len > 0) {
                s_pwd_len = 0;
                draw_pwd_dots();
            }
        } else if (ev->type == EV_KEY_DOWN) {
            /* SW 按下：判定光标所在格 → 输入数字 / 退格 */
            uint8_t idx, ok = 1, i;
            char c;

            idx = hit_key(cx, cy);
            if (idx == 0xFF) break;          /* 点在键盘区外，忽略 */
            c = s_key[idx / KEY_COLS][idx % KEY_COLS];

            if (c == '<') {                  /* 退格：删最后一位 */
                if (s_pwd_len > 0) {
                    s_pwd_len--;
                    draw_pwd_dots();
                }
            } else if (c != '*') {           /* 数字 */
                if (s_pwd_len < 4) {
                    s_pwd[s_pwd_len++] = c;
                    draw_pwd_dots();
                }
                if (s_pwd_len == 4) {        /* 满 4 位自动比对（密码在配置里，
                                              * Settings 改过后这里自然用新密码） */
                    for (i = 0; i < 4; i++) {
                        if (s_pwd[i] != g_sys_cfg.pwd[i]) { ok = 0; break; }
                    }
                    s_pwd_len = 0;
                    draw_pwd_dots();
                    if (ok) {
                        enter_desktop();     /* 密码正确 → 桌面 */
                    } else {
                        s_fail_cnt++;
                        sys_log_add(LOG_LV_ERR, LOG_LOGIN_FAIL, s_fail_cnt);
                        if (s_fail_cnt >= LOGIN_MAX_FAIL) {
                            sys_log_add(LOG_LV_WARN, LOG_LOGIN_LOCK, LOGIN_LOCK_S);
                            lock_screen();   /* 连续错误 → 锁定 */
                        } else {
                            draw_hint("Wrong Password", CLR_ERR);
                        }
                    }
                }
            }
        }
        break;
    }

    case UI_PWD_SET: {
        /* 改密码（来源只有 Settings 的 Password 行）：键盘/圆点与登录共用，
         * 交互也一致——满 4 位自动进下一阶段。阶段 0 输新密码 → 阶段 1
         * 再输一遍：一致 → 写配置 + 立即落盘 → 回桌面；不一致 → 提示重来 */
        uint16_t cx, cy;
        cursor_get_pos(&cx, &cy);

        if (ev->type == EV_DEV_TOGGLE) {
            /* 设备开关变化（K0）：状态红/绿切换 + 提示行按阶段同步 */
            draw_js_status();
            pwd_set_hint();
        } else if (ev->type == EV_MOUSE_MOVE) {
            /* 光标已由 ui_task 移动：更新键盘高亮（重绘旧格+新格） */
            uint8_t idx = hit_key(cx, cy);
            if (idx != 0xFF && idx != s_hover) {
                uint8_t hid;
                uint16_t ox, oy, nx, ny, rx0, ry0, rx1, ry1;

                ox = KEY_X0 + (s_hover % KEY_COLS) * KEY_W;
                oy = KEY_Y0 + (s_hover / KEY_COLS) * KEY_H;
                nx = KEY_X0 + (idx % KEY_COLS) * KEY_W;
                ny = KEY_Y0 + (idx / KEY_COLS) * KEY_H;
                rx0 = (ox < nx) ? ox : nx;
                ry0 = (oy < ny) ? oy : ny;
                rx1 = ((ox > nx) ? ox : nx) + KEY_W - 1;
                ry1 = ((oy > ny) ? oy : ny) + KEY_H - 1;

                hid = redraw_protect_begin(rx0, ry0, rx1, ry1);
                draw_key_cell(s_hover, 0);
                s_hover = idx;
                draw_key_cell(idx, 1);
                redraw_protect_end(hid);
            }
        } else if (ev->type == EV_BACK) {
            /* K1 = 取消改密：不保存直接回桌面（来源必为 Settings，
             * s_clock_from_app=1 → clock_set_done 回桌面） */
            clock_set_done();
        } else if (ev->type == EV_KEY_DOWN) {
            uint8_t idx, i;
            char c;

            idx = hit_key(cx, cy);
            if (idx == 0xFF) break;          /* 点在键盘区外，忽略 */
            c = s_key[idx / KEY_COLS][idx % KEY_COLS];

            if (c == '<') {                  /* 退格：删最后一位 */
                if (s_pwd_len > 0) {
                    s_pwd_len--;
                    draw_pwd_dots();
                }
            } else if (c != '*') {           /* 数字 */
                if (s_pwd_len < 4) {
                    s_pwd[s_pwd_len++] = c;
                    draw_pwd_dots();
                }
                if (s_pwd_len == 4) {        /* 满 4 位自动推进（无确认键） */
                    if (s_pwd_phase == 0) {
                        /* 第一遍：暂存 → 清输入 → 提示切 "Confirm PWD" */
                        for (i = 0; i < 4; i++) s_pwd_verify[i] = s_pwd[i];
                        s_pwd_phase = 1;
                        s_pwd_len = 0;
                        draw_pwd_dots();
                        pwd_set_hint();
                    } else {
                        /* 第二遍：两遍一致才生效 */
                        uint8_t match = 1;

                        for (i = 0; i < 4; i++) {
                            if (s_pwd[i] != s_pwd_verify[i]) { match = 0; break; }
                        }
                        s_pwd_len = 0;
                        draw_pwd_dots();
                        if (match) {
                            memcpy(g_sys_cfg.pwd, s_pwd, 4);
                            g_sys_cfg.pwd[4] = '\0';
                            sys_cfg_save();    /* 密码即刻落盘（不等延迟合并） */
                            clock_set_done();  /* 回桌面（全屏重绘）= 改密成功 */
                        } else {
                            /* 不一致：回到第一遍重输 */
                            s_pwd_phase = 0;
                            draw_hint("Mismatch! Retry", CLR_ERR);
                            osDelay(800);      /* 红字停留片刻（同锁定阻塞先例） */
                            pwd_set_hint();    /* 恢复阶段 0 提示 */
                        }
                    }
                }
            }
        }
        break;
    }

    case UI_DESKTOP:
        if (s_cur_app != NULL) {
            /* --- 应用前台态 ---
             * EV_BACK 是系统级导航事件：框架直接接管（返回桌面），
             * 不发给应用——否则应用忽略它就会"按 K1 退不出来" */
            if (ev->type == EV_BACK) {
                if (s_cur_app->close != NULL) s_cur_app->close();  /* 退出钩子 */
                enter_desktop();        /* 内部置 s_cur_app = NULL 并重绘桌面 */
                break;
            }
            /* 其余事件（移动/SW/TICK/设备开关）转发给应用自己处理 */
            s_cur_app->handle(ev);
            break;
        }
        /* --- 桌面态 --- */
        if (ev->type == EV_DEV_TOGGLE) {
            draw_js_status();              /* 设备开关：红/绿切换 */
        } else if (ev->type == EV_TICK) {
            draw_clock();                  /* 每秒刷新状态栏时间 */
        } else if (ev->type == EV_BACK) {
            /* 桌面按 K1 = 锁屏：回登录页（重置连续错误计数，重新累计） */
            s_fail_cnt = 0;
            enter_login();
        } else if (ev->type == EV_MOUSE_MOVE) {
            /* 光标已由 ui_task 移动：图标 hover 高亮（重绘旧格+新格） */
            uint16_t cx, cy, nx, ny, ox, oy, rx0, ry0, rx1, ry1;
            uint8_t idx, hid;
            int8_t old = s_hover_icon;

            cursor_get_pos(&cx, &cy);
            idx = hit_icon(cx, cy);
            if (idx == 0xFF) {
                /* 光标移出图标区：熄灭高亮（否则停在空白处时旧图标仍暗着） */
                if (old >= 0) {
                    ox = ICON_X0 + (old % ICON_COLS) * ICON_GAP_X;
                    oy = ICON_Y0 + (old / ICON_COLS) * ICON_GAP_Y;
                    hid = redraw_protect_begin(ox, oy, ox + ICON_W, oy + ICON_H);
                    draw_icon((uint8_t)old, 0);
                    s_hover_icon = -1;
                    redraw_protect_end(hid);
                }
            } else if (idx != (uint8_t)old) {
                nx = ICON_X0 + (idx % ICON_COLS) * ICON_GAP_X;
                ny = ICON_Y0 + (idx / ICON_COLS) * ICON_GAP_Y;
                if (old >= 0) {
                    /* 并集矩形 = 旧图标 ∪ 新图标（图标界外扩 2px，给光标躲闪留余量） */
                    ox = ICON_X0 + (old % ICON_COLS) * ICON_GAP_X;
                    oy = ICON_Y0 + (old / ICON_COLS) * ICON_GAP_Y;
                    rx0 = (ox < nx ? ox : nx) - 2;
                    ry0 = (oy < ny ? oy : ny) - 2;
                    rx1 = (ox > nx ? ox : nx) + ICON_W + 1;
                    ry1 = (oy > ny ? oy : ny) + ICON_H + 1;
                } else {
                    rx0 = nx - 2; ry0 = ny - 2;
                    rx1 = nx + ICON_W + 1; ry1 = ny + ICON_H + 1;
                }
                hid = redraw_protect_begin(rx0, ry0, rx1, ry1);
                if (old >= 0) draw_icon(old, 0);
                s_hover_icon = idx;
                draw_icon(idx, 1);
                redraw_protect_end(hid);
            }
        } else if (ev->type == EV_KEY_DOWN) {
            /* SW 按下：命中图标则打开对应应用 */
            uint16_t cx, cy;
            uint8_t idx;

            cursor_get_pos(&cx, &cy);
            idx = hit_icon(cx, cy);
            if (idx != 0xFF) launch_app(idx);
        }
        break;
    }
}
