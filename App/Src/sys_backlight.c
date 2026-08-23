/**
 * @file    sys_backlight.c
 * @brief   背光 PWM 驱动：PB0 = TIM3_CH3，占空比 = 亮度百分比
 *
 * 背景：BSP 的 atk_md0280_backlight_on/off 是 GPIO 开关（PB0 推挽输出，
 * 高=亮低=灭），只有两态。Settings 要调节亮度 → PB0 复用为 TIM3_CH3：
 *   72MHz / (72-1) = 1MHz 计数时钟，ARR=50-1 → 20kHz PWM
 *   （超声频段：屏上无闪烁、蜂鸣器听不见）
 *   高电平 = 背光亮（MD0280 背光极性高有效）→ PWM1 模式，
 *   占空比 CCR/ARR 就是亮度百分比
 *
 * 时序：ui_task 在 atk_md0280_init 之后调用 sys_backlight_init——
 * BSP 先把 PB0 配成推挽输出（背光默认亮），这里重新配成复用开 PWM。
 *
 * 防御：PWM 任一初始化步骤失败 → 回退 GPIO 推挽高（保持最亮，界面可见，
 * 只是亮度调节失效）。诊断：烧录后屏幕亮 = PWM 失败被回退兜住；
 * 屏幕仍黑 = 初始化根本没执行到（问题在更早的调用方）
 */
#include "sys_backlight.h"
#include "app_config.h"
#include "main.h"
#include "stm32f1xx_hal_tim.h"

#define BL_ARR     50              /* ARR+1 = 50：20kHz（72MHz/72/50） */

static TIM_HandleTypeDef g_htim3;

void sys_backlight_set(uint8_t pct)
{
    uint32_t ccr;

    if (pct > 100) pct = 100;
    ccr = (uint32_t)(BL_ARR - 1) * pct / 100;   /* 100% → CCR=ARR-1 恒高 */
    __HAL_TIM_SET_COMPARE(&g_htim3, TIM_CHANNEL_3, ccr);
}

void sys_backlight_init(void)
{
    GPIO_InitTypeDef gi = {0};
    TIM_OC_InitTypeDef oc = {0};
    uint8_t ok = 1;

    __HAL_RCC_TIM3_CLK_ENABLE();

    /* PB0：复用推挽输出（TIM3_CH3 默认映射，无需 AFIO 重映射） */
    gi.Pin = GPIO_PIN_0;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gi);

    /* 1MHz 计数时钟 / 20kHz 周期 */
    g_htim3.Instance = TIM3;
    g_htim3.Init.Prescaler = 72 - 1;
    g_htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
    g_htim3.Init.Period = BL_ARR - 1;
    g_htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    g_htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&g_htim3) != HAL_OK) ok = 0;

    /* 配置结构必须清零：OCFastMode/OCNPolarity 等字段不填会写垃圾值进寄存器 */
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = 0;   /* 魔改 HAL 无 TIM_OCFASTMODE_DISABLE 宏，0=关闭快速比较 */
    if (HAL_TIM_PWM_ConfigChannel(&g_htim3, &oc, TIM_CHANNEL_3) != HAL_OK) ok = 0;
    if (HAL_TIM_PWM_Start(&g_htim3, TIM_CHANNEL_3) != HAL_OK) ok = 0;

    if (ok) {
        sys_backlight_set(g_sys_cfg.brightness);   /* 按配置亮度点亮 */
    } else {
        /* 回退 GPIO 全亮：界面至少可见（亮度调节失效，但系统可用可诊断）。
         * 注意 PB5 是心跳灯（DefaultTask 翻转），不可用作诊断输出 */
        gi.Pin = GPIO_PIN_0;
        gi.Mode = GPIO_MODE_OUTPUT_PP;
        HAL_GPIO_Init(GPIOB, &gi);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    }
}
