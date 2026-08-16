/**
 * @file    demo_show.c
 * @brief   v2.7 展示 + 稳定性验证：OLED 高速动画(FPS) + LED 呼吸灯 + GP15 PWM
 *
 * 三个展示要素（预设，开机自动运行，无需输入指令）：
 *   1. I2C OLED（SSD1306 128x64 @0x3C, I2C0 GP4/5, 400kHz）
 *      · 场景轮播（各 5s）：旋转线框立方体 → 双弹跳球 → 三重正弦示波器
 *      · 每帧右上角实时显示刷新率 FPS
 *      · "尽可能快"：一帧 8 页 × 132B，400kHz 总线时间 ~25ms，全速推送
 *   2. 板载 LED（GPIO25）呼吸灯：硬件 PWM（slice4 B, 1kHz 载波），
 *      占空比 0→100→0 缓慢扫动（三角波平方，周期 ~3s），非直亮直灭
 *   3. GP15 持续输出 PWM：硬件 PWM（slice7 B, 1kHz 载波），
 *      占空比与 LED 同步扫动 → 示波器上即呼吸包络的 PWM 波
 *
 * 稳定性验证（`show status`）：
 *   frames / i2c_err / reinits / fps(now/min/avg/max) / uptime
 *   —— 长跑这些数字即系统健康报告（调度、I2C、内存、tick 任何一处
 *      出问题，FPS 都会掉下来或任务死亡）。
 *
 * 指令控制：
 *   show status — 查看统计
 *   show stop   — 本次停止（不写 Flash）
 *   show start  — 本次启动（不写 Flash）
 *   show on     — 固化为开机自动运行（默认）
 *   show off    — 固化关闭：开机不再自动运行（= 删除该展示）
 *
 * 实现说明：
 *   · OLED 底层驱动复用 shell.c VT2 的成熟范式（v2.3 系列）：
 *     16 条 Adafruit 顺序初始化、页寻址 0xB0/0x00/0x10、132 列兼容
 *     SH1106、失败 reinit×3 重试。差异：不包 PRIMASK 临界区 ——
 *     show 任务不会被别的任务 suspend（无 ctrl 折腾），而关中断 3ms×8
 *     会让 1ms 内核 tick 每帧丢 ~20 个滴答；shell 端 i2c 命令并发最坏
 *     NACK → reinit 重试恢复（VT2 已验证该恢复路径）。
 *   · PWM 直接用 SDK hardware/pwm.h（exe 目标已链 pico_stdlib）。
 *     sysclk=125MHz 下 div=2 + wrap=62499 → 1kHz。若运行中超频，
 *     载波会按比例升高（仅影响观察值，无功能风险）。
 */
#include "task.h"
#include "sched.h"
#include "hal_interface.h"
#include "os_config.h"
#include "shell_core.h"
#include "hal/config_store.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* 【v2.7.1】超频切换进行中标志（定义于 hal_port.c，跨核可见）。
 * show 任务在 core1，超频切换只保护 core0 中断，此标志通知 show 暂停。 */
extern volatile uint32_t g_oc_switching;

#if OS_CFG_DEMO_APP && OS_CFG_PERIPH_SERVICE && OS_CFG_SHOW_DEMO

/* ================================================================
 * 配置常量
 * ================================================================ */
#define SHOW_I2C_BUS     0u
#define SHOW_OLED_ADDR   0x3Cu
#define SHOW_I2C_HZ      400000u     /* SSD1306 Fast-mode（VT2 验证过） */
#define SHOW_FB_COLS     128         /* 有效列 */
#define SHOW_TX_COLS     128         /* 发送列（本工程屏为 SSD1306 128x64）。
                                      * v2.7.1：原 132 会让 SSD1306 页寻址写满
                                      * 128 列后 column 指针回绕到 0，末 4 字节(0)
                                      * 每帧覆盖左上角列 0-3 → 左上角像素闪烁。
                                      * 若换 SH1106(132 列) 再改回 132。 */
#define SHOW_PAGES       8
#define SHOW_SCENE_MS    5000u       /* 场景轮播周期 */

