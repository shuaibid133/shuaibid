/**
 * @file    ui_task.c
 * @brief   UI 任务实现：独占屏幕渲染（所有绘制都在本任务完成，避免多任务抢 LCD）
 *          事件循环：收到事件 → 移动事件先更新光标 → 全部交给桌面框架处理
 *          桌面框架（desktop.c）内部管理界面状态机（BOOT/LOGIN/DESKTOP）
 */
#include "ui_task.h"
#include "main.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "cursor.h"
#include "desktop.h"
#include "rtc_app.h"
#include "sys_cfg.h"
#include "sys_backlight.h"
#include "app_music.h"
#include "app_config.h"
#include "sys_log.h"
#include "sys_stats.h"
#include "sys_wdg.h"
#include "sys_watch.h"
#include "sys_ota.h"
#include "FreeRTOS.h"
#include "task.h"

/* 进阶②演示 B：1 = ui_task 在开机 10s 起卡死 10 秒（屏幕定格 = 直观的
 * 卡死证据；LED 心跳灯也停闪）。喂狗者 input_task 照常 → IWDG 不触发
 * （证明硬件层只管喂狗链）；哨兵 2s 内报 ERR Task hung（param=2）；
 * 10s 后自动恢复 → 报 WARN Task recovered——"检测+恢复"闭环。
 * 已实测通过（2026-09-01），现为 0 = 正式版。答辩如需演示，
 * 改 1 重新编译烧录，演示完改回 0 */
#define WDG_DEMO_UI_HANG       0
#define WDG_DEMO_UI_HANG_AT    10000   /* 开机 10s 起 */
#define WDG_DEMO_UI_HANG_MS    10000   /* 卡死时长 */

