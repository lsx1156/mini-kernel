/**
 * @file    plc_core.h
 * @brief   PLC 循环指令核心定义（类梯形图指令集 + 扫描周期执行引擎）
 *
 * 设计目标：
 *   - 提供类 PLC 的循环扫描执行模型（输入刷新 → 指令执行 → 输出刷新）
 *   - 指令集覆盖：位逻辑、定时器、计数器、比较、数学、移位、跳转
 *   - 内存映射：X(输入) / Y(输出) / M(内部继电器) / T(定时器) / C(计数器) / D(数据寄存器) / V(特殊寄存器)
 *   - 可从 Shell 动态加载/保存/运行/停止用户程序（存 Flash 或 RAM）
 *   - 扫描周期可配置（默认 10ms），看门狗保护防死循环
 */

#ifndef PLC_CORE_H
#define PLC_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "os_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 1. PLC 内存映射常量
 * ================================================================ */

/* --- 位存储区 (bit 级寻址) --- */
#define PLC_X_MAX       256     /* X0~X255    外部输入 */
#define PLC_Y_MAX       256     /* Y0~Y255    外部输出 */
#define PLC_M_MAX       1024    /* M0~M1023   内部继电器 */
#define PLC_T_MAX       128     /* T0~T127    定时器（位=完成标志） */
#define PLC_C_MAX       128     /* C0~C127    计数器（位=完成标志） */
#define PLC_S_MAX       128     /* S0~S127    状态继电器 (SFC/步进) */

/* --- 字存储区 (16/32-bit 寻址) --- */
#define PLC_D_MAX       1024    /* D0~D1023   数据寄存器 (16-bit) */
#define PLC_D32_MAX     512     /* D0~D511    数据寄存器 (32-bit, 占用 2 个 D) */
#define PLC_V_MAX       64      /* V0~V63     特殊寄存器 (系统状态、扫描周期等) */
#define PLC_R_MAX       1024    /* R0~R1023   扩展数据寄存器 (掉电保持区，可选) */

/* 位存储总字节数 */
#define PLC_BIT_BYTES   ((PLC_X_MAX + PLC_Y_MAX + PLC_M_MAX + PLC_T_MAX + PLC_C_MAX + PLC_S_MAX + 7) / 8)
/* 字存储总字节数 */
#define PLC_WORD_BYTES  ((PLC_D_MAX * 2) + (PLC_V_MAX * 2) + (PLC_R_MAX * 2))

/* 特殊寄存器 V 区映射 */
#define PLC_V_SCAN_MAX     0   /* 最大扫描周期 (ms) */
#define PLC_V_SCAN_CUR     1   /* 当前扫描周期 (ms) */
#define PLC_V_SCAN_MIN     2   /* 最小扫描周期 (ms) */
#define PLC_V_CYCLE_CNT    3   /* 扫描周期计数器 */
#define PLC_V_ERR_CODE     4   /* 最后错误码 */
#define PLC_V_ERR_ADDR     5   /* 错误指令地址 */
#define PLC_V_RTC_YEAR     6   /* RTC 年 */
#define PLC_V_RTC_MONTH    7   /* RTC 月 */
#define PLC_V_RTC_DAY      8   /* RTC 日 */
#define PLC_V_RTC_HOUR     9   /* RTC 时 */
#define PLC_V_RTC_MIN     10   /* RTC 分 */
#define PLC_V_RTC_SEC     11   /* RTC 秒 */
#define PLC_V_RTC_WEEK    12   /* RTC 周 */
#define PLC_V_VER_MAJOR   13   /* PLC 版本主版本 */
#define PLC_V_VER_MINOR   14   /* PLC 版本次版本 */
#define PLC_V_CFG_FLAGS   15   /* 配置标志位 */

/* 配置标志位 (V15) */
#define PLC_CFG_RUN           (1u << 0)  /* 运行标志 */
#define PLC_CFG_PAUSE         (1u << 1)  /* 暂停标志 */
#define PLC_CFG_SINGLE_STEP   (1u << 2)  /* 单步执行 */
#define PLC_CFG_WDT_EN        (1u << 3)  /* 看门狗使能 */
#define PLC_CFG_RETAIN_EN     (1u << 4)  /* 掉电保持使能 */
#define PLC_CFG_HALT_ON_ERR   (1u << 5)  /* 出错停机 */