/* PWM：GP25=切片4B（LED），GP15=切片7B（输出） */
#define SHOW_LED_PIN     25u
#define SHOW_PWM_PIN     15u
#define SHOW_PWM_WRAP    62499u      /* 125MHz/2 分频 → 1kHz */
#define SHOW_PWM_DIV     2.0f

/* ================================================================
 * 统计（show status 读取；跨字段一致性由"单写者=show 任务"保证）
 * ================================================================ */
static volatile uint32_t g_frames   = 0;   /* 已推帧数（含失败） */
static volatile uint32_t g_fails    = 0;   /* 整帧推送失败次数 */
static volatile uint32_t g_i2c_errs = 0;   /* 底层 NACK/超时次数 */
static volatile uint32_t g_reinits  = 0;   /* I2C 外设复位次数 */
static volatile uint32_t g_fps_now  = 0;
static volatile uint32_t g_fps_min  = 0xFFFFFFFFu;
static volatile uint32_t g_fps_max  = 0;
static volatile uint64_t g_fps_sum  = 0;   /* fps 样本和（求 avg） */
static volatile uint32_t g_fps_n    = 0;
static volatile uint32_t g_start_tick = 0;
static volatile uint8_t  g_show_state = 0; /* 0=idle 1=running 2=stopping */
static tcb_t *g_show_task = NULL;

/* ================================================================
 * 帧缓冲 + 发送缓冲（全部静态 BSS，零栈开销 —— VT2 v2.3.2 教训）
 * ================================================================ */
static uint8_t s_fb[SHOW_PAGES][SHOW_TX_COLS];    /* 渲染帧缓冲（前 128 列有效） */
static uint8_t s_cmd_buf[18];                     /* OLED 命令编码缓冲（8 条 × 2B） */
static uint8_t s_page_buf[1 + SHOW_TX_COLS];      /* [0]=0x40 + 一页数据 */

/* ================================================================
 * 定点 sin 表：31 项 Q14（0°..90° 步 3°）——无 FPU 无除法的平滑旋转
 * ================================================================ */
static const uint16_t s_sin_q14[31] = {
       0,   857,  1714,  2563,  3407,  4241,  5064,  5873,  6666,  7441,
    8192,  8924,  9632, 10314, 10967, 11590, 12181, 12739, 13262, 13748,
   14197, 14606, 14976, 15304, 15591, 15835, 16034, 16191, 16303, 16370,
   16384
};

/* sin(deg) → Q14。象限折叠后查表（3° 粒度，主循环步进 3° 时零插值误差） */
static int32_t sin_q14(uint32_t deg)
{
    int neg = 0;
    deg %= 360u;
    if (deg >= 180u) { neg = 1; deg -= 180u; }
    if (deg > 90u) deg = 180u - deg;      /* sin(180-x)=sin(x) */
    int32_t v = (int32_t)s_sin_q14[deg / 3u];
    return neg ? -v : v;
}
static int32_t cos_q14(uint32_t deg) { return sin_q14(deg + 90u); }

/* ================================================================
 * 图形原语（128x64，页式帧缓冲）
 * ================================================================ */
static void fb_clr(void) { memset(s_fb, 0, sizeof(s_fb)); }

static void fb_px(int x, int y)
{
    if ((uint32_t)x >= SHOW_FB_COLS || (uint32_t)y >= 64u) return;
    s_fb[y >> 3][x] |= (uint8_t)(1u << (y & 7));
}

static void fb_rect(int x0, int y0, int w, int h)
{
    for (int x = x0; x < x0 + w; x++) { fb_px(x, y0); fb_px(x, y0 + h - 1); }
    for (int y = y0; y < y0 + h; y++) { fb_px(x0, y); fb_px(x0 + w - 1, y); }
}

static void fb_line(int x0, int y0, int x1, int y1)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        fb_px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void fb_disc(int cx, int cy, int r)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) fb_px(cx + dx, cy + dy);
}

/* ---- 5x7 点阵字体（"0-9 F"）——列式 glyph，bit0=顶行，5 列 × 7 行 ----
 * v2.7.1：原 3x5 迷你字体在 128x64 屏上几乎看不清，改为 5x7 提升可读性。 */
