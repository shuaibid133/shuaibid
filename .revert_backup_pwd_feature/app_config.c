/**
 * @file    app_config.c
 * @brief   系统配置默认值（sys_cfg_load 启动时用 Flash 存储覆盖）
 */
#include "app_config.h"

/* 默认配置：光标大小1/灵敏度3/亮度100%/音量80%/熄屏60秒/版本V1/JS Lock 开/密码1234 */
sys_config_t g_sys_cfg = { .cursor_size = 1, .cursor_sens = 3,
                           .brightness = 100, .volume = 80,
                           .screen_time = 60, .version = 1, .js_lock = 1,
                           .pwd = "1234" };
