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
void cursor_get_pos(uint16_t *x, uint16_t *y); /* 读取光标位置 */

#endif