static const uint8_t FONT_5X7_DIGIT[10][5] = {
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  /* 0 */
    {0x00, 0x02, 0x7F, 0x40, 0x00},  /* 1 */
    {0x60, 0x51, 0x49, 0x47, 0x40},  /* 2 */
    {0x08, 0x49, 0x49, 0x77, 0x08},  /* 3 */
    {0x18, 0x14, 0x12, 0x79, 0x70},  /* 4 */
    {0x47, 0x49, 0x49, 0x49, 0x79},  /* 5 */
    {0x3E, 0x49, 0x49, 0x49, 0x31},  /* 6 */
    {0x01, 0x01, 0x79, 0x07, 0x01},  /* 7 */
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  /* 8 */
    {0x06, 0x49, 0x49, 0x49, 0x7E},  /* 9 */
};
/* 'F'（单位标记） */
static const uint8_t FONT_5X7_F[5] = {0x7F, 0x09, 0x09, 0x01, 0x01};
#define FONT_W 5
#define FONT_H 7
#define FONT_PITCH 6          /* 5 宽 + 1 间距 */

/* 画数字串（十进制），返回字符数 */
static int fb_num(int x, int y, uint32_t v)
{
    char b[11]; int n = 0;
    if (v == 0) b[n++] = 0;
    while (v) { b[n++] = (char)(v % 10u); v /= 10u; }
    for (int i = n - 1; i >= 0; i--) {
        const uint8_t *g = FONT_5X7_DIGIT[(int)b[i]];
        for (int c = 0; c < FONT_W; c++)
            for (int r = 0; r < FONT_H; r++)
                if (g[c] & (1u << r)) fb_px(x + c, y + r);
        x += FONT_PITCH;
    }
    return n;
}

/* 右上角 FPS 牌：清黑底 + 白边框 + 白字（数字 + F 单位标记） */
static void fb_fps_badge(uint32_t fps)
{
    if (fps > 99u) fps = 99u;
    int digits = (fps >= 10u) ? 2 : 1;
    int w = digits * FONT_PITCH + FONT_W + 2;   /* 边距1 + 数字 + F + 边距1 */
    int x0 = SHOW_FB_COLS - w - 1, y0 = 1;
    for (int y = y0; y < y0 + FONT_H + 2; y++)
        for (int x = x0; x < x0 + w; x++)
            s_fb[y >> 3][x] &= (uint8_t)~(1u << (y & 7));
    fb_rect(x0, y0, w, FONT_H + 2);
    int tx = x0 + 1;
    (void)fb_num(tx, y0 + 1, fps);
    tx += digits * FONT_PITCH;
    {
        const uint8_t *gF = FONT_5X7_F;
        for (int c = 0; c < FONT_W; c++)
            for (int r = 0; r < FONT_H; r++)
                if (gF[c] & (1u << r)) fb_px(tx + c, y0 + 1 + r);
    }
}

/* ================================================================
 * 场景 1：旋转线框立方体（u8g2 经典 demo）
 * ================================================================ */
static const int8_t CUBE_V[8][3] = {
    {-1,-1,-1}, {+1,-1,-1}, {+1,+1,-1}, {-1,+1,-1},
    {-1,-1,+1}, {+1,-1,+1}, {+1,+1,+1}, {-1,+1,+1},
};
static const uint8_t CUBE_E[12][2] = {
    {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7},
};
static void scene_cube(uint32_t ang)
{
    int32_t sy = sin_q14(ang),        cy_ = cos_q14(ang);       /* 绕 Y */
    int32_t sx = sin_q14(ang / 2u),   cx_ = cos_q14(ang / 2u);  /* 绕 X（半速） */
    int32_t px[8], py[8];
    const int32_t S = 18;             /* 半边长（像素） */

    for (int i = 0; i < 8; i++) {
        int32_t x = CUBE_V[i][0] * S, y = CUBE_V[i][1] * S, z = CUBE_V[i][2] * S;
        /* 绕 Y：x' = x·cos + z·sin ; z' = -x·sin + z·cos */
        int32_t x1 = (x * cy_ + z * sy) >> 14;
        int32_t z1 = (z * cy_ - x * sy) >> 14;
        /* 绕 X：y' = y·cos - z·sin ; z'' = y·sin + z·cos */
        int32_t y1 = (y * cx_ - z1 * sx) >> 14;
        int32_t z2 = (y * sx + z1 * cx_) >> 14;
        /* 弱透视：f=64, 视距 d=96（Q8 定点避免除大数） */
        int32_t zi = z2 + 96;
        if (zi < 24) zi = 24;         /* 防翻转除小数 */
        px[i] = 64 + (int32_t)(((int64_t)x1 * 64) / zi);
        py[i] = 32 + (int32_t)(((int64_t)y1 * 64) / zi);
    }
    for (int e = 0; e < 12; e++)
        fb_line(px[CUBE_E[e][0]], py[CUBE_E[e][0]], px[CUBE_E[e][1]], py[CUBE_E[e][1]]);
}

