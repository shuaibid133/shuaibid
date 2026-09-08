/**
 * @file    app_settings.c
 * @brief   Settings 应用：系统设置调节 + 持久化 + 校时入口
 *
 * 功能（对照题目）：
 *   - 8 行列表：Cursor Sens / Cursor Size / Brightness / Volume /
 *     Screen Off / JS Lock / Password（改登录密码，两遍确认）/
 *     Set Clock（校时入口，复用开机校时界面）
 *   - 交互：光标上下移选行（品牌黄底高亮，黑字）；每行右侧有 [-] [+] 两个按钮，
 *     光标移到按钮上（反色提示）按 SW 调节一格——"点按钮调值"
 *   - 即改即生效：灵敏度 joystick 实时读、光标大小 hide/show 重画、
 *     亮度直接写 PWM 占空比；音量/熄屏时间存入配置供 Music/熄屏模块用
 *   - 持久化：调节后延迟 2 秒无操作自动 sys_cfg_save（Flash 擦写约 300ms，
 *     每格都保存会卡顿，故延迟合并）；进校时前强制保存
 *
 * 线程模型：全部在 ui_task 事件循环内执行（单写者），Flash 擦写阻塞
 * 可接受（只在 3 秒静默时发生一次）
 */
#include "app_settings.h"
#include "app.h"
#include "cursor.h"
#include "desktop.h"
#include "sys_cfg.h"
#include "sys_backlight.h"
#include "app_config.h"
#include "sys_log.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include <string.h>

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* ---------- 布局 ---------- */
#define SET_Y0      52               /* 列表区起始 y（标题栏 24 + 提示行 28） */
#define SET_ROW_H   30               /* 行高（8 行 → 52..289，屏幕 320 内） */
#define SET_ROWS    8
#define BTN_W       28               /* [-] [+] 按钮尺寸（行内垂直居中） */
#define BTN_H       20
#define BTN_MINUS_X 156              /* [-] 按钮 x 范围 156~184 */
#define BTN_PLUS_X  190              /* [+] 按钮 x 范围 190~218 */
#define VAL_RX      148              /* 值右端对齐到 [-] 按钮左侧 */

/* 选中行 + 按钮 hover（1=[-] 2=[+] 0=无） */
static uint8_t s_row = 0;
static uint8_t s_btn_hover = 0;
static uint8_t s_dirty = 0;          /* 有修改未落盘 */
static uint8_t s_idle = 0;           /* 无调节持续秒数（EV_TICK 计数） */

/* ---------- 局部重绘保护（与桌面框架同协议） ---------- */
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

/* 无前导零十进制（≤999，避开 sprintf 重库） */
static void fmt_u8(char *buf, uint8_t v)
{
    uint8_t i = 0;

    if (v >= 100) buf[i++] = (char)('0' + v / 100);
    if (v >= 10)  buf[i++] = (char)('0' + (v / 10) % 10);
    buf[i++] = (char)('0' + v % 10);
    buf[i] = 0;
}

/* 画一行：hover=1 品牌黄底黑字（与标题栏同色，亮底必配黑字），0 白底黑字。
 * 名称 + 右侧当前值 + [-][+] 按钮
 * （Password/Set Clock 行除外：没有按钮，SW 直接进专用键盘界面） */
