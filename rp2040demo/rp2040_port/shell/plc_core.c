/**
 * @file    plc_core.c
 * @brief   PLC 循环指令核心实现
 */

#include "hal/plc_core.h"
#include "hal_interface.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

/* ================================================================
 * 指令定义表 (用于反汇编/汇编器)
 * ================================================================ */

const plc_inst_def_t plc_inst_table[] = {
    { "NOP",      PLC_OP_NOP,      0, {0} },
    { "END",      PLC_OP_END,      0, {0} },
    { "RET",      PLC_OP_RET,      0, {0} },

    { "LD",       PLC_OP_LD,       1, {PLC_OPERAND_BIT} },
    { "LDI",      PLC_OP_LDI,      1, {PLC_OPERAND_BIT} },
    { "AND",      PLC_OP_AND,      1, {PLC_OPERAND_BIT} },
    { "ANI",      PLC_OP_ANI,      1, {PLC_OPERAND_BIT} },
    { "OR",       PLC_OP_OR,       1, {PLC_OPERAND_BIT} },
    { "ORI",      PLC_OP_ORI,      1, {PLC_OPERAND_BIT} },
    { "OUT",      PLC_OP_OUT,      1, {PLC_OPERAND_BIT} },
    { "OUTI",     PLC_OP_OUTI,     1, {PLC_OPERAND_BIT} },
    { "SET",      PLC_OP_SET,      1, {PLC_OPERAND_BIT} },
    { "RST",      PLC_OP_RST,      1, {PLC_OPERAND_BIT} },
    { "PLS",      PLC_OP_PLS,      1, {PLC_OPERAND_BIT} },
    { "PLF",      PLC_OP_PLF,      1, {PLC_OPERAND_BIT} },
    { "INV",      PLC_OP_INV,      0, {0} },
    { "MEP",      PLC_OP_MEP,      0, {0} },
    { "MEF",      PLC_OP_MEF,      0, {0} },

    { "TMR",      PLC_OP_TMR,      2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },
    { "TMR10",    PLC_OP_TMR10,    2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },
    { "TMR1",     PLC_OP_TMR1,     2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },
    { "CNT",      PLC_OP_CNT,      2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },
    { "CNT32",    PLC_OP_CNT32,    2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },
    { "DCNT",     PLC_OP_DCNT,     2, {PLC_OPERAND_BIT, PLC_OPERAND_CONST} },

    { "CMP",      PLC_OP_CMP,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "CMPP",     PLC_OP_CMPP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "ZCP",      PLC_OP_ZCP,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },

    { "DCMP",     PLC_OP_DCMP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DCMPP",    PLC_OP_DCMPP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DZCP",     PLC_OP_DZCP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },

    { "MOV",      PLC_OP_MOV,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "MOVP",     PLC_OP_MOVP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "MOVW",     PLC_OP_MOVW,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "BMOV",     PLC_OP_BMOV,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "FMOV",     PLC_OP_FMOV,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "XCH",      PLC_OP_XCH,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SWAP",     PLC_OP_SWAP,     1, {PLC_OPERAND_WORD} },

    { "DMOV",     PLC_OP_DMOV,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DMOVP",    PLC_OP_DMOVP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DMOVW",    PLC_OP_DMOVW,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },

    { "ADD",      PLC_OP_ADD,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "ADDP",     PLC_OP_ADDP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SUB",      PLC_OP_SUB,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SUBP",     PLC_OP_SUBP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "MUL",      PLC_OP_MUL,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "MULP",     PLC_OP_MULP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DIV",      PLC_OP_DIV,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DIVP",     PLC_OP_DIVP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "INC",      PLC_OP_INC,      1, {PLC_OPERAND_WORD} },
    { "INCP",     PLC_OP_INCP,     1, {PLC_OPERAND_WORD} },
    { "DEC",      PLC_OP_DEC,      1, {PLC_OPERAND_WORD} },
    { "DECP",     PLC_OP_DECP,     1, {PLC_OPERAND_WORD} },

    { "DADD",     PLC_OP_DADD,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DADDP",    PLC_OP_DADDP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DSUB",     PLC_OP_DSUB,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DSUBP",    PLC_OP_DSUBP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DMUL",     PLC_OP_DMUL,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DMULP",    PLC_OP_DMULP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DDIV",     PLC_OP_DDIV,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DDIVP",    PLC_OP_DDIVP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DINC",     PLC_OP_DINC,     1, {PLC_OPERAND_WORD} },
    { "DINCP",    PLC_OP_DINCP,    1, {PLC_OPERAND_WORD} },
    { "DDEC",     PLC_OP_DDEC,     1, {PLC_OPERAND_WORD} },
    { "DDECP",    PLC_OP_DDECP,    1, {PLC_OPERAND_WORD} },

    { "WAND",     PLC_OP_WAND,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WANDP",    PLC_OP_WANDP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WOR",      PLC_OP_WOR,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WORP",     PLC_OP_WORP,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WXOR",     PLC_OP_WXOR,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WXORP",    PLC_OP_WXORP,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "NEG",      PLC_OP_NEG,      1, {PLC_OPERAND_WORD} },
    { "NEGP",     PLC_OP_NEGP,     1, {PLC_OPERAND_WORD} },

    { "ROL",      PLC_OP_ROL,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "ROLP",     PLC_OP_ROLP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "ROR",      PLC_OP_ROR,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "RORP",     PLC_OP_RORP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SHL",      PLC_OP_SHL,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SHLP",     PLC_OP_SHLP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SHR",      PLC_OP_SHR,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SHRP",     PLC_OP_SHRP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SAL",      PLC_OP_SAL,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SALP",     PLC_OP_SALP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SAR",      PLC_OP_SAR,      2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "SARP",     PLC_OP_SARP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },

    { "DROL",     PLC_OP_DROL,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DROLP",    PLC_OP_DROLP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DROR",     PLC_OP_DROR,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DRORP",    PLC_OP_DRORP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSHL",     PLC_OP_DSHL,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSHLP",    PLC_OP_DSHLP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSHR",     PLC_OP_DSHR,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSHRP",    PLC_OP_DSHRP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSAL",     PLC_OP_DSAL,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSALP",    PLC_OP_DSALP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSAR",     PLC_OP_DSAR,     2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },
    { "DSARP",    PLC_OP_DSARP,    2, {PLC_OPERAND_WORD, PLC_OPERAND_CONST} },

    { "JMP",      PLC_OP_JMP,      1, {PLC_OPERAND_LABEL} },
    { "JMPC",     PLC_OP_JMPC,     1, {PLC_OPERAND_LABEL} },
    { "JMPNC",    PLC_OP_JMPNC,    1, {PLC_OPERAND_LABEL} },
    { "CALL",     PLC_OP_CALL,     1, {PLC_OPERAND_LABEL} },
    { "FOR",      PLC_OP_FOR,      1, {PLC_OPERAND_WORD} },
    { "NEXT",     PLC_OP_NEXT,     0, {0} },
    { "BREAK",    PLC_OP_BREAK,    0, {0} },

    { "BCD",      PLC_OP_BCD,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "BCDP",     PLC_OP_BCDP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "BIN",      PLC_OP_BIN,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "BINP",     PLC_OP_BINP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "ASC",      PLC_OP_ASC,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "HEX",      PLC_OP_HEX,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "ENCO",     PLC_OP_ENCO,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "DECO",     PLC_OP_DECO,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SEGD",     PLC_OP_SEGD,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SORT",     PLC_OP_SORT,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "WSUM",     PLC_OP_WSUM,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "MEAN",     PLC_OP_MEAN,     3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SQR",      PLC_OP_SQR,      2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "SQRP",     PLC_OP_SQRP,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "PID",      PLC_OP_PID,      3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },

    { "MODRW",    PLC_OP_MODRW,    4, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "IVCK",     PLC_OP_IVCK,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "IVDR",     PLC_OP_IVDR,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "IVRD",     PLC_OP_IVRD,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "IVWR",     PLC_OP_IVWR,     2, {PLC_OPERAND_WORD, PLC_OPERAND_WORD} },
    { "ADPRW",    PLC_OP_ADPRW,    3, {PLC_OPERAND_WORD, PLC_OPERAND_WORD, PLC_OPERAND_WORD} },

    { "LABEL",    PLC_OP_LABEL,    1, {PLC_OPERAND_LABEL} },
};

const uint16_t plc_inst_table_size = sizeof(plc_inst_table) / sizeof(plc_inst_table[0]);

/* ================================================================
 * 内部辅助宏/函数
 * ================================================================ */

#define PLC_BIT_ARR_IDX(area, index)  \
    ((area == PLC_AREA_X) ? (index) : \
     (area == PLC_AREA_Y) ? (PLC_X_MAX + index) : \
     (area == PLC_AREA_M) ? (PLC_X_MAX + PLC_Y_MAX + index) : \
     (area == PLC_AREA_T) ? (PLC_X_MAX + PLC_Y_MAX + PLC_M_MAX + index) : \
     (area == PLC_AREA_C) ? (PLC_X_MAX + PLC_Y_MAX + PLC_M_MAX + PLC_T_MAX + index) : \
     (PLC_X_MAX + PLC_Y_MAX + PLC_M_MAX + PLC_T_MAX + PLC_C_MAX + index))

static inline uint8_t *bit_byte_ptr(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    uint16_t bit_idx = PLC_BIT_ARR_IDX(area, index);
    return &ctx->prev_bit_state[bit_idx >> 3];  /* 复用 prev_bit_state 作为位存储区基址 */
}

static inline bool get_bit_raw(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    uint16_t bit_idx = PLC_BIT_ARR_IDX(area, index);
    return (ctx->prev_bit_state[bit_idx >> 3] >> (bit_idx & 7)) & 1;
}

static inline void set_bit_raw(plc_ctx_t *ctx, plc_area_t area, uint16_t index, bool val) {
    uint16_t bit_idx = PLC_BIT_ARR_IDX(area, index);
    uint8_t *byte = &ctx->prev_bit_state[bit_idx >> 3];
    if (val) *byte |= (1 << (bit_idx & 7));
    else *byte &= ~(1 << (bit_idx & 7));
}

static inline int16_t *word_ptr(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    /* 我们用一个统一的字存储区，这里简化：直接用 prev_bit_state 后续空间
     * 实际项目中应单独分配 word 区 */
    static int16_t word_storage[PLC_D_MAX + PLC_V_MAX + PLC_R_MAX];
    switch (area) {
        case PLC_AREA_D: return &word_storage[index];
        case PLC_AREA_V: return &word_storage[PLC_D_MAX + index];
        case PLC_AREA_R: return &word_storage[PLC_D_MAX + PLC_V_MAX + index];
        case PLC_AREA_T: return &ctx->timer_cur[index];
        case PLC_AREA_C: return (int16_t*)&ctx->counter_cur[index];
        default: return NULL;
    }
}

static inline int32_t *dword_ptr(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    static int32_t dword_storage[PLC_D32_MAX + PLC_V_MAX/2 + PLC_R_MAX/2];
    switch (area) {
        case PLC_AREA_D: return &dword_storage[index];
        case PLC_AREA_V: return &dword_storage[PLC_D32_MAX + index];
        case PLC_AREA_R: return &dword_storage[PLC_D32_MAX + PLC_V_MAX/2 + index];
        case PLC_AREA_C: return &ctx->counter_cur[index];
        default: return NULL;
    }
}

/* 读取操作数值 (位/字/常数) */
static bool read_operand_bit(plc_ctx_t *ctx, uint16_t encoded) {
    plc_area_t area = PLC_DECODE_AREA(encoded);
    uint16_t idx = PLC_DECODE_INDEX(encoded);
    if (area == PLC_AREA_K) return (bool)idx;
    if (idx >= (area == PLC_AREA_X ? PLC_X_MAX :
                area == PLC_AREA_Y ? PLC_Y_MAX :
                area == PLC_AREA_M ? PLC_M_MAX :
                area == PLC_AREA_T ? PLC_T_MAX :
                area == PLC_AREA_C ? PLC_C_MAX : PLC_S_MAX)) {
        ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return false;
    }
    return get_bit_raw(ctx, area, idx);
}

static int16_t read_operand_word(plc_ctx_t *ctx, uint16_t encoded) {
    plc_area_t area = PLC_DECODE_AREA(encoded);
    uint16_t idx = PLC_DECODE_INDEX(encoded);
    if (area == PLC_AREA_K) return (int16_t)idx;
    int16_t *ptr = word_ptr(ctx, area, idx);
    if (!ptr) { ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return 0; }
    return *ptr;
}

static int32_t read_operand_dword(plc_ctx_t *ctx, uint16_t encoded_lo, uint16_t encoded_hi) {
    plc_area_t area = PLC_DECODE_AREA(encoded_lo);
    uint16_t idx = PLC_DECODE_INDEX(encoded_lo);
    if (area == PLC_AREA_K) {
        return ((int32_t)encoded_hi << 16) | (encoded_lo & 0xFFFF);
    }
    int32_t *ptr = dword_ptr(ctx, area, idx);
    if (!ptr) { ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return 0; }
    return *ptr;
}

static void write_operand_bit(plc_ctx_t *ctx, uint16_t encoded, bool val) {
    plc_area_t area = PLC_DECODE_AREA(encoded);
    uint16_t idx = PLC_DECODE_INDEX(encoded);
    if (area == PLC_AREA_K) return; /* 常数不可写 */
    if (idx >= (area == PLC_AREA_X ? PLC_X_MAX :
                area == PLC_AREA_Y ? PLC_Y_MAX :
                area == PLC_AREA_M ? PLC_M_MAX :
                area == PLC_AREA_T ? PLC_T_MAX :
                area == PLC_AREA_C ? PLC_C_MAX : PLC_S_MAX)) {
        ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return;
    }
    set_bit_raw(ctx, area, idx, val);
}

static void write_operand_word(plc_ctx_t *ctx, uint16_t encoded, int16_t val) {
    plc_area_t area = PLC_DECODE_AREA(encoded);
    uint16_t idx = PLC_DECODE_INDEX(encoded);
    if (area == PLC_AREA_K) return;
    int16_t *ptr = word_ptr(ctx, area, idx);
    if (!ptr) { ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return; }
    *ptr = val;
}

static void write_operand_dword(plc_ctx_t *ctx, uint16_t encoded_lo, uint16_t encoded_hi, int32_t val) {
    plc_area_t area = PLC_DECODE_AREA(encoded_lo);
    uint16_t idx = PLC_DECODE_INDEX(encoded_lo);
    if (area == PLC_AREA_K) return;
    int32_t *ptr = dword_ptr(ctx, area, idx);
    if (!ptr) { ctx->error = 1; ctx->last_error = PLC_ERR_INVALID_ADDR; return; }
    *ptr = val;
}

/* 边沿检测辅助 */
static bool check_rising_edge(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    bool cur = get_bit_raw(ctx, area, index);
    bool prev = (ctx->prev_bit_state[PLC_BIT_ARR_IDX(area, index) >> 3] >> (PLC_BIT_ARR_IDX(area, index) & 7)) & 1;
    return cur && !prev;
}

static bool check_falling_edge(plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    bool cur = get_bit_raw(ctx, area, index);
    bool prev = (ctx->prev_bit_state[PLC_BIT_ARR_IDX(area, index) >> 3] >> (PLC_BIT_ARR_IDX(area, index) & 7)) & 1;
    return !cur && prev;
}

/* 标号查找 */
static int16_t find_label_pc(plc_ctx_t *ctx, uint16_t label_id) {
    for (uint16_t i = 0; i < ctx->label_count; i++) {
        if (ctx->label_table[i].label_id == label_id) return ctx->label_table[i].pc;
    }
    return -1;
}

/* 刷新标号表 (非 static，供 shell_plc.c 调用) */
void refresh_label_table(plc_ctx_t *ctx) {
    ctx->label_count = 0;
    for (uint16_t pc = 0; pc < ctx->program_len; pc++) {
        if (ctx->program[pc].opcode == PLC_OP_LABEL) {
            uint16_t label_id = PLC_DECODE_INDEX(ctx->program[pc].op1);
            if (ctx->label_count < 256) {
                ctx->label_table[ctx->label_count].label_id = label_id;
                ctx->label_table[ctx->label_count].pc = pc;
                ctx->label_count++;
            }
        }
    }
}

/* ================================================================
 * 核心 API 实现
 * ================================================================ */

plc_err_t plc_init(plc_ctx_t *ctx, uint16_t max_inst) {
    if (!ctx || max_inst == 0 || max_inst > PLC_MAX_INST) return PLC_ERR_INVALID_ADDR;

    memset(ctx, 0, sizeof(plc_ctx_t));

    ctx->program = (plc_inst_t*)kmalloc(max_inst * sizeof(plc_inst_t));
    if (!ctx->program) return PLC_ERR_MEMORY_FULL;

    ctx->program_max = max_inst;
    ctx->program_len = 0;
    ctx->scan_period_us = 10000;   /* 默认 10ms */
    ctx->watchdog_us = 100000;     /* 默认 100ms 看门狗 */
    ctx->config_flags = PLC_CFG_WDT_EN | PLC_CFG_HALT_ON_ERR;
    ctx->scan_min_us = 0xFFFFFFFF;

    /* 位存储区使用 prev_bit_state 数组 (复用) */
    memset(ctx->prev_bit_state, 0, PLC_BIT_BYTES);

    return PLC_ERR_OK;
}

void plc_deinit(plc_ctx_t *ctx) {
    if (ctx && ctx->program) {
        kfree(ctx->program);
        ctx->program = NULL;
    }
}

plc_err_t plc_load_program(plc_ctx_t *ctx, const uint8_t *buf, size_t len) {
    if (!ctx || !buf || len % sizeof(plc_inst_t) != 0) return PLC_ERR_INVALID_ADDR;
    uint16_t inst_count = len / sizeof(plc_inst_t);
    if (inst_count > ctx->program_max) return PLC_ERR_MEMORY_FULL;

    memcpy(ctx->program, buf, len);
    ctx->program_len = inst_count;
    ctx->pc = 0;
    ctx->error = 0;
    ctx->last_error = PLC_ERR_OK;
    refresh_label_table(ctx);
    return PLC_ERR_OK;
}

size_t plc_save_program(const plc_ctx_t *ctx, uint8_t *buf, size_t max_len) {
    if (!ctx || !buf) return 0;
    size_t need = ctx->program_len * sizeof(plc_inst_t);
    if (need > max_len) need = max_len;
    memcpy(buf, ctx->program, need);
    return need;
}

/* 单条指令执行 */
static plc_err_t exec_inst(plc_ctx_t *ctx, const plc_inst_t *inst) {
    plc_opcode_t op = inst->opcode;
    uint16_t op1 = inst->op1, op2 = inst->op2, op3 = inst->op3;

    /* 脉冲指令前缀检查：P 指令只在累加器上升沿执行 */
    bool is_pulse = (op == PLC_OP_LDI || op == PLC_OP_ANI || op == PLC_OP_ORI ||
                     op == PLC_OP_OUTI || op == PLC_OP_MOVP || op == PLC_OP_ADDP ||
                     op == PLC_OP_SUBP || op == PLC_OP_MULP || op == PLC_OP_DIVP ||
                     op == PLC_OP_INCP || op == PLC_OP_DECP || op == PLC_OP_CMPP ||
                     op == PLC_OP_WANDP || op == PLC_OP_WORP || op == PLC_OP_WXORP ||
                     op == PLC_OP_NEGP || op == PLC_OP_ROLP || op == PLC_OP_RORP ||
                     op == PLC_OP_SHLP || op == PLC_OP_SHRP || op == PLC_OP_SALP ||
                     op == PLC_OP_SARP || op == PLC_OP_BCDP || op == PLC_OP_BINP ||
                     op == PLC_OP_DADDP || op == PLC_OP_DSUBP || op == PLC_OP_DMULP ||
                     op == PLC_OP_DDIVP || op == PLC_OP_DINCP || op == PLC_OP_DDECP ||
                     op == PLC_OP_DROLP || op == PLC_OP_DRORP || op == PLC_OP_DSHLP ||
                     op == PLC_OP_DSHRP || op == PLC_OP_DSALP || op == PLC_OP_DSARP ||
                     op == PLC_OP_SQRP || op == PLC_OP_DMOVP || op == PLC_OP_DMOVW);

    /* 脉冲指令执行条件：上一扫描周期 acc=0 且本周期条件成立
     * 这里简化：脉冲指令在 acc=1 时执行，执行后自动清 acc=0 (模拟上升沿)
     * 实际 PLC 中脉冲指令配合上升沿检测指令(MEP/MEF)使用 */
    if (is_pulse && ctx->acc == 0) {
        return PLC_ERR_OK;  /* 不执行，相当于跳过 */
    }

    switch (op) {
        case PLC_OP_NOP:
            break;

        case PLC_OP_END:
            ctx->pc = ctx->program_len;  /* 结束扫描 */
            break;

        case PLC_OP_RET:
            if (ctx->call_depth == 0) return PLC_ERR_UNDERFLOW;
            ctx->pc = ctx->call_stack[--ctx->call_depth];
            break;

        /* --- 位逻辑 --- */
        case PLC_OP_LD:   ctx->acc = read_operand_bit(ctx, op1); break;
        case PLC_OP_LDI:  ctx->acc = !read_operand_bit(ctx, op1); break;
        case PLC_OP_AND:  ctx->acc = ctx->acc && read_operand_bit(ctx, op1); break;
        case PLC_OP_ANI:  ctx->acc = ctx->acc && !read_operand_bit(ctx, op1); break;
        case PLC_OP_OR:   ctx->acc = ctx->acc || read_operand_bit(ctx, op1); break;
        case PLC_OP_ORI:  ctx->acc = ctx->acc || !read_operand_bit(ctx, op1); break;
        case PLC_OP_OUT:  write_operand_bit(ctx, op1, ctx->acc); break;
        case PLC_OP_OUTI: write_operand_bit(ctx, op1, !ctx->acc); break;
        case PLC_OP_SET:  if (ctx->acc) write_operand_bit(ctx, op1, true); break;
        case PLC_OP_RST:  if (ctx->acc) write_operand_bit(ctx, op1, false); break;

        case PLC_OP_PLS: {
            bool cur = read_operand_bit(ctx, op1);
            bool prev = (ctx->prev_bit_state[PLC_BIT_ARR_IDX(PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1)) >> 3] >> (PLC_BIT_ARR_IDX(PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1)) & 7)) & 1;
            write_operand_bit(ctx, op1, cur && !prev);
            break;
        }
        case PLC_OP_PLF: {
            bool cur = read_operand_bit(ctx, op1);
            bool prev = (ctx->prev_bit_state[PLC_BIT_ARR_IDX(PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1)) >> 3] >> (PLC_BIT_ARR_IDX(PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1)) & 7)) & 1;
            write_operand_bit(ctx, op1, !cur && prev);
            break;
        }
        case PLC_OP_INV:  ctx->acc = !ctx->acc; break;
        case PLC_OP_MEP: {
            bool cur = ctx->acc;
            bool prev = ctx->carry;  /* 用 carry 存上一周期 acc */
            ctx->acc = cur && !prev;
            ctx->carry = cur;
            break;
        }
        case PLC_OP_MEF: {
            bool cur = ctx->acc;
            bool prev = ctx->carry;
            ctx->acc = !cur && prev;
            ctx->carry = cur;
            break;
        }

        /* --- 定时器 --- */
        case PLC_OP_TMR:
        case PLC_OP_TMR10:
        case PLC_OP_TMR1: {
            uint16_t t_idx = PLC_DECODE_INDEX(op1);
            if (t_idx >= PLC_T_MAX) return PLC_ERR_INVALID_ADDR;
            int16_t preset = (int16_t)PLC_DECODE_INDEX(op2);
            ctx->timer_pre[t_idx] = preset;
            bool en = read_operand_bit(ctx, op1);
            if (en) {
                uint32_t tick_us = (op == PLC_OP_TMR1) ? 1000 : (op == PLC_OP_TMR10) ? 10000 : 100000;
                /* 使用配置周期作为基准，避免 scan_cycle_us=0 导致除零 */
                uint32_t base_cycle_us = (ctx->scan_cycle_us > 0) ? ctx->scan_cycle_us : ctx->scan_period_us;
                uint32_t inc = base_cycle_us / tick_us;
                if (inc == 0) inc = 1;  /* 至少增 1 */
                if (ctx->timer_cur[t_idx] < preset) {
                    ctx->timer_cur[t_idx] += inc;
                    if (ctx->timer_cur[t_idx] >= preset) ctx->timer_cur[t_idx] = preset;
                }
            } else {
                ctx->timer_cur[t_idx] = 0;
            }
            write_operand_bit(ctx, op1, ctx->timer_cur[t_idx] >= preset);
            break;
        }

        /* --- 计数器 --- */
        case PLC_OP_CNT:
        case PLC_OP_CNT32: {
            uint16_t c_idx = PLC_DECODE_INDEX(op1);
            if (c_idx >= PLC_C_MAX) return PLC_ERR_INVALID_ADDR;
            int16_t preset = (int16_t)PLC_DECODE_INDEX(op2);
            ctx->counter_pre[c_idx] = preset;
            bool en = read_operand_bit(ctx, op1);
            static bool cnt_prev[PLC_C_MAX] = {0};
            if (en && !cnt_prev[c_idx]) {
                if (ctx->counter_dir[c_idx] != 1) ctx->counter_cur[c_idx]++;
            }
            cnt_prev[c_idx] = en;
            write_operand_bit(ctx, op1, ctx->counter_cur[c_idx] >= preset);
            break;
        }
        case PLC_OP_DCNT: {
            /* 双向计数器：需要配合 U/D 指令，简化版只做 Up */
            uint16_t c_idx = PLC_DECODE_INDEX(op1);
            if (c_idx >= PLC_C_MAX) return PLC_ERR_INVALID_ADDR;
            int16_t preset = (int16_t)PLC_DECODE_INDEX(op2);
            ctx->counter_pre[c_idx] = preset;
            ctx->counter_dir[c_idx] = 0; /* Up */
            bool en = read_operand_bit(ctx, op1);
            static bool dcnt_prev[PLC_C_MAX] = {0};
            if (en && !dcnt_prev[c_idx]) ctx->counter_cur[c_idx]++;
            dcnt_prev[c_idx] = en;
            write_operand_bit(ctx, op1, ctx->counter_cur[c_idx] >= preset);
            break;
        }

        /* --- 比较 (16-bit) --- */
        case PLC_OP_CMP:
        case PLC_OP_CMPP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            ctx->zero = (v1 == v2);
            ctx->carry = (v1 > v2);  /* carry 用作 > 标志 */
            /* M8000=>, M8001== , M8002=< 简化：用 acc/carry/zero 表示 */
            break;
        }
        case PLC_OP_ZCP: {
            int16_t v1 = read_operand_word(ctx, op1);  /* 下限 */
            int16_t v2 = read_operand_word(ctx, op2);  /* 上限 */
            int16_t v3 = read_operand_word(ctx, op3);  /* 比较值 */
            ctx->acc = (v3 >= v1 && v3 <= v2);
            break;
        }

        /* --- 比较 (32-bit) --- */
        case PLC_OP_DCMP:
        case PLC_OP_DCMPP: {
            int32_t v1 = read_operand_dword(ctx, op1, op2);
            int32_t v2 = read_operand_dword(ctx, op3, 0);  /* 简化：op3 只用低 16 位 */
            ctx->zero = (v1 == v2);
            ctx->carry = (v1 > v2);
            break;
        }
        case PLC_OP_DZCP: {
            int32_t v1 = read_operand_dword(ctx, op1, 0);
            int32_t v2 = read_operand_dword(ctx, op2, 0);
            int32_t v3 = read_operand_dword(ctx, op3, 0);
            ctx->acc = (v3 >= v1 && v3 <= v2);
            break;
        }

        /* --- 数据传送 (16-bit) --- */
        case PLC_OP_MOV:
        case PLC_OP_MOVP: {
            int16_t val = read_operand_word(ctx, op1);
            write_operand_word(ctx, op2, val);
            break;
        }
        case PLC_OP_MOVW:
        case PLC_OP_BMOV: {
            int16_t count = read_operand_word(ctx, op2);
            for (int16_t i = 0; i < count; i++) {
                int16_t val = read_operand_word(ctx, PLC_ENCODE_OPERAND(PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1) + i));
                write_operand_word(ctx, PLC_ENCODE_OPERAND(PLC_DECODE_AREA(op3), PLC_DECODE_INDEX(op3) + i), val);
            }
            break;
        }
        case PLC_OP_FMOV: {
            int16_t val = read_operand_word(ctx, op1);
            int16_t count = read_operand_word(ctx, op2);
            for (int16_t i = 0; i < count; i++) {
                write_operand_word(ctx, PLC_ENCODE_OPERAND(PLC_DECODE_AREA(op3), PLC_DECODE_INDEX(op3) + i), val);
            }
            break;
        }
        case PLC_OP_XCH: {
            int16_t *p1 = word_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            int16_t *p2 = word_ptr(ctx, PLC_DECODE_AREA(op2), PLC_DECODE_INDEX(op2));
            if (p1 && p2) { int16_t t = *p1; *p1 = *p2; *p2 = t; }
            break;
        }
        case PLC_OP_SWAP: {
            int16_t *p = word_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            if (p) *p = ((*p >> 8) & 0xFF) | ((*p << 8) & 0xFF00);
            break;
        }

        /* --- 数据传送 (32-bit) --- */
        case PLC_OP_DMOV:
        case PLC_OP_DMOVP: {
            int32_t val = read_operand_dword(ctx, op1, op2);
            write_operand_dword(ctx, op3, 0, val);
            break;
        }

        /* --- 四则运算 (16-bit) --- */
        case PLC_OP_ADD:
        case PLC_OP_ADDP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            int32_t res = (int32_t)v1 + v2;
            ctx->carry = (res > 32767 || res < -32768);
            write_operand_word(ctx, op3, (int16_t)res);
            break;
        }
        case PLC_OP_SUB:
        case PLC_OP_SUBP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            int32_t res = (int32_t)v1 - v2;
            ctx->carry = (res > 32767 || res < -32768);
            write_operand_word(ctx, op3, (int16_t)res);
            break;
        }
        case PLC_OP_MUL:
        case PLC_OP_MULP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            int32_t res = (int32_t)v1 * v2;
            write_operand_dword(ctx, op3, 0, res);  /* 结果占 2 个寄存器 */
            break;
        }
        case PLC_OP_DIV:
        case PLC_OP_DIVP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            if (v2 == 0) return PLC_ERR_DIV_ZERO;
            write_operand_word(ctx, op3, v1 / v2);
            write_operand_word(ctx, PLC_ENCODE_OPERAND(PLC_DECODE_AREA(op3), PLC_DECODE_INDEX(op3) + 1), v1 % v2);
            break;
        }
        case PLC_OP_INC:
        case PLC_OP_INCP: {
            int16_t *p = word_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            if (p) { (*p)++; ctx->zero = (*p == 0); ctx->carry = (*p == -32768); }
            break;
        }
        case PLC_OP_DEC:
        case PLC_OP_DECP: {
            int16_t *p = word_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            if (p) { (*p)--; ctx->zero = (*p == 0); ctx->carry = (*p == 32767); }
            break;
        }

        /* --- 四则运算 (32-bit) 简化实现 --- */
        case PLC_OP_DADD:
        case PLC_OP_DADDP: {
            int32_t v1 = read_operand_dword(ctx, op1, op2);
            int32_t v2 = read_operand_dword(ctx, op3, 0);
            int64_t res = (int64_t)v1 + v2;
            ctx->carry = (res > 0x7FFFFFFF || res < -0x80000000);
            write_operand_dword(ctx, op3, 0, (int32_t)res);
            break;
        }
        case PLC_OP_DSUB:
        case PLC_OP_DSUBP: {
            int32_t v1 = read_operand_dword(ctx, op1, op2);
            int32_t v2 = read_operand_dword(ctx, op3, 0);
            int64_t res = (int64_t)v1 - v2;
            ctx->carry = (res > 0x7FFFFFFF || res < -0x80000000);
            write_operand_dword(ctx, op3, 0, (int32_t)res);
            break;
        }
        case PLC_OP_DMUL:
        case PLC_OP_DMULP: {
            int32_t v1 = read_operand_dword(ctx, op1, op2);
            int32_t v2 = read_operand_dword(ctx, op3, 0);
            int64_t res = (int64_t)v1 * v2;
            /* 结果存 64-bit 简化存低 32 位 */
            write_operand_dword(ctx, op3, 0, (int32_t)res);
            break;
        }
        case PLC_OP_DDIV:
        case PLC_OP_DDIVP: {
            int32_t v1 = read_operand_dword(ctx, op1, op2);
            int32_t v2 = read_operand_dword(ctx, op3, 0);
            if (v2 == 0) return PLC_ERR_DIV_ZERO;
            write_operand_dword(ctx, op3, 0, v1 / v2);
            /* 余数存下一组寄存器，简化忽略 */
            break;
        }
        case PLC_OP_DINC:
        case PLC_OP_DINCP: {
            int32_t *p = dword_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            if (p) { (*p)++; ctx->zero = (*p == 0); }
            break;
        }
        case PLC_OP_DDEC:
        case PLC_OP_DDECP: {
            int32_t *p = dword_ptr(ctx, PLC_DECODE_AREA(op1), PLC_DECODE_INDEX(op1));
            if (p) { (*p)--; ctx->zero = (*p == 0); }
            break;
        }

        /* --- 逻辑运算 --- */
        case PLC_OP_WAND:
        case PLC_OP_WANDP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            write_operand_word(ctx, op3, v1 & v2);
            break;
        }
        case PLC_OP_WOR:
        case PLC_OP_WORP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            write_operand_word(ctx, op3, v1 | v2);
            break;
        }
        case PLC_OP_WXOR:
        case PLC_OP_WXORP: {
            int16_t v1 = read_operand_word(ctx, op1);
            int16_t v2 = read_operand_word(ctx, op2);
            write_operand_word(ctx, op3, v1 ^ v2);
            break;
        }
        case PLC_OP_NEG:
        case PLC_OP_NEGP: {
            int16_t v = read_operand_word(ctx, op1);
            write_operand_word(ctx, op1, -v);
            break;
        }

        /* --- 移位/旋转 (16-bit) 简化 --- */
        case PLC_OP_ROL:
        case PLC_OP_ROLP: {
            int16_t v = read_operand_word(ctx, op1);
            int n = PLC_DECODE_INDEX(op2) & 0xF;
            int16_t res = (v << n) | ((v >> (16 - n)) & ((1 << n) - 1));
            write_operand_word(ctx, op1, res);
            break;
        }
        case PLC_OP_ROR:
        case PLC_OP_RORP: {
            int16_t v = read_operand_word(ctx, op1);
            int n = PLC_DECODE_INDEX(op2) & 0xF;
            int16_t res = (v >> n) | ((v << (16 - n)) & 0xFFFF);
            write_operand_word(ctx, op1, res);
            break;
        }
        case PLC_OP_SHL:
        case PLC_OP_SHLP: {
            int16_t v = read_operand_word(ctx, op1);
            int n = PLC_DECODE_INDEX(op2) & 0xF;
            write_operand_word(ctx, op1, v << n);
            break;
        }
        case PLC_OP_SHR:
        case PLC_OP_SHRP: {
            int16_t v = read_operand_word(ctx, op1);
            int n = PLC_DECODE_INDEX(op2) & 0xF;
            write_operand_word(ctx, op1, (uint16_t)v >> n);
            break;
        }
        case PLC_OP_SAR:
        case PLC_OP_SARP: {
            int16_t v = read_operand_word(ctx, op1);
            int n = PLC_DECODE_INDEX(op2) & 0xF;
            write_operand_word(ctx, op1, v >> n);  /* 算术右移保持符号 */
            break;
        }

        /* --- 跳转/分支 --- */
        case PLC_OP_JMP: {
            int16_t target = find_label_pc(ctx, PLC_DECODE_INDEX(op1));
            if (target < 0) return PLC_ERR_LABEL_NOT_FOUND;
            ctx->pc = target;
            return PLC_ERR_OK;  /* 不自增 PC */
        }
        case PLC_OP_JMPC: {
            if (ctx->acc) {
                int16_t target = find_label_pc(ctx, PLC_DECODE_INDEX(op1));
                if (target < 0) return PLC_ERR_LABEL_NOT_FOUND;
                ctx->pc = target;
                return PLC_ERR_OK;
            }
            break;
        }
        case PLC_OP_JMPNC: {
            if (!ctx->acc) {
                int16_t target = find_label_pc(ctx, PLC_DECODE_INDEX(op1));
                if (target < 0) return PLC_ERR_LABEL_NOT_FOUND;
                ctx->pc = target;
                return PLC_ERR_OK;
            }
            break;
        }
        case PLC_OP_CALL: {
            if (ctx->call_depth >= 16) return PLC_ERR_STACK_DEPTH;
            ctx->call_stack[ctx->call_depth++] = ctx->pc + 1;
            int16_t target = find_label_pc(ctx, PLC_DECODE_INDEX(op1));
            if (target < 0) return PLC_ERR_LABEL_NOT_FOUND;
            ctx->pc = target;
            return PLC_ERR_OK;
        }
        case PLC_OP_FOR: {
            if (ctx->loop_depth >= 8) return PLC_ERR_STACK_DEPTH;
            int16_t count = read_operand_word(ctx, op1);
            ctx->loop_stack[ctx->loop_depth].start_pc = ctx->pc + 1;
            ctx->loop_stack[ctx->loop_depth].counter_reg = PLC_DECODE_INDEX(op1);
            ctx->loop_stack[ctx->loop_depth].remaining = count;
            ctx->loop_depth++;
            break;
        }
        case PLC_OP_NEXT: {
            if (ctx->loop_depth == 0) break;
            ctx->loop_depth--;
            ctx->loop_stack[ctx->loop_depth].remaining--;
            if (ctx->loop_stack[ctx->loop_depth].remaining > 0) {
                ctx->pc = ctx->loop_stack[ctx->loop_depth].start_pc;
                ctx->loop_depth++;  /* 恢复深度 */
                return PLC_ERR_OK;
            }
            break;
        }
        case PLC_OP_BREAK: {
            if (ctx->loop_depth > 0) {
                ctx->loop_depth--;
                /* 跳转到 NEXT 后，这里简化：需要预知 NEXT 位置，实际应扫描查找 */
            }
            break;
        }

        /* --- 高级指令简化实现 --- */
        case PLC_OP_BCD:
        case PLC_OP_BCDP: {
            int16_t v = read_operand_word(ctx, op1);
            int16_t res = ((v / 1000) << 12) | (((v / 100) % 10) << 8) |
                          (((v / 10) % 10) << 4) | (v % 10);
            write_operand_word(ctx, op2, res);
            break;
        }
        case PLC_OP_BIN:
        case PLC_OP_BINP: {
            int16_t v = read_operand_word(ctx, op1);
            int16_t res = ((v >> 12) & 0xF) * 1000 + ((v >> 8) & 0xF) * 100 +
                          ((v >> 4) & 0xF) * 10 + (v & 0xF);
            write_operand_word(ctx, op2, res);
            break;
        }

        /* PID 简化：只做比例项 */
        case PLC_OP_PID: {
            int16_t sv = read_operand_word(ctx, op1);
            int16_t pv = read_operand_word(ctx, op2);
            int16_t kp = read_operand_word(ctx, PLC_ENCODE_OPERAND(PLC_AREA_D, PLC_DECODE_INDEX(op3) + 1));
            int32_t err = sv - pv;
            int32_t out = (err * kp) / 100;  /* Kp 放大 100 倍 */
            if (out > 32767) out = 32767;
            if (out < -32768) out = -32768;
            write_operand_word(ctx, op3, (int16_t)out);
            break;
        }

        case PLC_OP_LABEL:
            /* 伪指令：标号定义，运行时不做任何操作，相当于 NOP */
            break;

        default:
            return PLC_ERR_INVALID_OP;
    }

    return PLC_ERR_OK;
}

