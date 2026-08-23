#ifndef __APP_MUSIC_H
#define __APP_MUSIC_H

#include "event.h"

void app_music_open(void);
void app_music_handle(input_event_t *ev);
void app_music_close(void);
void app_music_pin_idle(void);   /* PA1 推挽输出高（空闲静音），ui_task 启动时调用 */

#endif
