/**
 * @file    sys_wdg.h
 * @brief   系统看门狗：IWDG 独立看门狗 + 复位原因记录（进阶②）
 *
 * 分层职责：
 *   - 硬件层：IWDG（LSI 40kHz / 预分频 64 / 重载 1250 → 2 秒超时），
 *     喂狗点 = input_task（15ms 周期、从不长阻塞）。喂狗链断了
 *     （关键任务死循环）→ 2 秒后硬件复位，系统自动恢复。
 *   - 证据层：复位后查 RCC 复位标志，判断"上次复位是否看门狗触发"，
 *     供启动流程记日志（RAM 日志复位即丢，标志是唯一证据）。
 *
 * 喂狗点选择：不放在 ui_task——格式化文件要 2 秒，会误复位；
 * input_task 是最短周期、最不会阻塞的任务，喂狗余量巨大（15ms vs 2s）
 */
#ifndef __SYS_WDG_H
#define __SYS_WDG_H

#include <stdint.h>

void sys_wdg_init(void);            /* 使能 IWDG（2s 超时）+ 记录复位原因（启动时调一次） */
void sys_wdg_feed(void);            /* 喂狗（input_task 每轮循环调） */
uint8_t sys_wdg_was_reset(void);    /* 上次复位是否为看门狗触发（init 后查询） */

#endif
