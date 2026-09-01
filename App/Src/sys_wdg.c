/**
 * @file    sys_wdg.c
 * @brief   系统看门狗实现：IWDG 独立看门狗 + 复位原因记录（进阶②）
 *
 * 超时计算：LSI 标称 40kHz → 预分频 64 = 625Hz → 重载 1250 = 2.0 秒。
 * 注意 LSI 实际频率有偏差（30~60kHz），标称 2s 实际约 2.6~1.3s，
 * 喂狗周期 15ms 余量依旧巨大，演示无影响。
 *
 * IWDG 一旦使能无法软件关闭（只有复位/断电才能停）——这也是看门狗
 * 的语义：它不可被"误关"，只能被喂。
 */
#include "sys_wdg.h"
#include "main.h"
#include "stm32f1xx_hal.h"

static IWDG_HandleTypeDef s_hiwdg;
static uint8_t s_wdg_reset = 0;     /* init 时查出的复位原因 */

void sys_wdg_init(void)
{
    /* 先查复位原因（RCC_CSR 标志，看门狗复位会置 IWDGRSTF），再清标志——
     * 不清的话下次复位标志叠加，无法区分"这次是不是看门狗复位" */
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) {
        s_wdg_reset = 1;
    }
    __HAL_RCC_CLEAR_RESET_FLAGS();

    s_hiwdg.Instance = IWDG;
    s_hiwdg.Init.Prescaler = IWDG_PRESCALER_64;   /* LSI 40k/64 = 625Hz */
    s_hiwdg.Init.Reload = 1250;                   /* 625Hz × 2s = 1250 拍 */
    if (HAL_IWDG_Init(&s_hiwdg) != HAL_OK) {
        s_wdg_reset |= 0x80;    /* 使能失败：标记异常（sys_wdg_was_reset 返回非 0） */
    }
}

void sys_wdg_feed(void)
{
    /* 关键时序：喂狗者（input_task）优先级最高、先于 sys_wdg_init
     * （ui_task 里）执行——句柄未初始化时 Instance 为 NULL，直接刷新
     * 会 HardFault 打崩整个系统。IWDG 未使能时喂狗无意义，跳过即可
     * （sys_wdg_init 完成后下一轮自然开始喂） */
    if (s_hiwdg.Instance == NULL) return;
    HAL_IWDG_Refresh(&s_hiwdg);
}

uint8_t sys_wdg_was_reset(void)
{
    return s_wdg_reset;
}
