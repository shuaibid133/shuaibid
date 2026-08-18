/**
 * @file    w25q128.c
 * @brief   W25Q128 SPI NOR Flash 驱动（SPI2 18MHz，Mode 0）
 *
 * 约束：
 *   - 页编程（0x02）一次最多 256 字节，且不能跨页边界 → write() 自动切页
 *   - 扇区擦除（0x20）粒度 4KB，擦除前必须先写使能（0x06）
 *   - 所有命令/数据都以 CS 低有效开始、高电平结束
 *   - 本驱动只被 ui_task 调用（单写者原则），无锁
 */
#include "w25q128.h"
#include "main.h"      /* W25Q_CS 引脚宏（CubeMX 生成） */
#include "spi.h"       /* hspi2 */

/* ---------- 命令码 ---------- */
#define W25Q_CMD_WRITE_ENABLE   0x06
#define W25Q_CMD_READ_STATUS    0x05
#define W25Q_CMD_READ_DATA      0x03
#define W25Q_CMD_PAGE_PROGRAM   0x02
#define W25Q_CMD_SECTOR_ERASE   0x20
#define W25Q_CMD_JEDEC_ID       0x9F

/* ---------- 芯片参数 ---------- */
#define W25Q_PAGE_SIZE     256
#define W25Q_SECTOR_SIZE   4096
#define W25Q_ID_W25Q128    0xEF4018

/* 状态寄存器 1 的 WIP 位（Busy 标志） */
#define W25Q_SR1_WIP      0x01

/* 读数据时 MOSI 输出的空数据（内容无关，Flash 只读） */
static uint8_t s_dummy[256];

static void cs_low(void)  { HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET); }
static void cs_high(void) { HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET); }

/* 单字节全双工交换 */
static uint8_t spi_byte(uint8_t tx)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, 100);
    return rx;
}

/* 批量接收（读时序要求 SCK 持续翻转，MOSI 输出 dummy） */
static void spi_rx(uint8_t *buf, uint32_t n)
{
    while (n > 0) {
        uint32_t k = (n > 256) ? 256 : n;
        HAL_SPI_TransmitReceive(&hspi2, s_dummy, buf, k, 1000);
        buf += k;
        n -= k;
    }
}

/* 等待内部忙完（WIP 清零），超时返回 1=失败 */
static uint8_t wait_busy(uint32_t ms_timeout)
{
    uint32_t t0 = HAL_GetTick();

    cs_low();
    spi_byte(W25Q_CMD_READ_STATUS);
    while (spi_byte(0xFF) & W25Q_SR1_WIP) {
        if (HAL_GetTick() - t0 > ms_timeout) {
            cs_high();
            return 1;
        }
    }
    cs_high();
    return 0;
}

/* 写使能：擦除/编程前必须执行（W25Q 每次编程后自动清 WEL） */
static void write_enable(void)
{
    cs_low();
    spi_byte(W25Q_CMD_WRITE_ENABLE);
    cs_high();
}

uint32_t w25q128_read_id(void)
{
    uint32_t id;

    cs_low();
    spi_byte(W25Q_CMD_JEDEC_ID);
    id  = (uint32_t)spi_byte(0xFF) << 16;
    id |= (uint32_t)spi_byte(0xFF) << 8;
    id |= (uint32_t)spi_byte(0xFF);
    cs_high();
    return id;
}

uint8_t w25q128_init(void)
{
    uint8_t i;

    for (i = 0; i < sizeof(s_dummy); i++) s_dummy[i] = 0xFF;
    return (w25q128_read_id() == W25Q_ID_W25Q128) ? 1 : 0;
}

/* 连续读：0x03 命令不限制页边界，可一次读完任意长度 */
void w25q128_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
    cs_low();
    spi_byte(W25Q_CMD_READ_DATA);
    spi_byte(addr >> 16);
    spi_byte(addr >> 8);
    spi_byte(addr);
    spi_rx(buf, n);
    cs_high();
}

/* 写：按 256B 页切分，每页独立 写使能→页编程→等忙 */
void w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t n)
{
    while (n > 0) {
        uint32_t page_left = W25Q_PAGE_SIZE - (addr & (W25Q_PAGE_SIZE - 1));
        uint32_t k = (n < page_left) ? n : page_left;   /* 不跨页边界 */
        uint32_t i;

        write_enable();
        cs_low();
        spi_byte(W25Q_CMD_PAGE_PROGRAM);
        spi_byte(addr >> 16);
        spi_byte(addr >> 8);
        spi_byte(addr);
        for (i = 0; i < k; i++) spi_byte(buf[i]);
        cs_high();
        wait_busy(50);
        addr += k;
        buf += k;
        n -= k;
    }
}

void w25q128_erase_sector(uint32_t addr)
{
    write_enable();
    cs_low();
    spi_byte(W25Q_CMD_SECTOR_ERASE);
    spi_byte(addr >> 16);
    spi_byte(addr >> 8);
    spi_byte(addr);
    cs_high();
    wait_busy(500);      /* 4K 擦除典型 40ms，留足余量 */
}
