/**
 * @file    cursor.h
 * @brief   光标模块：UI 任务的子模块，负责光标移动/绘制/边界钳制
 *          注意：光标不是任务，渲染统一由 UI 任务（屏幕独占）调用
 */
#ifndef __CURSOR_H
#define __CURSOR_H

#include "main.h"

void cursor_init(uint16_t x, uint16_t y);      /* 设置初始位置 */
void cursor_move(int8_t dx, int8_t dy);        /* 移动（含边界钳制 + 重绘） */
void cursor_show(void);                        /* 绘制当前光标 */
void cursor_hide(void);                        /* 用已存背景把光标从屏幕移除（局部重绘前调用） */
uint8_t cursor_overlap(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1); /* 矩形与光标外框是否相交（未显示返回 0） */
void cursor_get_pos(uint16_t *x, uint16_t *y); /* 读取光标位置 */

#endif
