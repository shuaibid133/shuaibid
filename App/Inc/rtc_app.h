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

/* 完整日期时间（FATFS 文件时间戳等用途） */
typedef struct {
    uint16_t year;    /* 4 位年份，如 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  min;
    uint8_t  sec;
} rtc_datetime_t;

void rtc_app_get_datetime(rtc_datetime_t *dt);

#endif
