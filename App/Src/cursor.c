/**
 * @file    cursor.c
 * @brief   光标模块实现：经典鼠标箭头，尺寸读系统配置，先擦后画防残影
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
    uint8_t s = g_sys_cfg.cursor_size;
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

void cursor_init(uint16_t x, uint16_t y)
{
    g_cursor.x = x;
    g_cursor.y = y;
}

void cursor_show(void)
{
    cursor_draw_at(g_cursor.x, g_cursor.y, ATK_MD0280_BLACK);
}

/* 擦除：整个外框（黑格 16x18 + 描边 1s，宽 18s、高 20s）填背景色，比逐像素重画快 */
static void cursor_erase(void)
{
    uint8_t s = g_sys_cfg.cursor_size;

    atk_md0280_fill(g_cursor.x - s, g_cursor.y - s,
                    g_cursor.x + 17 * s - 1, g_cursor.y + 19 * s - 1,
                    ATK_MD0280_WHITE);
}

void cursor_move(int8_t dx, int8_t dy)
{
    uint8_t s = g_sys_cfg.cursor_size;
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

    cursor_erase();                          /* 擦旧 */
    g_cursor.x = (uint16_t)nx;
    g_cursor.y = (uint16_t)ny;
    cursor_show();                           /* 画新 */
}

void cursor_get_pos(uint16_t *x, uint16_t *y)
{
    *x = g_cursor.x;
    *y = g_cursor.y;
}
