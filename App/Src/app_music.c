/**
 * @file    app_music.c
 * @brief   Music 应用：无源蜂鸣器播放内置曲目（真实旋律）
 *
 * 硬件：无源蜂鸣器模块 VCC=3V3 / GND=GND / IO=PA1（TIM2_CH2 默认映射，
 *       PA1 无板载外设占用，PA0 是 WKUP 按键不冲突）。
 *       实测确认本模块为**低电平触发**（IO 接 GND 才响）→ 处理：
 *         - 音量 = 低电平占比：CCR 反算为 (1 - vol%) × ARR（音量越大低电平越长）
 *         - 空闲（未 Start/休止/退出）引脚拉高：CCxE=0 时 AF 引脚电平由
 *           ODR 决定，ODR=1 → 不响（否则进 Music 就响、退出也响）
 *       TIM2 计数 1MHz（72MHz/72），ARR = 1MHz/音符频率-1 → PWM 输出音符频率。
 *
 * 曲目：6 首内置曲谱，来源为开源 Arduino 蜂鸣器乐谱（见每首歌注释）。
 *       存储格式 {音高索引, 时长ms} 平铺数组——索引查频率表省 Flash，
 *       时长直接存毫秒（忠实原谱节奏，如 Mario 的 83ms 八分音符）。
 *
 * 播放：常驻 FreeRTOS 任务（open 时创建一次），逐音符：
 *       设频率/占空比 → Start → 分片延时（50ms 片轮询暂停标志，响应快）
 *       → Stop → 30% 间隙（同音分离，Arduino 原版 1.3× 惯例）→ 下一音。
 *
 * 状态同步：s_song/s_playing/s_paused 由 ui_task 事件线程写、任务读，
 *       均为单字节原子访问（单核共享内存，与 joystick/亮度同模式）。
 *
 * 退出：框架 EV_BACK 接管 K1 → 注册表 close() 钩子被框架调用 →
 *       app_music_close 停止播放 + 停 PWM（否则退出后蜂鸣器一直响）。
 *
 * 冲突：Music 前台时其他应用不可达（单前台），W25Q 不被访问；
 *       PA1 不与 FSMC/SPI/摇杆共享，无引脚冲突。
 */
#include "app_music.h"
#include "app.h"
#include "cursor.h"
#include "app_config.h"
#include "main.h"
#include "stm32f1xx_hal_tim.h"
#include "./BSP/ATK_MD0280/atk_md0280.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define SCR_W   ATK_MD0280_LCD_WIDTH
#define SCR_H   ATK_MD0280_LCD_HEIGHT

#define SONG_Y0    52                /* 曲目列表区（6 行 × 30）52..232 */
#define SONG_ROW_H 30
#define SONG_N     6
#define STAT_Y0    234               /* 播放状态区 234..292 */
#define BTN_Y0     294               /* 底部按钮区 294..316：Prev/Next/Mode */
#define BTN_H      22
#define BTN_W0     66                /* Prev/Next 按钮宽 */
#define BTN_W2     92                /* MODE 按钮宽（模式名 8 字符 64px） */
#define BTN_GAP    5
#define GAP_RATIO  3                 /* 音符间隙 = 时长×3/10（1.3× 惯例） */

/* ---------- 音符频率表：半音索引 → 频率（0=休止，C4 起 4 个八度） ---------- */
static const uint16_t s_freq[47] = {
    0, 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,      /* C4..B4 */
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,         /* C5..B5 */
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976,  /* C6..B6 */
    2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520           /* C7..A7 */
};

/* ---------- 曲谱：{音高索引, 时长ms} 平铺 ----------
 * 音高索引：0=C4 2=D4 4=E4 5=F4 7=G4 8=G#4 9=A4 11=B4 12=C5 13=C#5
 *           14=D5 15=D#5 16=E5 17=F5 19=G5 20=G#5 21=A5 28=E6 31=G6
 *           33=A6 34=A#6 35=B6 36=C7 38=D7 40=E7 41=F7 43=G7 45=A7
 *
 * 曲 1：Mario Theme（超级玛丽主旋律）——Dipro Pratyaksa 2013 开源版
 *       （GitHub ramer/Mario 等大量派生谱的源头），80 音
 *       tempo 12→83ms（八分），9→111ms（附点八分） */