int plc_scan(plc_ctx_t *ctx) {
    if (!ctx || !(ctx->config_flags & PLC_CFG_RUN) || (ctx->config_flags & PLC_CFG_PAUSE))
        return 0;

    uint32_t scan_start = hal_systick_get_tick();  /* 单位：ms */

    int executed = 0;
    ctx->pc = 0;
    ctx->acc = 1;  /* 每扫描周期开始，累加器初始为 1 (LD 从 1 开始) */
    ctx->error = 0;

    /* 首次扫描时，用配置周期作为预估值，避免除零 */
    uint32_t est_cycle_us = (ctx->scan_cycle_us > 0) ? ctx->scan_cycle_us : ctx->scan_period_us;

    while (ctx->pc < ctx->program_len) {
        const plc_inst_t *inst = &ctx->program[ctx->pc];
        plc_err_t err = exec_inst(ctx, inst);
        if (err != PLC_ERR_OK) {
            ctx->error = 1;
            ctx->last_error = err;
            ctx->error_pc = ctx->pc;
            if (ctx->config_flags & PLC_CFG_HALT_ON_ERR) {
                ctx->config_flags &= ~PLC_CFG_RUN;
            }
            break;
        }
        ctx->pc++;
        executed++;

        /* 看门狗检查（使用 ms 单位，避免乘法溢出） */
        if ((ctx->config_flags & PLC_CFG_WDT_EN) &&
            (hal_systick_get_tick() - scan_start) > (ctx->watchdog_us / 1000)) {
            ctx->last_error = PLC_ERR_WDT_TIMEOUT;
            ctx->error_pc = ctx->pc;
            ctx->config_flags &= ~PLC_CFG_RUN;
            break;
        }
    }

    uint32_t scan_end = hal_systick_get_tick();
    ctx->scan_cycle_us = (scan_end - scan_start) * 1000;  /* ms -> us */
    if (ctx->scan_cycle_us == 0) ctx->scan_cycle_us = ctx->scan_period_us;  /* 首次保护 */

    if (ctx->scan_cycle_us > ctx->scan_max_us) ctx->scan_max_us = ctx->scan_cycle_us;
    if (ctx->scan_cycle_us < ctx->scan_min_us) ctx->scan_min_us = ctx->scan_cycle_us;
    ctx->scan_total_cycles++;

    /* 更新特殊寄存器 */
    int16_t *v_scan_max = word_ptr(ctx, PLC_AREA_V, PLC_V_SCAN_MAX);
    int16_t *v_scan_cur = word_ptr(ctx, PLC_AREA_V, PLC_V_SCAN_CUR);
    int16_t *v_scan_min = word_ptr(ctx, PLC_AREA_V, PLC_V_SCAN_MIN);
    int16_t *v_cycle_cnt = word_ptr(ctx, PLC_AREA_V, PLC_V_CYCLE_CNT);
    if (v_scan_max) *v_scan_max = ctx->scan_max_us / 1000;
    if (v_scan_cur) *v_scan_cur = ctx->scan_cycle_us / 1000;
    if (v_scan_min) *v_scan_min = ctx->scan_min_us / 1000;
    if (v_cycle_cnt) *v_cycle_cnt = (int16_t)(ctx->scan_total_cycles & 0xFFFF);

    return executed;
}

