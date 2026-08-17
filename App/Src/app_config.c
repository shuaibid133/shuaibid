/**
 * @file    app_config.c
 * @brief   系统配置默认值
 */
#include "app_config.h"

/* 默认配置：光标大小1，灵敏度5（阶段3后从 Flash 加载覆盖） */
sys_config_t g_sys_cfg = { .cursor_size = 1, .cursor_sens = 3 };