static const uint16_t s_mario[] = {
    40,83, 40,83, 0,83, 40,83, 0,83, 36,83, 40,83, 0,83,
    43,83, 0,83, 0,83, 0,83, 31,83, 0,83, 0,83, 0,83,
    36,83, 0,83, 0,83, 31,83, 0,83, 0,83, 28,83, 0,83,
    0,83, 33,83, 0,83, 35,83, 0,83, 34,83, 33,83, 0,83,
    31,111, 40,111, 43,111, 45,83, 0,83, 41,83, 43,83, 0,83,
    40,83, 0,83, 36,83, 0,83, 38,83, 35,83, 0,83, 0,83,
    36,111, 0,111, 0,111, 31,83, 0,83, 0,83, 28,83, 0,83,
    0,83, 33,83, 0,83, 35,83, 0,83, 34,83, 33,83, 0,83,
    31,111, 40,111, 43,111, 45,83, 0,83, 41,83, 43,83, 0,83,
    40,83, 0,83, 36,83, 0,83, 38,83, 35,83, 0,83, 0,83
};

/* 曲 2：Imperial March（帝国进行曲主旋律段）——robsoncouto/arduino-songs
 *       开源集合谱（tempo=120，16=125ms 十六分 / 8=250ms / 4=500ms / 2=1s，
 *       负值=附点×1.5），44 音 */
static const uint16_t s_imperial[] = {
    9,750, 9,750, 9,125, 9,125, 9,125, 9,125, 5,250, 0,250,
    9,500, 9,500, 9,500, 5,375, 12,125,
    9,500, 5,375, 12,125, 9,1000,
    16,500, 16,500, 16,500, 17,375, 12,125,
    21,500, 9,375, 9,125, 21,500, 20,375, 19,125,
    15,125, 14,125, 15,250, 0,250, 9,250, 15,500, 14,375, 13,125,
    12,125, 11,125, 12,125, 0,250, 5,250, 8,500, 5,375, 9,187,
    12,500, 9,375, 12,125, 16,1000
};

/* 曲 3：Happy Birthday（生日快乐）——经典 C 调版，25 音
 *       tempo 8=125ms（八分）/ 4=250ms（四分） */
static const uint16_t s_birthday[] = {
    7,125, 7,125, 9,125, 7,125, 12,250, 11,250,
    7,125, 7,125, 9,125, 7,125, 14,250, 12,250,
    7,125, 7,125, 19,125, 16,125, 12,125, 11,125, 9,250,
    17,125, 17,125, 16,125, 12,125, 14,250, 12,250
};

/* 曲 4：Twinkle Little Star（小星星）——1 拍=250ms（tempo 120），42 音 */
static const uint16_t s_twinkle[] = {
    0,250, 0,250, 7,250, 7,250, 9,250, 9,250, 7,500,
    5,250, 5,250, 4,250, 4,250, 2,250, 2,250, 0,500,
    7,250, 7,250, 5,250, 5,250, 4,250, 4,250, 2,500,
    7,250, 7,250, 5,250, 5,250, 4,250, 4,250, 2,500,
    0,250, 0,250, 7,250, 7,250, 9,250, 9,250, 7,500,
    5,250, 5,250, 4,250, 4,250, 2,250, 2,250, 0,500
};

/* 曲 5：Ode to Joy（欢乐颂）——4/4 拍，八分=125ms（tempo 120），33 音
 *       "3."=附点四分 375ms，"-"=二分 500ms */
static const uint16_t s_ode[] = {
    4,125, 4,125, 5,125, 7,125, 7,125, 5,125, 4,125, 2,125,
    0,125, 0,125, 2,125, 4,125, 4,375, 2,125, 2,125, 0,500,
    4,125, 4,125, 5,125, 7,125, 7,125, 5,125, 4,125, 2,125,
    0,125, 0,125, 2,125, 4,125, 2,375, 0,125, 0,125, 0,500
};

/* 曲 6：Two Tigers（两只老虎）——八分=125ms，"-"=二分 500ms，32 音 */
static const uint16_t s_tigers[] = {
    0,125, 2,125, 4,125, 0,125, 0,125, 2,125, 4,125, 0,125,
    4,125, 5,125, 7,500, 4,125, 5,125, 7,500,
    7,125, 9,125, 7,125, 5,125, 4,125, 0,125, 7,125, 9,125, 7,125, 5,125, 4,125, 0,125,
    0,125, 7,125, 0,500, 0,125, 7,125, 0,500
};

typedef struct {
    const char *name;
    const uint16_t *notes;       /* {音高, 时长ms} 平铺 */
    uint16_t count;              /* 音符数 */
} song_t;

