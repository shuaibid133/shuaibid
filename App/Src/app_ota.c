/**
 * @file    app_ota.c
 * @brief   OTA Update 应用（进阶⑥）：伪 OTA 的演示入口
 *
 * 主流程（Check Update）：无包 → 模拟下载（擦 2 扇区 → 逐块写 256B
 *   → 进度条 + 50ms/块模拟网络延迟）→ 读回 CRC 校验 → 应用（状态
 *   0x5A→0xA5）→ 提示 2s → 软复位 → 重启后 boot_check 版本生效。
 *
 * 回退路径（答辩重点）：
 *   - Inject Bad Pkg 注入坏包 → Check 发现包头 CRC 与镜像不符 →
 *     丢弃更新区 + ERR 日志 → 系统保持 V1.0——升级失败绝不让系统
 *     带病启动（与③配置校验、②启动回退同一工程思想）
 *   - 下载落盘校验失败 → 同样回退
 *
 * 线程：全部在 ui_task 事件循环（单写者）。擦写 2s/扇区会阻塞事件
 * 循环——正在升级，输入积压/丢弃可接受；但心跳哨兵会把"擦写阻塞"
 * 误判成任务卡死，所以流程全程用 sys_watch_set_interval 豁免窗口
 * （这是豁免机制的第二个应用场景：长阻塞操作前声明，哨兵不误报）。
 */
#include "app_ota.h"
#include "app.h"
#include "cursor.h"
#include "sys_ota.h"
#include "sys_watch.h"
#include "sys_log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f1xx_hal.h"     /* NVIC_SystemReset */
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include <string.h>

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* ---------- 布局 ---------- */
#define BTN_X0      60               /* 按钮（200×40） */
#define BTN_W       200
#define BTN_H       40
#define BTN_CHECK_Y 210              /* Check Update */
#define BTN_BAD_Y   262              /* Inject Bad Pkg */

static uint8_t s_btn_hover = 0;      /* 1=Check 2=InjectBad 0=无 */
static char s_info[32];              /* 状态行文本 */

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

/* ---------- 绘制 ---------- */
static void draw_current(void)
{
    /* "Current: Vx"（24 号）——无 sprintf 重库，手动拼 */
    uint8_t v = sys_ota_current_version();
    char buf[20];
    uint8_t i = 0;
    const char *t = "Current: V";

    while (*t) buf[i++] = *t++;
    if (v >= 10) buf[i++] = (char)('0' + v / 10);
    buf[i++] = (char)('0' + v % 10);
    buf[i] = 0;
    atk_md0280_show_string(16, 44, 220, 24, buf, ATK_MD0280_LCD_FONT_24, ATK_MD0280_BLACK);
}

