/**
 * @file    app_files.c
 * @brief   Files 应用：W25Q128 上的 FATFS 文件系统
 *
 * 功能：
 *   - 目录列表：文件名 + 大小，光标选行（蓝条高亮），边缘推出滚动
 *   - SW 按下：打开选中文件查看内容（文本视图，最多 800 字节）；
 *     查看态再按 SW 关闭回列表
 *   - K1 退出应用（EV_BACK 由框架接管）
 *   - 首次上电（未格式化）：f_mkfs 自动建 FAT 卷 + 预置演示文件
 *   - 文件时间戳来自内部 RTC（get_fattime → rtc_app_get_datetime）
 *
 * 线程模型：所有 FS 操作只在 ui_task 里执行（事件驱动 + 单写者原则），
 *           FF_FS_REENTRANT=0，FATFS 不需要互斥锁。
 */
#include "app.h"
#include "app_files.h"
#include "cursor.h"
#include "rtc_app.h"
#include "sys_stats.h"
#include "w25q128.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "ff.h"
#include "diskio.h"
#include <string.h>

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

/* ---------- 布局 ---------- */
#define LIST_Y0     56               /* 列表区起始 y（标题栏 24px 之下） */
#define ROW_H       18
#define LIST_ROWS   9                /* 一屏 9 行 */
#define VIEW_LINES  20               /* 查看模式：20 行 × 40 字符（FONT_12） */
#define VIEW_CHARS  40

/* ---------- 文件系统状态 ---------- */
static FATFS g_fs;
static uint8_t g_fs_ready = 0;
static uint8_t g_fs_err = 0;         /* 0=正常 1=Flash 初始化失败 2=格式化失败 */

/* ---------- 目录缓存（静态区：不能放 1KB 的任务栈） ---------- */
#define MAX_FILES 16
typedef struct {
    char name[13];                   /* 8.3 短名（FF_USE_LFN=0） */
    uint32_t size;
    uint8_t is_dir;
} file_item_t;
static file_item_t g_files[MAX_FILES];
static uint8_t g_file_count;         /* 已缓存条数 */
static uint8_t g_total_count;        /* 目录总条数（决定滚动范围） */
static uint8_t g_scroll;             /* 滚动偏移 */
static int8_t  g_sel;                /* 当前选中（绝对索引） */

/* ---------- 查看模式 ---------- */
static uint8_t g_viewing = 0;
static char g_view_raw[VIEW_LINES * VIEW_CHARS];   /* 原始内容缓冲 */
static char g_view_grid[VIEW_LINES][VIEW_CHARS + 1]; /* 压行后的网格 */
static uint8_t g_view_lines;
static char g_view_name[13];
static uint32_t g_view_size;

static char s_work[4096];            /* f_mkfs 工作缓冲（必须 ≥ 4KB） */

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

/* ---------- FATFS 时间戳：内部 RTC → FAT 时间格式 ---------- */
DWORD get_fattime(void)
{
    rtc_datetime_t dt;

    rtc_app_get_datetime(&dt);
    return ((DWORD)(dt.year - 1980) << 25) | ((DWORD)dt.month << 21) |
           ((DWORD)dt.day << 16) | ((DWORD)dt.hour << 11) |
           ((DWORD)dt.min << 5) | ((DWORD)dt.sec >> 1);
}

/* 写一行文本（自动补 \r\n；长度用 strlen 算，避免手数） */
static void write_line(FIL *f, const char *s)
{
    UINT bw, n = 0;

    while (s[n]) n++;
    if (f_write(f, s, n, &bw) == FR_OK) f_write(f, "\r\n", 2, &bw);
}

