/**
 * @file    cursor.c
 * @brief   光标模块实现：经典鼠标箭头，尺寸读系统配置
 *          无痕移动：画光标前先把外框背景存进内存缓冲，移走时把缓冲
 *          写回屏幕恢复（不能"读屏再写回"——那会读到光标自己固化残影）
 */
#include "cursor.h"
#include "app_config.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"

static struct
{
    uint16_t x;
    uint16_t y;
} g_cursor = {0, 0};

/* 16x18 经典鼠标箭头位图：'1' = 黑色实心，白色描边由绘制时 8 邻域膨胀生成 */
static const char CURSOR_ARROW[18][19] = {
    "1...............",
    "11..............",
    "1.1.............",
    "1..1............",
    "1...1...........",
    "1....1..........",
    "1.....1.........",
    "1......1........",
    "1.......1.......",
    "1........1......",
    "1.........1.....",
    "1..........1....",
    "111111111111....",
    "....11..........",
    ".....11.........",
    ".....11.........",
    "......11........",
    ".......11.......",
};

/* ---------- 光标大小（限制 1~CUR_MAX_S：背景缓冲按上限静态分配） ---------- */
#define CUR_MAX_S       4
#define CUR_BG_W        (18 * CUR_MAX_S)     /* 外框宽 = 18s 像素 */
#define CUR_BG_H        (20 * CUR_MAX_S)     /* 外框高 = 20s 像素 */

/* 光标外框背景缓冲：画光标前保存，移动后写回恢复 */
static uint16_t g_cur_bg[CUR_BG_W * CUR_BG_H];

/* 光标当前是否显示在屏幕上（hide 后可恢复；overlap 判断用） */
static uint8_t g_visible = 0;

static uint8_t cursor_size(void)
{
    uint8_t s = g_sys_cfg.cursor_size;

    return (s > CUR_MAX_S) ? CUR_MAX_S : s;
}

/* 填一个 s x s 色块，越界部分裁剪（白描边可能伸出屏幕 1 格） */
static void cursor_fill_block(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ATK_MD0280_LCD_WIDTH - 1)  x1 = ATK_MD0280_LCD_WIDTH - 1;
    if (y1 > ATK_MD0280_LCD_HEIGHT - 1) y1 = ATK_MD0280_LCD_HEIGHT - 1;
    if (x0 > x1 || y0 > y1) return;
    atk_md0280_fill((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, color);
}

/* 在指定位置绘制光标：经典鼠标箭头（黑实心 + 白描边），
 * 每个位图像素放大成 s x s 方块（外框宽 18s、高 20s，含描边） */
static void cursor_draw_at(uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t s = cursor_size();
    int16_t r, c, dr, dc;

    /* 第一遍：白色描边 = 黑色像素的 8 邻域 */
    for (r = 0; r < 18; r++)
    {
        for (c = 0; c < 16; c++)
        {
            if (CURSOR_ARROW[r][c] != '1') continue;
            for (dr = -1; dr <= 1; dr++)
            {
                for (dc = -1; dc <= 1; dc++)
                {
                    cursor_fill_block(x + (c + dc) * s, y + (r + dr) * s,
                                      x + (c + dc) * s + s - 1, y + (r + dr) * s + s - 1,
                                      ATK_MD0280_WHITE);
                }
            }
        }
    }
    /* 第二遍：黑色实心覆盖中心 */
    for (r = 0; r < 18; r++)
    {
        for (c = 0; c < 16; c++)
        {
            if (CURSOR_ARROW[r][c] != '1') continue;
            cursor_fill_block(x + c * s, y + r * s,
                              x + c * s + s - 1, y + r * s + s - 1, color);
        }
    }
}

/* 外框矩形（含白描边）：(x-s, y-s) ~ (x+17s-1, y+19s-1) */
static void cursor_frame(int16_t *x0, int16_t *y0, int16_t *x1, int16_t *y1)
{
    uint8_t s = cursor_size();

    *x0 = (int16_t)g_cursor.x - s;
    *y0 = (int16_t)g_cursor.y - s;
    *x1 = (int16_t)g_cursor.x + 17 * s - 1;
    *y1 = (int16_t)g_cursor.y + 19 * s - 1;
}

/* 保存：把当前外框内容读进缓冲（必须在画光标之前调用——存的是干净背景） */
static void cursor_save_bg(void)
{
    int16_t x0, y0, x1, y1;

    cursor_frame(&x0, &y0, &x1, &y1);
    atk_md0280_read_area((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, g_cur_bg);
}

/* 恢复：把缓冲写回外框。顺序必须是"先恢复、再保存新位置、再画"——
 * 若新旧位置重叠，恢复完成后新位置区域也已变干净，读到的背景才对 */
static void cursor_restore_bg(void)
{
    int16_t x0, y0, x1, y1;

    cursor_frame(&x0, &y0, &x1, &y1);
    atk_md0280_write_area((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, g_cur_bg);
}

void cursor_init(uint16_t x, uint16_t y)
{
    g_cursor.x = x;
    g_cursor.y = y;
}

void cursor_show(void)
{
    cursor_save_bg();               /* 先存干净背景 */
    cursor_draw_at(g_cursor.x, g_cursor.y, ATK_MD0280_BLACK);
    g_visible = 1;
}

/* 隐藏：把已存背景写回外框，光标从屏幕消失（屏幕恢复干净背景）。
 * 局部重绘（格子高亮/提示行/圆点）与光标外框相交时，必须先藏再画——
 * 否则 fill 只盖住光标一部分，"截断光标"会被当成背景存进缓冲固化残影 */
void cursor_hide(void)
{
    if (!g_visible) return;

    cursor_restore_bg();
    g_visible = 0;
}

/* 矩形与光标外框是否相交（未显示时返回 0，绘制方用于决定是否 hide/show） */
uint8_t cursor_overlap(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    int16_t fx0, fy0, fx1, fy1;

    if (!g_visible) return 0;
    cursor_frame(&fx0, &fy0, &fx1, &fy1);
    if ((int16_t)x1 < fx0 || (int16_t)x0 > fx1) return 0;
    if ((int16_t)y1 < fy0 || (int16_t)y0 > fy1) return 0;
    return 1;
}

void cursor_move(int8_t dx, int8_t dy)
{
    uint8_t s = cursor_size();
    int16_t nx = (int16_t)g_cursor.x + dx;
    int16_t ny = (int16_t)g_cursor.y + dy;
    /* 钳制基于箭头外框（宽 18s、高 20s，含白描边），保证整个箭头不出屏 */
    uint16_t max_x = ATK_MD0280_LCD_WIDTH - 1 - 17 * s;
    uint16_t max_y = ATK_MD0280_LCD_HEIGHT - 1 - 19 * s;

    /* 边界钳制：光标整体不出屏幕 */
    if (nx < s)     nx = s;
    if (nx > max_x) nx = max_x;
    if (ny < s)     ny = s;
    if (ny > max_y) ny = max_y;

    if (nx == (int16_t)g_cursor.x && ny == (int16_t)g_cursor.y) return;  /* 位置没变 */

    cursor_restore_bg();            /* ① 写回旧位置背景（用内存里存的，不重新读屏） */
    g_cursor.x = (uint16_t)nx;
    g_cursor.y = (uint16_t)ny;
    cursor_save_bg();               /* ② 读新位置干净背景 */
    cursor_draw_at(g_cursor.x, g_cursor.y, ATK_MD0280_BLACK);  /* ③ 画新光标 */
    g_visible = 1;
}

void cursor_get_pos(uint16_t *x, uint16_t *y)
{
    *x = g_cursor.x;
    *y = g_cursor.y;
}