void plc_run(plc_ctx_t *ctx) { if (ctx) ctx->config_flags |= PLC_CFG_RUN; }
void plc_stop(plc_ctx_t *ctx) { if (ctx) ctx->config_flags &= ~PLC_CFG_RUN; }
void plc_pause(plc_ctx_t *ctx) { if (ctx) ctx->config_flags |= PLC_CFG_PAUSE; }
bool plc_is_running(const plc_ctx_t *ctx) { return ctx && (ctx->config_flags & PLC_CFG_RUN) && !(ctx->config_flags & PLC_CFG_PAUSE); }

plc_err_t plc_single_step(plc_ctx_t *ctx) {
    if (!ctx || !(ctx->config_flags & PLC_CFG_PAUSE)) return PLC_ERR_INVALID_OP;
    if (ctx->pc >= ctx->program_len) return PLC_ERR_OK;
    plc_err_t err = exec_inst(ctx, &ctx->program[ctx->pc]);
    ctx->pc++;
    return err;
}

void plc_reset(plc_ctx_t *ctx) {
    if (!ctx) return;
    ctx->pc = 0;
    ctx->sp = 0;
    ctx->acc = 1;
    ctx->carry = 0;
    ctx->zero = 0;
    ctx->error = 0;
    ctx->last_error = PLC_ERR_OK;
    ctx->call_depth = 0;
    ctx->loop_depth = 0;
    memset(ctx->timer_cur, 0, sizeof(ctx->timer_cur));
    memset(ctx->counter_cur, 0, sizeof(ctx->counter_cur));
    /* 非保持区清零：X, Y, M, T(位), C(位), S, D, V(部分) */
    memset(ctx->prev_bit_state, 0, PLC_X_MAX + PLC_Y_MAX + PLC_M_MAX + PLC_T_MAX + PLC_C_MAX + PLC_S_MAX >> 3);
    /* D/V/R 保留或按配置清零 */
    if (!(ctx->config_flags & PLC_CFG_RETAIN_EN)) {
        static int16_t word_storage[PLC_D_MAX + PLC_V_MAX + PLC_R_MAX];
        memset(word_storage, 0, sizeof(word_storage));
    }
    refresh_label_table(ctx);
}

