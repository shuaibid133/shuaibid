/**
 * @file    app_config.h
 * @brief   系统配置：光标/亮度/音量/熄屏等参数
 *          设置应用（app_settings）修改字段，sys_cfg 持久化到 Flash，
 *          各模块（joystick/cursor/sys_backlight/music/熄屏）实时读字段
 */
#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

#include <stdint.h>

/* 系统配置结构体——设置应用改字段，各模块读字段，即改即生效 */
typedef struct
{
    uint8_t cursor_size;    /* 光标大小 1~4（上限由 cursor.c 背景缓冲决定，>4 会被钳制） */
    uint8_t cursor_sens;    /* 光标灵敏度 1~10 */
    uint8_t brightness;     /* 屏幕亮度 10~100%（sys_backlight PWM 占空比） */
    uint8_t volume;         /* 系统音量 0~100%（Music 蜂鸣器 PWM 占空比） */
    uint8_t screen_time;    /* 熄屏时间（秒）：0=永不熄屏，10~300 */
    uint8_t version;        /* 系统版本（OTA 生效后写入；1 = V1.0，与更新包解耦——
                             * 已升级的设备不会因更新区被擦而"降级"） */
    uint8_t js_lock;        /* JS Lock 开机摇杆默认状态：1=自动锁（默认 OFF，
                             * 需按 K0 开启——现状）；0=开机直接可用（跳过按 K0）。
                             * joystick_set_default 在 sys_cfg_load 后读取 */
    char pwd[5];            /* 登录密码：4 位数字 '0'~'9' + '\0'（默认 "1234"）。
                             * 登录/锁屏验证逐位比对；Settings 的 Password 项
                             * 可改，改完立即 sys_cfg_save 落盘 */
} sys_config_t;

extern sys_config_t g_sys_cfg;

#endif
