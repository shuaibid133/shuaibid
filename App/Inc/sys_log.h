/**
 * @file    sys_log.h
 * @brief   系统运行日志接口：RAM 环形缓冲 + RTC 时间戳
 */
#ifndef __SYS_LOG_H
#define __SYS_LOG_H

#include <stdint.h>

/* 日志消息 ID：字符串表在 app_logs.c 里与之一一对应 */
typedef enum {
    LOG_BOOT_OK = 0,     /* 系统启动完成 */
    LOG_RTC_LSE,         /* LSE 起振失败，降级 LSI */
    LOG_RTC_SET,         /* 用户手动校时 */
    LOG_FS_MOUNT,        /* Flash 文件系统挂载失败（param=失败阶段 1/2/3） */
    LOG_FS_REBUILD,      /* 卷内容损坏，自动格式化重建 */
    LOG_FS_UPGRADE,      /* 旧版演示文件升级为新版 */
    LOG_FS_WRITE,        /* 文件创建/写入失败 */
    LOG_FS_DEL,          /* 文件删除失败 */
    LOG_FS_FULL,         /* 文件编号用尽（防御性记录） */
    LOG_PAINT_OK,        /* 画图保存成功（param=耗时秒数） */
    LOG_PAINT_FAIL,      /* 画图保存失败（param=错误码 E1-E5） */
    LOG_MSG_N
} log_msg_id_t;

/* 日志级别 */
#define LOG_LV_INFO   0
#define LOG_LV_WARN   1
#define LOG_LV_ERR    2

typedef struct {
    uint8_t  level;      /* LOG_LV_* */
    uint8_t  msg_id;     /* log_msg_id_t */
    uint16_t param;      /* 附加参数（秒数/错误码/失败阶段） */
    uint8_t  hour, min, sec;   /* RTC 时间戳 */
} log_entry_t;

void sys_log_add(uint8_t level, uint8_t msg_id, uint16_t param);  /* 追加一条（覆盖最旧） */
uint8_t sys_log_count(void);                                      /* 当前条数（0-32） */
const log_entry_t *sys_log_get(uint8_t idx);                      /* 0=最新 1=次新 ... */
void sys_log_clear(void);                                         /* 清空 */

#endif
