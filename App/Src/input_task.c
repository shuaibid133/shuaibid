/**
 * @file    input_task.c
 * @brief   输入任务实现：15ms 周期轮询
 *          ADC1_IN0（PA0/VRx）、ADC2_IN2（PA2/VRy）+ 按键（PA3/SW）
 *          处理：死区 ±100 → 灵敏度换算 (delta*sens)/512 → 步长限幅 ±8
 */
#include "input_task.h"
#include "main.h"
#include "adc.h"
#include "app_config.h"
#include "event.h"
#include "cmsis_os.h"

#define JOY_CENTER      2048   /* 12bit ADC 中点 */
#define JOY_DEADZONE    100    /* 死区：回中误差范围 */
#define JOY_STEP_MAX    8      /* 单次移动步长限幅 */
#define JOY_PERIOD_MS   15     /* 采样周期 */

void input_task(void *argument)
{
    uint16_t adc1, adc2;
    uint8_t sw_prev = 1;          /* 上次按键状态：1 = 释放 */
    int16_t dx, dy, delta;
    input_event_t ev;

    for (;;)
    {
        /* 读 X 轴（PA0 → ADC1_IN0） */
        HAL_ADC_Start(&hadc1);
        HAL_ADC_PollForConversion(&hadc1, 10);
        adc1 = HAL_ADC_GetValue(&hadc1);

        /* 读 Y 轴（PA2 → ADC2_IN2） */
        HAL_ADC_Start(&hadc2);
        HAL_ADC_PollForConversion(&hadc2, 10);
        adc2 = HAL_ADC_GetValue(&hadc2);

        /* X 轴：向右推 VRx 电压升高 → dx 为正（方向不对改这里符号） */
        delta = (int16_t)adc1 - JOY_CENTER;
        if (delta > JOY_DEADZONE || delta < -JOY_DEADZONE)
        {
            dx = (int16_t)((delta * g_sys_cfg.cursor_sens) / 512);
            if (dx > JOY_STEP_MAX)  dx = JOY_STEP_MAX;
            if (dx < -JOY_STEP_MAX) dx = -JOY_STEP_MAX;
        }
        else
        {
            dx = 0;
        }

        /* Y 轴：屏幕 y 向下为正，向前推 VRy 电压升高 → dy 取负 */
        delta = (int16_t)adc2 - JOY_CENTER;
        if (delta > JOY_DEADZONE || delta < -JOY_DEADZONE)
        {
            dy = (int16_t)((delta * g_sys_cfg.cursor_sens) / 512);
            if (dy > JOY_STEP_MAX)  dy = JOY_STEP_MAX;
            if (dy < -JOY_STEP_MAX) dy = -JOY_STEP_MAX;
        }
        else
        {
            dy = 0;
        }

        /* 有位移才发移动事件（静置不发，省队列占用） */
        if (dx != 0 || dy != 0)
        {
            ev.type = EV_MOUSE_MOVE;
            ev.dx = dx;
            ev.dy = dy;
            xQueueSend(g_event_queue, &ev, 0);
        }

        /* 按键：边沿检测（按下瞬间发 EV_KEY_DOWN，松开发 EV_KEY_UP） */
        if (HAL_GPIO_ReadPin(JOY_SW_GPIO_Port, JOY_SW_Pin) == GPIO_PIN_RESET)
        {
            if (sw_prev == 1)
            {
                ev.type = EV_KEY_DOWN;
                ev.dx = 0;
                ev.dy = 0;
                xQueueSend(g_event_queue, &ev, 0);
            }
            sw_prev = 0;
        }
        else
        {
            if (sw_prev == 0)
            {
                ev.type = EV_KEY_UP;
                ev.dx = 0;
                ev.dy = 0;
                xQueueSend(g_event_queue, &ev, 0);
            }
            sw_prev = 1;
        }

        osDelay(JOY_PERIOD_MS);
    }
}