/* ================================================================
 * 2. 指令操作码
 * ================================================================ */

typedef enum {
    /* --- 空指令/程序控制 --- */
    PLC_OP_NOP      = 0x00,
    PLC_OP_END      = 0x01,   /* 程序结束（主程序必须以此结尾） */
    PLC_OP_RET      = 0x02,   /* 子程序返回 */

    /* --- 位逻辑指令 --- */
    PLC_OP_LD       = 0x10,   /* LD  X/Y/M/T/C/S  - 起始常开触点 */
    PLC_OP_LDI      = 0x11,   /* LDI X/Y/M/T/C/S  - 起始常闭触点 */
    PLC_OP_AND      = 0x12,   /* AND X/Y/M/T/C/S  - 串联常开触点 */
    PLC_OP_ANI      = 0x13,   /* ANI X/Y/M/T/C/S  - 串联常闭触点 */
    PLC_OP_OR       = 0x14,   /* OR  X/Y/M/T/C/S  - 并联常开触点 */
    PLC_OP_ORI      = 0x15,   /* ORI X/Y/M/T/C/S  - 并联常闭触点 */
    PLC_OP_OUT      = 0x16,   /* OUT Y/M/S        - 线圈输出 */
    PLC_OP_OUTI     = 0x17,   /* OUTI Y/M/S       - 反相线圈输出 */
    PLC_OP_SET      = 0x18,   /* SET Y/M/S        - 置位保持 */
    PLC_OP_RST      = 0x19,   /* RST Y/M/S/T/C    - 复位 */
    PLC_OP_PLS      = 0x1A,   /* PLS M            - 上升沿脉冲 (1 扫描周期) */
    PLC_OP_PLF      = 0x1B,   /* PLF M            - 下降沿脉冲 */
    PLC_OP_INV      = 0x1C,   /* INV              - 逻辑反相 (累加器取反) */
    PLC_OP_MEP      = 0x1D,   /* MEP              - 上升沿检测 (结果存 MEP/M8000) */
    PLC_OP_MEF      = 0x1E,   /* MEF              - 下降沿检测 */

    /* --- 定时器/计数器 --- */
    PLC_OP_TMR      = 0x20,   /* TMR Tn K         - 100ms 定时器 (K=设定值 0~32767) */
    PLC_OP_TMR10    = 0x21,   /* TMR10 Tn K       - 10ms 定时器 */
    PLC_OP_TMR1     = 0x22,   /* TMR1  Tn K       - 1ms 定时器 */
    PLC_OP_CNT      = 0x23,   /* CNT Cn K         - 16位计数器 (K=设定值) */
    PLC_OP_CNT32    = 0x24,   /* CNT32 Cn K       - 32位计数器 */
    PLC_OP_DCNT     = 0x25,   /* DCNT Cn K        - 双向计数器 (需配合 U/D 指令) */

    /* --- 比较指令 (16-bit) --- */
    PLC_OP_CMP      = 0x30,   /* CMP  Dn1 Dn2     - 比较 (结果存 M8000~M8002: < = >) */
    PLC_OP_CMPP     = 0x31,   /* CMPP Dn1 Dn2     - 脉冲执行比较 */
    PLC_OP_ZCP      = 0x32,   /* ZCP  Dn1 Dn2 Dn3 - 区间比较 (Dn1<=Dn3<=Dn2) */

    /* --- 比较指令 (32-bit) --- */
    PLC_OP_DCMP     = 0x33,   /* DCMP Dn1 Dn2     - 32位比较 */
    PLC_OP_DCMPP    = 0x34,   /* DCMPP Dn1 Dn2    - 32位脉冲比较 */
    PLC_OP_DZCP     = 0x35,   /* DZCP Dn1 Dn2 Dn3 - 32位区间比较 */

    /* --- 数据传送 (16-bit) --- */
    PLC_OP_MOV      = 0x40,   /* MOV  Dn1 Dn2     - 传送 */
    PLC_OP_MOVP     = 0x41,   /* MOVP Dn1 Dn2     - 脉冲传送 */
    PLC_OP_MOVW     = 0x42,   /* MOVW Dn1 Dn2 Dn3 - 批量传送 (Dn2 个从 Dn1 到 Dn3) */
    PLC_OP_BMOV     = 0x43,   /* BMOV Dn1 Dn2 Dn3 - 块传送 (同 MOVW) */
    PLC_OP_FMOV     = 0x44,   /* FMOV Dn1 Dn2 Dn3 - 填充传送 (Dn1 值填充 Dn2 个到 Dn3) */
    PLC_OP_XCH      = 0x45,   /* XCH  Dn1 Dn2     - 交换 */
    PLC_OP_SWAP     = 0x46,   /* SWAP Dn          - 高低字节交换 */

    /* --- 数据传送 (32-bit) --- */
    PLC_OP_DMOV     = 0x47,   /* DMOV Dn1 Dn2     - 32位传送 */
    PLC_OP_DMOVP    = 0x48,   /* DMOVP Dn1 Dn2    - 32位脉冲传送 */
    PLC_OP_DMOVW    = 0x49,   /* DMOVW Dn1 Dn2 Dn3 - 32位批量传送 */

    /* --- 四则运算 (16-bit) --- */
    PLC_OP_ADD      = 0x50,   /* ADD  Dn1 Dn2 Dn3 - Dn3 = Dn1 + Dn2 */
    PLC_OP_ADDP     = 0x51,   /* ADDP Dn1 Dn2 Dn3 - 脉冲加法 */
    PLC_OP_SUB      = 0x52,   /* SUB  Dn1 Dn2 Dn3 - Dn3 = Dn1 - Dn2 */
    PLC_OP_SUBP     = 0x53,   /* SUBP Dn1 Dn2 Dn3 - 脉冲减法 */
    PLC_OP_MUL      = 0x54,   /* MUL  Dn1 Dn2 Dn3 - Dn3 = Dn1 * Dn2 (32-bit 结果) */
    PLC_OP_MULP     = 0x55,   /* MULP Dn1 Dn2 Dn3 - 脉冲乘法 */
    PLC_OP_DIV      = 0x56,   /* DIV  Dn1 Dn2 Dn3 - Dn3 = Dn1 / Dn2, Dn3+1 = 余数 */
    PLC_OP_DIVP     = 0x57,   /* DIVP Dn1 Dn2 Dn3 - 脉冲除法 */
    PLC_OP_INC      = 0x58,   /* INC  Dn          - 加 1 */
    PLC_OP_INCP     = 0x59,   /* INCP Dn          - 脉冲加 1 */
    PLC_OP_DEC      = 0x5A,   /* DEC  Dn          - 减 1 */
    PLC_OP_DECP     = 0x5B,   /* DECP Dn          - 脉冲减 1 */

    /* --- 四则运算 (32-bit) --- */
    PLC_OP_DADD     = 0x5C,   /* DADD Dn1 Dn2 Dn3 - 32位加法 */
    PLC_OP_DADDP    = 0x5D,   /* DADDP Dn1 Dn2 Dn3 */
    PLC_OP_DSUB     = 0x5E,   /* DSUB Dn1 Dn2 Dn3 - 32位减法 */
    PLC_OP_DSUBP    = 0x5F,   /* DSUBP Dn1 Dn2 Dn3 */
    PLC_OP_DMUL     = 0x60,   /* DMUL Dn1 Dn2 Dn3 - 32位乘法 (64-bit 结果) */
    PLC_OP_DMULP    = 0x61,   /* DMULP Dn1 Dn2 Dn3 */
    PLC_OP_DDIV     = 0x62,   /* DDIV Dn1 Dn2 Dn3 - 32位除法 */
    PLC_OP_DDIVP    = 0x63,   /* DDIVP Dn1 Dn2 Dn3 */
    PLC_OP_DINC     = 0x64,   /* DINC Dn          - 32位加 1 */
    PLC_OP_DINCP    = 0x65,   /* DINCP Dn */
    PLC_OP_DDEC     = 0x66,   /* DDEC Dn          - 32位减 1 */
    PLC_OP_DDECP    = 0x67,   /* DDECP Dn */

    /* --- 逻辑运算 (16-bit) --- */
    PLC_OP_WAND     = 0x70,   /* WAND Dn1 Dn2 Dn3 - 逻辑与 */
    PLC_OP_WANDP    = 0x71,
    PLC_OP_WOR      = 0x72,   /* WOR  Dn1 Dn2 Dn3 - 逻辑或 */
    PLC_OP_WORP     = 0x73,
    PLC_OP_WXOR     = 0x74,   /* WXOR Dn1 Dn2 Dn3 - 逻辑异或 */
    PLC_OP_WXORP    = 0x75,
    PLC_OP_NEG      = 0x76,   /* NEG  Dn          - 取反 (补码) */
    PLC_OP_NEGP     = 0x77,

    /* --- 移位/旋转 (16-bit) --- */
    PLC_OP_ROL      = 0x80,   /* ROL  Dn n        - 左旋转 n 位 */
    PLC_OP_ROLP     = 0x81,
    PLC_OP_ROR      = 0x82,   /* ROR  Dn n        - 右旋转 n 位 */
    PLC_OP_RORP     = 0x83,
    PLC_OP_SHL      = 0x84,   /* SHL  Dn n        - 左移 n 位 (低位补 0) */
    PLC_OP_SHLP     = 0x85,
    PLC_OP_SHR      = 0x86,   /* SHR  Dn n        - 右移 n 位 (高位补 0) */
    PLC_OP_SHRP     = 0x87,
    PLC_OP_SAL      = 0x88,   /* SAL  Dn n        - 算术左移 (同 SHL) */
    PLC_OP_SALP     = 0x89,
    PLC_OP_SAR      = 0x8A,   /* SAR  Dn n        - 算术右移 (保持符号位) */
    PLC_OP_SARP     = 0x8B,

    /* --- 移位/旋转 (32-bit) --- */
    PLC_OP_DROL     = 0x8C,   /* DROL Dn n        - 32位左旋转 */
    PLC_OP_DROLP    = 0x8D,
    PLC_OP_DROR     = 0x8E,   /* DROR Dn n        - 32位右旋转 */
    PLC_OP_DRORP    = 0x8F,
    PLC_OP_DSHL     = 0x90,   /* DSHL Dn n        - 32位左移 */
    PLC_OP_DSHLP    = 0x91,
    PLC_OP_DSHR     = 0x92,   /* DSHR Dn n        - 32位右移 */
    PLC_OP_DSHRP    = 0x93,
    PLC_OP_DSAL     = 0x94,   /* DSAL Dn n        - 32位算术左移 */
    PLC_OP_DSALP    = 0x95,
    PLC_OP_DSAR     = 0x96,   /* DSAR Dn n        - 32位算术右移 */
    PLC_OP_DSARP    = 0x97,

    /* --- 跳转/分支 --- */
    PLC_OP_JMP      = 0xA0,   /* JMP  LABEL       - 无条件跳转 */
    PLC_OP_JMPC     = 0xA1,   /* JMPC LABEL       - 条件跳转 (累加器为 1 时) */
    PLC_OP_JMPNC    = 0xA2,   /* JMPNC LABEL      - 条件跳转 (累加器为 0 时) */
    PLC_OP_CALL     = 0xA3,   /* CALL LABEL       - 调用子程序 */
    PLC_OP_FOR      = 0xA4,   /* FOR  Dn          - 循环开始 (Dn 次) */
    PLC_OP_NEXT     = 0xA5,   /* NEXT             - 循环结束 */
    PLC_OP_BREAK    = 0xA6,   /* BREAK            - 跳出循环 */

    /* --- 高级功能指令 --- */
    PLC_OP_BCD      = 0xB0,   /* BCD  Dn1 Dn2     - BIN→BCD 转换 */
    PLC_OP_BCDP     = 0xB1,
    PLC_OP_BIN      = 0xB2,   /* BIN  Dn1 Dn2     - BCD→BIN 转换 */
    PLC_OP_BINP     = 0xB3,
    PLC_OP_ASC      = 0xB4,   /* ASC  Dn1 Dn2     - HEX→ASCII */
    PLC_OP_HEX      = 0xB5,   /* HEX  Dn1 Dn2     - ASCII→HEX */
    PLC_OP_ENCO     = 0xB6,   /* ENCO Dn1 Dn2     - 编码器 (优先编码) */
    PLC_OP_DECO     = 0xB7,   /* DECO Dn1 Dn2     - 解码器 */
    PLC_OP_SEGD     = 0xB8,   /* SEGD Dn1 Dn2     - 7 段解码 */
    PLC_OP_SORT     = 0xB9,   /* SORT Dn1 Dn2 Dn3 - 排序 */
    PLC_OP_WSUM     = 0xBA,   /* WSUM Dn1 Dn2 Dn3 - 求和 */
    PLC_OP_MEAN     = 0xBB,   /* MEAN Dn1 Dn2 Dn3 - 平均值 */
    PLC_OP_SQR      = 0xBC,   /* SQR  Dn1 Dn2     - 平方根 (整数) */
    PLC_OP_SQRP     = 0xBD,
    PLC_OP_PID      = 0xBE,   /* PID  Dn1 Dn2 Dn3 - PID 运算 (Dn1=SV, Dn2=PV, Dn3=输出, 参数在后续 D) */

    /* --- 通信/扩展指令 --- */
    PLC_OP_MODRW    = 0xC0,   /* MODRW Dn1 Dn2 Dn3 Dn4 - Modbus RTU 读写 */
    PLC_OP_IVCK     = 0xC1,   /* IVCK Dn1 Dn2     - 逆变器状态检查 */
    PLC_OP_IVDR     = 0xC2,   /* IVDR Dn1 Dn2     - 逆变器驱动 */
    PLC_OP_IVRD     = 0xC3,   /* IVRD Dn1 Dn2     - 逆变器读参数 */
    PLC_OP_IVWR     = 0xC4,   /* IVWR Dn1 Dn2     - 逆变器写参数 */
    PLC_OP_ADPRW    = 0xC5,   /* ADPRW Dn1 Dn2 Dn3 - 绝对位置读写 */

    /* --- 标号/标记 (伪指令，编译时处理) --- */
    PLC_OP_LABEL    = 0xF0,   /* LABEL n:         - 标号定义 */
} plc_opcode_t;

