/**
 * @file    app_monitor.h
 * @brief   System Monitor 应用接口（注册表 g_apps 中通过这两个函数挂载）
 */
#ifndef __APP_MONITOR_H
#define __APP_MONITOR_H

#include "event.h"

void app_monitor_open(void);               /* 进入应用：全屏绘制监控界面 */
void app_monitor_handle(input_event_t *ev);/* 事件分发：EV_TICK 每秒刷新 */

#endif
