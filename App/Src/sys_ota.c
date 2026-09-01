/**
 * @file    sys_ota.c
 * @brief   伪 OTA 实现（进阶⑥）
 *
 * "伪"在哪：固件是演示数据（const 数组模拟服务器包），机制全真：
 * 下载 → 读回 CRC32 校验 → 应用（状态 0x5A→0xA5）→ 软复位 →
 * 启动校验版本生效；任一步损坏 → 回退旧版 + ERR 日志。
 *
 * 断电安全：下载 = 先擦全区 → 写镜像 → 最后写包头。中途断电时包头
 * 还是擦除态 0xFF → 启动视为"无更新"——半截包永远不会被当作新版本。
 * 应用 = 包头状态位 0x5A→0xA5（一次性迁移）：重启只读状态，不重复应用。
 *
 * 并发：全部在 ui_task 执行流（单写者原则），无锁。
 * 时序：w25q128 克隆片 4K 擦除实测 1.5~2s——阻塞期间由调用方
 *        （app_ota）做心跳豁免，否则哨兵误报 Task hung。
 */
#include "sys_ota.h"
#include "w25q128.h"
#include "sys_log.h"
#include "app_config.h"
#include "sys_cfg.h"
#include <string.h>

#define OTA_SECTOR        0xFFC000u   /* 包头扇区（0xFFE000 是配置区，勿重叠） */
#define OTA_IMAGE_SECTOR  0xFFD000u   /* 镜像扇区 */
#define OTA_BLOCK         256         /* 页写粒度（w25q128 页编程上限） */
#define OTA_HEAD_LEN      16

/* 包头偏移 */
#define OTA_H_VER     6
#define OTA_H_CRC     7
#define OTA_H_LEN     11
#define OTA_H_STATE   13

/* 状态 */
#define OTA_ST_DOWN   0x5A    /* 已下载待应用 */
#define OTA_ST_APPLY  0xA5    /* 已应用待重启生效 */

/* "服务器"新版本号 + 固件内容（演示数据，≤4KB） */
#define OTA_PKG_VER   2

static const uint8_t s_pkg_image[] = {
    "KazepOS v2.0 firmware image (demo data)\r\n"
    "==========================================\r\n"
    "New in this release:\r\n"
    " 1. OTA update mechanism with rollback\r\n"
    " 2. Boot-time integrity verification\r\n"
    " 3. Runtime load analysis (Monitor)\r\n"
    " 4. Task heartbeat watchdog (IWDG+watch)\r\n"
    " 5. W25Q flash wear optimization\r\n"
    "\r\n"
    "Changelog: see Logs app after reboot\r\n"
};

static uint8_t s_head[OTA_HEAD_LEN];    /* 包头缓冲 */
static uint8_t s_img_buf[4096];         /* 镜像读回校验缓冲（1 扇区） */
static uint8_t s_dl_idx = 0;            /* 已写块数 */

/* ---------- CRC32（查表法，标准多项式 0xEDB88320） ----------
 * 表运行时生成放 RAM（256×4B = 1KB）：静态 const 表要写 256 行，
 * 生成代码更紧凑；项目 RAM 64KB，1KB 无压力 */
static uint32_t s_crc_table[256];
static uint8_t  s_crc_ready = 0;

static void crc32_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[i] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    if (!s_crc_ready) crc32_init();
    for (i = 0; i < n; i++)
        crc = s_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---------- 包头打包/解析 ---------- */
static void head_pack(uint8_t ver, uint32_t crc, uint16_t len, uint8_t st)
{
    s_head[0] = 'K'; s_head[1] = 'Z'; s_head[2] = 'O';
    s_head[3] = 'T'; s_head[4] = 'A'; s_head[5] = '!';
    s_head[OTA_H_VER]   = ver;
    s_head[OTA_H_CRC]   = (uint8_t)crc;
    s_head[OTA_H_CRC+1] = (uint8_t)(crc >> 8);
    s_head[OTA_H_CRC+2] = (uint8_t)(crc >> 16);
    s_head[OTA_H_CRC+3] = (uint8_t)(crc >> 24);
    s_head[OTA_H_LEN]   = (uint8_t)len;
    s_head[OTA_H_LEN+1] = (uint8_t)(len >> 8);
    s_head[OTA_H_STATE] = st;
}

