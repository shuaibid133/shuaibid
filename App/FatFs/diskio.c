/**
 * @file    diskio.c
 * @brief   FATFS 底层磁盘接口：W25Q128（16MB，512B 扇区，4KB 擦除块）
 *
 * 写路径设计（读-改-擦-写 + 4K 缓存）：
 *   W25Q 擦除粒度是 4KB，而 FATFS 只按 512B 扇区写。若每写一个扇区就
 *   擦一次 4K，FATFS 写目录/FAT 表时会被擦除操作拖垮（每次 ~100ms）。
 *   所以维护一块 4KB 写缓存：扇区先落进缓存，换块/同步时才擦+写整块，
 *   目录和 FAT 表的连续小写命中缓存，擦除次数从"每扇区 1 次"降到"每 4K 1 次"。
 *
 * 线程模型：FATFS 全部操作只在 ui_task 里执行（FF_FS_REENTRANT=0，
 *           不需要互斥锁），本文件无锁。
 */
#include "ff.h"
#include "diskio.h"
#include "w25q128.h"
#include <string.h>

#define FLASH_BYTES     (16 * 1024 * 1024)   /* W25Q128 = 16MB */
#define SECTOR_SIZE     512
#define BLOCK_SECTORS   8                    /* 4KB 擦除块 = 8 个扇区 */

static uint8_t g_inited = 0;                 /* disk_initialize 结果缓存 */

/* 4K 写缓存 */
static uint8_t  s_cache[4096];
static uint32_t s_cache_block = 0xFFFFFFFF;  /* 当前缓存的是哪个 4K 块（0xFFFFFFFF=空） */
static uint8_t  s_cache_dirty = 0;           /* 缓存有新数据未落盘 */
static uint8_t  s_cache_clean = 0;           /* 缓存内容全 0xFF：可直接页编程，无需擦除 */

/* 把指定 4K 块读入缓存，并统计是否全 0xFF（干净块）。
 * 2026-08-24 免擦优化：本板 W25Q128 是克隆片，4K 擦除实测 1.5~2s（原厂
 * 规格 40-400ms），擦除次数直接决定保存耗时。页编程只能 1→0，若块已
 * 全 0xFF（出厂状态/已擦过/写失败的残块），跳过擦除直接写，新建文件的
 * 数据区几乎全是干净块 → 保存从 40s 级降到 4s 级 */
static void cache_load(uint32_t blk)
{
    uint32_t i;
    uint8_t clean = 1;

    w25q128_read(blk * 4096, s_cache, 4096);
    for (i = 0; i < 4096; i++) {
        if (s_cache[i] != 0xFFu) { clean = 0; break; }
    }
    s_cache_clean = clean;
    s_cache_block = blk;
}

static void cache_flush(void)
{
    if (!s_cache_dirty) return;
    /* 卷头块（0-3：引导+2×FAT+根目录）必须永远擦除：免擦优化只用于
     * 数据块。FAT12 卷布局：扇区 0=引导，1-12=FAT1，13-24=FAT2，
     * 25=根目录 → 4K 块 0-2=FAT，块 3=根目录+数据区开头。
     * 实际上块 1-3 也不会被判"干净"（FAT 空闲项是 0x0000、根目录项
     * 非 0xFF），不会触发免擦，但显式写块 0 条件更稳——文件系统骨架
     * 绝不允许被"跳过擦除"赌运气 */
    if (!s_cache_clean || s_cache_block < 4)
        w25q128_erase_sector(s_cache_block * 4096);
    w25q128_write(s_cache_block * 4096, s_cache, 4096);
    s_cache_clean = 0;              /* 写过后块里已有数据（保守：下次需擦） */
    s_cache_dirty = 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (!g_inited) g_inited = w25q128_init();   /* 只初始化一次 */
    return g_inited ? 0 : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    (void)pdrv;
    return g_inited ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !g_inited) return RES_NOTRDY;
    w25q128_read((uint32_t)sector * SECTOR_SIZE, buff, (uint32_t)count * SECTOR_SIZE);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    UINT i;

    if (pdrv != 0 || !g_inited) return RES_NOTRDY;
    for (i = 0; i < count; i++) {
        LBA_t s = sector + i;
        uint32_t blk = s / BLOCK_SECTORS;

        if (blk != s_cache_block) {
            cache_flush();                    /* 换块：旧块先落盘 */
            cache_load(blk);                  /* 新块读入缓存（读-改-擦-写，免擦优化） */
        }
        memcpy(s_cache + (s % BLOCK_SECTORS) * SECTOR_SIZE,
               buff + i * SECTOR_SIZE, SECTOR_SIZE);
        s_cache_dirty = 1;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC:            /* 强制落盘：FATFS 关键操作后调用 */
        cache_flush();
        return RES_OK;
    case GET_SECTOR_COUNT:     /* f_mkfs 需要 */
        *(LBA_t *)buff = FLASH_BYTES / SECTOR_SIZE;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:       /* 擦除块大小（扇区数）：f_mkfs 对齐用 */
        *(DWORD *)buff = BLOCK_SECTORS;
        return RES_OK;
    case CTRL_TRIM:            /* 可忽略：直接报成功 */
        return RES_OK;
    }
    return RES_PARERR;
}

/* 全片擦除（重建卷）后调用：4K 缓存里的旧块内容已作废，必须置空
 * 强制下次 disk_write 重新 cache_load。否则缓存脏块会被当作"当前
 * 块"继续累加，flush 时把旧数据写回刚擦过的块 → 卷头损坏 */
void diskio_cache_invalidate(void)
{
    s_cache_block = 0xFFFFFFFF;
    s_cache_dirty = 0;
    s_cache_clean = 0;
}