/* ================================================================
 * 3. 操作数类型与编码
 * ================================================================ */

typedef enum {
    PLC_OPERAND_NONE    = 0,    /* 无操作数 */
    PLC_OPERAND_BIT     = 1,    /* 位操作数：区域(高4位) + 编号(12位) */
    PLC_OPERAND_WORD    = 2,    /* 字操作数：区域(高4位) + 编号(12位) */
    PLC_OPERAND_CONST   = 3,    /* 常数：16-bit 有符号立即数 */
    PLC_OPERAND_CONST32 = 4,    /* 32-bit 常数：需 2 个指令字 */
    PLC_OPERAND_LABEL   = 5,    /* 标号：16-bit 标号索引 */
} plc_operand_type_t;

/* 操作数区域码 (高 4 位) */
typedef enum {
    PLC_AREA_X  = 0x0,  /* 输入继电器 */
    PLC_AREA_Y  = 0x1,  /* 输出继电器 */
    PLC_AREA_M  = 0x2,  /* 内部继电器 */
    PLC_AREA_T  = 0x3,  /* 定时器 (位=完成标志, 字=当前值) */
    PLC_AREA_C  = 0x4,  /* 计数器 (位=完成标志, 字=当前值) */
    PLC_AREA_S  = 0x5,  /* 状态继电器 */
    PLC_AREA_D  = 0x6,  /* 数据寄存器 (16-bit) */
    PLC_AREA_V  = 0x7,  /* 特殊寄存器 */
    PLC_AREA_R  = 0x8,  /* 扩展寄存器 (掉电保持) */
    PLC_AREA_K  = 0xF,  /* 常数 (立即数) */
} plc_area_t;

