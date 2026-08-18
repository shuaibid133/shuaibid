/**
 * @file    fault_report.h
 * @brief   硬件故障现场报告器（HardFault 时把 PC/LR/故障地址显示到 LCD）
 */
#ifndef __FAULT_REPORT_H
#define __FAULT_REPORT_H

#include <stdint.h>

/* msp = 异常入栈帧指针（Handler 模式下 __get_MSP()），内部格式：
 * [0..3]=R0-R3 [4]=R12 [5]=LR [6]=PC [7]=xPSR */
void fault_report(uint32_t *msp);

#endif