void ui_task(void *argument)
{
    input_event_t ev, ev2;
    uint32_t delay, q_now;
    uint8_t ret;

    /* 诊断灯（PB5/DS0，低电平点亮）：定位启动卡死阶段——
     * 灭=进入 ui_task（FreeRTOS 起来了）；亮=LCD 初始化成功；
     * 再灭=全部初始化完成；事件循环每秒闪=系统正常。
     * 若灯 2s 一亮一灭循环 = IWDG 看门狗复位循环 */
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);   /* 灭 */

    ret = atk_md0280_init();   /* 0 = 屏幕初始化成功 */

    if (ret == 0)
    {
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);  /* 亮：LCD OK */
        atk_md0280_clear(ATK_MD0280_WHITE);
        rtc_app_init();                /* 时间源：内部 RTC（LSE，VBAT 保持） */
        sys_log_add(LOG_LV_INFO, LOG_BOOT_OK, 0);   /* 日志：开机启动完成 */
        sys_backlight_init();          /* 先点亮背光（默认亮度），再加载配置——
                                        * 即使配置读取异常，屏幕也可见（好诊断） */
        sys_cfg_load();                /* Flash → g_sys_cfg：灵敏度/亮度等在登录前生效 */
        sys_backlight_set(g_sys_cfg.brightness);   /* 应用配置的亮度 */
        app_music_pin_idle();          /* Music 蜂鸣器空闲静音：PA1 推挽输出高。
                                        * 开机即拉高，进 Music 前不依赖任何应用
                                        * 初始化，杜绝"打开应用前引脚悬空/被拉低" */
        desktop_init();                /* BOOT 2秒 → LOGIN，内部完成光标接管 */
    }
    else
    {
        /* LCD 初始化失败：屏幕不可用（日志数组在 RAM，固件复位后可从
         * 代码路径/串口排查；此处记录符合"错误事件"验收点） */
        sys_log_add(LOG_LV_ERR, LOG_LCD_FAIL, ret);
    }

    /* 进阶②：所有初始化完成后再开看门狗（2s 超时，喂狗者 input_task
     * 已稳定运行多时）。复位原因也在此查——RTC 已就绪，日志时间戳有效。
     * 注意：不能放在最前面——RTC 未初始化时 sys_log_add 读时间会 HardFault */
    sys_wdg_init();
    if (sys_wdg_was_reset()) {
        sys_log_add(LOG_LV_ERR, LOG_WDG_RESET, 0);
    }

    /* 进阶⑥ 伪 OTA：启动校验更新区——版本生效 / 应用断电补应用 /
     * 包损坏回退。放这里：RTC 已就绪（日志时间戳有效）；哨兵尚未
     * 收到本任务打卡（beat==0 跳过），阻塞擦写不会误报卡死 */
    sys_ota_boot_check();

    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);   /* 灭：初始化全完成 */

    for (;;)
    {
        /* 演示 B：开机 10s 起模拟卡死 10 秒。窗口内不处理任何事件——
         * 屏幕定格、LED 停闪、心跳停打（卡死证据全部可见），但必须让出
         * CPU：喂狗者 input_task 照常跑 → IWDG 不触发，哨兵 2s 内报
         * ERR Task hung(param=2)；窗口结束自然恢复 → WARN Task recovered。
         * 注意不能 while(1) 死循环——那是"永久卡死"（演示 A 用，喂狗者
         * 自己也卡死、靠 IWDG 兜底）；"暂时卡死"的恢复检查必须在循环里，
         * vTaskDelay 让出 CPU 的同时每 100ms 回来查一次窗口 */
        if (WDG_DEMO_UI_HANG) {
            uint32_t now = xTaskGetTickCount();
            if (now >= WDG_DEMO_UI_HANG_AT &&
                now < WDG_DEMO_UI_HANG_AT + WDG_DEMO_UI_HANG_MS) {
                vTaskDelay(100);
                continue;   /* 窗口内永不处理事件 = 卡死 */
            }
        }

        if (xQueueReceive(g_event_queue, &ev, portMAX_DELAY) == pdPASS)
        {
            /* 进阶④ 负载分析（消费侧统计，满足单写者）：
             * 排队延迟 = 当前 tick - 生产 tick（1kHz → 单位 ms；
             * uint32 无符号减法天然处理回绕，生产者总早于消费者） */
            delay = xTaskGetTickCount() - ev.tick;
            if (delay > g_stats_dly_max) g_stats_dly_max = delay;
            g_stats_dly_sum += delay;
            g_stats_dly_cnt++;

            /* 队列峰值积压：+1 = 刚取出的这条也算"曾占队列" */
            q_now = uxQueueMessagesWaiting(g_event_queue) + 1;
            if (q_now > g_stats_q_peak) g_stats_q_peak = q_now;

            /* 进阶① 输入事件优化：消费侧合并——把队列里紧随其后的移动
             * 事件捞出来合并成一条（dx/dy 累加，光标只动一次）。
             * 猛推摇杆时 input_task 15ms 产一批增量事件，逐条处理会让
             * 光标滞后（④ 里 Dly-Max 30ms 就是证据）；合并不丢信息：
             * 移动是增量式，2 条 dx=4 合 1 条 dx=8，最终位置相同。
             * 顺序保持：捞到非移动事件（EV_TICK/按键）立即插回队首，
             * 下一轮主循环先处理它——全系统单消费者，插回必然被自己
             * 取到，无竞态 */
            while (xQueueReceive(g_event_queue, &ev2, 0) == pdPASS) {
                if (ev2.type != EV_MOUSE_MOVE) {
                    xQueueSendToFront(g_event_queue, &ev2, 0);
                    break;
                }
                ev.dx = (int16_t)(ev.dx + ev2.dx);
                ev.dy = (int16_t)(ev.dy + ev2.dy);
                g_stats_merged++;
            }

            /* 移动事件：先更新光标，再通知桌面框架（键盘高亮等） */
            if (ev.type == EV_MOUSE_MOVE)
            {
                cursor_move(ev.dx, ev.dy);
            }
            desktop_handle_event(&ev);

            /* 进阶②：ui_task 心跳——EV_TICK 每秒必有，事件驱动下
             * 每轮循环都打卡，哨兵（input_task）据此判定本任务是否卡死 */
            sys_watch_beat(WATCH_UI);

            /* 诊断灯：每秒闪一下 = 事件循环活着 */
            if (ev.type == EV_TICK) HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
        }
    }
}
