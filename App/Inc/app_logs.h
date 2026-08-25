/**
 * @file    app_logs.h
 * @brief   Logs 日志查看应用
 */
#ifndef __APP_LOGS_H
#define __APP_LOGS_H

#include "event.h"

void app_logs_open(void);
void app_logs_handle(input_event_t *ev);

#endif