/* ================================================================
 * 场景 2：双弹跳球（经典 OLED demo）
 * ================================================================ */
/* 三角波：wave 在 0..period..0 间往复（整数，无除法） */
static uint32_t tri_wave(uint32_t wave, uint32_t period)
{
    uint32_t t = wave % (period * 2u);
    return (t < period) ? t : (period * 2u - t);
}

static void scene_bounce(uint32_t frame)
{
    fb_rect(0, 0, SHOW_FB_COLS, 64);
    fb_line(0, 0, SHOW_FB_COLS - 1, 63);       /* 对角装饰线 */
    fb_line(SHOW_FB_COLS - 1, 0, 0, 63);

    /* 球 1：r=5，速度 (3,2) 像素/帧 */
    fb_disc(6 + (int)tri_wave(frame * 3u, 55), 6 + (int)tri_wave(frame * 2u, 24), 5);
    /* 球 2：r=3，速度 (2,3)，相位错开 */
    fb_disc(4 + (int)tri_wave(frame * 2u + 37, 59), 4 + (int)tri_wave(frame * 3u + 11, 26), 3);
}

/* ================================================================
 * 场景 3：三重正弦示波器
 * ================================================================ */
static void scene_wave(uint32_t ang)
{
    int py0 = -1, py1 = -1, py2 = -1;
    for (int x = 0; x < SHOW_FB_COLS; x += 2) {
        int32_t a = (int32_t)((ang + (uint32_t)x * 5u) % 360u);
        int32_t b = (int32_t)((ang * 2u + (uint32_t)x * 3u) % 360u);
        int32_t c = (int32_t)((ang / 2u + (uint32_t)x * 7u) % 360u);
        int y0 = 32 + (int)((sin_q14((uint32_t)a) * 20) >> 14);
        int y1 = 32 + (int)((sin_q14((uint32_t)b) * 12) >> 14);
        int y2 = 32 + (int)((sin_q14((uint32_t)c) * 26) >> 14);
        if (py0 >= 0) fb_line(x - 2, py0, x, y0);
        if (py1 >= 0) fb_line(x - 2, py1, x, y1);
        if (py2 >= 0) fb_line(x - 2, py2, x, y2);
        py0 = y0; py1 = y1; py2 = y2;
    }
}

/* ================================================================
 * OLED 底层（VT2 范式：16b 命令编码 / reinit×3 / 页寻址）
 * ================================================================ */
#define PADS_BANK0_BASE  0x4001C000u
#define PADS_GPIO(n)     (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x04u + 4u * (n)))
#define PADS_PUE         (1u << 3)
#define PADS_PDE         (1u << 2)

static void show_i2c_reinit(void)
{
    hal_i2c_init(SHOW_I2C_BUS, SHOW_I2C_HZ);
    PADS_GPIO(4) = (PADS_GPIO(4) | PADS_PUE) & ~PADS_PDE;   /* 内部上拉（VT2 同款） */
    PADS_GPIO(5) = (PADS_GPIO(5) | PADS_PUE) & ~PADS_PDE;
    g_reinits++;
}

static int oled_tx_cmds(const uint8_t *cmds, int ncmds)
{
    if (ncmds <= 0 || ncmds > 8) return -1;
    for (int i = 0; i < ncmds; i++) {
        s_cmd_buf[2 * i    ] = (i == ncmds - 1) ? 0x00 : 0x80;  /* Co=1 续 / 0 末条 */
        s_cmd_buf[2 * i + 1] = cmds[i];
    }
    for (int retry = 0; retry < 3; retry++) {
        if (hal_i2c_tx(SHOW_I2C_BUS, SHOW_OLED_ADDR, s_cmd_buf, (size_t)(2 * ncmds)) == HAL_OK)
            return 0;
        g_i2c_errs++;
        show_i2c_reinit();
    }
    return -1;
}

