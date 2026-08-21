/**
 * @file    shell_ovclk.c
 * @brief   v2.4 Shell 超频 / 多核固化命令 `ovclk`
 *
 *  用法（对应需求：默认单核+125MHz，通过指令设置并固化，重启生效）：
 *    ovclk list                  — 列出预设档位
 *    ovclk set <tier|MHz>        — 设置**待固化**频率（仅 RAM，不改当前时钟）
 *    ovclk try <tier|MHz>        — 运行时临时切换（不固化，reboot 恢复固化/125）
 *    ovclk get                   — 查看当前实际主频 + 已固化频率/多核标志
 *    ovclk mcore <0|1>           — 设置**待固化**多核标志（0=单核 1=双核）
 *    ovclk save                  — 把待固化频率+多核写入 Flash（掉电保留）
 *    ovclk unsave | reset        — 擦除固化区 → 下次启动恢复 单核 + 125MHz
 *
 *  设计说明（v2.4.2 起支持任意频率）：
 *    · 参数即"档位号"（0..COUNT-1）也接受"任意 MHz"（100..500），由
 *      ovclk_to_mhz() 统一解析；频率相关参数自动匹配 PLL/flash/clk_peri/电压
 *      （见 hal_port.c sysclk_apply_mhz）。
 *    · 所有"变更"只写 RAM（s_pending），不实时改时钟；`try` 例外 —— 它
 *      运行时临时切换（测试稳定性用），重启即回固化值/125MHz。
 *    · 必须 `save` 固化后 **重启** 才在冷启动阶段应用（安全兜底：
 *      未固化 / CRC 损坏 / >250MHz 极限频率 → 一律回到 单核 + 125MHz）。
 */
#include "shell_core.h"
#include "hal/config_store.h"
#include "hal/sysclk.h"
#include <string.h>
#include <stdlib.h>

/* 待固化配置镜像（RAM），set/mcore 修改它，save 写 Flash */
static config_data_t s_pending;
static bool s_pending_loaded = false;

/* 【Bugfix】只从 flash 载入一次。
 *   之前 ovclk_init_pending() 在每条命令开头都调用，导致 `ovclk set 2`
 *   （RAM: tier=2）后执行 `ovclk save` 时，save 里的 init 又把 s_pending
 *   从 flash 重载回旧值 → 固化区永远写错。改为一次性载入，之后
 *   set/mcore/save 都基于同一份 RAM 镜像操作。 */
static void ovclk_init_pending(void) {
    if (s_pending_loaded) return;
    s_pending_loaded = true;
    config_data_t saved;
    if (config_read(&saved)) {
        s_pending = saved;                 /* 以已固化值为基准 */
    } else {
        config_defaults(&s_pending);       /* 未固化 → 默认单核+125 */
    }
}

/* 解析 "<tier|MHz>" 为频率（MHz）。
 *   · 0..SYSCLK_TIER_COUNT-1      → 档位号，取该档 MHz；
 *   · 其他输入视为非法，仅支持预设档位 (125/250/375/500MHz)，
 *     避免任意频率导致 PLL 锁定失败 / 电压档位不匹配。 */
static int ovclk_to_mhz(const char *s, uint32_t *out) {
    if (!s || !out) return 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') return 0;
    if (v >= 0 && v < SYSCLK_TIER_COUNT) { *out = g_sysclk_tiers[v].mhz; return 1; }
    return 0;
}

/* 格式化已固化频率：从 Flash 读取实际固化值 */
static uint32_t ovclk_saved_mhz(void) {
    config_data_t cfg;
    if (config_read(&cfg)) {
        return cfg.clock_mhz ? cfg.clock_mhz : SYSCLK_MHZ_DEFAULT;
    }
    return SYSCLK_MHZ_DEFAULT;
}

