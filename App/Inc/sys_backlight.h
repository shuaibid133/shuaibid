/**
 * @file    sys_backlight.h
 * @brief   背光亮度控制接口（PB0 = TIM3_CH3 PWM）
 */
#ifndef __SYS_BACKLIGHT_H
#define __SYS_BACKLIGHT_H

#include <stdint.h>

/* 上电调用一次：PB0 复用为 TIM3_CH3 PWM，按 g_sys_cfg.brightness 输出 */
void sys_backlight_init(void);

/* 设置亮度 0~100%：0 = 背光全灭（熄屏用），100 = 最亮 */
void sys_backlight_set(uint8_t pct);

#endif
