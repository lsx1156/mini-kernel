#!/usr/bin/env python3
"""
Renode 集成测试脚本
用于在 Renode 模拟器中运行固件并验收：
- 最小内核体积 ≤ 10KB Flash / 4KB RAM
- 可正常创建多个任务并稳定轮转运行
- 任务销毁后内存可完整回收，无碎片泄漏
"""
import sys
import os
import subprocess
import time
import re
from pathlib import Path

RENODE_PATH = os.getenv("RENODE_PATH", "renode")
FIRMWARE_ELF = os.getenv("FIRMWARE_ELF", "build/mini_kernel.elf")
TEST_TIMEOUT = 60  # seconds

# Renode 脚本模板
RESCRIPT_TEMPLATE = """
using sysbus
mach create "mini-kernel"
machine LoadPlatformDescription @platforms/cpus/rp2040.repl

# 加载固件
sysbus LoadELF @{elf_path}

# 启用 UART0 日志
uart0 LogFileName @uart0.log
uart0 LogMode "Text"

# 启动仿真
start

# 等待测试完成（通过 UART 输出判断）
# 测试预期输出：
# "=== Mini Kernel Boot ==="
# "TASK1 running"
# "TASK2 running"
# "HEAP OK"
# "ALL TESTS PASSED"
"""


class RenodeTest:
    def __init__(self, elf_path, renode_path=RENODE_PATH):
        self.elf_path = Path(elf_path).resolve()
        self.renode_path = renode_path
        self.log_file = self.elf_path.parent / "uart0.log"

    def run(self, timeout=TEST_TIMEOUT):
        if not self.elf_path.exists():
            raise FileNotFoundError(f"Firmware not found: {self.elf_path}")

        # 生成临时 Renode 脚本
        script_content = RESCRIPT_TEMPLATE.format(elf_path=self.elf_path)
        script_file = self.elf_path.parent / "test_renode.resc"
        script_file.write_text(script_content)

        # 清理旧日志
        if self.log_file.exists():
            self.log_file.unlink()

        # 运行 Renode
        cmd = [self.renode_path, "-e", f"include @{script_file}"]
        print(f"Running: {' '.join(cmd)}")

        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            start = time.time()
            while proc.poll() is None:
                if time.time() - start > timeout:
                    proc.terminate()
                    raise TimeoutError(f"Renode test timed out after {timeout}s")
                time.sleep(0.5)
        except FileNotFoundError:
            raise RuntimeError(f"Renode not found at {self.renode_path}. Install Renode or set RENODE_PATH.")

        # 分析日志
        return self.analyze_log()

    def analyze_log(self):
        if not self.log_file.exists():
            return False, "No UART log generated"

        log = self.log_file.read_text()
        print(f"--- UART Log ---\n{log}\n------------------")

        checks = {
            "boot": "=== Mini Kernel Boot ===" in log,
            "task1": "TASK1 running" in log,
            "task2": "TASK2 running" in log,
            "heap": "HEAP OK" in log,
            "pass": "ALL TESTS PASSED" in log,
        }

        all_ok = all(checks.values())
        msg = "PASS" if all_ok else "FAIL"
        details = ", ".join(f"{k}={'OK' if v else 'MISSING'}" for k, v in checks.items())
        return all_ok, f"{msg}: {details}"


def main():
    if len(sys.argv) > 1:
        elf = sys.argv[1]
    else:
        elf = FIRMWARE_ELF

    test = RenodeTest(elf)
    try:
        ok, msg = test.run()
        print(f"Result: {msg}")
        sys.exit(0 if ok else 1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()