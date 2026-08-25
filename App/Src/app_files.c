/**
 * @file    app_files.c
 * @brief   Files 应用：W25Q128 上的 FATFS 文件系统
 *
 * 功能（对照题目）：
 *   - 文件列表：文件名 + 大小，光标尖端选行（蓝条高亮），边缘推出滚动
 *   - 新建文件：点 [New] 自动编号 FILE001.TXT→FILE999.TXT，
 *     重名检测（f_stat 查重，重名则编号+1），写入模板内容（含 RTC 时间戳）
 *   - 删除文件：点 [Del] 进入确认态（红字提示），停在 [Del] 上按 SW 取消，
 *     光标移到别处按 SW 确认删除（f_unlink）；K1 被桌面框架接管收不到
 *   - 读写保存：查看 = 读（文本视图最多 800 字节；识别 "KP1" magic 的
 *     Paint 图片文件 → 图片预览逐行读回写屏）；断电后文件保持
 *     （初始化时 README.TXT 标志文件验证，见 fs_init）
 *   - 文件表：内存缓存 = 目录的缓存视图，每条 5 个字段对照题目——
 *     name=文件名 type=文件类型 size=文件大小 cluster=存储位置(FAT 起始簇)
 *     status=状态；数据来自 FATFS 目录项（FATFS = 文件表等效持久结构）
 *   - 首次上电（无标志文件）：f_mkfs 自动建 FAT 卷 + 预置演示文件
 *   - 文件时间戳来自内部 RTC（get_fattime → rtc_app_get_datetime）
 *   - K1 退出应用（EV_BACK 由桌面框架直接接管，应用收不到；
 *     删除确认态取消 = 再次点击 [Del]，见 draw_toolbar 提示 "Delete? SW=Y Del=N"）
 *
 * 线程模型：所有 FS 操作只在 ui_task 里执行（事件驱动 + 单写者原则），
 *           FF_FS_REENTRANT=0，FATFS 不需要互斥锁。
 */
#include "app.h"
#include "sys_log.h"
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

/* 工具栏（New/Del 按钮，位于列表与状态区之间） */
#define TB_Y0       226
#define TB_H        28
#define BTN_W       44
#define BTN_H       24
#define BTN_NEW_X   8                /* [New] 按钮矩形 */
#define BTN_DEL_X   (BTN_NEW_X + BTN_W + 6)   /* [Del] 按钮矩形 */

/* ---------- 文件系统状态 ---------- */
FATFS g_fs;   /* 全局共享挂载对象（app_files.h 声明 extern）：Files/Paint 共用，
               * FATFS 同一卷同一时刻只能有一个对象挂载 */
static uint8_t g_fs_ready = 0;
static uint8_t g_fs_err = 0;         /* 0=正常 1=初始化失败 2=格式化失败 3=读写自检失败 */
static uint32_t g_rw_bad = 0;        /* 自检首个错误字节地址（err==3 时显示） */

/* ---------- 文件表（静态区：不能放 1KB 的任务栈） ----------
 * 内存文件表 = FATFS 根目录的缓存视图，每条 5 字段对照题目要求：
 *   name    文件名（8.3 短名）
 *   type    文件类型：0=普通文件 1=目录
 *   size    文件大小（字节）
 *   cluster 存储位置：FAT 起始簇（目录项记录，f_stat 填充）
 *   status  状态：0=正常（预留：文件表与目录同步时标记） */
#define MAX_FILES 16
#define FILE_T_FILE  0
#define FILE_T_DIR   1
#define FILE_S_OK    0
typedef struct {
    char     name[13];
    uint8_t  type;
    uint8_t  status;
    uint32_t size;
    uint32_t cluster;
} file_item_t;
static file_item_t g_files[MAX_FILES];
static uint8_t g_file_count;         /* 已缓存条数 */
static uint8_t g_total_count;        /* 目录总条数（决定滚动范围） */
static uint8_t g_scroll;             /* 滚动偏移 */
static int8_t  g_sel;                /* 当前选中（绝对索引，-1=无选中；与光标位置绑定，
                                      * 光标移出列表即 -1，仅用于列表高亮/打开） */