static int oled_set_addr(uint8_t page, uint8_t col)
{
    uint8_t c[3];
    c[0] = (uint8_t)(0xB0 | (page & 0x07));
    c[1] = (uint8_t)(0x00 | (col & 0x0F));
    c[2] = (uint8_t)(0x10 | ((col >> 4) & 0x0F));
    return oled_tx_cmds(c, 3);
}

static int oled_init(void)
{
    /* Adafruit/U8g2 通用序列（VT2 v2.3 定稿，Contrast 紧跟 Charge Pump） */
    static const uint8_t init1[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40};
    static const uint8_t init2[] = {0x8D, 0x14, 0x81, 0xFF};        /* ChargePump+Contrust */
    static const uint8_t init3[] = {0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12};
    static const uint8_t init4[] = {0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
    show_i2c_reinit();          /* 首次 init 也走 reinit 路径（含上拉配置） */
    int r = 0;
    r |= oled_tx_cmds(init1, 8);   /* 分 4 组发送，Co 编码自动处理 */
    r |= oled_tx_cmds(init2, 4);
    r |= oled_tx_cmds(init3, 6);
    r |= oled_tx_cmds(init4, 7);
    return r;
}

static int oled_push_fb(void)
{
    s_page_buf[0] = 0x40;      /* D/C=1：后续全为 GDRAM 数据 */
    for (int page = 0; page < SHOW_PAGES; page++) {
        if (oled_set_addr((uint8_t)page, 0) != 0) return -1;
        memcpy(&s_page_buf[1], s_fb[page], SHOW_TX_COLS);
        int ok = 0;
        for (int retry = 0; retry < 3; retry++) {
            if (hal_i2c_tx(SHOW_I2C_BUS, SHOW_OLED_ADDR, s_page_buf, 1u + SHOW_TX_COLS) == HAL_OK) {
                ok = 1; break;
            }
            g_i2c_errs++;
            show_i2c_reinit();
        }
        if (!ok) return -1;
    }
    return 0;
}

static void oled_display_off(void)
{
    uint8_t c = 0xAE;
    oled_tx_cmds(&c, 1);
}

/* ================================================================
 * 呼吸 PWM：LED GP25 平滑呼吸；GP15 独立精确台阶梯度（电机测试）
 *   LED  ：tri = 0..255 三角波；level = tri² 占满 0..62499（正弦近似呼吸）
 *   GP15 ：每级停留 GP15_STEP_HOLD_MS，占空比 0→100% 步进 10% 循环，
 *          level = (WRAP+1)*percent/100 → 每个台阶占空比精确（电机阶梯电压）
 * ================================================================ */
static uint32_t s_breath_step = 0;

/* GP15 台阶状态机参数（电机测试用，可调） */
#define GP15_STEP_COUNT    11u   /* 0,10,20,...,100 共 11 级 */
#define GP15_STEP_HOLD_MS  500u  /* 每级停留毫秒 */
static uint8_t  s_gp15_step = 0;
static uint32_t s_gp15_next_tick = 0;

static void breath_pwm_start(void)
{
    gpio_set_function(SHOW_LED_PIN, GPIO_FUNC_PWM);
    gpio_set_function(SHOW_PWM_PIN, GPIO_FUNC_PWM);
    uint sliceL = pwm_gpio_to_slice_num(SHOW_LED_PIN);   /* 4 */
    uint sliceP = pwm_gpio_to_slice_num(SHOW_PWM_PIN);   /* 7 */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, SHOW_PWM_WRAP);
    pwm_config_set_clkdiv(&cfg, SHOW_PWM_DIV);
    pwm_init(sliceL, &cfg, true);
    pwm_init(sliceP, &cfg, true);
    s_breath_step = 0;
    s_gp15_step = 0;
    s_gp15_next_tick = 0;
}

/* GP15 独立精确台阶：0→10%→…→100% 循环，每级停留 HOLD_MS。
 * 用 (WRAP+1) 作占空比基准：level=percent/100*(WRAP+1)，
 * percent=100 → level=WRAP+1 → 100% 恒高，台阶间占空比精确。 */
