/**
 * @file    rtc_app.h
 * @brief   时间源接口：STM32 内部 RTC（真实时钟，VBAT 掉电保持）
 */
#ifndef __RTC_APP_H
#define __RTC_APP_H

#include <stdint.h>

/* 上电初始化（ui_task 启动时调用一次）：
 * LSE 起振失败自动降级 LSI；首次上电（无 VBAT/刚烧录）设编译时刻 */
void rtc_app_init(void);

/* RTC 是否已校时（BKP 魔数判断）：
 * 0 = 有效（用户校过时，RTC 正常走）→ 直接进登录
 * 1 = 无效（首次/断电后 BKP 丢失）→ 开机强制进 Set Clock 界面 */
uint8_t rtc_app_needs_setup(void);

/* 手动校时：写入用户输入的真实时间（年取编译年份，秒自动置 0），
 * 成功后写 BKP 魔数（下次开机不再要求校时，直到断电） */
void rtc_app_set_datetime(uint8_t month, uint8_t day, uint8_t hour, uint8_t min);

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
