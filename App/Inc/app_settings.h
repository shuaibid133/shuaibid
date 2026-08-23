/**
 * @file    app_settings.h
 * @brief   Settings 应用接口（注册表调用）
 */
#ifndef __APP_SETTINGS_H
#define __APP_SETTINGS_H

#include "app.h"

void app_settings_open(void);
void app_settings_handle(input_event_t *ev);
void app_settings_close(void);

#endif