static void draw_row(uint8_t row, uint8_t hover)
{
    uint16_t x = 0, y, by;
    const char *name;
    char val[10];   /* 值缓冲：最长 "Change >" = 8 字符 + NUL。
                     * 曾写成 val[8]："Change >" 的 '\0' 溢出 1 字节踩坏
                     * 栈上相邻内容 → 进 Settings 后随机 HardFault */
    uint8_t b_hover;

    y = SET_Y0 + row * SET_ROW_H;
    switch (row) {
    case 0: name = "Cursor Sens"; fmt_u8(val, g_sys_cfg.cursor_sens); break;
    case 1: name = "Cursor Size"; fmt_u8(val, g_sys_cfg.cursor_size); break;
    case 2: name = "Brightness"; fmt_u8(val, g_sys_cfg.brightness); strcat(val, "%"); break;
    case 3: name = "Volume"; fmt_u8(val, g_sys_cfg.volume); strcat(val, "%"); break;
    case 4: name = "Screen Off";
        if (g_sys_cfg.screen_time == 0) strcpy(val, "Off");
        else { fmt_u8(val, g_sys_cfg.screen_time); strcat(val, "s"); }
        break;
    case 5: name = "JS Lock";        /* 开机摇杆默认态：On=自动锁（现状，
                                      * 开机按 K0 开）；Off=开机直接可用 */
        strcpy(val, g_sys_cfg.js_lock ? "On" : "Off");
        break;
    case 6: name = "Password"; strcpy(val, "Change >"); break;  /* 改登录密码
                                      *（专用键盘界面，输两遍确认后生效） */
    default: name = "Set Clock"; strcpy(val, "Enter >"); break;   /* row 7 */
    }

    atk_md0280_fill(0, y - 2, SCR_W - 1, y + SET_ROW_H - 3, hover ? ATK_MD0280_YELLOW : ATK_MD0280_WHITE);
    atk_md0280_show_string(16, y + 7, 120, 16, (char *)name, ATK_MD0280_LCD_FONT_16,
                           ATK_MD0280_BLACK);   /* 黄/白底都配黑字 */
    x = VAL_RX - strlen(val) * 8;    /* 值右端对齐到 [-] 按钮左侧 */
    atk_md0280_show_string(x, y + 7, 60, 16, val, ATK_MD0280_LCD_FONT_16,
                           hover ? ATK_MD0280_BLACK : ATK_MD0280_BLUE);

    if (row >= 6) return;            /* Password/Set Clock 行无按钮（都是 SW 直进） */

    /* [-] [+] 按钮：行内垂直居中；光标 hover 的按钮品牌黄底黑字，其余灰底黑字 */
    by = y + (SET_ROW_H - BTN_H) / 2;
    b_hover = (row == s_row) ? s_btn_hover : 0;
    atk_md0280_fill(BTN_MINUS_X, by, BTN_MINUS_X + BTN_W - 1, by + BTN_H - 1,
                    b_hover == 1 ? ATK_MD0280_YELLOW : ATK_MD0280_GRAY);
    atk_md0280_show_string(BTN_MINUS_X + 10, by + 4, 16, 12, (char *)"-", ATK_MD0280_LCD_FONT_12,
                           ATK_MD0280_BLACK);   /* 黄/灰底都配黑字 */
    atk_md0280_fill(BTN_PLUS_X, by, BTN_PLUS_X + BTN_W - 1, by + BTN_H - 1,
                    b_hover == 2 ? ATK_MD0280_YELLOW : ATK_MD0280_GRAY);
    atk_md0280_show_string(BTN_PLUS_X + 10, by + 4, 16, 12, (char *)"+", ATK_MD0280_LCD_FONT_12,
                           ATK_MD0280_BLACK);   /* 黄/灰底都配黑字 */
}

/* 光标尖端 y → 行索引；不在列表区返回 0xFF */
static uint8_t hit_row(uint16_t cy)
{
    if (cy < SET_Y0 || cy > SET_Y0 + SET_ROWS * SET_ROW_H - 3) return 0xFF;
    return (uint8_t)((cy - SET_Y0) / SET_ROW_H);
}

/* 光标尖端 → 按钮命中：1=[-] 2=[+] 0=无（只在选中行的按钮 y 范围内判断） */
static uint8_t hit_btn(uint16_t cx, uint16_t cy)
{
    uint16_t y0 = SET_Y0 + s_row * SET_ROW_H + (SET_ROW_H - BTN_H) / 2;
    uint16_t y1 = y0 + BTN_H - 1;

    if (cy < y0 || cy > y1) return 0;
    if (cx >= BTN_MINUS_X && cx <= BTN_MINUS_X + BTN_W - 1) return 1;
    if (cx >= BTN_PLUS_X && cx <= BTN_PLUS_X + BTN_W - 1) return 2;
    return 0;
}

