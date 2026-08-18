/**
 * @file    desktop.c
 * @brief   桌面框架：上电启动界面 → 密码登录 → 桌面主界面
 *
 * 架构：由 ui_task 逐事件调用（ui_task 只做事件分发，本文件是桌面框架）
 *   - 状态机：UI_BOOT → UI_LOGIN → UI_DESKTOP（桌面态内嵌应用前台态：
 *     s_cur_app 非空时事件全部转发给注册表中的应用，K1 返回桌面）
 *   - 密码 4 位，错误提示，连续错误进入锁定（倒计时，输入全部无效）
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

/* 桌面图标区：3 列 × 2 行，色块 68×60（名字在色块下方；2 行高度内放得下） */
#define ICON_COLS           3
#define ICON_W              68
#define ICON_H              60
#define ICON_X0             18
#define ICON_Y0             60
#define ICON_GAP_X          73
#define ICON_GAP_Y          80

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
static int8_t  s_hover = 4;     /* 键盘高亮格索引 0~11 */
static const app_t *s_cur_app = NULL;  /* 非空 = 应用前台态（注册表指针） */
static int8_t  s_hover_icon = -1;      /* 桌面 hover 图标索引（-1 = 无） */

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

/* 画一个桌面图标：idx = 注册表索引；hover=1 高亮（色块外侧 2px 白色外框） */
static void draw_icon(uint8_t idx, uint8_t hover)
{
    uint16_t x = ICON_X0 + (idx % ICON_COLS) * ICON_GAP_X;
    uint16_t y = ICON_Y0 + (idx / ICON_COLS) * ICON_GAP_Y;

    /* 先整块擦成桌面底色（白）：hover 切回普通时不残留白色外框 */
    atk_md0280_fill(x - 2, y - 2, x + ICON_W + 1, y + ICON_H + 1, ATK_MD0280_WHITE);
    atk_md0280_fill(x, y, x + ICON_W, y + ICON_H, g_apps[idx].color);
    if (hover) {
        atk_md0280_draw_rect(x - 2, y - 2, x + ICON_W + 1, y + ICON_H + 1, ATK_MD0280_WHITE);
    } else {
        atk_md0280_draw_rect(x, y, x + ICON_W, y + ICON_H, ATK_MD0280_GRAY);
    }
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
            atk_md0280_fill(x, 52, x + 6, 58, ATK_MD0280_WHITE);
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

    atk_md0280_fill(0, 21, SCR_W - 1, 47, ATK_MD0280_WHITE);  /* 先擦后写 */
    atk_md0280_show_string(x, 24, 240, 16, (char *)str, ATK_MD0280_LCD_FONT_16, color);
    redraw_protect_end(hid);
}

/* 锁定提示：显示剩余秒数，每秒刷新 */
static void draw_lock_hint(uint8_t sec)
{
    uint8_t hid = redraw_protect_begin(0, 21, SCR_W - 1, 47);

    atk_md0280_fill(0, 21, SCR_W - 1, 47, ATK_MD0280_WHITE);
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
    uint16_t bg = (s_state == UI_DESKTOP) ? ATK_MD0280_BLUE : ATK_MD0280_WHITE;
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

    /* 设备默认未连接：提示先按 K0 打开摇杆 */
    draw_hint(g_js_on ? "Enter Password" : "Joystick OFF - Press K0",
              g_js_on ? ATK_MD0280_GRAY : CLR_ERR);
    draw_js_status();
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

    /* 顶部状态栏：MINI OS | JS 状态 | 时间 */
    atk_md0280_fill(0, 0, SCR_W - 1, 24, ATK_MD0280_BLUE);
    atk_md0280_show_string(8, 5, 100, 16, (char *)"MINI OS",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
    draw_js_status();
    draw_clock();

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
        if (s_cur_app != NULL) {
            /* --- 应用前台态 ---
             * EV_BACK 是系统级导航事件：框架直接接管（返回桌面），
             * 不发给应用——否则应用忽略它就会"按 K1 退不出来" */
            if (ev->type == EV_BACK) {
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
            if (idx != 0xFF && idx != (uint8_t)old) {
                nx = ICON_X0 + (idx % ICON_COLS) * ICON_GAP_X;
                ny = ICON_Y0 + (idx / ICON_COLS) * ICON_GAP_Y;
                if (old >= 0) {
                    /* 并集矩形 = 旧图标 ∪ 新图标（含 2px 高亮外框余量） */
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
