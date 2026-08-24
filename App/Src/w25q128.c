/**
 * @file    w25q128.c
 * @brief   W25Q128 SPI NOR Flash 驱动（位带 GPIO，Mode 0）
 *
 * 约束：
 *   - 页编程（0x02）一次最多 256 字节，且不能跨页边界 → write() 自动切页
 *   - 扇区擦除（0x20）粒度 4KB，擦除前必须先写使能（0x06）
 *   - 所有命令/数据都以 CS 低有效开始、高电平结束
 *   - 本驱动只被 ui_task 调用（单写者原则），无锁
 *
 * 2026-08-18 改为位带（bit-bang）实现：HAL SPI2 阻塞传输导致全系统
 * 冻结（现象/复现/根因见开发日志），位带只操作 GPIOB 寄存器，
 * 不依赖 SPI2 外设时钟与 HAL 超时逻辑，运行稳定。
 * 注意：使用前会把 PB13/14/15 从 SPI2 复用改回普通 GPIO（CRH 直写）。
 */
#include "w25q128.h"
#include "main.h"      /* W25Q_CS 引脚宏（CubeMX 生成，仅借用端口定义） */

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

/* 引脚位带定义（GPIOB 寄存器直写，不经过 HAL）：
 * CS=PB12 SCK=PB13 MISO=PB14 MOSI=PB15
 * 寄存器偏移：CRL 0x00 CRH 0x04 IDR 0x08 ODR 0x0C BSRR 0x10
 * 2026-08-19 修正：IDR 曾错写成 0x40010C10（那是只写寄存器 BSRR，
 * 读恒 0）→ MISO 采样恒 0 → 读 ID 恒 0x00000000（开发日志有记录） */
#define BB_GPIOB_ODR      (*(volatile uint32_t *)0x40010C0Cu)
#define BB_GPIOB_IDR      (*(volatile uint32_t *)0x40010C08u)
#define BB_GPIOB_CRH      (*(volatile uint32_t *)0x40010C04u)
#define BB_CS_PIN   (1u << 12)
#define BB_SCK_PIN  (1u << 13)
#define BB_MISO_PIN (1u << 14)
#define BB_MOSI_PIN (1u << 15)

/* 位延时：主频 72MHz 下 8 周期 ≈ 0.11us（建立/低相位，保持短）。
 * 2026-08-19 曾用 60 NOP（~250kHz）：当时引脚还是 2MHz 弱驱动，SCK
 * 边沿畸变、采样点落临界区导致读 ID 前两字节错乱；后已改 50MHz 强驱动。
 * 2026-08-23 实测三个采样余量点（克隆片 JEDEC 00522118，tV 远大于原厂）：
 *   60 NOP（0.83us）→ 状态读取正确（保存成功、内容正确）
 *    8 NOP（0.11us）→ 状态读取错（WIP 假 1，擦除等待超时，保存 20s 且白纸）
 *   30 NOP（0.42us）→ 仍然错（说明克隆片 tV 在 0.42~0.83us 之间）
 * 修复：采样余量固定回 60 NOP（已验证可靠点），建立/低相位仍用 8 NOP
 * 保持短 —— SCK ~1MHz，读 105KB 约 0.5s（仍是 60 NOP 全速版的 4 倍） */
static void bb_delay(void)
{
    uint8_t i;
    for (i = 0; i < 8; i++) { __NOP(); }
}

/* SCK 上升沿后：MISO 采样余量（约 0.83us，覆盖克隆片大 tV） */
static void bb_delay_sample(void)
{
    uint8_t i;
    for (i = 0; i < 60; i++) { __NOP(); }
}

/* 把 PB13/14/15 从 SPI2 复用改为 GPIO（CRH 直接写）：
 * 13/15 = 推挽输出 50MHz（CNF=00 MODE=11，强驱动快边沿），
 * 14 = 上拉输入（CNF=10 MODE=00） */
static void bb_pin_setup(void)
{
    BB_GPIOB_CRH = (BB_GPIOB_CRH & ~(0xFFFFu << 20))   /* 清 PB13-15 四位的配置 */
                 | (0x3u << 20)   /* PB13 输出 50MHz */
                 | (0x8u << 24)   /* PB14 输入（CNF=10 上拉/下拉，MODE=00） */
                 | (0x3u << 28);  /* PB15 输出 50MHz */
    /* 输入上拉（CNF=10 时 ODR 决定上/下拉）：MISO 空闲高 */
    BB_GPIOB_ODR |= BB_MISO_PIN;
}

static void bb_cs_low(void)  { BB_GPIOB_ODR &= ~BB_CS_PIN; }
static void bb_cs_high(void) { BB_GPIOB_ODR |= BB_CS_PIN; }

/* 单字节全双工交换（MSB 先出，CPOL=0/CPHA=1：SCK 上升沿采样） */
static uint8_t bb_byte(uint8_t tx)
{
    uint8_t rx = 0, i;

    for (i = 0; i < 8; i++) {
        if (tx & 0x80u) BB_GPIOB_ODR |= BB_MOSI_PIN;
        else            BB_GPIOB_ODR &= ~BB_MOSI_PIN;
        tx <<= 1;
        bb_delay();
        BB_GPIOB_ODR &= ~BB_SCK_PIN;   /* 低半周期 */
        bb_delay();
        BB_GPIOB_ODR |= BB_SCK_PIN;    /* 上升沿：Flash 在此时锁存 MOSI */
        bb_delay_sample();             /* 高相位 + MISO 采样余量 */
        rx = (uint8_t)((rx << 1) | ((BB_GPIOB_IDR & BB_MISO_PIN) ? 1u : 0u));
    }
    return rx;
}

