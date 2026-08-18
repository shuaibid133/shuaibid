/**
 * @file    rtc_app.c
 * @brief   时间源实现：STM32F1 内部 RTC
 *
 * 原理：
 *   - RTC 走秒靠 LSE 32.768kHz 晶振，掉电后由 VBAT 电池维持
 *   - 分频：32768 / (127+1) = 256Hz，再 / (255+1) = 1Hz 秒脉冲
 *   - 首次上电判断用 BKP 后备寄存器魔数（0xA5A5）：不是魔数说明
 *     RTC 从没初始化过（无电池/刚烧录），设默认时间并写魔数；
 *     是魔数说明 RTC 一直在走，保留当前时间
 *   - LSE 起振失败降级 LSI（内部 RC，精度差但时间能走，演示可接受）
 */
#include "rtc_app.h"
#include "sys_stats.h"
#include "main.h"
#include "stm32f1xx_hal_rtc.h"   /* HAL RTC：CubeMX 未开 RTC 中间件，手动包含 */

#define RTC_BKP_MAGIC   0xA5A5   /* BKP 后备寄存器魔数：RTC 已初始化过 */
#define RTC_INIT_HOUR   12       /* 首次上电默认时间 */
#define RTC_INIT_MIN    0

static RTC_HandleTypeDef g_hrtc;

void rtc_app_init(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};
    RTC_TimeTypeDef t = {0};

    /* 1. PWR/BKP 时钟 + 解锁后备域写保护（RTC 与 BKP 共域） */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    /* 2. 启动 LSE；失败则关掉 LSE 降级 LSI */
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.LSEState = RCC_LSE_ON;
    if (HAL_RCC_OscConfig(&osc) == HAL_OK)
    {
        pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    }
    else
    {
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
        osc.LSEState = RCC_LSE_OFF;
        HAL_RCC_OscConfig(&osc);
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc.LSIState = RCC_LSI_ON;
        HAL_RCC_OscConfig(&osc);
        pclk.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;  /* 内部 RC，秒会偏 */
        g_stats_errors++;   /* 统计：LSE 起振失败降级 LSI = 系统级错误 */
    }

    /* 3. 选择 RTC 时钟源 */
    pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    HAL_RCCEx_PeriphCLKConfig(&pclk);

    /* 4. RTC 外设时钟使能（F1：BDCR 寄存器 RTCEN 位。
     *    正点原子版 HAL 没有 __HAL_RCC_RTC_CLK_ENABLE 宏，直接操作寄存器） */
    RCC->BDCR |= RCC_BDCR_RTCEN;

    /* 5. 初始化：RTC_AUTO_1_SECOND 让库按时钟源自动算分频（正点原子魔改版
     *    无 SynchPrediv/HourFormat 字段，24 小时制固定） */
    g_hrtc.Instance = RTC;
    g_hrtc.Init.AsynchPrediv = RTC_AUTO_1_SECOND;
    g_hrtc.Init.OutPut = RTC_OUTPUTSOURCE_NONE;
    HAL_RTC_Init(&g_hrtc);

    /* 6. 首次上电：设默认时间 + 写魔数 */
    if (HAL_RTCEx_BKUPRead(&g_hrtc, RTC_BKP_DR1) != RTC_BKP_MAGIC)
    {
        t.Hours = RTC_INIT_HOUR;
        t.Minutes = RTC_INIT_MIN;
        t.Seconds = 0;
        HAL_RTC_SetTime(&g_hrtc, &t, RTC_FORMAT_BIN);
        HAL_RTCEx_BKUPWrite(&g_hrtc, RTC_BKP_DR1, RTC_BKP_MAGIC);
    }
}

void rtc_app_get_time(uint8_t *hour, uint8_t *min)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    /* shadow 寄存器：GetTime 后必须 GetDate 一次解锁（HAL 要求） */
    HAL_RTC_GetTime(&g_hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&g_hrtc, &d, RTC_FORMAT_BIN);
    *hour = t.Hours;
    *min  = t.Minutes;
}

void rtc_app_get_datetime(rtc_datetime_t *dt)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    HAL_RTC_GetTime(&g_hrtc, &t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&g_hrtc, &d, RTC_FORMAT_BIN);   /* shadow 解锁（HAL 要求） */
    dt->year  = 2000 + d.Year;   /* F1 HAL 日期里只有 2 位年份 */
    dt->month = d.Month;
    dt->day   = d.Date;
    dt->hour  = t.Hours;
    dt->min   = t.Minutes;
    dt->sec   = t.Seconds;
}
