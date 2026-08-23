/**
 * @file    app.h
 * @brief   应用注册表接口：每个应用实现统一接口（open/handle），
 *          桌面框架通过 g_apps 表管理全部应用
 *
 * 概念：注册表 = 一张"应用信息表"，每行是一个应用的名片。
 *       框架不认识具体应用，只查表：g_apps[idx].open() 让应用自己出现、
 *       g_apps[idx].handle(ev) 把事件交给应用自己处理——同一句代码对
 *       不同应用做不同的事（C 语言用结构体+函数指针模拟多态）。
 *       新增应用：写一个 app_xxx.c 实现 open/handle，注册表加一行，
 *       框架代码零改动。
 */
#ifndef __APP_H
#define __APP_H

#include <stdint.h>
#include "event.h"

/* 应用统一接口 */
typedef struct {
    const char *name;                   /* 图标名（ASCII；atk 库只支持 ASCII） */
    uint16_t    color;                  /* 图标色块颜色（RGB565） */
    void (*open)(void);                 /* 进入应用：全屏自绘（进入时光标已隐藏） */
    void (*handle)(input_event_t *ev);  /* 应用内事件分发（框架只转发不解析） */
    void (*close)(void);                /* 退出钩子：EV_BACK 返回桌面时框架调用
                                         * （保存脏数据/停硬件；可为 NULL） */
} app_t;

extern const app_t g_apps[];            /* 应用注册表 */
extern const uint8_t g_app_count;

/* 应用公共标题栏：蓝条 + 应用名 + "K1:Back" 提示（所有应用统一风格） */
void app_draw_title(const char *name);

#endif