/* 编码为 16-bit：(area << 12) | (index & 0xFFF) */
#define PLC_ENCODE_OPERAND(area, index)  (((uint16_t)(area) << 12) | ((index) & 0xFFF))
#define PLC_DECODE_AREA(operand)         ((plc_area_t)((operand) >> 12))
#define PLC_DECODE_INDEX(operand)        ((operand) & 0xFFF)

/* ================================================================
 * 4. 指令结构 (每条指令 1~3 个 16-bit 字)
 * ================================================================ */

typedef struct {
    uint16_t opcode;      /* 操作码 */
    uint16_t op1;         /* 操作数 1 (编码后) */
    uint16_t op2;         /* 操作数 2 */
    uint16_t op3;         /* 操作数 3 / 32-bit 常数高 16 位 */
} plc_inst_t;

/* 最大程序大小 (指令数) */
#define PLC_MAX_INST    2048

/* ================================================================
 * 5. PLC 运行时上下文
 * ================================================================ */

typedef struct {
    /* 程序存储 */
    plc_inst_t  *program;        /* 指令数组 */
    uint16_t     program_len;    /* 当前程序长度 (指令数) */
    uint16_t     program_max;    /* 最大容量 */

    /* 执行状态 */
    uint16_t     pc;             /* 程序计数器 (指令索引) */
    uint16_t     sp;             /* 栈指针 (CALL/RET/中断用) */
    uint8_t      acc;            /* 累加器 / 逻辑运算结果 (0/1) */
    uint8_t      carry;          /* 进位/借位标志 */
    uint8_t      zero;           /* 零标志 */
    uint8_t      error;          /* 错误标志 */

    /* 调用栈 (最大 16 层) */
    uint16_t     call_stack[16];
    uint8_t      call_depth;

    /* FOR/NEXT 循环栈 (最大 8 层) */
    struct {
        uint16_t start_pc;
        uint16_t counter_reg;    /* D 寄存器索引 */
        int16_t  remaining;
    } loop_stack[8];
    uint8_t      loop_depth;

    /* 定时器/计数器运行时状态 */
    uint16_t     timer_cur[PLC_T_MAX];   /* 定时器当前值 */
    uint16_t     timer_pre[PLC_T_MAX];   /* 定时器预设值 */
    int32_t      counter_cur[PLC_C_MAX]; /* 计数器当前值 */
    int32_t      counter_pre[PLC_C_MAX]; /* 计数器预设值 */
    uint8_t      counter_dir[PLC_C_MAX]; /* 计数方向 (0=Up, 1=Down, 2=Up/Down) */

    /* 上一扫描周期位状态 (用于边沿检测 PLS/PLF/MEP/MEF) */
    uint8_t      prev_bit_state[PLC_BIT_BYTES];

    /* 扫描周期统计 */
    uint32_t     scan_cycle_us;    /* 本次扫描耗时 (us) */
    uint32_t     scan_max_us;      /* 最大扫描耗时 */
    uint32_t     scan_min_us;      /* 最小扫描耗时 */
    uint32_t     scan_total_cycles;/* 总扫描次数 */

    /* 配置 */
    uint32_t     scan_period_us;   /* 目标扫描周期 (us)，默认 10ms */
    uint32_t     watchdog_us;      /* 看门狗超时 (us)，默认 100ms */
    uint16_t     config_flags;     /* PLC_CFG_* */

    /* 标号表 (运行时建立，最多 256 个标号) */
    struct {
        uint16_t label_id;
        uint16_t pc;
    } label_table[256];
    uint16_t     label_count;

    /* 错误信息 */
    int          last_error;       /* 最后错误码 */
    uint16_t     error_pc;         /* 错误指令 PC */

} plc_ctx_t;