void plc_set_bit(plc_ctx_t *ctx, plc_area_t area, uint16_t index, bool value) {
    if (ctx) set_bit_raw(ctx, area, index, value);
}

bool plc_get_bit(const plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    if (!ctx) return false;
    uint16_t bit_idx = PLC_BIT_ARR_IDX(area, index);
    return (ctx->prev_bit_state[bit_idx >> 3] >> (bit_idx & 7)) & 1;
}

void plc_set_word(plc_ctx_t *ctx, plc_area_t area, uint16_t index, int16_t value) {
    if (ctx) write_operand_word(ctx, PLC_ENCODE_OPERAND(area, index), value);
}

void plc_set_dword(plc_ctx_t *ctx, plc_area_t area, uint16_t index, int32_t value) {
    if (ctx) write_operand_dword(ctx, PLC_ENCODE_OPERAND(area, index), 0, value);
}

int16_t plc_get_word(const plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    if (!ctx) return 0;
    int16_t *ptr = word_ptr((plc_ctx_t*)ctx, area, index);  /* const cast 仅读 */
    return ptr ? *ptr : 0;
}

int32_t plc_get_dword(const plc_ctx_t *ctx, plc_area_t area, uint16_t index) {
    if (!ctx) return 0;
    int32_t *ptr = dword_ptr((plc_ctx_t*)ctx, area, index);
    return ptr ? *ptr : 0;
}

