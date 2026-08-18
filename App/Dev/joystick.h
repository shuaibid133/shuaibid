/**
 * @file    joystick.h
 * @brief   dev 层：输入设备驱动接口（摇杆 ADC + SW 按键 + K0 设备开关）
 *
 * 分层：dev 层屏蔽硬件细节（ADC/GPIO/消抖），input_task 只做事件聚合；
 *       应用层（desktop）通过事件感知设备，通过 g_js_on 读设备在线状态
 */
#ifndef __JOYSTICK_H
#define __JOYSTICK_H

#include "event.h"

/* 设备开关：上电默认 OFF（未连接），K0 按下取反。
 * 单字节、input_task 独占写、ui_task 只读——CM3 单字节访问天然原子，无需锁 */
extern uint8_t g_js_on;

/* 扫描一次输入设备（15ms 周期由 input_task 调用）：
 * 产生事件时填充 *ev 并返回 1，无事件返回 0 */
uint8_t joystick_scan(input_event_t *ev);

/* 设备开关取反（K0 按下 / 设置应用都可调用） */
void joystick_toggle(void);

/* 设备是否在线 */
uint8_t joystick_is_on(void);

#endif
