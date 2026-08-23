/**
 * @file    sys_cfg.h
 * @brief   系统配置持久化接口：W25Q128 尾部专用扇区
 */
#ifndef __SYS_CFG_H
#define __SYS_CFG_H

/* 启动时调用：读 Flash 覆盖 g_sys_cfg（未保存过/校验失败 → 保持默认值） */
void sys_cfg_load(void);

/* 设置修改后调用：g_sys_cfg 写入 Flash（擦 4KB + 写，约 300ms） */
void sys_cfg_save(void);

#endif
