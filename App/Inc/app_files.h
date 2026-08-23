/**
 * @file    app_files.h
 * @brief   Files 应用接口（W25Q128 + FATFS）
 */
#ifndef __APP_FILES_H
#define __APP_FILES_H

#include "event.h"
#include "ff.h"

/* 全局共享 FATFS 挂载对象（定义在 app_files.c）：Files/Paint 共用同一对象，
 * 避免同一卷被两个对象挂载的冲突 */
extern FATFS g_fs;

void app_files_open(void);
void app_files_handle(input_event_t *ev);

#endif
