/**
 * @file    app_paint.h
 * @brief   Paint 应用接口（注册表调用）
 */
#ifndef __APP_PAINT_H
#define __APP_PAINT_H

#include "event.h"

void app_paint_open(void);
void app_paint_handle(input_event_t *ev);

#endif
