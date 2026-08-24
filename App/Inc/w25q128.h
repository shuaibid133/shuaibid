/**
 * @file    w25q128.h
 * @brief   W25Q128 SPI NOR Flash 驱动接口（SPI2，CS=PB12 软件控制）
 */
#ifndef __W25Q128_H
#define __W25Q128_H

#include <stdint.h>

uint8_t  w25q128_init(void);                       /* 读 JEDEC ID 校验芯片，1=正常 0=失败 */
uint32_t w25q128_read_id(void);                    /* JEDEC ID（W25Q128 = 0xEF4018） */
void     w25q128_read(uint32_t addr, uint8_t *buf, uint32_t n);               /* 连续读，无页限制 */
void     w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t n);        /* 自动切 256B 页写 */
void     w25q128_erase_sector(uint32_t addr);      /* 4KB 扇区擦除（addr 需 4K 对齐） */
void     w25q128_chip_erase(void);                 /* 全片擦除（重建卷前用，擦完须重新 f_mkfs） */

#endif