static uint8_t head_ok(void)
{
    return s_head[0]=='K' && s_head[1]=='Z' && s_head[2]=='O' &&
           s_head[3]=='T' && s_head[4]=='A' && s_head[5]=='!';
}

static uint32_t head_crc(void)
{
    return (uint32_t)s_head[OTA_H_CRC] | ((uint32_t)s_head[OTA_H_CRC+1] << 8) |
           ((uint32_t)s_head[OTA_H_CRC+2] << 16) | ((uint32_t)s_head[OTA_H_CRC+3] << 24);
}

static uint16_t head_len(void)
{
    return (uint16_t)(s_head[OTA_H_LEN] | (s_head[OTA_H_LEN+1] << 8));
}

static uint8_t head_st_is_valid(void);   /* 前向声明（定义在文件尾） */

/* 读包头；返回 0 = 擦除态（无更新） */
static uint8_t read_head(void)
{
    w25q128_read(OTA_SECTOR, s_head, OTA_HEAD_LEN);
    return (s_head[0] == 0xFF && s_head[1] == 0xFF) ? 0 : 1;
}

/* 丢弃坏包（擦 2 扇区） + 记回退日志 */
static void discard_pkg(uint16_t why)
{
    sys_log_add(LOG_LV_ERR, LOG_OTA_ROLLBACK, why);
    w25q128_erase_sector(OTA_SECTOR);
    w25q128_erase_sector(OTA_IMAGE_SECTOR);
}

/* ---------- 对外接口 ---------- */
uint8_t sys_ota_check(void)
{
    uint32_t crc;

    if (!read_head()) return 0;                 /* 无包 */
    if (!head_ok()) { discard_pkg(2); return 2; }              /* 包头烂 */
    if (head_st_is_valid() == 0) { discard_pkg(3); return 2; } /* 未知状态 */

    w25q128_read(OTA_IMAGE_SECTOR, s_img_buf, head_len());
    crc = crc32(s_img_buf, head_len());
    if (crc != head_crc()) { discard_pkg(1); return 2; }       /* 镜像对不上 */

    return 1;
}

void sys_ota_download_erase(void)
{
    w25q128_erase_sector(OTA_SECTOR);           /* ~2s 阻塞（克隆片实测） */
}

void sys_ota_download_erase_img(void)
{
    w25q128_erase_sector(OTA_IMAGE_SECTOR);     /* ~2s 阻塞 */
    s_dl_idx = 0;
}

uint8_t sys_ota_download_step(void)
{
    uint32_t off = (uint32_t)s_dl_idx * OTA_BLOCK;
    uint32_t n = (uint32_t)sizeof(s_pkg_image) - off;

    if (n > OTA_BLOCK) n = OTA_BLOCK;
    w25q128_write(OTA_IMAGE_SECTOR + off, s_pkg_image + off, n);
    s_dl_idx++;
    return ((uint32_t)s_dl_idx * OTA_BLOCK < (uint32_t)sizeof(s_pkg_image)) ? 1 : 0;
}

uint8_t sys_ota_download_pct(void)
{
    uint32_t total = ((uint32_t)sizeof(s_pkg_image) + OTA_BLOCK - 1) / OTA_BLOCK;

    if (s_dl_idx >= total) return 100;
    return (uint8_t)(s_dl_idx * 100u / total);
}

uint8_t sys_ota_download_finish(void)
{
    uint32_t crc_rd;

    /* 读回镜像重新算 CRC——校验"写进 Flash 的是不是真的对了"，
     * 不信任内存里的源数据（真 OTA 的落盘校验） */
    w25q128_read(OTA_IMAGE_SECTOR, s_img_buf, sizeof(s_pkg_image));
    crc_rd = crc32(s_img_buf, sizeof(s_pkg_image));
    if (crc_rd != crc32(s_pkg_image, sizeof(s_pkg_image))) return 0;

    head_pack(OTA_PKG_VER, crc_rd, (uint16_t)sizeof(s_pkg_image), OTA_ST_DOWN);
    w25q128_write(OTA_SECTOR, s_head, OTA_HEAD_LEN);   /* 包头最后写：断电安全 */
    sys_log_add(LOG_LV_INFO, LOG_OTA_DOWNLOAD, OTA_PKG_VER);
    return 1;
}