void plc_set_scan_period(plc_ctx_t *ctx, uint32_t period_us) {
    if (ctx) ctx->scan_period_us = period_us;
}

void plc_set_watchdog(plc_ctx_t *ctx, uint32_t timeout_us) {
    if (ctx) ctx->watchdog_us = timeout_us;
}

void plc_get_stats(const plc_ctx_t *ctx, uint32_t *cur_us, uint32_t *max_us, uint32_t *min_us, uint32_t *total) {
    if (!ctx) return;
    if (cur_us) *cur_us = ctx->scan_cycle_us;
    if (max_us) *max_us = ctx->scan_max_us;
    if (min_us) *min_us = (ctx->scan_min_us == 0xFFFFFFFF) ? 0 : ctx->scan_min_us;
    if (total) *total = ctx->scan_total_cycles;
}

/* 反汇编打印辅助 */
static void put_hex16(void (*putc_fn)(char), uint16_t v) {
    const char hex[] = "0123456789ABCDEF";
    putc_fn(hex[(v >> 12) & 0xF]);
    putc_fn(hex[(v >> 8) & 0xF]);
    putc_fn(hex[(v >> 4) & 0xF]);
    putc_fn(hex[v & 0xF]);
}

static void put_dec(void (*putc_fn)(char), int v) {
    if (v < 0) { putc_fn('-'); v = -v; }
    char buf[16]; int i = 0;
    if (v == 0) buf[i++] = '0';
    else while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) putc_fn(buf[i]);
}

