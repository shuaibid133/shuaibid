/**
 * @file    app_paint.c
 * @brief   Paint 应用：画图 + FATFS 保存/加载（重新上电后可查看）
 *
 * 功能（对照题目）：
 *   - 画笔：SW 按一次进入画线模式（状态区显示 DRW），此后摇杆移动即画线
 *     （3px 粗笔头）；再按一次退出。不用"按住拖动"——摇杆按键手感软，
 *     按住易松脱/抖动，开关式更稳定
 *   - 颜色：6 色格（黑/红/蓝/绿/黄/白=橡皮），光标点选
 *   - 清空：[CLR] 按钮（画布刷白）
 *   - 保存：[SAVE] → 扫描根目录找最大 IMGxxx.IMG 编号 +1 → 从 LCD 逐行
 *     读回像素写新文件（自动编号，不覆盖旧图）
 *   - 画板：每次进入都是空白（未保存的画退出即丢弃，不做自动加载——
 *     想要保留的画点 SAVE 存成编号文件，之后在 Files 里随时查看）
 *   - 预览：Files 应用打开 IMG*.IMG 时识别 "KP1" magic 直接显示图像，
 *     用 atk_md0280_write_area 整行批量写屏（~10ms，无逐行刷新感）
 *     （二进制像素文件用文本查看器只会看到乱码，见 app_files.c draw_img_view）
 *
 * 存储格式（0:/IMGxxx.IMG）：3 字节 magic "KP1" + 画布像素流
 *   （RGB565 小端，240×220，行优先）。无内嵌宽高——画布尺寸固定，
 *   读取时按固定尺寸解析；magic 校验防读入非图片文件。
 *
 * 内存：画布不缓存（240×220×2 ≈ 103KB 超 64KB SRAM），直接画在 LCD，
 *       保存时读 GRAM（BSP atk_md0280_read_point），只有一行缓冲 480B。
 *
 * 线程模型：FS 操作只在 ui_task 事件循环内（单写者，与 Files 同模式）；
 *       保存/加载约 0.5~1.5s 阻塞（W25Q 擦写 + 读 GRAM），期间事件队列
 *       堆积在保存后处理，可接受。FF_FS_REENTRANT=0 无需互斥锁。
 *
 * FATFS：与 Files 共用全局挂载对象 g_fs（app_files.h 声明），f_mount
 *       幂等；卷不存在（从未格式化）时自动 f_mkfs 建卷。
 */
#include "app_paint.h"
#include "app.h"
#include "cursor.h"
#include "app_files.h"
#include "ff.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include <string.h>

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* ---------- 布局 ---------- */
#define CANVAS_Y0   24               /* 画布：标题栏之下，x 0..239 */
#define CANVAS_Y1   243
#define COLOR_Y0    250              /* 色格行 */
#define COLOR_Y1    280
#define COLOR_X0    4                /* 色格起点 x，每格 36px 间距 3px */
#define COLOR_STEP  39
#define COLOR_N     6
#define BTN_Y0      286              /* CLR/SAVE 按钮行 */
#define BTN_Y1      316
#define CLR_X       4                /* [CLR] x 4..103 */
#define SAVE_X      108              /* [SAVE] x 108..207 */
#define BTN_W       100
#define STAT_X      212              /* 状态文字区（3 字符）x 212..239 */

/* ---------- 画笔颜色表 ---------- */
static const uint16_t s_colors[COLOR_N] = {
    ATK_MD0280_BLACK, ATK_MD0280_RED, ATK_MD0280_BLUE,
    ATK_MD0280_GREEN, ATK_MD0280_YELLOW, ATK_MD0280_WHITE   /* 白 = 橡皮 */
};

/* ---------- 状态 ---------- */
static uint8_t  s_color_idx = 0;     /* 当前颜色索引 */
static uint8_t  s_drawing = 0;       /* 按住 SW 画线中 */
static int8_t   s_hover = -1;        /* 工具栏 hover：0..5 色格 6=CLR 7=SAVE -1=无 */
static uint16_t s_lx = 0, s_ly = 0;  /* 上一笔位置（连线用） */
static const char *s_status = "";    /* 状态文字（"OK!"/"ERR"/"NEW"） */
static uint8_t  s_status_sec = 0;    /* 状态文字剩余显示秒数 */
static uint8_t  s_line[SCR_W * 2];   /* 行缓冲 480B（保存/加载逐行读写） */

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

