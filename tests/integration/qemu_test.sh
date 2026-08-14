#!/usr/bin/env bash
# ================================================================
# QEMU 集成测试脚本（备选方案，适合 CI 环境）
# 用法：./qemu_test.sh build/mini_kernel.elf
# ================================================================
set -euo pipefail

ELF_FILE="${1:-build/mini_kernel.elf}"
QEMU_BIN="${QEMU_BIN:-qemu-system-arm}"
TIMEOUT="${TIMEOUT:-30}"

if [[ ! -f "$ELF_FILE" ]]; then
    echo "Error: ELF file not found: $ELF_FILE"
    exit 1
fi

# QEMU 机器类型：Raspberry Pi Pico (RP2040)
MACHINE="raspberrypi-pico"
CPU="cortex-m0plus"

# 串口输出到 stdio，无图形界面
QEMU_ARGS=(
    -M "$MACHINE"
    -cpu "$CPU"
    -kernel "$ELF_FILE"
    -nographic
    -monitor none
    -serial stdio
    -semihosting-config enable=on,target=native
)

echo "Running QEMU: ${QEMU_BIN} ${QEMU_ARGS[*]}"

# 运行 QEMU，捕获输出
timeout "$TIMEOUT" "${QEMU_BIN}" "${QEMU_ARGS[@]}" 2>&1 | tee qemu_output.log

# 检查输出中的测试结果
if grep -q "ALL TESTS PASSED" qemu_output.log; then
    echo "✅ QEMU Integration Test PASSED"
    exit 0
else
    echo "❌ QEMU Integration Test FAILED"
    grep -E "(PASSED|FAILED|FAIL|ERROR)" qemu_output.log || true
    exit 1
fi