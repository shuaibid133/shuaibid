/**
 * @file    sys_ota.h
 * @brief   伪 OTA 系统接口（进阶⑥）：W25Q 更新区 + 版本状态机 + 启动校验回退
 *
 * 更新区布局（0xFFC000，8KB = 2 扇区，夹在 FAT 卷尾和配置区之间）：
 *   扇区0 = 包头 [magic "KZOTA!" 6B][版本 1B][CRC32 4B][包长 2B][状态 1B]
 *   扇区1 = 镜像数据（≤4KB，演示"固件"）
 *
 * 状态机：空闲 →下载→ 0x5A 待应用 →应用→ 0xA5 已应用 →重启生效
 *         任一步校验失败 → 回退（保持旧版 + ERR 日志）
 *
 * 断电安全：包头最后写——下载中断时包头为擦除态 0xFF = "无更新"，
 *           半截包永远不会被当作新版本（与③存储一致性同一工程思想）
 *
 * 版本语义：生效后的版本写入配置扇区（sys_config_t.version），
 *           与更新包解耦——已升级的设备不会因更新区被擦而"降级"，
 *           只有恢复出厂（FMT 全盘擦）才回到 V1.0
 */
#ifndef __SYS_OTA_H
#define __SYS_OTA_H

#include <stdint.h>

uint8_t sys_ota_check(void);            /* 检查更新区包：0=无 1=完好 2=损坏(已丢弃+日志) */
void    sys_ota_download_erase(void);   /* 擦包头扇区（~2s 阻塞，克隆片） */
void    sys_ota_download_erase_img(void); /* 擦镜像扇区（~2s 阻塞） */
uint8_t sys_ota_download_step(void);    /* 写下一块镜像（256B 页）；返回 1=还有块 */
uint8_t sys_ota_download_pct(void);     /* 下载进度 0-100 */
uint8_t sys_ota_download_finish(void);  /* 读回 CRC 校验 + 写包头 0x5A；返回 1=成功 */
uint8_t sys_ota_apply(void);            /* 0x5A→0xA5（擦包头重写，~2s）；1=成功 */
uint8_t sys_ota_current_version(void);  /* 当前运行版本（1 = V1.0） */
void    sys_ota_boot_check(void);       /* 启动校验：版本生效/补应用/回退（ui_task 调） */
void    sys_ota_inject_bad(void);       /* 演示工具：写入 CRC 故意错误的坏包 */

#endif