static int cmd_ovclk(int argc, char **argv, shell_ctx_t *ctx) {
    ovclk_init_pending();

    if (argc < 2) {
        shell_puts(ctx,
            "ovclk: overclock / multi-core persistence (takes effect after SAVE + REBOOT)\n"
            "  ovclk list               - list preset tiers (125/250/375/500MHz)\n"
            "  ovclk set <tier>         - set frequency tier to persist (RAM only)\n"
            "  ovclk try <tier>         - switch clock NOW (NOT persisted, reboot reverts)\n"
            "  ovclk mcore <0|1>        - set multi-core flag to persist (0=single 1=dual)\n"
            "  ovclk get                - show current clock + saved config\n"
            "  ovclk save               - persist frequency + mcore to flash\n"
            "  ovclk unsave | reset     - erase persisted config (back to 125MHz single-core)\n"
        );
        return 0;
    }

    const char *sub = argv[1];

    /* ---- list ---- */
    if (!strcmp(sub, "list")) {
        shell_puts(ctx, "Preset tiers (CPU MHz, both cores together):\n");
        for (int i = 0; i < SYSCLK_TIER_COUNT; i++) {
            char line[48];
            shell_snprintf(line, sizeof(line), "  %d: %5u MHz%s\n",
                i, (unsigned)g_sysclk_tiers[i].mhz,
                g_sysclk_tiers[i].hidden ? "  (hidden)" : "");
            shell_puts(ctx, line);
        }
        shell_puts(ctx, "  or input any MHz from 100..500 (PLL matched automatically).\n");
        return 0;
    }

    /* ---- set ---- */
    if (!strcmp(sub, "set")) {
        if (argc < 3) { shell_puts(ctx, "usage: ovclk set <tier 0..3>\n"); return 1; }
        uint32_t mhz = 0u;
        if (!ovclk_to_mhz(argv[2], &mhz)) {
            shell_puts(ctx, "ERROR: invalid tier. Use 0=125MHz 1=250MHz 2=375MHz 3=500MHz.\n");
            return 2;
        }
        s_pending.clock_mhz = (uint16_t)mhz;
        char line[56];
        shell_snprintf(line, sizeof(line), "pending clock = %u MHz. Run `ovclk save` + reboot to apply.\n",
                       (unsigned)mhz);
        shell_puts(ctx, line);
        return 0;
    }

    /* ---- try ---- 运行时临时切换（不固化，reboot 后回到固化值/125MHz） */
    if (!strcmp(sub, "try")) {
        if (argc < 3) { shell_puts(ctx, "usage: ovclk try <tier 0..3>\n"); return 1; }
        uint32_t mhz = 0u;
        if (!ovclk_to_mhz(argv[2], &mhz)) {
            shell_puts(ctx, "ERROR: invalid tier. Use 0=125MHz 1=250MHz 2=375MHz 3=500MHz.\n");
            return 2;
        }
        shell_puts(ctx, "applying now... (NOT persisted; reboot returns to saved/125MHz)\n");
        bool ok = sysclk_apply_mhz(mhz);
        char line[64];
        shell_snprintf(line, sizeof(line), "apply %s, current clock = %u MHz\n",
                       ok ? "OK" : "FAILED (fell back)", (unsigned)sysclk_current_mhz());
        shell_puts(ctx, line);
        return 0;
    }

    /* ---- mcore ---- */
    if (!strcmp(sub, "mcore")) {
        if (argc < 3) { shell_puts(ctx, "usage: ovclk mcore <0|1>\n"); return 1; }
        int on = atoi(argv[2]);
        if (on != 0 && on != 1) { shell_puts(ctx, "ERROR: mcore accepts 0 (single) or 1 (dual).\n"); return 2; }
        s_pending.multi_core = (uint8_t)on;
        shell_puts(ctx, on ? "pending multi-core = DUAL. Run `ovclk save` + reboot.\n"
                           : "pending multi-core = SINGLE. Run `ovclk save` + reboot.\n");
        return 0;
    }

    /* ---- get ---- */
    if (!strcmp(sub, "get")) {
        char line[64];
        shell_snprintf(line, sizeof(line), "current clock  : %u MHz\n", (unsigned)sysclk_current_mhz());
        shell_puts(ctx, line);
        shell_snprintf(line, sizeof(line), "saved clock    : %u MHz\n", (unsigned)ovclk_saved_mhz());
        shell_puts(ctx, line);
        shell_puts(ctx, s_pending.multi_core ? "saved multi-core: DUAL\n" : "saved multi-core: SINGLE\n");
        return 0;
    }

    /* ---- save ---- */
    if (!strcmp(sub, "save")) {
        hal_err_t e = config_write(&s_pending);
        if (e == HAL_OK) {
            char line[64];
            shell_snprintf(line, sizeof(line),
                "OK: %u MHz persisted. Reboot to apply (if unsafe, we stay 125MHz).\n",
                (unsigned)ovclk_saved_mhz());
            shell_puts(ctx, line);
        } else {
            shell_puts(ctx, "ERROR: config write failed (flash I/O).\n");
        }
        return (e == HAL_OK) ? 0 : 3;
    }

    /* ---- unsave / reset ---- */
    if (!strcmp(sub, "unsave") || !strcmp(sub, "reset")) {
        hal_err_t e = config_clear_all();
        config_defaults(&s_pending);
        if (e == HAL_OK) {
            shell_puts(ctx, "OK: persisted config erased. Next boot = 125MHz single-core.\n");
        } else {
            shell_puts(ctx, "ERROR: erase failed (flash I/O).\n");
        }
        return (e == HAL_OK) ? 0 : 3;
    }

    shell_puts(ctx, "unknown ovclk sub-command. Try `ovclk` for help.\n");
    return 1;
}

void shell_ovclk_register(void) {
    shell_register("ovclk", cmd_ovclk, "ovclk list|set|try|mcore|get|save|unsave|reset",
                   "Overclock frequency + multi-core persistence (save + reboot to apply)");
}
