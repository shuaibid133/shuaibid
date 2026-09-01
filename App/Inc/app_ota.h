/**
 * @file    app_ota.h
 * @brief   OTA Update 应用接口（注册表调用）
 */
#ifndef __APP_OTA_H
#define __APP_OTA_H

#include "app.h"

void app_ota_open(void);
void app_ota_handle(input_event_t *ev);
void app_ota_close(void);    /* 无退出清理（返回流程是阻塞式的），框架用 NULL 即可 */

#endif