/* 批量接收（读时序要求 SCK 持续翻转，MOSI 输出 dummy） */
static void spi_rx(uint8_t *buf, uint32_t n)
{
    while (n > 0) {
        *buf++ = bb_byte(0xFF);
        n--;
    }
}

/* 等待内部忙完（WIP 清零），超时返回 1=失败
 * （位带版超时用自减计数器，不依赖 HAL_GetTick） */
static uint8_t wait_busy(uint32_t loops)
{
    uint32_t t = 0;

    bb_cs_low();
    bb_byte(W25Q_CMD_READ_STATUS);
    while (bb_byte(0xFF) & W25Q_SR1_WIP) {
        if (++t > loops) {
            bb_cs_high();
            return 1;
        }
    }
    bb_cs_high();
    return 0;
}

/* 写使能：擦除/编程前必须执行（W25Q 每次编程后自动清 WEL） */
static void write_enable(void)
{
    bb_cs_low();
    bb_byte(W25Q_CMD_WRITE_ENABLE);
    bb_cs_high();
}

uint32_t w25q128_read_id(void)
{
    uint32_t id;

    bb_cs_low();
    bb_byte(W25Q_CMD_JEDEC_ID);
    id  = (uint32_t)bb_byte(0xFF) << 16;
    id |= (uint32_t)bb_byte(0xFF) << 8;
    id |= (uint32_t)bb_byte(0xFF);
    bb_cs_high();
    return id;
}

/* 克隆芯片兼容：本板实测 0x9F 响应 00522118（厂商/类型字节非原厂 EF/40，
 * 容量字节 0x18 与 W25Q128 一致，读写时序正常）——克隆片 JEDEC ID 不保证
 * 等于原厂值，只校验容量字节（0x18 = 128Mbit）。0xFFFFFF = MISO 恒高
 * （CS 未生效/通路断），0x000000 = 采样恒 0（引脚配置错），都过不了检查 */
static uint8_t id_plausible(uint32_t id)
{
    return ((id >> 16) & 0xFFu) != 0xFFu
        && ((id >> 16) & 0xFFu) != 0x00u
        && (id & 0xFFu) == (W25Q_ID_W25Q128 & 0xFFu);
}

uint8_t w25q128_init(void)
{
    bb_pin_setup();                          /* PB13-15: SPI2 复用 → 普通 GPIO */
    bb_cs_high();                            /* CS 空闲高 */
    return id_plausible(w25q128_read_id()) ? 1 : 0;
}

/* 连续读：0x03 命令不限制页边界，可一次读完任意长度 */
void w25q128_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
    bb_cs_low();
    bb_byte(W25Q_CMD_READ_DATA);
    bb_byte(addr >> 16);
    bb_byte(addr >> 8);
    bb_byte(addr);
    spi_rx(buf, n);
    bb_cs_high();
}

/* 写：按 256B 页切分，每页独立 写使能→页编程→等忙 */
void w25q128_write(uint32_t addr, const uint8_t *buf, uint32_t n)
{
    while (n > 0) {
        uint32_t page_left = W25Q_PAGE_SIZE - (addr & (W25Q_PAGE_SIZE - 1));
        uint32_t k = (n < page_left) ? n : page_left;   /* 不跨页边界 */
        uint32_t i;

        write_enable();
        bb_cs_low();
        bb_byte(W25Q_CMD_PAGE_PROGRAM);
        bb_byte(addr >> 16);
        bb_byte(addr >> 8);
        bb_byte(addr);
        for (i = 0; i < k; i++) bb_byte(buf[i]);
        bb_cs_high();
        wait_busy(60000);                    /* 页编程典型 3ms；上限 60k 轮 ≈ 0.5s 保险
                                              * （克隆片慢 + 防状态读取抖动） */
        addr += k;
        buf += k;
        n -= k;
    }
}

void w25q128_erase_sector(uint32_t addr)
{
    write_enable();
    bb_cs_low();
    bb_byte(W25Q_CMD_SECTOR_ERASE);
    bb_byte(addr >> 16);
    bb_byte(addr >> 8);
    bb_byte(addr);
    bb_cs_high();
    wait_busy(800000);                       /* 4K 擦除典型 40ms，最坏 400ms；上限 800k 轮
                                              * ≈ 6.8s 保险（克隆片擦除慢，防误判完成） */
}

/* 全片擦除（0xC7）：重建卷（f_mkfs）后调用，把整个数据区一次性清成
 * 0xFF。2026-08-24 性能修复：f_mkfs 只写引导/FAT/根目录，不擦数据区
 * —— 数据区残留历次格式化前的旧文件数据，之后每次保存落到这些块上
 * 都要逐个 4K 擦除（35S/张），免擦优化完全失效。全片擦除一条命令 +
 * 一次等忙（典型 40-60s），只付一次成本。注意：擦除后卷头也没了，
 * 必须重新 f_mkfs；本函数只擦不写 */
void w25q128_chip_erase(void)
{
    write_enable();
    bb_cs_low();
    bb_byte(0xC7);
    bb_cs_high();
    wait_busy(6000000);                      /* 全片擦除典型 40-60s；上限 600 万轮
                                              * ≈ 60s（克隆片更慢，防误判完成） */
}