/* 错误码 */
typedef enum {
    PLC_ERR_OK           = 0,
    PLC_ERR_OVERFLOW     = -1,  /* 栈/程序溢出 */
    PLC_ERR_UNDERFLOW    = -2,  /* 栈下溢 */
    PLC_ERR_DIV_ZERO     = -3,  /* 除零 */
    PLC_ERR_INVALID_OP   = -4,  /* 无效指令 */
    PLC_ERR_INVALID_ADDR = -5,  /* 地址越界 */
    PLC_ERR_WDT_TIMEOUT  = -6,  /* 看门狗超时 */
    PLC_ERR_LABEL_NOT_FOUND = -7, /* 标号未找到 */
    PLC_ERR_STACK_DEPTH  = -8,  /* 调用/循环层数超限 */
    PLC_ERR_MEMORY_FULL  = -9,  /* 内存不足 */
    PLC_ERR_PROG_CORRUPT = -10, /* 程序损坏 (校验失败) */
} plc_err_t;

/* ================================================================
 * 6. 核心 API
 * ================================================================ */

/** 初始化 PLC 上下文 (分配内存、清零、建立标号表) */
plc_err_t plc_init(plc_ctx_t *ctx, uint16_t max_inst);

/** 反初始化，释放资源 */
void plc_deinit(plc_ctx_t *ctx);

