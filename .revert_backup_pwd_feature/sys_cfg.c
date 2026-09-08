/**
 * @file    sys_cfg.c
 * @brief   系统配置持久化：外部 Flash（W25Q128）尾部专用扇区裸写
 *
 * 为什么裸写扇区而不是存 FATFS 文件：
 *   - FATFS 卷由 Files 应用独占挂载，配置若存文件，开机加载必须等 FS
 *     初始化（首次还要格式化 3~10 秒）——而光标灵敏度/亮度/音量在
 *     登录界面就该生效，等不起
 *   - 专用扇区 0xFFE000（末 8KB 区）在 FAT 卷尾部：FAT16 的簇分配从
 *     数据区头部顺序增长，演示期文件总量远小于 16MB 卷容量，不会
 *     分配到这里（与 rw_self_test 的 0xFFF000 同一前提，工程折衷）
 *   - 读写毫秒级：启动加载零等待；保存 = 擦 4KB + 写 10B ≈ 300ms
 *
 * 扇区布局：magic(2B "KZ") + 校验和(2B，配置字节求和) + sys_config_t
 * 校验和防脏数据（半写/错位）→ 校验不过就回默认，不让硬件带病启动
 */
#include "sys_cfg.h"
#include "app_config.h"
#include "w25q128.h"
#include "sys_log.h"
#include <string.h>

#define CFG_SECTOR      0xFFE000u   /* 专用扇区：末 8KB 区（0xFFF000 是自检区） */
#define CFG_MAGIC       0x4B5Au     /* "KZ" = KazepOS */

static uint8_t s_buf[256];          /* 读写缓冲（4KB 扇区只用到前 10 字节） */

/* 字节求和校验（数据 = 配置结构体） */
static uint16_t cfg_sum(const uint8_t *p, uint16_t n)
{
    uint16_t s = 0;

    while (n--) s = (uint16_t)(s + *p++);
    return s;
}

void sys_cfg_load(void)
{
    uint16_t sum;

    w25q128_init();                 /* 位带驱动初始化（幂等，毫秒级） */

    /* 先写默认值：未保存过/校验失败都保持默认，不带病启动 */
    g_sys_cfg.cursor_size = 1;
    g_sys_cfg.cursor_sens = 3;
    g_sys_cfg.brightness  = 100;
    g_sys_cfg.volume      = 80;
    g_sys_cfg.screen_time = 60;
    g_sys_cfg.version     = 1;   /* 出厂版本 V1.0 */
    g_sys_cfg.js_lock     = 1;   /* 出厂 JS Lock：开（摇杆默认 OFF，按 K0 用） */
    strcpy(g_sys_cfg.pwd, "1234");   /* 出厂登录密码 */

    w25q128_read(CFG_SECTOR, s_buf, sizeof(s_buf));
    if (s_buf[0] == 0xFF && s_buf[1] == 0xFF) return;   /* 擦除态：从未保存过（首启，不算损坏） */

    /* 进阶③ 存储一致性：magic/校验和任一不符 = 半写/错位脏数据。
     * 回退默认值 + 记日志——"配置损坏"必须留证据（Logs 可查），
     * 而不是静默回到默认（用户会以为设置被清空了） */
    if (s_buf[0] != (CFG_MAGIC & 0xFF) || s_buf[1] != (CFG_MAGIC >> 8)) {
        sys_log_add(LOG_LV_ERR, LOG_CFG_CORRUPT, 1);    /* param=1: magic 不符 */
        return;
    }
    sum = cfg_sum(s_buf + 4, sizeof(sys_config_t));
    if (s_buf[2] != (sum & 0xFF) || s_buf[3] != (sum >> 8)) {
        sys_log_add(LOG_LV_ERR, LOG_CFG_CORRUPT, 2);    /* param=2: 校验和不符 */
        return;
    }

    memcpy(&g_sys_cfg, s_buf + 4, sizeof(sys_config_t));

    /* 范围钳制：旧版本/脏数据的越界值可能把硬件带坏。
     * version 钳制为 1：老固件保存的配置没有 version 字段（rsv=0），
     * 升级固件后读到的 0 视为出厂版本——不能带 0 运行 */
    if (g_sys_cfg.cursor_size < 1 || g_sys_cfg.cursor_size > 4) g_sys_cfg.cursor_size = 1;
    if (g_sys_cfg.cursor_sens < 1 || g_sys_cfg.cursor_sens > 10) g_sys_cfg.cursor_sens = 3;
    if (g_sys_cfg.brightness < 10 || g_sys_cfg.brightness > 100) g_sys_cfg.brightness = 100;
    if (g_sys_cfg.volume > 100) g_sys_cfg.volume = 80;
    if (g_sys_cfg.screen_time > 300) g_sys_cfg.screen_time = 60;
    if (g_sys_cfg.version < 1 || g_sys_cfg.version > 99) g_sys_cfg.version = 1;
    if (g_sys_cfg.js_lock > 1) g_sys_cfg.js_lock = 1;   /* 布尔值：脏数据一律按 1（自动锁，最保守） */

    /* 密码钳制：任一位不是数字 = 脏数据（半写/擦除态 0xFF 都是垃圾字节）→
     * 整体回默认。必须兜底——带垃圾字节的密码会让登录比对永远失败锁死入口 */
    {
        uint8_t i;
        for (i = 0; i < 4; i++) {
            if (g_sys_cfg.pwd[i] < '0' || g_sys_cfg.pwd[i] > '9') {
                strcpy(g_sys_cfg.pwd, "1234");
                break;
            }
        }
    }
}

void sys_cfg_save(void)
{
    uint16_t sum = cfg_sum((const uint8_t *)&g_sys_cfg, sizeof(sys_config_t));

    w25q128_init();                 /* 幂等 */

    s_buf[0] = (uint8_t)(CFG_MAGIC & 0xFF);
    s_buf[1] = (uint8_t)(CFG_MAGIC >> 8);
    s_buf[2] = (uint8_t)(sum & 0xFF);
    s_buf[3] = (uint8_t)(sum >> 8);
    memcpy(s_buf + 4, &g_sys_cfg, sizeof(sys_config_t));

    w25q128_erase_sector(CFG_SECTOR);            /* 写前必擦（NOR 只支持 1→0） */
    w25q128_write(CFG_SECTOR, s_buf, 4 + sizeof(sys_config_t));
}
