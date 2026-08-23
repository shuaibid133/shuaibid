/**
 * @file    desktop.h
 * @brief   桌面框架接口：上电启动 → 密码登录 → 桌面主界面
 */
#ifndef __DESKTOP_H
#define __DESKTOP_H

#include "event.h"

void desktop_init(void);                      /* 上电调用一次：BOOT 2秒 → LOGIN */
void desktop_handle_event(input_event_t *ev); /* ui_task 每收到事件调用 */

/* 从应用（Settings）进入校时界面：校时完成/跳过 → 回桌面而非登录页 */
void desktop_enter_clock_set(void);

#endif