static void draw_info(void)
{
    atk_md0280_fill(16, 76, 240, 96, ATK_MD0280_WHITE);    /* 清旧状态行 */
    atk_md0280_show_string(16, 78, 220, 16, s_info, ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
}

/* 进度条：label 在 y=104，外框 y=128~152，填充按 pct */
static void draw_progress(uint8_t pct, const char *label)
{
    atk_md0280_fill(16, 104, 240, 152, ATK_MD0280_WHITE);
    atk_md0280_show_string(16, 104, 220, 16, (char *)label, ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_draw_rect(16, 128, 236, 152, ATK_MD0280_BLUE);
    if (pct > 0)
        atk_md0280_fill(18, 130, (uint16_t)(18 + (216u * pct) / 100u - 1u), 150, ATK_MD0280_BLUE);
}

static void draw_btn(uint8_t which, const char *label)
{
    uint16_t y = (which == 1) ? BTN_CHECK_Y : BTN_BAD_Y;
    uint8_t on = (s_btn_hover == which);

    atk_md0280_fill(BTN_X0, y, BTN_X0 + BTN_W - 1, y + BTN_H - 1,
                    on ? ATK_MD0280_BLUE : ATK_MD0280_GRAY);
    atk_md0280_show_string(BTN_X0 + (BTN_W - strlen(label) * 16) / 2, y + 12,
                           BTN_W, 16, (char *)label, ATK_MD0280_LCD_FONT_16,
                           on ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
}

static uint8_t hit_btn(uint16_t cx, uint16_t cy)
{
    if (cx < BTN_X0 || cx > BTN_X0 + BTN_W - 1) return 0;
    if (cy >= BTN_CHECK_Y && cy <= BTN_CHECK_Y + BTN_H - 1) return 1;
    if (cy >= BTN_BAD_Y && cy <= BTN_BAD_Y + BTN_H - 1) return 2;
    return 0;
}

/* ---------- 流程（阻塞式：升级期间无交互，进度实时画） ---------- */

/* 应用 + 提示 + 软复位 */
static void apply_and_reboot(void)
{
    sys_watch_set_interval(WATCH_UI, 10000);   /* 擦写豁免 */

    draw_progress(0, "Applying...");
    if (!sys_ota_apply()) {
        strcpy(s_info, "Apply failed (no pkg)");
        sys_watch_set_interval(WATCH_UI, 2000);
        draw_info();
        return;
    }
    draw_progress(100, "Applied! Rebooting...");
    sys_watch_set_interval(WATCH_UI, 2000);
    vTaskDelay(2000);                          /* 提示停留 2s 再复位 */
    NVIC_SystemReset();
}

static void start_update(void)
{
    uint8_t r = sys_ota_check();               /* 先查更新区现状 */

    if (r == 2) {
        /* 已有坏包：sys_ota_check 已丢弃 + 记回退日志——回退完成 */
        strcpy(s_info, "Corrupt pkg discarded");
        draw_info();
        return;
    }
    if (r == 1) {
        /* 已有完好包（上次下载完没应用）：直接应用 */
        apply_and_reboot();
        return;
    }

    /* ---- 无包：完整下载流程（~7s，全程可见） ---- */
    sys_watch_set_interval(WATCH_UI, 10000);   /* Flash 擦写豁免窗口 */

    draw_progress(0, "Erasing 1/2...");
    sys_ota_download_erase();                  /* ~2s（克隆片实测 1.5~2s） */
    sys_watch_beat(WATCH_UI);                  /* 每步打卡：长阻塞也活着 */

    draw_progress(0, "Erasing 2/2...");
    sys_ota_download_erase_img();              /* ~2s */
    sys_watch_beat(WATCH_UI);

    while (sys_ota_download_step()) {          /* 逐块写镜像 */
        draw_progress(sys_ota_download_pct(), "Downloading...");
        sys_watch_beat(WATCH_UI);
        vTaskDelay(50);                        /* 模拟网络延迟（演示效果） */
    }

    draw_progress(100, "Verifying...");
    sys_watch_beat(WATCH_UI);
    if (!sys_ota_download_finish()) {          /* 落盘校验失败 → 回退 */
        sys_log_add(LOG_LV_ERR, LOG_OTA_ROLLBACK, 1);
        sys_watch_set_interval(WATCH_UI, 2000);
        strcpy(s_info, "Verify FAILED, rollback V1");
        draw_info();
        return;
    }
    sys_watch_set_interval(WATCH_UI, 2000);
    apply_and_reboot();
}

/* ---------- 应用入口 ---------- */
void app_ota_open(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("OTA Update");
    atk_md0280_show_string(16, 28, 220, 12, (char *)"SW on button to act",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    s_btn_hover = 0;
    strcpy(s_info, "No update");
    draw_current();
    draw_info();
    draw_btn(1, "Check Update");
    draw_btn(2, "Inject Bad");
    cursor_init(160, BTN_CHECK_Y + BTN_H / 2);
    cursor_show();
}

void app_ota_handle(input_event_t *ev)
{
    if (ev->type == EV_MOUSE_MOVE) {
        uint16_t cx, cy;
        uint8_t btn, hid;

        cursor_get_pos(&cx, &cy);
        btn = hit_btn(cx, cy);
        if (btn != s_btn_hover) {
            hid = redraw_protect_begin(BTN_X0, BTN_CHECK_Y - 2,
                                       BTN_X0 + BTN_W - 1, BTN_BAD_Y + BTN_H + 1);
            s_btn_hover = btn;
            draw_btn(1, "Check Update");
            draw_btn(2, "Inject Bad");
            redraw_protect_end(hid);
        }
    } else if (ev->type == EV_KEY_DOWN) {
        if (s_btn_hover == 1) {
            start_update();
        } else if (s_btn_hover == 2) {
            sys_ota_inject_bad();
            strcpy(s_info, "Bad pkg injected, Check again");
            draw_info();
        }
        /* 光标没在按钮上：SW 无效，避免误触 */
    }
}