static const song_t s_songs[SONG_N] = {
    {"Mario Theme",         s_mario,    sizeof(s_mario) / 2},
    {"Imperial March",      s_imperial, sizeof(s_imperial) / 2},
    {"Happy Birthday",      s_birthday, sizeof(s_birthday) / 2},
    {"Twinkle Little Star", s_twinkle,  sizeof(s_twinkle) / 2},
    {"Ode to Joy",          s_ode,      sizeof(s_ode) / 2},
    {"Two Tigers",          s_tigers,   sizeof(s_tigers) / 2},
};

/* ---------- 播放状态（事件线程写 / 任务读，单字节原子） ---------- */
static const song_t *s_song = NULL;      /* NULL=停止 */
static volatile uint8_t  s_playing = 0;
static volatile uint8_t  s_paused = 0;
static volatile uint16_t s_cur_note = 0; /* 当前音符下标（进度） */
static volatile uint32_t s_elapsed_ms = 0;  /* 已播时长 */
static volatile uint8_t  s_ui_dirty = 0;    /* 任务侧状态变化，主线程刷 UI */

static uint8_t  s_row = 0;               /* 光标选中行 */
static int8_t   s_btn = -1;              /* 按钮 hover：0=Prev 1=Next 2=Mode -1=无 */
static uint8_t  s_mode = 0;              /* 播放模式：0=顺序循环 1=单曲循环 2=随机 */
static uint32_t s_rnd = 0;               /* 随机播放的 LCG 状态 */
static TaskHandle_t s_task = NULL;
static TIM_HandleTypeDef g_htim2;

/* ---------- 局部重绘保护（与桌面框架同协议） ---------- */
static uint8_t redraw_protect_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (cursor_overlap(x0, y0, x1, y1)) {
        cursor_hide();
        return 1;
    }
    return 0;
}

static void redraw_protect_end(uint8_t hidden)
{
    if (hidden) cursor_show();
}

/* mm:ss（4 字符），ms→秒进位 */
static void fmt_mmss(char *buf, uint32_t ms)
{
    uint32_t s = ms / 1000;

    buf[0] = (char)('0' + s / 60);
    buf[1] = ':';
    buf[2] = (char)('0' + (s % 60) / 10);
    buf[3] = (char)('0' + s % 10);
    buf[4] = 0;
}

/* 设音符：频率（ARR）+ 音量（CCR，低电平触发 → 反相占空比）。
 * 低电平时间占比 = 音量%：vol=100 → CCR=0 → 恒低（最响）；
 * vol=0 → CCR=ARR+1 → 恒高（无声） */
static void set_note(uint8_t idx)
{
    uint32_t arr = 1000000u / s_freq[idx] - 1;
    uint32_t ccr = (arr + 1) * (100u - (uint32_t)g_sys_cfg.volume) / 100;

    __HAL_TIM_SET_AUTORELOAD(&g_htim2, arr);
    __HAL_TIM_SET_COMPARE(&g_htim2, TIM_CHANNEL_2, ccr);
}

/* 蜂鸣器引脚空闲态：PA1 配回推挽输出并拉高（低电平触发模块 → 静音）。
 * 不用 AF 模式下的 ODR 空闲电平（行为有不确定性），播放前后显式切换，
 * 保证任何"未播放"时刻引脚电平确定是高 */
void app_music_pin_idle(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    gi.Pin = GPIO_PIN_1;
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gi);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);
}

/* TIM2_CH2 初始化（open 时调用一次；懒初始化，不播放不占资源） */
static void music_pwm_init(void)
{
    GPIO_InitTypeDef gi = {0};
    TIM_OC_InitTypeDef oc = {0};

    __HAL_RCC_TIM2_CLK_ENABLE();
    gi.Pin = GPIO_PIN_1;                 /* PA1：TIM2_CH2 默认映射 */
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gi);
    /* 低电平触发模块：空闲必须拉高，否则 CCxE=0 时 ODR=0 引脚为低 → 一直响 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_1, GPIO_PIN_SET);

    g_htim2.Instance = TIM2;
    g_htim2.Init.Prescaler = 72 - 1;     /* 1MHz 计数时钟 */
    g_htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    g_htim2.Init.Period = 0xFFFF;        /* ARR 由音符频率覆写 */
    g_htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    g_htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    HAL_TIM_PWM_Init(&g_htim2);

    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode = 0;                   /* 魔改 HAL 无 OCFASTMODE 宏，0=关 */
    HAL_TIM_PWM_ConfigChannel(&g_htim2, &oc, TIM_CHANNEL_2);
}