/* ---------- FATFS 卷确保：已挂载则幂等；未格式化则自动建卷 ---------- */
static uint8_t s_mkfs_work[4096];   /* f_mkfs 工作缓冲（必须 ≥ 4KB，FAT12 卷一次性使用） */

static uint8_t fs_ensure(void)
{
    MKFS_PARM mpar = {FM_FAT, 0, 0, 0, 0};

    if (f_mount(&g_fs, "0:", 1) == FR_OK) return 1;
    /* 挂载失败 = 卷未格式化：建 FAT12 卷再挂（Files 已完成首次格式化时不会走到） */
    if (f_mkfs("0:", &mpar, s_mkfs_work, sizeof(s_mkfs_work)) == FR_OK)
        return f_mount(&g_fs, "0:", 1) == FR_OK;
    return 0;
}

/* ---------- 画布 ---------- */

/* 落笔/连线：1px 连线 + 3px 笔头方块（边界钳位，防止 fill 参数下溢） */
static void paint_stroke(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t xa, ya, xb, yb;

    atk_md0280_draw_line(x0, y0, x1, y1, s_colors[s_color_idx]);
    xa = (x1 > 0) ? (uint16_t)(x1 - 1) : 0;
    ya = (y1 > CANVAS_Y0) ? (uint16_t)(y1 - 1) : CANVAS_Y0;
    xb = (x1 < SCR_W - 1) ? (uint16_t)(x1 + 1) : (uint16_t)(SCR_W - 1);
    yb = (y1 < CANVAS_Y1) ? (uint16_t)(y1 + 1) : CANVAS_Y1;
    atk_md0280_fill(xa, ya, xb, yb, s_colors[s_color_idx]);
}

/* 清空画布 */
static void paint_clear(void)
{
    atk_md0280_fill(0, CANVAS_Y0, SCR_W - 1, CANVAS_Y1, ATK_MD0280_WHITE);
}

/* ---------- 保存 ---------- */

/* 扫描根目录找最大 IMGxxx.IMG 编号，返回下一个（1..999，999 后回 1） */
static uint16_t next_img_no(void)
{
    DIR dj;
    FILINFO fi;
    uint16_t max_no = 0;

    if (f_opendir(&dj, "0:/") != FR_OK) return 1;
    while (f_readdir(&dj, &fi) == FR_OK && fi.fname[0] != 0) {
        char *n = fi.fname;   /* 8.3 短名（FATFS 存大写） */

        if (n[0] == 'I' && n[1] == 'M' && n[2] == 'G' &&
            n[3] >= '0' && n[3] <= '9' && n[4] >= '0' && n[4] <= '9' &&
            n[5] >= '0' && n[5] <= '9' && n[6] == '.' &&
            n[7] == 'I' && n[8] == 'M' && n[9] == 'G') {
            uint16_t no = (uint16_t)((n[3] - '0') * 100 + (n[4] - '0') * 10 + (n[5] - '0'));

            if (no > max_no) max_no = no;
        }
    }
    if (max_no >= 999) return 1;
    return (uint16_t)(max_no + 1);
}

/* 保存：自动编号新文件 IMGxxx.IMG，读 GRAM 逐行写（小端 RGB565）。
 * 编号扫描在 FATFS 目录缓存上做（几十项），耗时 <1ms 可忽略 */