static const char* operand_str(uint16_t op, char *buf, size_t sz) {
    plc_area_t area = PLC_DECODE_AREA(op);
    uint16_t idx = PLC_DECODE_INDEX(op);
    const char *pref = (area == PLC_AREA_X) ? "X" :
                       (area == PLC_AREA_Y) ? "Y" :
                       (area == PLC_AREA_M) ? "M" :
                       (area == PLC_AREA_T) ? "T" :
                       (area == PLC_AREA_C) ? "C" :
                       (area == PLC_AREA_S) ? "S" :
                       (area == PLC_AREA_D) ? "D" :
                       (area == PLC_AREA_V) ? "V" :
                       (area == PLC_AREA_R) ? "R" :
                       (area == PLC_AREA_K) ? "K" : "?";
    snprintf(buf, sz, "%s%u", pref, idx);
    return buf;
}

void plc_disasm(const plc_ctx_t *ctx, void (*putc_fn)(char), void (*puts_fn)(const char*)) {
    if (!ctx || !ctx->program) return;
    char buf[64];
    for (uint16_t pc = 0; pc < ctx->program_len; pc++) {
        const plc_inst_t *inst = &ctx->program[pc];
        const plc_inst_def_t *def = NULL;
        for (uint16_t i = 0; i < plc_inst_table_size; i++) {
            if (plc_inst_table[i].opcode == inst->opcode) { def = &plc_inst_table[i]; break; }
        }
        putc_fn('['); put_hex16(putc_fn, pc); puts_fn("] ");
        if (def) puts_fn(def->mnemonic);
        else { puts_fn("UNK_"); put_hex16(putc_fn, inst->opcode); }

        if (inst->op1 != 0xFFFF) {
            putc_fn(' ');
            operand_str(inst->op1, buf, sizeof(buf));
            puts_fn(buf);
        }
        if (inst->op2 != 0xFFFF) {
            puts_fn(", ");
            operand_str(inst->op2, buf, sizeof(buf));
            puts_fn(buf);
        }
        if (inst->op3 != 0xFFFF) {
            puts_fn(", ");
            operand_str(inst->op3, buf, sizeof(buf));
            puts_fn(buf);
        }
        puts_fn("\r\n");
    }
}