static int8_t  g_sel_last;           /* 最后选中的文件（Del 按钮用：选中后光标移到
                                      * 工具栏时 g_sel 已 -1，删除仍针对最后选中者） */
static uint8_t g_del_arm = 0;        /* 删除确认态：1=等待 SW 确认 / K1 取消 */
static uint8_t g_tb_hover = 0;       /* 工具栏 hover：1=[New] 2=[Del] 0=无 */

/* ---------- 查看模式 ---------- */
static uint8_t g_viewing = 0;
static uint8_t g_view_img = 0;          /* 查看模式：1=图片（PAINT.IMG）0=文本 */
static char g_view_raw[VIEW_LINES * VIEW_CHARS];   /* 原始内容缓冲 */
static char g_view_grid[VIEW_LINES][VIEW_CHARS + 1]; /* 压行后的网格 */
static uint8_t g_view_lines;
static uint8_t g_img_buf[240 * 2 * 8];  /* 图片查看缓冲：8 行 3840B（批量读/写，减少 SPI 扇区读取次数） */
static char g_view_name[13];
static uint32_t g_view_size;

static char s_work[4096];            /* f_mkfs 工作缓冲（必须 ≥ 4KB） */

/* 前置声明：new_file 在 draw_status 定义之前调用它（C90 无隐式声明，
 * 缺声明会先隐式成 int 返回类型，与后面的 static void 定义冲突） */
static void draw_status(void);

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

/* 首次格式化/升级后预置演示文件（内容贴项目背景：2026 电子科技协会考核）。
 * README.TXT 第一段含 "KazepOS" 标记——fs_init 用它在升级时识别旧版
 * 演示文件（旧内容只有重建演示文件，不碰用户新建的文件） */