/* 当前行调节一格（dir=+1 增大 / -1 减小），修改即生效 + 标记待保存 */
static void adjust(int8_t dir)
{
    uint8_t hid;

    switch (s_row) {
    case 0:    /* 光标灵敏度：joystick.c 实时读 g_sys_cfg，无需额外生效动作 */
        if (dir > 0 && g_sys_cfg.cursor_sens < 10) {
            g_sys_cfg.cursor_sens++;
            sys_log_add(LOG_LV_INFO, LOG_SET_SENS, g_sys_cfg.cursor_sens);
        } else if (dir < 0 && g_sys_cfg.cursor_sens > 1) {
            g_sys_cfg.cursor_sens--;
            sys_log_add(LOG_LV_INFO, LOG_SET_SENS, g_sys_cfg.cursor_sens);
        }
        break;
    case 1:    /* 光标大小：先 hide（旧尺寸恢复背景）→ 改值 → show（新尺寸画）。
                * 顺序反了 cursor_restore_bg 会按新尺寸把缓冲垃圾写回屏幕 */
        cursor_hide();
        if (dir > 0 && g_sys_cfg.cursor_size < 4) {
            g_sys_cfg.cursor_size++;
            sys_log_add(LOG_LV_INFO, LOG_SET_CSIZE, g_sys_cfg.cursor_size);
        } else if (dir < 0 && g_sys_cfg.cursor_size > 1) {
            g_sys_cfg.cursor_size--;
            sys_log_add(LOG_LV_INFO, LOG_SET_CSIZE, g_sys_cfg.cursor_size);
        }
        draw_row(s_row, 1);      /* 光标已藏，直画不保护 */
        cursor_show();
        break;
    case 2:    /* 亮度：直接写 PWM 占空比 */
        if (dir > 0 && g_sys_cfg.brightness < 100) {
            g_sys_cfg.brightness = (uint8_t)(g_sys_cfg.brightness + 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_BRIGHT, g_sys_cfg.brightness);
        } else if (dir < 0 && g_sys_cfg.brightness > 10) {
            g_sys_cfg.brightness = (uint8_t)(g_sys_cfg.brightness - 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_BRIGHT, g_sys_cfg.brightness);
        }
        sys_backlight_set(g_sys_cfg.brightness);
        break;
    case 3:    /* 音量：存入配置，Music 播放时读 */
        if (dir > 0 && g_sys_cfg.volume < 100) {
            g_sys_cfg.volume = (uint8_t)(g_sys_cfg.volume + 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_VOL, g_sys_cfg.volume);
        } else if (dir < 0 && g_sys_cfg.volume > 0) {
            g_sys_cfg.volume = (uint8_t)(g_sys_cfg.volume - 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_VOL, g_sys_cfg.volume);
        }
        break;
    case 4:    /* 熄屏时间：存入配置，熄屏模块读 */
        if (dir > 0 && g_sys_cfg.screen_time < 300) {
            g_sys_cfg.screen_time = (uint8_t)(g_sys_cfg.screen_time + 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_STIME, g_sys_cfg.screen_time);
        } else if (dir < 0 && g_sys_cfg.screen_time > 0) {
            g_sys_cfg.screen_time = (uint8_t)(g_sys_cfg.screen_time - 10);
            sys_log_add(LOG_LV_INFO, LOG_SET_STIME, g_sys_cfg.screen_time);
        }
        break;
    case 5:    /* JS Lock 开关：On(1)=开机自动锁（默认 OFF 按 K0，现状）/
                * Off(0)=开机直接可用。只影响下次开机（joystick_set_default
                * 启动时读），本次不动 g_js_on——否则调个设置当前设备状态
                * 就变了，用户以为坏了 */
        if (dir > 0 && g_sys_cfg.js_lock == 0) {
            g_sys_cfg.js_lock = 1;
            sys_log_add(LOG_LV_INFO, LOG_SET_JSLOCK, g_sys_cfg.js_lock);
        } else if (dir < 0 && g_sys_cfg.js_lock == 1) {
            g_sys_cfg.js_lock = 0;
            sys_log_add(LOG_LV_INFO, LOG_SET_JSLOCK, g_sys_cfg.js_lock);
        }
        break;
    default:
        return;    /* Password/Set Clock 行不可调 */
    }

    s_dirty = 1;
    s_idle = 0;
    if (s_row != 1) {            /* case 1 已重绘，跳过统一重绘 */
        hid = redraw_protect_begin(0, SET_Y0 + s_row * SET_ROW_H - 2, SCR_W - 1,
                                   SET_Y0 + s_row * SET_ROW_H + SET_ROW_H - 3);
        draw_row(s_row, 1);
        redraw_protect_end(hid);
    }
}