/* 画一行：hover=1 蓝底白字；右侧状态标记（[>>] 播放中 / [||] 暂停） */
static void draw_row(uint8_t row, uint8_t hover)
{
    uint16_t y = SONG_Y0 + row * SONG_ROW_H;
    char line[24];
    const char *mark = "";
    uint16_t mark_c = ATK_MD0280_BLUE;

    line[0] = (char)('1' + row);
    line[1] = '.';
    line[2] = ' ';
    strcpy(line + 3, s_songs[row].name);

    if (s_song == &s_songs[row]) {
        if (s_playing && !s_paused)      { mark = "[>>]"; mark_c = ATK_MD0280_RED; }
        else if (s_playing && s_paused)  { mark = "[||]"; mark_c = ATK_MD0280_GRAY; }
        else                             { mark = "[  ]"; }
    }

    atk_md0280_fill(0, y - 2, SCR_W - 1, y + SONG_ROW_H - 3,
                    hover ? ATK_MD0280_BLUE : ATK_MD0280_WHITE);
    atk_md0280_show_string(16, y + 7, 170, 16, line, ATK_MD0280_LCD_FONT_16,
                           hover ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
    if (mark[0] != 0)
        atk_md0280_show_string(192, y + 7, 40, 16, (char *)mark, ATK_MD0280_LCD_FONT_16,
                               hover ? ATK_MD0280_WHITE : mark_c);
}

/* 画播放状态区（整块重画）：曲名 / 时间+音量 / 进度条 */
static void draw_status(void)
{
    const song_t *sg = (s_song != NULL) ? s_song : &s_songs[s_row];
    char buf[24];
    uint32_t total_ms = 0, w;
    uint16_t i;
    uint8_t v = g_sys_cfg.volume;

    for (i = 0; i < sg->count; i++)              /* 总时长（播放曲，无播放用选中行） */
        total_ms += sg->notes[i * 2 + 1];

    atk_md0280_fill(0, STAT_Y0 - 2, SCR_W - 1, STAT_Y0 + 56, ATK_MD0280_WHITE);

    if (s_song != NULL) {
        strcpy(buf, "Now: ");
        strcat(buf, s_song->name);
        atk_md0280_show_string(16, STAT_Y0 + 4, 200, 16, buf, ATK_MD0280_LCD_FONT_16,
                               ATK_MD0280_BLUE);
    } else {
        atk_md0280_show_string(16, STAT_Y0 + 4, 200, 16, (char *)"Select a song",
                               ATK_MD0280_LCD_FONT_16, ATK_MD0280_GRAY);
    }

    /* 时间 + 音量："0:12/0:56  Vol 80%"（fmt_mmss 各占 4+1 字符） */
    fmt_mmss(buf, s_song != NULL ? s_elapsed_ms : 0);
    buf[4] = '/';
    fmt_mmss(buf + 5, total_ms);
    strcat(buf, "  Vol ");
    buf[15] = (char)('0' + v / 100);
    buf[16] = (char)('0' + (v / 10) % 10);
    buf[17] = (char)('0' + v % 10);
    buf[18] = '%';
    buf[19] = 0;
    atk_md0280_show_string(16, STAT_Y0 + 24, 200, 12, buf, ATK_MD0280_LCD_FONT_12,
                           ATK_MD0280_GRAY);

    /* 进度条：浅灰底 200px（驱动无浅灰宏，手写 0xDEFB），蓝色按进度填充 */
    atk_md0280_fill(16, STAT_Y0 + 42, 216, STAT_Y0 + 50, 0xDEFB);
    if (total_ms > 0) {
        w = 200u * s_elapsed_ms / total_ms;
        if (w > 200) w = 200;
        if (w > 0)
            atk_md0280_fill(16, STAT_Y0 + 42, (uint16_t)(16 + w), STAT_Y0 + 50, ATK_MD0280_BLUE);
    }
}

/* 播放状态变化后的整屏刷新：列表行（状态标记）+ 状态区 */
static void refresh_all(void)
{
    uint8_t i, hid;

    hid = redraw_protect_begin(0, SONG_Y0 - 2, SCR_W - 1, SONG_Y0 + SONG_N * SONG_ROW_H - 3);
    for (i = 0; i < SONG_N; i++) draw_row(i, i == s_row);
    redraw_protect_end(hid);
    draw_status();
}

/* 底部按钮区：Prev / Next / Mode（hover 蓝底白字，MODE 显示当前模式） */
static void draw_btns(void)
{
    static const char *const m[3] = { "MODE ALL", "MODE ONE", "MODE RND" };
    uint8_t i;

    for (i = 0; i < 3; i++) {
        uint16_t x0 = (i < 2) ? (uint16_t)(4 + i * (BTN_W0 + BTN_GAP))
                              : (uint16_t)(4 + 2 * (BTN_W0 + BTN_GAP));
        uint16_t w = (i < 2) ? BTN_W0 : BTN_W2;
        uint16_t x1 = (uint16_t)(x0 + w - 1);
        uint8_t  hov = (s_btn == i);
        const char *txt;

        if (i == 2) txt = m[s_mode];
        else        txt = (i == 0) ? "<< Prev" : "Next >>";

        atk_md0280_fill(x0, BTN_Y0, x1, BTN_Y0 + BTN_H - 1,
                        hov ? ATK_MD0280_BLUE : ATK_MD0280_WHITE);
        atk_md0280_show_string(x0 + 5, BTN_Y0 + 3, w - 10, 16, (char *)txt,
                               ATK_MD0280_LCD_FONT_16,
                               hov ? ATK_MD0280_WHITE : ATK_MD0280_BLACK);
    }
}

/* 按钮命中：0=Prev 1=Next 2=Mode，未命中 -1 */
static int8_t hit_btn(uint16_t cx, uint16_t cy)
{
    if (cy < BTN_Y0 || cy > BTN_Y0 + BTN_H - 1) return -1;
    if (cx >= 4 && cx <= 4 + BTN_W0 - 1) return 0;
    if (cx >= 4 + BTN_W0 + BTN_GAP && cx <= 4 + BTN_W0 + BTN_GAP + BTN_W0 - 1) return 1;
    if (cx >= 4 + 2 * (BTN_W0 + BTN_GAP) && cx <= 4 + 2 * (BTN_W0 + BTN_GAP) + BTN_W2 - 1) return 2;
    return -1;
}

/* 切歌：s_song 指向 s_row 并从第一音播起 */
static void start_song(void)
{
    s_song = &s_songs[s_row];
    s_playing = 1;
    s_paused = 0;
    s_cur_note = 0;
    s_elapsed_ms = 0;
}

/* 分片延时：每 50ms 检查暂停/停止标志，响应快（不阻塞退出/暂停） */
static void delay_check(uint32_t ms)
{
    while (ms > 0) {
        uint32_t chunk = ms > 50 ? 50 : ms;

        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
        if (s_paused || !s_playing) break;
        s_elapsed_ms += chunk;
    }
}

/* 播放任务：常驻，open 时创建；按谱逐音符播放，播完自动停 */
static void music_task(void *argument)
{
    (void)argument;
    for (;;) {
        const song_t *sg = s_song;
        uint16_t i;

        if (sg == NULL || !s_playing) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (s_paused)                 { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

        for (i = 0; i < sg->count; i++) {
            uint16_t n = sg->notes[i * 2];
            uint16_t d = sg->notes[i * 2 + 1];

            if (s_song != sg || !s_playing || s_paused) break;
            s_cur_note = i;
            if (n != 0) { set_note((uint8_t)n); HAL_TIM_PWM_Start(&g_htim2, TIM_CHANNEL_2); }
            delay_check(d);
            HAL_TIM_PWM_Stop(&g_htim2, TIM_CHANNEL_2);
            delay_check((uint32_t)d * GAP_RATIO / 10);   /* 间隙：同音分离 */
            if (s_song != sg || !s_playing) break;
        }

        if (s_song == sg && s_playing && !s_paused) {
            /* 整曲播完：按播放模式续播（s_playing 保持 1 直接接下一首） */
            if (s_mode == 0) {                    /* 顺序循环 */
                s_row = (uint8_t)((s_row + 1) % SONG_N);
                s_song = &s_songs[s_row];
            } else if (s_mode == 1) {             /* 单曲循环：本曲重播 */
                /* s_song 不变，清进度从头播 */
            } else {                              /* 随机：LCG 伪随机，避开当前曲 */
                if (s_rnd == 0) s_rnd = (uint32_t)xTaskGetTickCount();
                s_rnd = s_rnd * 1103515245u + 12345u;
                s_row = (uint8_t)(s_rnd % SONG_N);
                if (s_row == (uint8_t)(sg - s_songs))
                    s_row = (uint8_t)((s_row + 1) % SONG_N);
                s_song = &s_songs[s_row];
            }
            s_cur_note = 0;
            s_elapsed_ms = 0;
            HAL_TIM_PWM_Stop(&g_htim2, TIM_CHANNEL_2);
            s_ui_dirty = 1;
        }
    }
}

/* ---------- 应用接口 ---------- */

void app_music_open(void)
{
    uint8_t i;

    atk_md0280_fill(0, 0, SCR_W - 1, SCR_H - 1, ATK_MD0280_WHITE);
    app_draw_title("Music");
    atk_md0280_show_string(16, 28, 220, 12, (char *)"SW=Play/Pause  Btns below",
                           ATK_MD0280_LCD_FONT_12, ATK_MD0280_GRAY);

    s_row = 0;
    s_btn = -1;
    for (i = 0; i < SONG_N; i++) draw_row(i, i == s_row);
    draw_status();
    draw_btns();

    music_pwm_init();   /* 每次进入都重配 PA1 为 AF（close 会把它切回推挽输出，
                         * 若只在第一次初始化，第二次进入后 PWM 出不到引脚）
                         * 重复 Init 幂等：State != RESET 跳过 MspInit，仅重写寄存器 */
    if (s_task == NULL) {
        xTaskCreate(music_task, "music", 512, NULL, 1, &s_task);   /* 任务只创建一次 */
    }

    cursor_init(SCR_W / 2, SONG_Y0 + SONG_ROW_H / 2);
    cursor_show();
}

void app_music_handle(input_event_t *ev)
{
    if (ev->type == EV_MOUSE_MOVE) {
        uint16_t cx, cy;
        uint8_t row, hid;
        int8_t b;

        cursor_get_pos(&cx, &cy);
        if (cy >= SONG_Y0 && cy <= SONG_Y0 + SONG_N * SONG_ROW_H - 3) {
            row = (uint8_t)((cy - SONG_Y0) / SONG_ROW_H);
            if (row != s_row) {
                uint16_t y0 = SONG_Y0 + (row < s_row ? row : s_row) * SONG_ROW_H - 2;
                uint16_t y1 = SONG_Y0 + (row > s_row ? row : s_row) * SONG_ROW_H + SONG_ROW_H - 3;

                hid = redraw_protect_begin(0, y0, SCR_W - 1, y1);
                draw_row(s_row, 0);
                s_row = row;
                draw_row(row, 1);
                redraw_protect_end(hid);
            }
        }
        /* 按钮 hover 检测（列表区之外）：变化才重绘按钮区 */
        b = hit_btn(cx, cy);
        if (b != s_btn) {
            s_btn = b;
            draw_btns();
        }
    } else if (ev->type == EV_KEY_DOWN) {
        if (s_btn == 0) {
            /* 上一首：列表首尾循环，自动从头播 */
            s_row = (s_row == 0) ? (uint8_t)(SONG_N - 1) : (uint8_t)(s_row - 1);
            start_song();
            refresh_all();
        } else if (s_btn == 1) {
            /* 下一首：列表首尾循环，自动从头播 */
            s_row = (uint8_t)((s_row + 1) % SONG_N);
            start_song();
            refresh_all();
        } else if (s_btn == 2) {
            /* 播放模式切换：顺序循环 → 单曲循环 → 随机 */
            s_mode = (s_mode + 1) % 3;
            draw_btns();
        } else {
            /* SW 在列表：选中行 = 播放行 → 播放/暂停切换；否则切歌（从头播） */
            if (s_song == &s_songs[s_row] && s_playing) {
                s_paused = s_paused ? 0 : 1;   /* 任务 ≤50ms 内停/续音 */
            } else {
                start_song();
            }
            refresh_all();
        }
    } else if (ev->type == EV_TICK) {
        /* 每秒刷新：播完的收尾刷新 + 进度条推进 */
        if (s_ui_dirty) { s_ui_dirty = 0; refresh_all(); }
        else            { draw_status(); }
    }
}

/* 退出钩子：框架在 EV_BACK 回桌面时调用——必须停播放，
 * 否则蜂鸣器会一直响到断电 */
void app_music_close(void)
{
    s_playing = 0;
    s_paused = 0;
    s_song = NULL;
    s_cur_note = 0;
    s_elapsed_ms = 0;
    HAL_TIM_PWM_Stop(&g_htim2, TIM_CHANNEL_2);
    app_music_pin_idle();   /* 切回推挽输出高：100% 静音 */
}