/** 加载程序 (从二进制缓冲区) */
plc_err_t plc_load_program(plc_ctx_t *ctx, const uint8_t *buf, size_t len);

/** 保存程序 (到二进制缓冲区，返回实际长度) */
size_t plc_save_program(const plc_ctx_t *ctx, uint8_t *buf, size_t max_len);

/** 单次扫描周期执行 (输入刷新 → 指令执行 → 输出刷新)
 *  返回执行的指令数，负值为错误码 */
int plc_scan(plc_ctx_t *ctx);

/** 运行/停止控制 */
void plc_run(plc_ctx_t *ctx);
void plc_stop(plc_ctx_t *ctx);
void plc_pause(plc_ctx_t *ctx);
bool plc_is_running(const plc_ctx_t *ctx);

/** 单步执行 (需暂停状态) */
plc_err_t plc_single_step(plc_ctx_t *ctx);

/** 复位 (清零所有非保持区、PC=0、错误清零) */
void plc_reset(plc_ctx_t *ctx);

/** 写入位变量 (X/Y/M/T/C/S) */
void plc_set_bit(plc_ctx_t *ctx, plc_area_t area, uint16_t index, bool value);

/** 读取位变量 */
bool plc_get_bit(const plc_ctx_t *ctx, plc_area_t area, uint16_t index);