static void gp15_pwm_step_update(void)
{
    uint32_t now = hal_systick_get_tick();
    if ((int32_t)(now - s_gp15_next_tick) >= 0) {
        s_gp15_next_tick = now + GP15_STEP_HOLD_MS;
        if (++s_gp15_step >= GP15_STEP_COUNT) s_gp15_step = 0;
    }
    uint32_t percent = s_gp15_step * 10u;                 /* 0..100 */
    uint32_t level = (uint32_t)(((uint64_t)(SHOW_PWM_WRAP + 1u) * percent) / 100u);
    pwm_set_chan_level(pwm_gpio_to_slice_num(SHOW_PWM_PIN),
                       pwm_gpio_to_channel(SHOW_PWM_PIN), (uint16_t)level);
}

static void breath_pwm_update(void)      /* 每帧调用一次（~40fps → LED 周期 ~6.4s 慢呼吸） */
{
    s_breath_step++;
    uint32_t t = s_breath_step & 0xFFu;
    uint32_t tri = (t < 128u) ? t : (255u - t);
    uint32_t level = (tri * tri * (SHOW_PWM_WRAP + 1u)) >> 16;   /* tri²≤65025，不溢出 */
    pwm_set_chan_level(pwm_gpio_to_slice_num(SHOW_LED_PIN), pwm_gpio_to_channel(SHOW_LED_PIN), (uint16_t)level);
    /* GP15 独立：精确台阶梯度（电机测试），与 LED 呼吸解耦 */
    gp15_pwm_step_update();
}

static void breath_pwm_stop(void)
{
    pwm_set_enabled(pwm_gpio_to_slice_num(SHOW_LED_PIN), false);
    pwm_set_enabled(pwm_gpio_to_slice_num(SHOW_PWM_PIN), false);
    gpio_set_function(SHOW_LED_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SHOW_PWM_PIN, GPIO_FUNC_SIO);
    gpio_put(SHOW_LED_PIN, 0);           /* LED 灭 */
    gpio_put(SHOW_PWM_PIN, 0);           /* GP15 输出低 */
    gpio_set_dir(SHOW_PWM_PIN, GPIO_OUT);
}

/* ================================================================
 * show 主任务
 * ================================================================ */
static void task_show(void *arg)
{
    (void)arg;
    g_show_state = 1;
    g_start_tick = hal_systick_get_tick();
    g_frames = g_fails = g_i2c_errs = g_reinits = 0;
    g_fps_min = 0xFFFFFFFFu; g_fps_max = 0; g_fps_sum = 0; g_fps_n = 0;

    /* 启动延迟：让 shell/FS banner 先打完（VT2 v2.2.10 交错教训）。
     * 分片睡眠保证 stop 请求能在延迟期内立即生效。 */
    for (int i = 0; i < 25 && g_show_state != 2u; i++) task_sleep(100);

    if (oled_init() != 0) {
        shell_puts(NULL, "[SHOW ] WARN: OLED init NACK (screen absent?) - PWM still running\r\n");
    }
    breath_pwm_start();

    uint32_t frame = 0;
    uint32_t fps_win_frames = 0;
    uint32_t fps_win_tick = hal_systick_get_tick();
    uint32_t ang = 0;

    for (;;) {
        if (g_show_state == 2) break;    /* stop 请求 */

        /* 【v2.7.1】超频切换期间暂停（volatile 跨核可见）。
         * sysclk_apply_mhz 切换时钟时只保护 core0 中断，core1 不受保护；
         * 检测到 g_oc_switching 则自我暂停，避免在 clk_sys 过渡频率下
         * 运行 I2C/渲染破坏共享 RAM。 */
        while (g_oc_switching) task_sleep(5);

        /* —— 渲染 —— */
        fb_clr();
        uint32_t scene = (hal_systick_get_tick() - g_start_tick) / SHOW_SCENE_MS;
        switch (scene % 3u) {
            case 0: scene_cube(ang); break;
            case 1: scene_bounce(frame); break;
            default: scene_wave(ang); break;
        }
        fb_fps_badge(g_fps_now);

        /* —— 推送 —— */
        if (oled_push_fb() == 0) g_frames++;
        else g_fails++;

        breath_pwm_update();
        frame++;
        fps_win_frames++;

        /* —— FPS 1s 窗口统计 —— */
        uint32_t now = hal_systick_get_tick();
        if ((uint32_t)(now - fps_win_tick) >= 1000u) {
            uint32_t fps = fps_win_frames * 1000u / (uint32_t)(now - fps_win_tick);
            g_fps_now = fps;
            g_fps_sum += fps; g_fps_n++;
            if (fps < g_fps_min) g_fps_min = fps;
            if (fps > g_fps_max) g_fps_max = fps;
            fps_win_frames = 0;
            fps_win_tick = now;
        }

        ang += 3u;                        /* 3°/帧 ≈ 120 帧/圈 */
        task_sleep(1);                    /* 让出 CPU（时间片轮转友好） */
    }

    /* —— 收尾：恢复 GPIO / 关屏 / 自挂起（TCB 由 stop 命令侧 destroy 回收） —— */
    breath_pwm_stop();
    oled_display_off();
    g_show_state = 0;
    g_show_task = NULL;
    task_suspend(g_current_task);         /* 自行退出，禁止任务函数返回 */
}