/* 十进制按固定位数写入（避开 sprintf 重库），调用方保证 buf 有 digits 位 */
static void fmt_dec(char *buf, uint8_t digits, uint32_t v)
{
    uint8_t i = digits;

    while (i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
}

/* 首次格式化后预置演示文件 */
static void create_demo(void)
{
    FIL f;
    rtc_datetime_t dt;
    char line[40];

    rtc_app_get_datetime(&dt);

    if (f_open(&f, "0:/README.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        write_line(&f, "MINI OS - File System Demo");
        write_line(&f, "FreeRTOS + FATFS on STM32F103ZET6");
        write_line(&f, "W25Q128 16MB @ SPI2 18MHz");
        memcpy(line, "Created: ", 9);
        fmt_dec(line + 9, 4, dt.year);  line[13] = '-';
        fmt_dec(line + 14, 2, dt.month); line[16] = '-';
        fmt_dec(line + 17, 2, dt.day);  line[19] = ' ';
        fmt_dec(line + 20, 2, dt.hour); line[22] = ':';
        fmt_dec(line + 23, 2, dt.min);  line[25] = ':';
        fmt_dec(line + 26, 2, dt.sec);  line[28] = 0;
        write_line(&f, line);
        write_line(&f, "SW: open / close file, K1: exit");
        write_line(&f, "This file proves FATFS works.");
        f_close(&f);
    }

    if (f_open(&f, "0:/DATA.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        uint8_t i;
        write_line(&f, "MINI OS sample data 0-31:");
        for (i = 0; i < 4; i++) {        /* 4 行 × 8 个数 */
            uint8_t j, pos = 0;
            for (j = 0; j < 8; j++) {
                uint8_t v = i * 8 + j;
                line[pos++] = '0' + v / 10;
                line[pos++] = '0' + v % 10;
                line[pos++] = ' ';
            }
            line[pos - 1] = 0;           /* 去掉末尾空格 */
            write_line(&f, line);
        }
        f_close(&f);
    }
}

/* 挂载（首次进入调用一次）：未格式化自动 f_mkfs + 建演示文件
 * 进度提示：首次进入格式化要 3~10 秒（4K 擦除逐块进行），期间界面
 * 没有内容也没有光标——看起来像死机。这里在屏幕中部逐阶段显示进度。 */
static void fs_init(void)
{
    FRESULT fr;

    if (g_fs_ready || g_fs_err) return;

    atk_md0280_show_string(8, 120, 200, 16, (char *)"Flash init...",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    if (disk_initialize(0) != 0) {       /* Flash 读 ID 失败（没焊/坏片） */
        g_fs_err = 1;
        g_stats_errors++;
        return;
    }

    atk_md0280_show_string(8, 120, 200, 16, (char *)"Mounting...",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    fr = f_mount(&g_fs, "0:", 1);
    if (fr == FR_NO_FILESYSTEM) {        /* 新片：建卷 + 演示文件 */
        MKFS_PARM mpar = {FM_FAT, 0, 0, 0, 0};
        atk_md0280_show_string(8, 120, 200, 16, (char *)"Formatting flash...",
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        fr = f_mkfs("0:", &mpar, s_work, sizeof(s_work));
        if (fr == FR_OK) {
            f_mount(NULL, "0:", 0);      /* 重新挂载新卷 */
            fr = f_mount(&g_fs, "0:", 1);
        }
        if (fr == FR_OK) create_demo();
        else {
            g_fs_err = 2;
            g_stats_errors++;
            return;
        }
    } else if (fr != FR_OK) {
        g_fs_err = 2;
        g_stats_errors++;
        return;
    }
    g_fs_ready = 1;
}

/* 刷新目录缓存 */
static void list_refresh(void)
{
    DIR dj;
    FILINFO fi;
    uint8_t max_scroll;

    g_file_count = 0;
    g_total_count = 0;
    if (f_opendir(&dj, "0:/") != FR_OK) return;
    while (f_readdir(&dj, &fi) == FR_OK && fi.fname[0] != 0) {
        if (g_total_count < 255) g_total_count++;
        if (g_file_count < MAX_FILES) {
            memcpy(g_files[g_file_count].name, fi.fname, 13);
            g_files[g_file_count].size = fi.fsize;
            g_files[g_file_count].is_dir = (fi.fattrib & AM_DIR) ? 1 : 0;
            g_file_count++;
        }
    }
    f_closedir(&dj);
    if (g_sel >= g_file_count) g_sel = (int8_t)(g_file_count - 1);
    max_scroll = (g_total_count > LIST_ROWS) ? g_total_count - LIST_ROWS : 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
}

/* 列表区整块重绘（选中行蓝底白字） */
static void draw_list(void)
{
    uint8_t i, hid;

    hid = redraw_protect_begin(0, LIST_Y0 - 3, SCR_W - 1, LIST_Y0 + LIST_ROWS * ROW_H - 3);
    atk_md0280_fill(0, LIST_Y0 - 3, SCR_W - 1, LIST_Y0 + LIST_ROWS * ROW_H - 3, ATK_MD0280_WHITE);
    for (i = 0; i < LIST_ROWS; i++) {
        uint8_t fi = g_scroll + i;
        uint16_t y;

        if (fi >= g_file_count) break;
        y = LIST_Y0 + i * ROW_H;
        if (fi == g_sel) {
            atk_md0280_fill(0, y - 2, SCR_W - 1, y + ROW_H - 3, ATK_MD0280_BLUE);
        }
        atk_md0280_show_string(8, y, 160, 16, g_files[fi].name, ATK_MD0280_LCD_FONT_16,
                               (fi == g_sel) ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
        if (g_files[fi].is_dir) {
            atk_md0280_show_string(168, y + 2, 60, 12, (char *)"[DIR]", ATK_MD0280_LCD_FONT_12,
                                   (fi == g_sel) ? ATK_MD0280_WHITE : ATK_MD0280_GRAY);
        } else {
            atk_md0280_show_xnum(168, y + 2, g_files[fi].size, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                                 ATK_MD0280_LCD_FONT_12,
                                 (fi == g_sel) ? ATK_MD0280_WHITE : ATK_MD0280_GRAY);
        }
    }
    redraw_protect_end(hid);
}

/* 状态区（列表之下）：Flash / 剩余空间 / 文件数 / 操作提示 */
static void draw_status(void)
{
    uint32_t fre = 0, kb = 0;
    FATFS *pf = NULL;

    atk_md0280_fill(0, 222, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    if (g_fs_err == 1) {
        atk_md0280_show_string(8, 226, 200, 16, (char *)"Flash error!", ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
    } else if (g_fs_err == 2) {
        atk_md0280_show_string(8, 226, 200, 16, (char *)"FS init failed!", ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
    } else if (g_fs_ready) {
        if (f_getfree("0:", &fre, &pf) == FR_OK) kb = fre * pf->csize / 2;  /* 512B 扇区 → KB */
        atk_md0280_show_string(8, 226, 140, 16, (char *)"Flash: W25Q128", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(8, 246, 44, 16, (char *)"Free", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_xnum(48, 246, kb, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                             ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(112, 246, 24, 16, (char *)"KB", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(8, 266, 120, 16, (char *)"Files:", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_xnum(64, 266, g_total_count, 3, ATK_MD0280_NUM_SHOW_NOZERO,
                             ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    }
    atk_md0280_show_string(8, 302, 150, 12, (char *)"SW: open  K1: exit", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
}

/* 打开选中文件（文本视图） */
static void open_file(void)
{
    FIL f;
    UINT br = 0;
    char path[16];
    uint16_t i;
    uint8_t line = 0, col = 0;

    memcpy(g_view_name, g_files[g_sel].name, 13);
    g_view_size = g_files[g_sel].size;
    memcpy(path, "0:/", 3);
    memcpy(path + 3, g_files[g_sel].name, 13);   /* 连 '\0' 一起拷，path 共 16 字节 */
    g_viewing = 1;

    memset(g_view_raw, 0, sizeof(g_view_raw));
    memset(g_view_grid, 0, sizeof(g_view_grid));
    if (f_open(&f, path, FA_READ) == FR_OK) {
        if (f_read(&f, g_view_raw, sizeof(g_view_raw), &br) != FR_OK) br = 0;
        f_close(&f);
    }
    /* 压成 40 列网格：丢弃 \r\n，其余照抄 */
    for (i = 0; i < br; i++) {
        char c = g_view_raw[i];
        if (c == '\r' || c == '\n') continue;
        if (col == VIEW_CHARS) { line++; col = 0; }
        if (line >= VIEW_LINES) break;
        g_view_grid[line][col++] = c;
    }
    g_view_lines = line + (col > 0 ? 1 : 0);
}

/* 文本视图重绘 */
static void draw_view(void)
{
    uint8_t i, hid;

    hid = redraw_protect_begin(0, 25, SCR_W - 1, SCR_H - 1);
    atk_md0280_fill(0, 25, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    atk_md0280_show_string(8, 30, 150, 16, g_view_name, ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(170, 32, g_view_size, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    for (i = 0; i < g_view_lines && i < VIEW_LINES; i++) {
        if (g_view_grid[i][0] == 0) break;   /* 空行跳过（内容结束） */
        atk_md0280_show_string(4, 52 + i * 13, 240, 12, g_view_grid[i],
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_BLACK);
    }
    if (g_view_size > VIEW_LINES * VIEW_CHARS) {
        atk_md0280_show_string(4, 290, 120, 12, (char *)"...(truncated)", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    }
    atk_md0280_show_string(8, 306, 150, 12, (char *)"SW: close  K1: exit", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    redraw_protect_end(hid);
}

/* 光标位置 → 选中行（含边缘滚动） */
static void update_selection(void)
{
    uint16_t cx, cy;
    int r, sel;

    cursor_get_pos(&cx, &cy);
    r = ((int)cy + 27 - LIST_Y0) / ROW_H;    /* 光标框中心所在行（可越界） */
    if (r > LIST_ROWS - 1 && g_scroll + LIST_ROWS < g_total_count) {
        g_scroll++;                          /* 推到列表下边缘：往下滚动 */
        draw_list();
        return;
    }
    if (r < 0 && g_scroll > 0) {
        g_scroll--;                          /* 推出上边缘：往上滚动 */
        draw_list();
        return;
    }
    sel = g_scroll + (r < 0 ? 0 : (r > LIST_ROWS - 1 ? LIST_ROWS - 1 : r));
    if (sel > g_file_count - 1) sel = g_file_count - 1;
    if (sel < 0) sel = 0;
    if (sel != g_sel) {
        g_sel = (int8_t)sel;
        draw_list();
    }
}

void app_files_handle(input_event_t *ev)
{
    switch (ev->type) {
    case EV_KEY_DOWN:                        /* SW：打开/关闭文件 */
        if (!g_fs_ready || g_fs_err) break;
        if (g_viewing) {
            g_viewing = 0;
            draw_list();                     /* 回列表（状态区不变） */
        } else if (g_file_count > 0 && !g_files[g_sel].is_dir) {
            open_file();
            draw_view();
        }
        break;

    case EV_MOUSE_MOVE:                      /* 光标已由框架移动：同步选中行 */
        if (!g_viewing && g_fs_ready) update_selection();
        break;

    default:
        break;   /* EV_TICK / EV_DEV_TOGGLE 不关心；EV_BACK 由框架接管 */
    }
}

void app_files_open(void)
{
    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("Files");
    fs_init();                               /* 首次进入可能格式化（约 1-2s） */
    list_refresh();
    draw_list();
    draw_status();
    cursor_init(35, LIST_Y0 - 18);           /* 光标框中心对齐第 0 行 */
    cursor_show();
}
