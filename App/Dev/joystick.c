/**
 * @file    joystick.c
 * @brief   dev 层：输入设备驱动实现
 *
 * 内容：
 *   - 摇杆双轴 ADC 采样：死区 ±100 → 灵敏度换算 (delta*sens)/512 → 步长限幅 ±8
 *   - SW 按键边沿检测：按下发 EV_KEY_DOWN，松开发 EV_KEY_UP
 *   - K0（PE4）设备开关：软件消抖（连续 2 次一致）+ 按下边沿 → EV_DEV_TOGGLE
 *   - K1（PE3）返回键：同样消抖，按下边沿 → EV_BACK（永远可扫，不受设备开关影响）
 *   - 设备开关 g_js_on：默认 OFF；OFF 时摇杆与 SW 事件全部停发
 *     （"设备未连接"：光标不动、按键无效，K0 本身永远可扫——开关不依赖设备）
 */
#include "joystick.h"
#include "main.h"
#include "adc.h"
#include "app_config.h"

#define JOY_CENTER      2048   /* 12bit ADC 中点 */
#define JOY_DEADZONE    100    /* 死区：回中误差范围 */
#define JOY_STEP_MAX    8      /* 单次移动步长限幅 */
#define K0_SHAKE_CNT    2      /* 消抖：连续 N 次采样一致才有效 */

/* 设备开关：上电默认 OFF（未连接） */
uint8_t g_js_on = 0;

void joystick_toggle(void)
{
    g_js_on ^= 1;
}

uint8_t joystick_is_on(void)
{
    return g_js_on;
}

/* 摇杆位移换算：返回 0 表示在死区内 */
static int16_t joy_axis_to_step(uint16_t adc)
{
    int16_t delta, step;

    delta = (int16_t)adc - JOY_CENTER;
    if (delta > -JOY_DEADZONE && delta < JOY_DEADZONE) return 0;

    step = (int16_t)((delta * g_sys_cfg.cursor_sens) / 512);
    if (step > JOY_STEP_MAX)  step = JOY_STEP_MAX;
    if (step < -JOY_STEP_MAX) step = -JOY_STEP_MAX;
    return step;
}

uint8_t joystick_scan(input_event_t *ev)
{
    uint16_t adc1, adc2;
    int16_t dx, dy;
    uint8_t lvl, sw_now;
    static uint8_t k0_lvl = 1;    /* K0 上次采样电平（1=释放，KEY0 低有效） */
    static uint8_t k0_cnt = 0;    /* K0 连续一致计数 */
    static uint8_t k0_prev = 1;   /* K0 上次稳定电平 */
    static uint8_t k1_lvl = 1;    /* K1（返回键）同样消抖变量，模式与 K0 完全一致 */
    static uint8_t k1_cnt = 0;
    static uint8_t k1_prev = 1;
    static uint8_t sw_prev = 1;   /* SW 上次状态（1=释放） */

    /* --- K0 设备开关：永远扫描（OFF 时开关本身必须可用） --- */
    lvl = (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == GPIO_PIN_RESET) ? 0 : 1;
    if (lvl == k0_lvl) {
        if (k0_cnt < 0xFF) k0_cnt++;
    } else {
        k0_cnt = 0;
        k0_lvl = lvl;
    }
    if (k0_cnt >= K0_SHAKE_CNT && k0_lvl == 0 && k0_prev == 1) {
        /* 消抖通过 + 按下边沿 → 开关取反，立即返回（边沿事件不能丢） */
        joystick_toggle();
        k0_prev = 0;
        ev->type = EV_DEV_TOGGLE;
        ev->dx = 0;
        ev->dy = 0;
        return 1;
    }
    if (k0_cnt >= K0_SHAKE_CNT && k0_lvl == 1 && k0_prev == 0) k0_prev = 1;

    /* --- K1 返回键：与 K0 一样永远可扫（板载按键，不依赖摇杆设备）。
     *     按下边沿 → EV_BACK（导航键：应用→桌面→锁屏逐级回退） --- */
    lvl = (HAL_GPIO_ReadPin(KEY1_GPIO_Port, KEY1_Pin) == GPIO_PIN_RESET) ? 0 : 1;
    if (lvl == k1_lvl) {
        if (k1_cnt < 0xFF) k1_cnt++;
    } else {
        k1_cnt = 0;
        k1_lvl = lvl;
    }
    if (k1_cnt >= K0_SHAKE_CNT && k1_lvl == 0 && k1_prev == 1) {
        k1_prev = 0;
        ev->type = EV_BACK;
        ev->dx = 0;
        ev->dy = 0;
        return 1;
    }
    if (k1_cnt >= K0_SHAKE_CNT && k1_lvl == 1 && k1_prev == 0) k1_prev = 1;

    /* --- 设备关闭：摇杆与 SW 全部停发（"设备未连接"：光标不动、按键无效） --- */
    if (!g_js_on) return 0;

    /* --- 摇杆双轴（PA4 → ADC1_IN4 / PA5 → ADC2_IN5） --- */
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 10);
    adc1 = HAL_ADC_GetValue(&hadc1);

    HAL_ADC_Start(&hadc2);
    HAL_ADC_PollForConversion(&hadc2, 10);
    adc2 = HAL_ADC_GetValue(&hadc2);

    dx = joy_axis_to_step(adc1);   /* X：向右推 VRx 电压升高 → dx 为正（方向不对改这里符号） */
    dy = joy_axis_to_step(adc2);   /* Y 方向已实测正确，如反向改这里取负 */

    /* 有位移才发移动事件（静置不发，省队列占用） */
    if (dx != 0 || dy != 0) {
        ev->type = EV_MOUSE_MOVE;
        ev->dx = dx;
        ev->dy = dy;
        return 1;
    }

    /* --- SW 按键边沿（按下 DOWN / 松开 UP） --- */
    sw_now = (HAL_GPIO_ReadPin(JOY_SW_GPIO_Port, JOY_SW_Pin) == GPIO_PIN_RESET) ? 0 : 1;
    if (sw_now != sw_prev) {
        sw_prev = sw_now;
        ev->type = sw_now ? EV_KEY_UP : EV_KEY_DOWN;
        ev->dx = 0;
        ev->dy = 0;
        return 1;
    }

    return 0;
}
