/**
 * @file    sys_watch.h
 * @brief   任务心跳监控（进阶②）：软件层"任务异常检测"
 *
 * 与硬件层 IWDG 的分工：
 *   - IWDG 只保护"喂狗链"（input_task 死循环 → 硬件复位），
 *     它不知道别的任务卡没卡——ui/music 卡死时 input_task 照常喂狗，
 *     系统不会复位，但系统其实已经"半瘫"。
 *   - 心跳监控补上这一层：每个任务在自己循环里"打卡"（记录当前 tick），
 *     input_task 作为健康哨兵每 15ms 检查所有打卡——某任务超过阈值
 *     没打卡 = 卡死 → 记日志（ERR Task hung），心跳恢复 → 记日志（恢复）。
 *
 * 心跳语义：不是"每秒必须动一次"，而是"任务最近一次活过的时间"。
 * 阻塞等待事件是正常态（music 等队列、ui 等事件），阈值按任务放宽：
 *   input：15ms 周期 → 1s 阈值（60 拍没打卡才算死）
 *   music：事件驱动 + 音符最长 1s → 3s 阈值
 *   ui：EV_TICK 每秒必有 → 2s 阈值
 * 哨兵自己不打卡？打——哨兵死了 IWDG 兜底（喂狗链就是它）。
 */
#ifndef __SYS_WATCH_H
#define __SYS_WATCH_H

#include <stdint.h>

#define WATCH_INPUT  0    /* input_task（喂狗者+哨兵） */
#define WATCH_MUSIC  1    /* music_task（事件驱动） */
#define WATCH_UI     2    /* ui_task（事件驱动） */
#define WATCH_N      3

void sys_watch_beat(uint8_t slot);   /* 任务循环里打卡：记当前 tick */
void sys_watch_check(void);          /* 哨兵每轮调用：超时打卡 → 记日志 */
void sys_watch_set_interval(uint8_t slot, uint32_t ms);
                                     /* 临时放宽阈值：长阻塞操作（Flash 擦写）
                                       前声明豁免窗口，哨兵不误报卡死 */

#endif
