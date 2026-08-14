#!/usr/bin/env python3
"""
PlatformIO 预处理脚本：从 os_config.h 解析 OS_CFG_* 宏并注入 build_flags
用法：在 platformio.ini 中配置 extra_scripts = pre:scripts/gen_config_macros.py
"""
import re
import os
import sys

Import("env")

CONFIG_H = os.path.join(env["PROJECT_DIR"], "include", "os_config.h")

def parse_config_macros(config_path):
    """解析 os_config.h，返回宏定义列表"""
    if not os.path.exists(config_path):
        print(f"Warning: {config_path} not found")
        return []

    with open(config_path, 'r') as f:
        content = f.read()

    # 匹配 #define OS_CFG_XXX value
    pattern = r'#define\s+(OS_CFG_[A-Z0-9_]+)\s+(\S+)'
    matches = re.findall(pattern, content)

    macros = []
    for name, value in matches:
        # 过滤掉注释行中的匹配
        line_start = content.rfind('\n', 0, content.find(f"#define {name}"))
        line = content[line_start:content.find('\n', line_start)]
        if line.strip().startswith('//'):
            continue
        macros.append(f"-D{name}={value}")

    return macros

macros = parse_config_macros(CONFIG_H)
if macros:
    print(f"Injecting {len(macros)} config macros: {macros}")
    env.Append(CCFLAGS=macros)
    env.Append(ASFLAGS=macros)
else:
    print("No OS_CFG_* macros found")