/** 写入字变量 (D/V/R) */
void plc_set_word(plc_ctx_t *ctx, plc_area_t area, uint16_t index, int16_t value);
void plc_set_dword(plc_ctx_t *ctx, plc_area_t area, uint16_t index, int32_t value);

/** 读取字变量 */
int16_t plc_get_word(const plc_ctx_t *ctx, plc_area_t area, uint16_t index);
int32_t plc_get_dword(const plc_ctx_t *ctx, plc_area_t area, uint16_t index);

/** 设置扫描周期 (us) */
void plc_set_scan_period(plc_ctx_t *ctx, uint32_t period_us);

/** 设置看门狗超时 (us) */
void plc_set_watchdog(plc_ctx_t *ctx, uint32_t timeout_us);

/** 获取扫描统计 */
void plc_get_stats(const plc_ctx_t *ctx, uint32_t *cur_us, uint32_t *max_us, uint32_t *min_us, uint32_t *total);

/** 打印程序反汇编 (调试用) */
void plc_disasm(const plc_ctx_t *ctx, void (*putc_fn)(char), void (*puts_fn)(const char*));

/* ================================================================
 * 7. 编译器/汇编器接口 (可选，用于从文本生成二进制)
 * ================================================================ */

typedef struct {
    const char *mnemonic;
    plc_opcode_t opcode;
    uint8_t      operand_count;
    plc_operand_type_t op_types[3];
} plc_inst_def_t;

/* 指令定义表 (用于汇编器/反汇编器) */
extern const plc_inst_def_t plc_inst_table[];
extern const uint16_t plc_inst_table_size;

/** 解析汇编行 -> 指令 (简易行汇编器) */
plc_err_t plc_asm_line(const char *line, plc_inst_t *out_inst, uint16_t *out_len);

/** 解析完整程序文本 -> 二进制 */
plc_err_t plc_asm_program(const char *text, uint8_t *out_buf, size_t max_len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* PLC_CORE_H */