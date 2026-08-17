/**
 * @file    desktop.c
 * @brief   桌面框架：上电启动界面 → 密码登录 → 桌面主界面
 *
 * 架构：由 ui_task 逐事件调用（ui_task 只做事件分发，本文件是第一个"应用"）
 *   - 状态机：UI_BOOT → UI_LOGIN → UI_DESKTOP
 *   - 密码 4 位，错误提示，连续错误进入锁定（倒计时，输入全部无效）
 *   - 数字键盘交互：摇杆移光标选格（蓝框高亮），SW 按下输入
 *
 * 关键点：
 *   - 屏幕 240×320 竖屏（ATK-MD0280 默认方向）
 *   - 界面切换 = 全屏重绘；动态变化（高亮/圆点/提示）= 局部重绘，避免卡顿
 *   - 锁定倒计时用 osDelay 阻塞实现（UI 任务独占渲染，锁定期只画倒计时）
 *   - 切换界面时 xQueueReset 清掉积压事件，防止旧事件污染新界面
 */
#include "desktop.h"
#include "main.h"
#include <string.h>
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "event.h"
#include "cmsis_os.h"

/* ---------- 配置参数（阶段6 可改为从 Flash 加载） ---------- */
#define LOGIN_PASSWORD      "1234"   /* 默认密码 */
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

/* ---------- 颜色（RGB565） ---------- */
#define CLR_KEY_HOV_BG      0x7DFC    /* 高亮格浅蓝底 */
#define CLR_ERR             ATK_MD0280_RED

/* ---------- 状态机 ---------- */
typedef enum {
    UI_BOOT,        /* 启动界面（停留 2 秒） */
    UI_LOGIN,       /* 密码登录 */
    UI_DESKTOP,     /* 桌面主界面 */
} ui_state_t;

static ui_state_t s_state = UI_BOOT;
static char  s_pwd[4];          /* 已输入密码 */
static uint8_t s_pwd_len = 0;
static uint8_t s_fail_cnt = 0;  /* 连续错误计数 */
static int8_t  s_hover = 4;     /* 高亮格索引 0~11 */

/* 键盘内容：'*' = 占位空格，'<' = 退格 */
static const char s_key[KEY_ROWS][KEY_COLS] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','<'},
};

/* 桌面图标（阶段4 逐个换成真应用）
 * 注意：atk_md0280 库的 show_string 只支持 ASCII（见库源码），界面文字统一用英文 */
typedef struct { const char *name; uint16_t color; } app_icon_t;
static const app_icon_t s_icons[4] = {
    {"Files",    ATK_MD0280_BLUE},
    {"Paint",    ATK_MD0280_GREEN},
    {"Music",    ATK_MD0280_MAGENTA},
    {"Settings", ATK_MD0280_YELLOW},
};

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
            atk_md0280_fill(x, 52, x + 6, 58, ATK_MD0280_WHITE);
            atk_md0280_draw_rect(x, 52, x + 6, 58, ATK_MD0280_GRAY);
        }
    }
    redraw_protect_end(hid);
}

/* 提示行（y=15~47 整行重绘，46 为界避免擦到 y=52 起的密码圆点） */
static void draw_hint(const char *str, uint16_t color)
{
    uint8_t hid = redraw_protect_begin(0, 15, SCR_W - 1, 47);

    atk_md0280_fill(0, 15, SCR_W - 1, 47, ATK_MD0280_WHITE);  /* 先擦后写 */
    atk_md0280_show_string(80, 24, 240, 16, (char *)str, ATK_MD0280_LCD_FONT_16, color);
    redraw_protect_end(hid);
}

/* 锁定提示：显示剩余秒数，每秒刷新 */
static void draw_lock_hint(uint8_t sec)
{
    uint8_t hid = redraw_protect_begin(0, 15, SCR_W - 1, 47);

    atk_md0280_fill(0, 15, SCR_W - 1, 47, ATK_MD0280_WHITE);
    atk_md0280_show_string(40, 24, 100, 16, (char *)"Locked ", ATK_MD0280_LCD_FONT_16, CLR_ERR);
    atk_md0280_show_xnum(104, 24, sec, 2, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_16, CLR_ERR);
    atk_md0280_show_string(128, 24, 40, 16, (char *)"s", ATK_MD0280_LCD_FONT_16, CLR_ERR);
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

/* ---------- 界面 ---------- */

/* BOOT：蓝底 + 项目名 */
static void draw_boot(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_BLUE);
    atk_md0280_show_string(72, 120, 160, 32, (char *)"MINI OS",
                           ATK_MD0280_LCD_FONT_32, ATK_MD0280_WHITE);
    atk_md0280_show_string(80, 168, 120, 16, (char *)"Starting...",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
}

/* 进入登录界面：清屏 → 标题 → 密码框 → 键盘 → 光标定位数字 5 */
static void enter_login(void)
{
    uint8_t i;

    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    s_state = UI_LOGIN;
    s_pwd_len = 0;
    s_hover = 4;

    draw_hint("Enter Password", ATK_MD0280_GRAY);
    draw_pwd_dots();
    atk_md0280_show_string(24, 96, 200, 12, (char *)"Move: Joystick  OK: SW",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
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

    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    s_state = UI_DESKTOP;

    /* 顶部状态栏 */
    atk_md0280_fill(0, 0, SCR_W - 1, 24, ATK_MD0280_BLUE);
    atk_md0280_show_string(8, 5, 100, 16, (char *)"MINI OS",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);

    /* 2×2 图标：80×80 色块 + 下方名字 */
    for (i = 0; i < 4; i++) {
        uint16_t x = 20 + (i % 2) * 120;
        uint16_t y = 60 + (i / 2) * 100;
        atk_md0280_fill(x, y, x + 80, y + 80, s_icons[i].color);
        atk_md0280_draw_rect(x, y, x + 80, y + 80, ATK_MD0280_GRAY);
        atk_md0280_show_string(x + (80 - strlen(s_icons[i].name) * 8) / 2, y + 84, 80, 16,
                               (char *)s_icons[i].name,
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    }

    cursor_init(60, 100);
    cursor_show();
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

/* 上电初始化：BOOT 界面 2 秒 → 自动进 LOGIN（在 ui_task 初始化时调用一次） */
void desktop_init(void)
{
    draw_boot();
    osDelay(2000);               /* 启动画面停留 2 秒 */
    xQueueReset(g_event_queue);  /* 丢弃启动期间积压的摇杆事件 */
    enter_login();
}

/* 事件分发：按当前状态决定事件含义 */
void desktop_handle_event(input_event_t *ev)
{
    switch (s_state) {
    case UI_BOOT:
        break;   /* 防御：BOOT 阶段由 desktop_init 阻塞渡过，不会到达这里 */

    case UI_LOGIN: {
        uint16_t cx, cy;
        cursor_get_pos(&cx, &cy);

        if (ev->type == EV_MOUSE_MOVE) {
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
                if (s_pwd_len == 4) {        /* 满 4 位自动比对 */
                    for (i = 0; i < 4; i++) {
                        if (s_pwd[i] != LOGIN_PASSWORD[i]) { ok = 0; break; }
                    }
                    s_pwd_len = 0;
                    draw_pwd_dots();
                    if (ok) {
                        enter_desktop();     /* 密码正确 → 桌面 */
                    } else {
                        s_fail_cnt++;
                        if (s_fail_cnt >= LOGIN_MAX_FAIL) {
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

    case UI_DESKTOP:
        if (ev->type == EV_KEY_DOWN) {
            /* 阶段4：按光标所在图标打开对应应用 */
        }
        break;
    }
}