/* ================================================================
 * 简易汇编器实现
 * ================================================================ */

static int parse_operand(const char *tok, plc_area_t *out_area, uint16_t *out_idx, bool *is_const) {
    if (!tok) return -1;
    if (tok[0] == 'K' || tok[0] == 'k') {
        *is_const = true;
        *out_area = PLC_AREA_K;
        *out_idx = (uint16_t)atoi(tok + 1);
        return 0;
    }
    *is_const = false;
    char type = tok[0];
    if (type == 'X' || type == 'x') *out_area = PLC_AREA_X;
    else if (type == 'Y' || type == 'y') *out_area = PLC_AREA_Y;
    else if (type == 'M' || type == 'm') *out_area = PLC_AREA_M;
    else if (type == 'T' || type == 't') *out_area = PLC_AREA_T;
    else if (type == 'C' || type == 'c') *out_area = PLC_AREA_C;
    else if (type == 'S' || type == 's') *out_area = PLC_AREA_S;
    else if (type == 'D' || type == 'd') *out_area = PLC_AREA_D;
    else if (type == 'V' || type == 'v') *out_area = PLC_AREA_V;
    else if (type == 'R' || type == 'r') *out_area = PLC_AREA_R;
    else return -1;
    *out_idx = (uint16_t)atoi(tok + 1);
    return 0;
}

