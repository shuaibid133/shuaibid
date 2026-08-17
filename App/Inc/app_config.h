/**
 * @file    app_config.h
 * @brief   系统配置：光标大小/灵敏度等参数
 *          设置应用修改这里，阶段3再序列化进 Flash 实现重启保持
 */
#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

#include <stdint.h>

/* 系统配置结构体——设置应用改字段，各任务读字段，即改即生效 */
typedef struct
{
    uint8_t cursor_size;    /* 光标大小 1~4（上限由 cursor.c 背景缓冲决定，>4 会被钳制） */
    uint8_t cursor_sens;    /* 光标灵敏度 1~10 */
} sys_config_t;

extern sys_config_t g_sys_cfg;

#endif
