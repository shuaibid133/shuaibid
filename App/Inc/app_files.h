/**
 * @file    app_files.h
 * @brief   Files 应用接口（W25Q128 + FATFS）
 */
#ifndef __APP_FILES_H
#define __APP_FILES_H

#include "event.h"

void app_files_open(void);
void app_files_handle(input_event_t *ev);

#endif