static void create_demo(void)
{
    FIL f;
    rtc_datetime_t dt;
    char line[40];

    rtc_app_get_datetime(&dt);

    if (f_open(&f, "0:/README.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        write_line(&f, "KazepOS - File System Demo");
        write_line(&f, "2026 ETAA Interview Project");
        write_line(&f, "Electronic Technology Association");
        write_line(&f, "admission assessment");
        write_line(&f, "STM32F103ZET6 Elite Board");
        write_line(&f, "FreeRTOS + FATFS on W25Q128");
        write_line(&f, "FSMC 16-bit 240x320 LCD");
        write_line(&f, "Joystick input, SW select, K1 back");
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
        write_line(&f, "KazepOS sample data 0-31:");
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

/* 裸读写自检：在 Flash 末尾 4KB 扇区做 擦→写→读→比对。
 * 克隆芯片的 JEDEC ID 可以过宽松检查，但 0x20 擦除/0x02 页编程/0x03
 * 读三条通路必须实测——写错擦错会导致文件系统数据损坏 */
static uint8_t rw_self_test(void)
{
    static const uint8_t pat[16] = { 0x55, 0xAA, 0x00, 0xFF, 0x5A, 0xA5, 0x0F, 0xF0,
                                     0x55, 0xAA, 0x00, 0xFF, 0x5A, 0xA5, 0x0F, 0xF0 };
    uint8_t rd[16];
    uint16_t i;
    const uint32_t addr = 0xFFF000u;        /* 末 4KB 扇区，与主区不冲突 */

    w25q128_erase_sector(addr);
    w25q128_write(addr, pat, sizeof(pat));
    w25q128_read(addr, rd, sizeof(rd));
    for (i = 0; i < sizeof(rd); i++) {
        if (rd[i] != pat[i]) {
            g_rw_bad = addr + i;            /* 记录首个错误字节地址 */
            return 0;
        }
    }
    return 1;
}

/* 挂载（首次进入调用一次）：无条件重建卷 + 建演示文件
 * 为什么无条件：Flash 出厂可能带测试残留数据，f_mount 会把它误认成
 * FAT 卷直接挂载成功（现象：目录项乱码——"system"、空名文件），
 * 此时 FR_NO_FILESYSTEM 检测永不触发 → 永远得不到干净卷。首次进入
 * 直接 f_mkfs 重建，一劳永逸。
 * 进度提示：格式化要 3~10 秒（4K 擦除逐块进行），期间界面没有内容
 * 也没有光标——看起来像死机。这里在屏幕中部显示进度。 */
static void fs_init(void)
{
    FRESULT fr;

    if (g_fs_ready || g_fs_err) return;

    /* 位带初始化 + 显示 JEDEC ID（克隆片允许非原厂 ID，只做粗略判断） */
    {
        uint8_t init_ok = w25q128_init();      /* 位带驱动初始化（幂等） */
        uint32_t id = w25q128_read_id();
        char buf[9], *p = buf + 8;
        uint8_t i;
        *p = 0;
        for (i = 0; i < 8; i++) {
            uint8_t d = (id >> (4 * i)) & 0xFu;
            *--p = (char)((d < 10) ? ('0' + d) : ('A' + d - 10));
        }
        atk_md0280_show_string(8, 120, 100, 16, (char *)"Flash ID:",
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(88, 120, 80, 16, p, ATK_MD0280_LCD_FONT_16,
                               init_ok ? ATK_MD0280_BLUE : ATK_MD0280_RED);
    }
    if (disk_initialize(0) != 0) {       /* Flash 无响应（没焊/坏片） */
        g_fs_err = 1;
        g_stats_errors++;
        sys_log_add(LOG_LV_ERR, LOG_FS_MOUNT, 1);
        return;
    }
    if (!rw_self_test()) {               /* 擦/写/读通路实测不过 */
        g_fs_err = 3;
        g_stats_errors++;
        sys_log_add(LOG_LV_ERR, LOG_FS_MOUNT, 3);
        return;
    }

    /* 验证式初始化：
     * 1) 先挂载。干净卷（带 README.TXT 标志文件）→ 直接使用，断电持久化
     * 2) 没有标志文件（出厂残留假卷/损坏卷/新片）→ 格式化重建
     * 3) 标志文件存在但内容不含 "KazepOS" 标记（旧版固件的演示文件）
     *    → 只重建 README/DATA 演示文件，保留用户新建的文件（升级路径）
     * 标志文件法能区分"我们的卷"和"残留数据碰巧像 FAT 的假卷"——
     * 假卷的目录项是垃圾（"system"、空名），f_mount 却会返回 FR_OK */
    {
        FRESULT fom = FR_OK, foo = FR_OK;   /* 诊断：挂载码 / 标志文件验证码 */

        /* 挂载+验证静默进行（~200ms），正常情况直接出列表不显示任何过程 */
        fr = f_mount(&g_fs, "0:", 1);
        fom = fr;
        if (fr == FR_OK) {
            FIL f;
            UINT br = 0;
            foo = f_open(&f, "0:/README.TXT", FA_READ);
            if (foo == FR_OK) {
                f_read(&f, (BYTE *)s_work, 64, &br);
                f_close(&f);
                /* 旧版演示文件（无 KazepOS 标记）→ 标记为升级重建 */
                if (br < 4 || !strstr((char *)s_work, "KazepOS")) foo = FR_NO_FILE;
            }
        }
        if (foo == FR_NO_FILE) {
            /* 升级路径：卷正常、README 是旧版内容 → 只重建演示文件 */
            f_unlink("0:/README.TXT");
            f_unlink("0:/DATA.TXT");
            create_demo();
            disk_ioctl(0, CTRL_SYNC, NULL);  /* 落盘（断电持久化） */
            sys_log_add(LOG_LV_WARN, LOG_FS_UPGRADE, 0);    /* 日志：旧演示文件升级 */
        } else if (fr == FR_NO_FILESYSTEM) {
            /* 诊断行（正常情况下"Mounting..."一闪而过，看不到这行）：
             * m=挂载码 o=验证码：0=OK 4=无文件 12=无文件系统 2=结构错 1=IO错 */
            char dbg[10];
            dbg[0] = 'm'; dbg[1] = '='; fmt_dec(dbg + 2, 2, fom);
            dbg[4] = ' '; dbg[5] = 'o'; dbg[6] = '='; fmt_dec(dbg + 7, 2, foo);
            dbg[9] = 0;
            atk_md0280_show_string(8, 140, 130, 16, dbg,
                                   ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLUE);
            atk_md0280_show_string(8, 120, 200, 16, (char *)"Formatting flash...",
                                   ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
            f_mount(NULL, "0:", 0);      /* 先卸，确保 f_mkfs 前无挂载态 */
            {
                MKFS_PARM mpar = {FM_FAT, 0, 0, 0, 0};
                fr = f_mkfs("0:", &mpar, s_work, sizeof(s_work));
            }
            if (fr == FR_OK) {
                f_mount(&g_fs, "0:", 1);     /* 挂载新卷 */
                sys_log_add(LOG_LV_WARN, LOG_FS_REBUILD, 0);  /* 日志：卷重建 */
                create_demo();               /* 预置演示文件 */
                disk_ioctl(0, CTRL_SYNC, NULL);  /* 强制全部落盘（断电持久化关键：
                                                 * f_mkfs/create_demo 的最后一块
                                                 * 4K 缓存可能未触发换块 flush） */
            } else {
                g_fs_err = 2;
                g_stats_errors++;
                sys_log_add(LOG_LV_ERR, LOG_FS_MOUNT, 2);
                return;
            }
        } else if (fr != FR_OK) {
            g_fs_err = 2;
            g_stats_errors++;
            sys_log_add(LOG_LV_ERR, LOG_FS_MOUNT, 2);
            return;
        }
    }
    g_fs_ready = 1;
}

/* 刷新文件表：枚举 FATFS 根目录填充缓存。
 * 文件表 5 字段 = name/type/size/cluster/status（见结构定义注释）。
 * 存储位置（cluster）取法：FILINFO 不提供起始簇——R0.16 里 sclust 字段
 * 只在 FF_FS_EXFAT=1 编译时才有（本项目 =0）。改从原始目录项读取：
 * f_readdir 返回后 DIR.dir 指针直接指向当前目录项（FATFS win 缓存内，
 * 本次迭代内有效），FAT16 目录项 字节 20-21 = 起始簇高字（FAT16 保留 0）、
 * 26-27 = 起始簇低字，拼成完整 32 位簇号 */
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
            file_item_t *it = &g_files[g_file_count];
            BYTE *e = dj.dir;                /* 当前目录项原始 32B */

            memcpy(it->name, fi.fname, 13);
            it->size = fi.fsize;
            it->type = (fi.fattrib & AM_DIR) ? FILE_T_DIR : FILE_T_FILE;
            it->status = FILE_S_OK;
            it->cluster = ((uint32_t)e[21] << 24) | ((uint32_t)e[20] << 16)
                        | ((uint32_t)e[27] << 8) | (uint32_t)e[26];
            g_file_count++;
        }
    }
    f_closedir(&dj);
    /* 越界回退；无文件 → -1 */
    if (g_sel >= g_file_count)      g_sel = (int8_t)(g_file_count - 1);
    if (g_sel_last >= g_file_count) g_sel_last = (int8_t)(g_file_count - 1);
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
        if (g_files[fi].type == FILE_T_DIR) {
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

/* ---------- 工具栏：New / Del ---------- */

/* 尖端坐标是否在矩形内 */
static uint8_t tip_in_rect(uint16_t x, uint16_t y, uint16_t x0, uint16_t y0,
                           uint16_t x1, uint16_t y1)
{
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* 工具栏重绘：New/Del 按钮（hover 反色）+ 右侧选中文件信息 / 删除确认提示 */
static void draw_toolbar(void)
{
    uint8_t i, hid;
    char b[22];

    hid = redraw_protect_begin(0, TB_Y0 - 2, SCR_W - 1, TB_Y0 + BTN_H + 2);
    atk_md0280_fill(0, TB_Y0 - 2, SCR_W - 1, TB_Y0 + BTN_H + 2, ATK_MD0280_WHITE);
    for (i = 0; i < 2; i++) {
        uint16_t bx = (i == 0) ? BTN_NEW_X : BTN_DEL_X;
        const char *label = (i == 0) ? "New" : "Del";
        uint8_t hover = (g_tb_hover == i + 1);

        atk_md0280_fill(bx, TB_Y0, bx + BTN_W - 1, TB_Y0 + BTN_H - 1,
                        hover ? ATK_MD0280_BLUE : ATK_MD0280_GRAY);
        atk_md0280_show_string(bx + 6, TB_Y0 + 3, 40, 16, (char *)label,
                               ATK_MD0280_LCD_FONT_16,
                               hover ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
    }
    /* 右侧：删除确认提示优先，否则显示最后选中文件信息 */
    if (g_del_arm && g_sel_last >= 0) {
        atk_md0280_show_string(150, TB_Y0 + 5, 90, 12, (char *)"Delete? SW=Y Del=N",
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_RED);
    } else if (g_sel_last >= 0) {
        memcpy(b, "Sel: ", 5);
        memcpy(b + 5, g_files[g_sel_last].name, 13);
        atk_md0280_show_string(150, TB_Y0 + 5, 90, 12, b,
                               ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    }
    redraw_protect_end(hid);
}

/* 更新工具栏 hover（尖端在按钮上反色提示），变化时才重绘 */
static void update_toolbar(void)
{
    uint16_t cx, cy;
    uint8_t hover = 0;

    cursor_get_pos(&cx, &cy);
    if (tip_in_rect(cx, cy, BTN_NEW_X, TB_Y0, BTN_NEW_X + BTN_W - 1, TB_Y0 + BTN_H - 1)) {
        hover = 1;
    } else if (tip_in_rect(cx, cy, BTN_DEL_X, TB_Y0, BTN_DEL_X + BTN_W - 1, TB_Y0 + BTN_H - 1)) {
        hover = 2;
    }
    if (hover != g_tb_hover) {
        g_tb_hover = hover;
        draw_toolbar();
    }
}

/* 新建文件：自动编号 FILE001..FILE999（重名检测 = 编号递增直到 f_stat 报无此文件），
 * 写入模板内容（含 RTC 时间戳）并立即落盘（断电不丢） */
static void new_file(void)
{
    FIL f;
    FILINFO fi;
    rtc_datetime_t dt;
    char path[16], line[40];
    uint16_t n;

    for (n = 1; n <= 999; n++) {
        memcpy(path, "0:/FILE", 7);
        fmt_dec(path + 7, 3, n);             /* FILE001 */
        memcpy(path + 10, ".TXT", 5);        /* 连 '\0' */
        if (f_stat(path, &fi) == FR_NO_FILE) break;   /* 无重名 → 用此编号 */
    }
    if (n > 999) { g_stats_errors++; return; }        /* 编号用尽（防御） */
    sys_log_add(LOG_LV_ERR, LOG_FS_FULL, 0);
    if (f_open(&f, path, FA_CREATE_NEW | FA_WRITE) != FR_OK) {
        g_stats_errors++;
        sys_log_add(LOG_LV_ERR, LOG_FS_WRITE, 0);   /* 日志：新建文件失败 */
        return;
    }
    rtc_app_get_datetime(&dt);
    write_line(&f, "KazepOS - new file");
    memcpy(line, "Name: ", 6);
    memcpy(line + 6, path + 3, 13);          /* FILExxx.TXT（连 '\0'） */
    write_line(&f, line);
    memcpy(line, "Time: ", 6);
    fmt_dec(line + 6, 4, dt.year);  line[10] = '-';
    fmt_dec(line + 11, 2, dt.month); line[13] = '-';
    fmt_dec(line + 14, 2, dt.day);  line[16] = ' ';
    fmt_dec(line + 17, 2, dt.hour); line[19] = ':';
    fmt_dec(line + 20, 2, dt.min);  line[22] = ':';
    fmt_dec(line + 23, 2, dt.sec);  line[25] = 0;
    write_line(&f, line);
    write_line(&f, "Created by New button");
    f_close(&f);
    disk_ioctl(0, CTRL_SYNC, NULL);          /* 立即落盘 */
    list_refresh();
    draw_list();
    draw_status();                           /* 文件数变化，状态区同步刷新 */
}

/* 删除最后选中的文件（调用前 g_del_arm 已确认） */
static void do_delete(void)
{
    char path[16];
    int8_t idx = g_sel_last;

    if (idx < 0) return;
    memcpy(path, "0:/", 3);
    memcpy(path + 3, g_files[idx].name, 13);
    if (f_unlink(path) == FR_OK) {
        list_refresh();
        draw_list();
    } else {
        g_stats_errors++;
        sys_log_add(LOG_LV_ERR, LOG_FS_DEL, 0);      /* 日志：删除失败 */
    }
}

/* 状态区（列表之下）：Flash / 剩余空间 / 文件数 / 操作提示 */
static void draw_status(void)
{
    uint32_t fre = 0, kb = 0;
    FATFS *pf = NULL;

    atk_md0280_fill(0, 256, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    if (g_fs_err == 1) {
        /* 错误行顺带显示位带实际读到的 JEDEC ID（0xFFFFFF = 通路断） */
        w25q128_init();                      /* 幂等：确保引脚处于位带模式 */
        {
            uint32_t id = w25q128_read_id();
            char buf[9], *p = buf + 8;
            uint8_t i;
            *p = 0;
            for (i = 0; i < 8; i++) {
                uint8_t d = (id >> (4 * i)) & 0xFu;
                *--p = (char)((d < 10) ? ('0' + d) : ('A' + d - 10));
            }
            atk_md0280_show_string(8, 256, 130, 16, (char *)"Flash error! ID:",
                                   ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
            atk_md0280_show_string(136, 256, 70, 16, p,
                                   ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
        }
    } else if (g_fs_err == 2) {
        atk_md0280_show_string(8, 256, 200, 16, (char *)"FS init failed!", ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
    } else if (g_fs_err == 3) {
        /* 擦/写/读自检失败：显示首个错误字节地址（定位克隆芯片通路问题） */
        uint32_t a = g_rw_bad;
        char buf[17], *p = buf + 16;
        uint8_t i;
        *p = 0;
        for (i = 0; i < 8; i++) {
            uint8_t d = (uint8_t)((a >> (4 * i)) & 0xFu);
            *--p = (char)((d < 10) ? ('0' + d) : ('A' + d - 10));
        }
        atk_md0280_show_string(8, 256, 150, 16, (char *)"RW self-test FAIL!",
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
        atk_md0280_show_string(8, 276, 130, 16, (char *)"bad byte @",
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_RED);
        atk_md0280_show_string(112, 276, 90, 16, p, ATK_MD0280_LCD_FONT_16,
                               ATK_MD0280_RED);
    } else if (g_fs_ready) {
        if (f_getfree("0:", &fre, &pf) == FR_OK) kb = fre * pf->csize / 2;  /* 512B 扇区 → KB */
        atk_md0280_show_string(8, 256, 140, 16, (char *)"Flash: W25Q128", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(8, 276, 44, 16, (char *)"Free", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_xnum(48, 276, kb, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                             ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(112, 276, 24, 16, (char *)"KB", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_string(8, 296, 120, 16, (char *)"Files:", ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
        atk_md0280_show_xnum(64, 296, g_total_count, 3, ATK_MD0280_NUM_SHOW_NOZERO,
                             ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    }
    atk_md0280_show_string(8, 306, 150, 12, (char *)"SW: use  K1: exit", ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
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
    /* 识别 Paint 图片（magic "KP1"）→ 图片模式；否则文本模式 */
    g_view_img = (br >= 3 && g_view_raw[0] == 'K' && g_view_raw[1] == 'P' &&
                  g_view_raw[2] == '1') ? 1 : 0;
    if (!g_view_img) {
        /* 压成 40 列网格：丢弃 \r\n，其余照抄 */
        for (i = 0; i < br; i++) {
            char c = g_view_raw[i];
            if (c == '\r' || c == '\n') continue;
            if (col == VIEW_CHARS) { line++; col = 0; }
            if (line >= VIEW_LINES) break;
            g_view_grid[line][col++] = c;
        }
        g_view_lines = line + (col > 0 ? 1 : 0);
    } else {
        g_view_lines = 0;
    }
}

/* 图片视图重绘：IMG*.IMG（"KP1" magic + RGB565 像素流，240×220）
 * 8 行批量读 + atk_md0280_write_area 批量写屏（窗口设置一次 + 连续写
 * 像素，28 次窗口设置、W25Q 连续扇区读，全图 ~70ms 显示完——一次读完
 * 整块再写，无逐行刷新感，这正是"打开图片直接显示"的关键）。
 * 二进制像素文件用文本视图只会看到乱码（"KP1"+不可打印字节），
 * 识别 magic 后按图像渲染。尺寸固定无需文件内宽高 */
static void draw_img_view(void)
{
    FIL f;
    UINT br;
    uint16_t y;
    char path[16];
    uint8_t hid;

    memcpy(path, "0:/", 3);
    memcpy(path + 3, g_view_name, 13);   /* 连 '\0' 一起拷 */
    hid = redraw_protect_begin(0, 25, SCR_W - 1, SCR_H - 1);
    atk_md0280_fill(0, 25, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    atk_md0280_show_string(8, 30, 150, 16, g_view_name, ATK_MD0280_LCD_FONT_16, ATK_MD0280_BLACK);
    atk_md0280_show_xnum(170, 32, g_view_size, 6, ATK_MD0280_NUM_SHOW_NOZERO,
                         ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    if (f_open(&f, path, FA_READ) == FR_OK) {
        if (f_lseek(&f, 3) == FR_OK) {   /* 跳过 magic */
            /* 一次读 8 行、一次写 8 行（窗口设一次写 1920 像素）：逐行读写时
             * 每行触发 1~2 次 W25Q 扇区 SPI 读（480B 跨 512B 扇区边界），
             * 220 行 ≈ 0.15s 的"逐行画"过程肉眼可见；批量后窗口设置 220→28
             * 次、SPI 读连续化，总耗时 ~70ms 无刷新感 */
            for (y = 0; y < 220; y += 8) {
                uint16_t rows = (uint16_t)((220 - y < 8) ? (220 - y) : 8);

                if (f_read(&f, g_img_buf, rows * 480, &br) != FR_OK || br != rows * 480) break;
                /* 小端字节对按 uint16_t 数组直写（ARM 小端，内存布局与
                 * 文件一致；静态数组天然对齐，M3 硬件支持非对齐读） */
                atk_md0280_write_area(0, (uint16_t)(52 + y), (uint16_t)(SCR_W - 1),
                                      (uint16_t)(52 + y + rows - 1), (const uint16_t *)g_img_buf);
            }
        }
        f_close(&f);
    }
    atk_md0280_show_string(8, 306, 150, 12, (char *)"SW: close  K1: exit",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);
    redraw_protect_end(hid);
}

/* 文本视图重绘 */
static void draw_view(void)
{
    uint8_t i, hid;

    if (g_view_img) { draw_img_view(); return; }

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

/* 光标位置 → 选中行（精确命中：指到哪行选哪行，空白处不选，含边缘滚动） */
static void update_selection(void)
{
    uint16_t cx, cy;
    int r, sel;

    cursor_get_pos(&cx, &cy);
    /* 命中点 = 箭头光标的尖端（左上角像素，坐标语义即 (x,y)），
     * 尖端指到哪一行就选哪一行 */
    r = ((int)cy - LIST_Y0) / ROW_H;         /* 尖端所在行（可越界） */
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
    /* 命中测试：只有光标中心落在"有文件的行"内才选中，否则 -1 不选 */
    sel = -1;
    if (r >= 0 && r < LIST_ROWS) {
        int fi = g_scroll + r;
        if (fi < g_file_count) sel = fi;     /* 该行真有文件 → 选中 */
    }
    if (sel != g_sel) {
        g_sel = (int8_t)sel;
        draw_list();
        draw_toolbar();                      /* 右侧选中信息同步刷新 */
    }
    if (sel >= 0) g_sel_last = (int8_t)sel;  /* 记住最后选中（Del 按钮用） */
}

void app_files_handle(input_event_t *ev)
{
    uint16_t cx, cy;

    switch (ev->type) {
    case EV_KEY_DOWN:                        /* SW：激活尖端所指（按钮/文件行） */
        if (!g_fs_ready || g_fs_err) break;
        if (g_del_arm) {                     /* 删除确认态：SW 确认 / 停在 [Del] 上按 SW = 取消 */
            cursor_get_pos(&cx, &cy);
            if (tip_in_rect(cx, cy, BTN_DEL_X, TB_Y0, BTN_DEL_X + BTN_W - 1, TB_Y0 + BTN_H - 1)) {
                g_del_arm = 0;               /* 在 [Del] 上按 = "Del=N" 取消 */
            } else {
                g_del_arm = 0;
                do_delete();
            }
            draw_toolbar();
            draw_status();
            break;
        }
        if (g_viewing) {
            uint8_t hid2;
            g_viewing = 0;
            /* 先整块擦白（标题栏 0-24 之外全部）再重绘：draw_view 全屏覆盖，
             * 而列表/工具栏/状态区只覆盖各自的条带，y 25-53（view 的文件名/
             * 大小）与 215-224 等缝隙会残留旧文字——擦白才能清干净 */
            hid2 = redraw_protect_begin(0, 25, SCR_W - 1, SCR_H - 1);
            atk_md0280_fill(0, 25, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
            redraw_protect_end(hid2);
            draw_list();                     /* 回列表 */
            draw_toolbar();                  /* 恢复工具栏 */
            draw_status();                   /* 恢复状态区 */
            break;
        }
        cursor_get_pos(&cx, &cy);
        if (tip_in_rect(cx, cy, BTN_NEW_X, TB_Y0, BTN_NEW_X + BTN_W - 1, TB_Y0 + BTN_H - 1)) {
            new_file();                      /* 新建：自动编号 + 重名检测 */
        } else if (tip_in_rect(cx, cy, BTN_DEL_X, TB_Y0, BTN_DEL_X + BTN_W - 1, TB_Y0 + BTN_H - 1)) {
            if (g_sel_last >= 0) {           /* 进入删除确认态（再按一次取消） */
                g_del_arm = 1;
                draw_toolbar();
            }
        } else if (g_sel >= 0 && g_files[g_sel].type != FILE_T_DIR) {
            open_file();
            draw_view();
        }
        break;

    case EV_MOUSE_MOVE:                      /* 光标已由框架移动：同步选中行 + 工具栏 hover */
        if (!g_viewing && g_fs_ready) {
            update_selection();
            update_toolbar();
        }
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
    draw_toolbar();
    cursor_init(35, LIST_Y0);                /* 箭头尖端对齐第 0 行顶部 */
    update_selection();                      /* 初始选中第 0 行 */
    cursor_show();
}
