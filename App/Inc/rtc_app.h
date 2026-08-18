/**
 * @file    rtc_app.h
 * @brief   时间源接口：STM32 内部 RTC（真实时钟，VBAT 掉电保持）
 */
#ifndef __RTC_APP_H
#define __RTC_APP_H

#include <stdint.h>

/* 上电初始化（ui_task 启动时调用一次）：
 * LSE 起振失败自动降级 LSI；首次上电（无 VBAT/刚烧录）设默认 12:00 */
void rtc_app_init(void);

/* 读当前时分（24 小时制） */
void rtc_app_get_time(uint8_t *hour, uint8_t *min);

#endif