plc_err_t plc_asm_line(const char *line, plc_inst_t *out_inst, uint16_t *out_len) {
    if (!line || !out_inst) return PLC_ERR_INVALID_ADDR;

    /* 跳过空白和注释 */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == ';' || *line == '\0' || *line == '\n' || *line == '\r') {
        *out_len = 0; return PLC_ERR_OK;
    }

    /* 提取助记符 */
    char mnemonic[16] = {0};
    int mi = 0;
    while (*line && *line != ' ' && *line != '\t' && *line != ',' && mi < 15) {
        mnemonic[mi++] = *line++;
    }
    mnemonic[mi] = 0;

    /* 查找指令定义 */
    const plc_inst_def_t *def = NULL;
    for (uint16_t i = 0; i < plc_inst_table_size; i++) {
        if (strcasecmp(plc_inst_table[i].mnemonic, mnemonic) == 0) {
            def = &plc_inst_table[i]; break;
        }
    }
    if (!def) return PLC_ERR_INVALID_OP;

    out_inst->opcode = def->opcode;
    out_inst->op1 = out_inst->op2 = out_inst->op3 = 0xFFFF;
    *out_len = 1;  /* 基本 1 字 */

    /* 解析操作数 */
    char *tokens[3] = {0};
    int tc = 0;
    while (*line && tc < 3) {
        while (*line == ' ' || *line == '\t' || *line == ',') line++;
        if (!*line) break;
        char tok[32] = {0};
        int ti = 0;
        while (*line && *line != ' ' && *line != '\t' && *line != ',' && *line != ';' && ti < 31) {
            tok[ti++] = *line++;
        }
        tok[ti] = 0;
        if (ti > 0) tokens[tc++] = strdup(tok);
    }

    for (int i = 0; i < tc && i < 3; i++) {
        plc_area_t area; uint16_t idx; bool is_const;
        if (parse_operand(tokens[i], &area, &idx, &is_const) == 0) {
            uint16_t encoded = is_const ? idx : PLC_ENCODE_OPERAND(area, idx);
            if (i == 0) out_inst->op1 = encoded;
            else if (i == 1) out_inst->op2 = encoded;
            else out_inst->op3 = encoded;
        }
        free(tokens[i]);
    }

    /* 32-bit 常数处理：如果是 DMOV 等且操作数是 K 常数 > 16-bit */
    if (def->opcode == PLC_OP_DMOV || def->opcode == PLC_OP_DADD || def->opcode == PLC_OP_DSUB) {
        if (tc >= 1 && tokens[0] && tokens[0][0] == 'K') {
            int32_t val = atol(tokens[0] + 1);
            out_inst->op1 = val & 0xFFFF;
            out_inst->op2 = (val >> 16) & 0xFFFF;
            *out_len = 2;
        }
    }

    return PLC_ERR_OK;
}

plc_err_t plc_asm_program(const char *text, uint8_t *out_buf, size_t max_len, size_t *out_len) {
    if (!text || !out_buf || !out_len) return PLC_ERR_INVALID_ADDR;

    size_t written = 0;
    const char *line = text;
    plc_inst_t inst;

    while (*line) {
        /* 查找行尾 */
        const char *line_end = line;
        while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;

        char line_buf[256];
        size_t line_len = line_end - line;
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, line, line_len);
        line_buf[line_len] = 0;

        uint16_t inst_len;
        plc_err_t err = plc_asm_line(line_buf, &inst, &inst_len);
        if (err != PLC_ERR_OK && err != PLC_ERR_OK) return err;
        if (inst_len > 0) {
            if (written + inst_len * sizeof(plc_inst_t) > max_len) return PLC_ERR_MEMORY_FULL;
            memcpy(out_buf + written, &inst, inst_len * sizeof(plc_inst_t));
            written += inst_len * sizeof(plc_inst_t);
        }

        line = line_end;
        while (*line == '\n' || *line == '\r') line++;
    }

    *out_len = written;
    return PLC_ERR_OK;
}