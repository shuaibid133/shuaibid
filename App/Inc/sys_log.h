/**
 * @file    sys_log.h
 * @brief   系统运行日志接口：RAM 环形缓冲 + RTC 时间戳
 *
 * 覆盖范围（对应题目"日志记录"验收点）：
 *   登录失败/锁定、文件操作、设置修改、输入设备断开与恢复、
 *   熄屏唤醒、错误事件；SW 清空日志（app_logs 内实现）
 */
#ifndef __SYS_LOG_H
#define __SYS_LOG_H

#include <stdint.h>

/* 日志消息 ID：字符串表在 app_logs.c 里与之一一对应（按分组排列） */
typedef enum {
    /* ---- 系统启动与时钟 ---- */
    LOG_BOOT_OK = 0,     /* 系统启动完成 */
    LOG_RTC_LSE,         /* LSE 起振失败，降级 LSI */
    LOG_RTC_SET,         /* 用户手动校时 */
    LOG_LCD_FAIL,        /* LCD 初始化失败 */

    /* ---- 登录 ---- */
    LOG_LOGIN_FAIL,      /* 密码错误（param=连续失败次数） */
    LOG_LOGIN_LOCK,      /* 连续错误触发锁定（param=锁定秒数） */

    /* ---- 文件操作 ---- */
    LOG_FS_MOUNT,        /* 挂载失败（param=失败阶段 1/2/3） */
    LOG_FS_REBUILD,      /* 卷内容损坏，自动格式化重建 */
    LOG_FS_UPGRADE,      /* 旧版演示文件升级为新版 */
    LOG_FS_WRITE,        /* 文件创建/写入失败 */
    LOG_FS_DEL,          /* 文件删除失败 */
    LOG_FS_DELETE,       /* 文件删除成功 */
    LOG_FS_FULL,         /* 文件编号用尽（防御性记录） */
    LOG_FILE_CREATED,    /* 文件新建成功（param=编号） */

    /* ---- 设置修改 ---- */
    LOG_SET_SENS,        /* 光标灵敏度（param=新值） */
    LOG_SET_CSIZE,       /* 光标大小（param=新值） */
    LOG_SET_BRIGHT,      /* 亮度（param=新值%） */
    LOG_SET_VOL,         /* 音量（param=新值%） */
    LOG_SET_STIME,       /* 熄屏时间（param=新值秒） */

    /* ---- 输入设备断开与恢复 ---- */
    LOG_INPUT_LOST,      /* 摇杆断开（K0 关闭设备） */
    LOG_INPUT_RECOVER,   /* 摇杆恢复（K0 打开设备） */

    /* ---- 熄屏唤醒 ---- */
    LOG_SCREEN_OFF,      /* 空闲超时熄屏 */
    LOG_SCREEN_ON,       /* 输入唤醒 */

    /* ---- 画图 ---- */
    LOG_PAINT_OK,        /* 保存成功（param=耗时秒数） */
    LOG_PAINT_FAIL,      /* 保存失败（param=错误码 E1-E5） */

    /* ---- 存储一致性（进阶③）---- */
    LOG_CFG_CORRUPT,     /* 配置扇区损坏（magic/校验失败），已回退默认值 */
    LOG_IMG_CORRUPT,     /* 图片文件损坏（校验和不符），预览被拒 */

    /* ---- 任务异常检测（进阶②）---- */
    LOG_WDG_RESET,       /* 看门狗超时复位（IWDG 2s 未喂 → 硬件复位后记录） */
    LOG_TASK_HUNG,       /* 任务心跳停止（param=任务ID 0=input 1=music 2=ui） */
    LOG_TASK_RECOVER,    /* 任务心跳恢复（param=任务ID） */

    /* ---- 伪 OTA（进阶⑥）---- */
    LOG_OTA_DOWNLOAD,    /* 更新包下载+落盘校验通过（param=新版本号） */
    LOG_OTA_APPLIED,     /* 更新已应用，重启后生效（param=新版本号） */
    LOG_OTA_ROLLBACK,    /* 校验失败回退（param=1 CRC不符 2 包头坏 3 未知状态） */

    LOG_MSG_N
} log_msg_id_t;

/* 日志级别 */
#define LOG_LV_INFO   0
#define LOG_LV_WARN   1
#define LOG_LV_ERR    2

typedef struct {
    uint8_t  level;      /* LOG_LV_* */
    uint8_t  msg_id;     /* log_msg_id_t */
    uint16_t param;      /* 附加参数（秒数/错误码/失败阶段/新值） */
    uint8_t  hour, min, sec;   /* RTC 时间戳 */
} log_entry_t;

void sys_log_add(uint8_t level, uint8_t msg_id, uint16_t param);  /* 追加一条（覆盖最旧） */
uint8_t sys_log_count(void);                                      /* 当前条数（0-32） */
const log_entry_t *sys_log_get(uint8_t idx);                      /* 0=最新 1=次新 ... */
void sys_log_clear(void);                                         /* 清空 */

#endif
