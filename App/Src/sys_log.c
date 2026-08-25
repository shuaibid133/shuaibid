/**
 * @file    sys_log.c
 * @brief   系统运行日志：RAM 环形缓冲（32 条），重启即清
 *
 * 为什么不写 Flash：W25Q128 克隆片 4K 擦除实测 1.5~2s，日志是高频小量
 * 写入，落盘既烧擦除寿命又拖慢主流程。演示期日志只作现场查看用，
 * 断电丢日志可以接受——这个权衡在头文件注释里写死，防止后人改成落盘。
 *
 * 线程模型：所有 sys_log_add 调用都发生在 ui_task 上下文（Boot OK 在
 * ui_task 启动序列，RTC/FS/Paint 埋点都在 ui_task 内），读方 app_logs
 * 也在 ui_task → 天然无竞争，不需要加锁。
 *
 * 时间戳注意：rtc_app_init 里 LSE 降级分支也会记日志，此时 RTC 还没
 * 初始化完成，读出来的时分秒是寄存器默认值（0:00:00），无妨——这条
 * 日志本身说明问题，时间戳误导不了人。
 */
#include "sys_log.h"
#include "rtc_app.h"

#define LOG_MAX   32

static log_entry_t s_log[LOG_MAX];
static uint8_t     s_head = 0;      /* 下一个写入位置 */
static uint8_t     s_count = 0;

void sys_log_add(uint8_t level, uint8_t msg_id, uint16_t param)
{
    rtc_datetime_t dt;
    log_entry_t *e = &s_log[s_head];

    rtc_app_get_datetime(&dt);
    e->level  = level;
    e->msg_id = msg_id;
    e->param  = param;
    e->hour   = dt.hour;
    e->min    = dt.min;
    e->sec    = dt.sec;

    s_head = (uint8_t)((s_head + 1) % LOG_MAX);
    if (s_count < LOG_MAX) s_count++;
}

uint8_t sys_log_count(void)
{
    return s_count;
}

/* idx=0 取最新：从 s_head 往回数 idx+1 条，越界回绕 */
const log_entry_t *sys_log_get(uint8_t idx)
{
    int16_t i;

    i = (int16_t)s_head - 1 - (int16_t)idx;
    if (i < 0) i += LOG_MAX;
    return &s_log[(uint8_t)i];
}

void sys_log_clear(void)
{
    s_count = 0;
}