/* 进入应用：清屏 → 标题 → 提示 → 8 行 → 光标定位第一行 */
void app_settings_open(void)
{
    uint8_t i;

    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("Settings");
    atk_md0280_show_string(16, 28, 220, 12, (char *)"Move: line  [-] [+] then SW",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    s_row = 0;
    s_btn_hover = 0;
    s_dirty = 0;
    s_idle = 0;
    for (i = 0; i < SET_ROWS; i++) draw_row(i, i == s_row);

    cursor_init(SCR_W / 2, SET_Y0 + SET_ROW_H / 2);
    cursor_show();
}

/* 事件处理：换行选择 / 按钮 hover / 定时保存 / SW 按按钮或进校时 */
void app_settings_handle(input_event_t *ev)
{
    if (ev->type == EV_MOUSE_MOVE) {
        uint16_t cx, cy;
        uint8_t row, btn, hid;

        cursor_get_pos(&cx, &cy);
        row = hit_row(cy);
        if (row != 0xFF && row != s_row) {
            /* 换行：重绘旧行 ∪ 新行（并集矩形保护）；按钮 hover 归零重检 */
            uint16_t y0 = SET_Y0 + (row < s_row ? row : s_row) * SET_ROW_H - 2;
            uint16_t y1 = SET_Y0 + (row > s_row ? row : s_row) * SET_ROW_H + SET_ROW_H - 3;

            hid = redraw_protect_begin(0, y0, SCR_W - 1, y1);
            draw_row(s_row, 0);
            s_row = row;
            s_btn_hover = 0;
            draw_row(row, 1);
            redraw_protect_end(hid);
        }
        if (row == s_row) {
            /* 按钮 hover 变化 → 重绘整行（只在一行范围内，开销小） */
            btn = hit_btn(cx, cy);
            if (btn != s_btn_hover) {
                hid = redraw_protect_begin(0, SET_Y0 + s_row * SET_ROW_H - 2, SCR_W - 1,
                                           SET_Y0 + s_row * SET_ROW_H + SET_ROW_H - 3);
                s_btn_hover = btn;
                draw_row(s_row, 1);
                redraw_protect_end(hid);
            }
        }
    } else if (ev->type == EV_TICK) {
        /* 连续调节停止 2 秒 → 落盘（Flash 擦写 ~300ms 阻塞，不每格都存；
         * 持续拨动时 idle 一直被重置，不会卡） */
        if (s_dirty) {
            if (s_idle >= 2) { sys_cfg_save(); s_dirty = 0; }
            else s_idle++;
        }
    } else if (ev->type == EV_KEY_DOWN) {
        if (s_row >= 6) {
            /* Password/Set Clock 入口：未落盘修改先保存再跳转。跳转后 Settings
             * 由 enter_desktop 直接关闭（close 钩子不跑），不先存会丢修改 */
            if (s_dirty) { sys_cfg_save(); s_dirty = 0; }
            if (s_row == 6) desktop_enter_pwd_set();   /* 改密码（两遍确认） */
            else desktop_enter_clock_set();            /* 校时 */
        } else if (s_btn_hover == 1) {
            adjust(-1);          /* 光标在 [-] 上按 SW：减一格 */
        } else if (s_btn_hover == 2) {
            adjust(1);           /* 光标在 [+] 上按 SW：加一格 */
        }
        /* 光标没在按钮上：SW 无效，避免误触（值不能被盲调） */
    }
}

/* 退出钩子：K1 返回桌面时框架调用——未落盘的修改立即保存
 * （否则"调完 2 秒内退出"时 EV_TICK 不再来，修改丢失） */
void app_settings_close(void)
{
    if (s_dirty) { sys_cfg_save(); s_dirty = 0; }
}