static uint8_t paint_save(void)
{
    FIL f;
    UINT bw;
    uint16_t x, y, no;
    uint8_t ok = 1;
    uint8_t hdr[3] = { 'K', 'P', '1' };
    char path[16];

    if (!fs_ensure()) return 0;
    no = next_img_no();
    path[0] = '0'; path[1] = ':'; path[2] = '/';
    path[3] = 'I'; path[4] = 'M'; path[5] = 'G';
    path[6] = (char)('0' + no / 100);
    path[7] = (char)('0' + (no / 10) % 10);
    path[8] = (char)('0' + no % 10);
    path[9] = '.'; path[10] = 'I'; path[11] = 'M'; path[12] = 'G';
    path[13] = 0;
    cursor_hide();                       /* 光标不入画 */
    if (f_open(&f, path, FA_CREATE_NEW | FA_WRITE) == FR_OK) {
        if (f_write(&f, hdr, 3, &bw) != FR_OK || bw != 3) ok = 0;
        for (y = CANVAS_Y0; ok && y <= CANVAS_Y1; y++) {
            for (x = 0; x < SCR_W; x++) {
                uint16_t c = atk_md0280_read_point(x, y);
                s_line[x * 2] = (uint8_t)(c & 0xFF);
                s_line[x * 2 + 1] = (uint8_t)(c >> 8);
            }
            if (f_write(&f, s_line, SCR_W * 2, &bw) != FR_OK || bw != SCR_W * 2) ok = 0;
        }
        f_close(&f);
    } else {
        ok = 0;   /* 编号已占用或卷满：自动编号冲突理论不存在（刚扫过），
                   * 卷满则 FS 报错，用户需在 Files 删旧图 */
    }
    cursor_show();
    return ok;
}

/* ---------- 工具栏 ---------- */

/* 状态文字区（SAVE 按钮右侧 3 字符位）：
 * 保存结果（2 秒）优先，其次画线模式指示 DRW，否则空白 */
static void draw_status_area(void)
{
    atk_md0280_fill(STAT_X, BTN_Y0, SCR_W - 1, BTN_Y1, 0xDEFB);
    if (s_status_sec > 0 && s_status[0] != 0)
        atk_md0280_show_string(STAT_X, BTN_Y0 + 9, 28, 16, (char *)s_status,
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_BLACK);
    else if (s_drawing)
        atk_md0280_show_string(STAT_X, BTN_Y0 + 9, 28, 16, (char *)"DRW",
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_RED);
}

/* 色格：外框 2px（选中=蓝，未选=浅灰），内实心色 */
static void draw_color_cell(int8_t i)
{
    uint16_t x = COLOR_X0 + i * COLOR_STEP;

    atk_md0280_fill(x, COLOR_Y0, (uint16_t)(x + 35), COLOR_Y1,
                    (i == s_color_idx) ? ATK_MD0280_BLUE : 0xBDF7);
    atk_md0280_fill((uint16_t)(x + 2), (uint16_t)(COLOR_Y0 + 2),
                    (uint16_t)(x + 33), (uint16_t)(COLOR_Y1 - 2), s_colors[i]);
}

