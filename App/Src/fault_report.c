/**
 * @file    fault_report.c
 * @brief   硬件故障现场报告器：HardFault 入口调用，把现场数据画到 LCD 上
 *
 * 用法：stm32f1xx_it.c 的 HardFault_Handler 里（USER CODE 块）调用
 *       fault_report((uint32_t *)__get_MSP());
 *
 * 屏幕布局（白底）：
 *   红色标题栏 HARD FAULT
 *   PC  / LR  —— 崩溃指令与返回地址（可从反汇编/映射文件查函数名）
 *   HFSR      —— 硬故障状态：bit30 FORCED（下级故障升级）最常见
 *   CFSR      —— 可配置故障状态：bit24..26 BFSR（总线故障）
 *   BFAR      —— 总线故障地址：0x4000xxxx = 外设寄存器 → 外设时钟未开！
 *   MMAR      —— 存储器管理故障地址
 *
 * 说明：本函数只做 FSMC 寄存器级绘制（不依赖 HAL_GetTick/FreeRTOS），
 *       在故障上下文里可以安全执行；画完进入死循环（LED 冻结即故障态）。
 */
#include "fault_report.h"
#include "main.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"

/* Cortex-M3 系统控制块寄存器（CMSIS 宏也可，这里直接取址防头文件差异） */
#define SCB_CFSR    (*(volatile uint32_t *)0xE000ED28u)
#define SCB_HFSR    (*(volatile uint32_t *)0xE000ED2Cu)
#define SCB_MMAR    (*(volatile uint32_t *)0xE000ED34u)
#define SCB_BFAR    (*(volatile uint32_t *)0xE000ED38u)

/* 8 位十六进制显示（FONT_16，宽 8 字符） */
static void show_hex(uint16_t x, uint16_t y, uint32_t v, uint16_t color)
{
    char buf[9];
    char *p = buf + 8;
    uint8_t i;

    *p = 0;
    for (i = 0; i < 8; i++) {
        uint8_t d = v & 0xFu;
        *--p = (char)((d < 10) ? ('0' + d) : ('A' + d - 10));
        v >>= 4;
    }
    atk_md0280_show_string(x, y, 80, 16, p, ATK_MD0280_LCD_FONT_16, color);
}

/* 一行：标签 + 8 位十六进制值 */
static void line(uint16_t y, const char *label, uint32_t v)
{
    atk_md0280_show_string(8, y, 60, 16, (char *)label, ATK_MD0280_LCD_FONT_16,
                           ATK_MD0280_BLACK);
    show_hex(76, y, v, ATK_MD0280_BLUE);
}

void fault_report(uint32_t *msp)
{
    uint32_t pc = msp[6];
    uint32_t lr = msp[5];

    atk_md0280_fill(0, 0, ATK_MD0280_LCD_WIDTH - 1, ATK_MD0280_LCD_HEIGHT - 1,
                    ATK_MD0280_WHITE);
    atk_md0280_fill(0, 0, ATK_MD0280_LCD_WIDTH - 1, 24, ATK_MD0280_RED);
    atk_md0280_show_string(8, 5, 150, 16, (char *)"HARD FAULT",
                           ATK_MD0280_LCD_FONT_16, ATK_MD0280_WHITE);
    line(40,  "PC  ", pc);
    line(70,  "LR  ", lr);
    line(100, "HFSR", SCB_HFSR);
    line(130, "CFSR", SCB_CFSR);
    line(160, "BFAR", SCB_BFAR);
    line(190, "MMAR", SCB_MMAR);
    /* 提示行：BFAR=0x4000xxxx 是外设总线故障（时钟未开/地址错），查外设基址 */
    atk_md0280_show_string(8, 230, 230, 12, (char *)"BFAR 4000xxxx: peri clock",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    for (;;) { }   /* 停在现场，LED 冻结 = 故障态 */
}