uint8_t sys_ota_apply(void)
{
    /* 0x5A → 0xA5。NOR 只能 1→0，改字节必须先擦包头扇区再重写；
     * 镜像没动，CRC/版本/包长原样保留 */
    if (!read_head()) return 0;
    if (s_head[OTA_H_STATE] != OTA_ST_DOWN) return 0;

    w25q128_erase_sector(OTA_SECTOR);            /* ~2s */
    s_head[OTA_H_STATE] = OTA_ST_APPLY;
    w25q128_write(OTA_SECTOR, s_head, OTA_HEAD_LEN);
    return 1;
}

uint8_t sys_ota_current_version(void)
{
    /* 版本唯一来源 = 配置扇区（与更新包解耦）：
     * 已升级的设备不会因更新区被擦而"降级"；
     * 只有恢复出厂（Paint FMT 全盘擦）才回到 V1——语义自洽 */
    return g_sys_cfg.version;
}

void sys_ota_boot_check(void)
{
    uint32_t crc;

    if (!read_head()) return;                     /* 无更新：保持当前版本 */
    if (!head_ok()) { discard_pkg(2); return; }

    if (s_head[OTA_H_STATE] == OTA_ST_APPLY) {
        /* 正常生效路径：版本写入配置（永久生效）。已是该版本则跳过
         * 保存——避免每次开机白擦一次配置扇区（300ms） */
        if (g_sys_cfg.version != s_head[OTA_H_VER]) {
            g_sys_cfg.version = s_head[OTA_H_VER];
            sys_cfg_save();
        }
        sys_log_add(LOG_LV_INFO, LOG_OTA_APPLIED, g_sys_cfg.version);
        return;
    }
    if (s_head[OTA_H_STATE] == OTA_ST_DOWN) {
        /* 应用步骤断电：启动时 CRC 再验——好：补应用；坏：回退。
         * 这是最后一道防线：更新区坏了就绝不升级 */
        w25q128_read(OTA_IMAGE_SECTOR, s_img_buf, head_len());
        crc = crc32(s_img_buf, head_len());
        if (crc != head_crc()) { discard_pkg(1); return; }
        w25q128_erase_sector(OTA_SECTOR);         /* 补应用（~2s，罕见路径） */
        s_head[OTA_H_STATE] = OTA_ST_APPLY;
        w25q128_write(OTA_SECTOR, s_head, OTA_HEAD_LEN);
        if (g_sys_cfg.version != s_head[OTA_H_VER]) {
            g_sys_cfg.version = s_head[OTA_H_VER];
            sys_cfg_save();
        }
        sys_log_add(LOG_LV_INFO, LOG_OTA_APPLIED, g_sys_cfg.version);
        return;
    }
    discard_pkg(3);                               /* 未知状态：回退 */
}

void sys_ota_inject_bad(void)
{
    uint32_t off, n;

    /* 演示工具：写入 CRC 故意错误的包（镜像是对的，包头 CRC 是错的） */
    w25q128_erase_sector(OTA_SECTOR);
    w25q128_erase_sector(OTA_IMAGE_SECTOR);
    for (off = 0; off < sizeof(s_pkg_image); off += OTA_BLOCK) {
        n = sizeof(s_pkg_image) - off;
        if (n > OTA_BLOCK) n = OTA_BLOCK;
        w25q128_write(OTA_IMAGE_SECTOR + off, s_pkg_image + off, n);
    }
    head_pack(OTA_PKG_VER, 0x13579BDFu, (uint16_t)sizeof(s_pkg_image), OTA_ST_DOWN);
    w25q128_write(OTA_SECTOR, s_head, OTA_HEAD_LEN);
}

/* sys_ota_check 里用的状态合法性辅助（避免重复分支） */
static uint8_t head_st_is_valid(void)
{
    return (s_head[OTA_H_STATE] == OTA_ST_DOWN || s_head[OTA_H_STATE] == OTA_ST_APPLY) ? 1 : 0;
}