/* CLR/SAVE 按钮：hover=蓝底白字，否则浅灰底黑字 */
static void draw_cmd_btn(int8_t i)
{
    uint16_t x = (i == 6) ? CLR_X : SAVE_X;
    uint8_t hover = (s_hover == i);
    const char *lbl = (i == 6) ? "CLR" : "SAVE";

    atk_md0280_fill(x, BTN_Y0, (uint16_t)(x + BTN_W - 1), BTN_Y1,
                    hover ? ATK_MD0280_BLUE : 0xBDF7);
    atk_md0280_show_string((uint16_t)(x + (BTN_W - strlen(lbl) * 8) / 2),
                           (uint16_t)(BTN_Y0 + 9), BTN_W, 16, (char *)lbl,
                           ATK_MD0280_LCD_FONT_12,
                           hover ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
}

/* 整个工具栏重绘（色格 + 按钮 + 状态区） */
static void draw_toolbar(void)
{
    int8_t i;

    atk_md0280_fill(0, COLOR_Y0, SCR_W - 1, BTN_Y1, 0xDEFB);   /* 工具栏底色 */
    for (i = 0; i < COLOR_N; i++) draw_color_cell(i);
    draw_cmd_btn(6);
    draw_cmd_btn(7);
    draw_status_area();
}

/* 工具栏变化后重绘（光标保护：避免光标区域被刷掉） */
static void toolbar_redraw(void)
{
    uint8_t hid = redraw_protect_begin(0, COLOR_Y0, SCR_W - 1, BTN_Y1);

    draw_toolbar();
    redraw_protect_end(hid);
}

/* 光标尖端 → 工具栏按钮：0..5 色格 6=CLR 7=SAVE，不在工具栏返回 -1 */
static int8_t hit_btn(uint16_t cx, uint16_t cy)
{
    if (cy >= COLOR_Y0 && cy <= COLOR_Y1 && cx >= COLOR_X0) {
        int8_t i = (int8_t)((cx - COLOR_X0) / COLOR_STEP);
        if (i < COLOR_N && cx <= COLOR_X0 + i * COLOR_STEP + 35) return i;
        return -1;
    }
    if (cy >= BTN_Y0 && cy <= BTN_Y1) {
        if (cx >= CLR_X && cx <= CLR_X + BTN_W - 1) return 6;
        if (cx >= SAVE_X && cx <= SAVE_X + BTN_W - 1) return 7;
    }
    return -1;
}

/* ---------- 应用接口 ---------- */

void app_paint_open(void)
{
    atk_md0280_clear(ATK_MD0280_WHITE);
    app_draw_title("Paint");
    s_drawing = 0;
    s_hover = -1;
    s_color_idx = 0;
    s_status = "";
    s_status_sec = 0;
    draw_toolbar();              /* 画布恒为空白画板：保存的画在 Files 查看 */

    cursor_init(SCR_W / 2, (CANVAS_Y0 + CANVAS_Y1) / 2);
    cursor_show();
}

void app_paint_handle(input_event_t *ev)
{
    uint16_t cx, cy;
    int8_t btn;

    if (ev->type == EV_MOUSE_MOVE) {
        cursor_get_pos(&cx, &cy);
        if (s_drawing) {
            if (cy >= CANVAS_Y0 && cy <= CANVAS_Y1) {
                if (s_lx == 0xFFFF) {
                    /* 刚从画布外回来：锚点失效，只落笔头不连线（防跨工具栏拉线） */
                    uint16_t xa = (cx > 0) ? (uint16_t)(cx - 1) : 0;
                    uint16_t ya = (cy > CANVAS_Y0) ? (uint16_t)(cy - 1) : CANVAS_Y0;
                    uint16_t xb = (cx < SCR_W - 1) ? (uint16_t)(cx + 1) : (uint16_t)(SCR_W - 1);
                    uint16_t yb = (cy < CANVAS_Y1) ? (uint16_t)(cy + 1) : CANVAS_Y1;

                    atk_md0280_fill(xa, ya, xb, yb, s_colors[s_color_idx]);
                } else {
                    paint_stroke(s_lx, s_ly, cx, cy);   /* 连续画线 */
                }
                s_lx = cx;
                s_ly = cy;
            } else {
                s_lx = 0xFFFF;   /* 光标在画布外：标记断线，回画布后不拉长线 */
            }
        }
        /* 工具栏 hover 变化 → 重绘按钮（色格 hover 不区分，只 CLR/SAVE） */
        btn = hit_btn(cx, cy);
        if (btn != s_hover) {
            if (btn < 0 || btn >= 6 || s_hover < 0 || s_hover >= 6) {
                s_hover = btn;
                toolbar_redraw();
            } else {
                s_hover = btn;   /* 色格间移动：无视觉变化，不重绘 */
            }
        }
    } else if (ev->type == EV_KEY_DOWN) {
        cursor_get_pos(&cx, &cy);
        if (cy >= CANVAS_Y0 && cy <= CANVAS_Y1) {
            /* 画线模式开关：按一次进入（此后移动即画），再按一次退出。
             * 进入/退出都刷新状态区（DRW 指示），不落笔点 */
            s_drawing = s_drawing ? 0 : 1;
            s_lx = cx;
            s_ly = cy;
            toolbar_redraw();
        } else {
            btn = hit_btn(cx, cy);
            if (btn >= 0 && btn < COLOR_N) {
                s_color_idx = btn;
                s_status = "";
                s_status_sec = 0;
                toolbar_redraw();
            } else if (btn == 6) {
                paint_clear();
            } else if (btn == 7) {
                /* 先显示 SAV 再保存：保存约 0.6s（W25Q 页编程物理时间 +
                 * 位带 SPI 读写），无反馈的死等体验差；结束后换 OK!/ERR */
                s_status = "SAV";
                s_status_sec = 2;
                toolbar_redraw();
                if (paint_save()) { s_status = "OK!"; s_status_sec = 2; }
                else              { s_status = "ERR"; s_status_sec = 2; }
                toolbar_redraw();
            }
        }
    } else if (ev->type == EV_TICK) {
        /* 状态文字超时清除 */
        if (s_status_sec > 0) {
            s_status_sec--;
            if (s_status_sec == 0) {
                s_status = "";
                toolbar_redraw();
            }
        }
    }
}
