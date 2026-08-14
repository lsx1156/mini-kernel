/**
 * @file    unity_config.h
 * @brief   Unity 测试框架配置（嵌入式适配）
 */
#ifndef UNITY_CONFIG_H
#define UNITY_CONFIG_H

/* 基础配置 */
#define UNITY_INT_WIDTH          32
#define UNITY_LONG_WIDTH         32
#define UNITY_POINTER_WIDTH      32

/* 输出配置 */
#define UNITY_OUTPUT_CHAR        hal_console_putc
#define UNITY_OUTPUT_FLUSH()     do {} while(0)
#define UNITY_OUTPUT_START()     do {} while(0)
#define UNITY_OUTPUT_COMPLETE()  do {} while(0)

/* 断言配置 */
#define UNITY_ASSERT_EQUAL_INT_ARRAY    1
#define UNITY_ASSERT_EQUAL_HEX8_ARRAY   1
#define UNITY_ASSERT_EQUAL_HEX16_ARRAY  1
#define UNITY_ASSERT_EQUAL_HEX32_ARRAY  1
#define UNITY_ASSERT_EQUAL_FLOAT_ARRAY  1
#define UNITY_ASSERT_EQUAL_DOUBLE_ARRAY 1

/* 测试固件配置 */
#define UNITY_FIXTURES             1
#define UNITY_INCLUDE_FLOAT        0
#define UNITY_INCLUDE_DOUBLE       0

/* 内存分配（测试用静态分配） */
#define UNITY_MALLOC(size)         kmalloc(size)
#define UNITY_FREE(ptr)            kfree(ptr)

/* 禁止使用标准库 */
#define UNITY_EXCLUDE_STDINT_H     0
#define UNITY_EXCLUDE_LIMITS_H     0
#define UNITY_EXCLUDE_STRING_H     1
#define UNITY_EXCLUDE_STDARG_H     1
#define UNITY_EXCLUDE_CTYPE_H      1

#endif /* UNITY_CONFIG_H */