/* 等待 show 任务收尾并回收 TCB（stop/off 命令用；返回 0=已停） */
static tcb_t *s_orphan = NULL;    /* stop 超时后遗留的已退/将退 TCB，下次 spawn 前回收 */

static int show_task_stop_join(void)
{
    tcb_t *t = g_show_task;
    if (!t) return 0;
    g_show_state = 2;                     /* 任务下帧自行收尾 */
    /* 上限 2s：正常 <50ms；I2C 最坏链（8 页 × 3 重试 × 200ms 超时）≈4.8s
     * 不可全等，超时即报告。任务随后仍会自行退出，TCB 记入 orphan。 */
    for (int i = 0; i < 100 && g_show_task; i++) task_sleep(20);
    if (g_show_task) { s_orphan = t; return -1; }
    task_destroy(t);                      /* 已 SUSPEND，安全回收栈与槽位 */
    return 0;
}

/* ================================================================
 * 启动控制
 * ================================================================ */
static void show_task_spawn(void)
{
    if (s_orphan) { task_destroy(s_orphan); s_orphan = NULL; }
    if (g_show_task) return;
    /* 2048 = shell 同级：I2C 驱动调用链深（SDK i2c_write_blocking_internal
     * 栈帧 ~100B + timeout 检查器 + hal 层），shell.c 注释明确警告
     * "768 会溢出"，VT2 用 1536；show 渲染局部（px/py 各 64B）更深，
     * 取 2048。1024 实测溢出 → r5 恢复成 g_tick_interval_us(1000) →
     * blx r5 跳 ROM 0x3E8 → HardFault（flash76 板上实测）。
     * v2.7.1：绑定到 core1（task_create_on core=1），OLED 刷屏在核1跑，
     * 把重负载 I2C/渲染从 core0 卸荷。core1 由 boot_setup 开机自动启动。
     * v2.7.1-fix：栈 2048→4096。show 在 core1 持续 I2C 传输，若 I2C 深链路
     * 偶发溢出会破坏共享 RAM（含 TinyUSB 状态 / idle 栈）→ USB 失效、
     * core0 idle PC=0 HardFault。取 4096 消除该越界源。 */
    g_show_task = task_create_on("show", task_show, NULL, 4096, 2, 1);
}

/* demo_app_init 调用（调度器启动前）：config 决定是否自动运行 */
void demo_show_boot_start(void)
{
    config_data_t cfg;
    bool valid = config_read(&cfg);
    uint8_t on = valid ? cfg.reserved[0] : 1u;   /* 无效/未固化 → 默认开 */
    if (on) {
        show_task_spawn();
        shell_puts(NULL, "[SHOW ] auto-start enabled (OLED FPS + breathing LED + PWM GP15; 'show off' to disable)\r\n");
    }
}

/* ================================================================
 * shell 命令：show status|stop|start|on|off
 * ================================================================ */
static int cmd_show(int argc, char **argv, shell_ctx_t *ctx)
{
    (void)ctx;
    const char *sub = (argc > 1) ? argv[1] : "status";

    if (!strcmp(sub, "status")) {
        char buf[96];
        uint32_t up_s = 0;
        if (g_start_tick) {
            uint32_t now = hal_systick_get_tick();
            up_s = (now >= g_start_tick) ? (now - g_start_tick) / 1000u : 0;
        }
        shell_puts(NULL, g_show_task ? "SHOW: running\r\n" : "SHOW: stopped\r\n");
        shell_snprintf(buf, sizeof(buf),
                       "SHOW: uptime=%us frames=%u fails=%u\r\n",
                       (unsigned)up_s, (unsigned)g_frames, (unsigned)g_fails);
        shell_puts(NULL, buf);
        shell_snprintf(buf, sizeof(buf),
                       "SHOW: fps now=%u min=%u avg=%u max=%u\r\n",
                       (unsigned)g_fps_now,
                       (unsigned)((g_fps_min == 0xFFFFFFFFu) ? 0 : g_fps_min),
                       (unsigned)(g_fps_n ? (uint32_t)(g_fps_sum / g_fps_n) : 0),
                       (unsigned)g_fps_max);
        shell_puts(NULL, buf);
        shell_snprintf(buf, sizeof(buf),
                       "SHOW: i2c_err=%u reinits=%u  (bus%u @0x%02X %ukHz)\r\n",
                       (unsigned)g_i2c_errs, (unsigned)g_reinits,
                       (unsigned)SHOW_I2C_BUS, (unsigned)SHOW_OLED_ADDR,
                       (unsigned)(SHOW_I2C_HZ / 1000u));
        shell_puts(NULL, buf);
        shell_puts(NULL, "SHOW: LED breath GP25 | PWM sweep GP15 (1kHz @125MHz)\r\n");
        return 0;
    }

    if (!strcmp(sub, "stop")) {
        if (!g_show_task) { shell_puts(NULL, "SHOW: not running\r\n"); return 0; }
        if (show_task_stop_join() != 0) {
            shell_puts(NULL, "SHOW: stop timeout (task busy?)\r\n");
            return 1;
        }
        shell_puts(NULL, "SHOW: stopped (this boot only; 'show on/off' persists)\r\n");
        return 0;
    }

    if (!strcmp(sub, "start")) {
        if (g_show_task) { shell_puts(NULL, "SHOW: already running\r\n"); return 0; }
        show_task_spawn();
        shell_puts(NULL, g_show_task ? "SHOW: started\r\n" : "SHOW: spawn FAILED\r\n");
        return g_show_task ? 0 : 1;
    }

    if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        int on = (sub[1] == 'n');
        config_data_t cfg;
        (void)config_read(&cfg);                /* 无效也得到 defaults 骨架 */
        cfg.reserved[0] = (uint8_t)on;
        hal_err_t e = config_write(&cfg);
        char buf[80];
        shell_snprintf(buf, sizeof(buf),
                       "SHOW: persist %s %s (next boot %s)\r\n",
                       on ? "ON" : "OFF",
                       (e == HAL_OK) ? "saved" : "FLASH WRITE FAIL",
                       on ? "auto-start" : "no auto-start");
        shell_puts(NULL, buf);
        if (on && !g_show_task) { show_task_spawn(); shell_puts(NULL, "SHOW: started\r\n"); }
        if (!on && g_show_task) {
            if (show_task_stop_join() == 0) shell_puts(NULL, "SHOW: stopped\r\n");
            else shell_puts(NULL, "SHOW: stop timeout\r\n");
        }
        return (e == HAL_OK) ? 0 : 1;
    }

    shell_puts(NULL,
        "show: demo/stability showcase (OLED FPS + breathing LED + PWM GP15)\r\n"
        "  show status - frames/fps(min,avg,max)/i2c errors/uptime\r\n"
        "  show stop   - stop this boot    show start - start this boot\r\n"
        "  show on     - persist auto-run  show off  - persist off (remove)\r\n");
    return 1;
}

void shell_show_register(void)
{
    shell_register("show", cmd_show, "show status|stop|start|on|off",
                   "Demo showcase: OLED FPS animation + breathing LED + GP15 PWM (stability validation)");
}

#else /* !OS_CFG_DEMO_APP || !OS_CFG_PERIPH_SERVICE —— 空桩保证链接 */

void demo_show_boot_start(void) { }
void shell_show_register(void) { }

#endif
