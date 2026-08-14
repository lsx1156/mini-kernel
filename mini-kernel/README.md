# Mini Kernel - 轻量分时通用操作内核

面向资源受限 32 位 MCU 的极简内核，**Flash ≤ 10KB / RAM ≤ 4KB** 即可运行，主打跨芯片可移植、类 POSIX 开发范式、零动态内存碎片。

> **定位**：填补「裸机开发门槛高、RTOS 偏向实时控制、Linux 无法运行」的中间空白地带。

---

## 🎯 核心特性

| 特性 | 指标 |
|------|------|
| **最小内核** | Flash ≤ 10KB, RAM ≤ 4KB |
| **完整基础版** | Flash ≤ 20KB, RAM ≤ 8KB (含 Shell + VFS + 外设服务) |
| **调度策略** | 时间片轮转 + 权重比例，**无优先级抢占**，非硬实时 |
| **内存模型** | 固定内存池 + 简易堆，**零碎片** |
| **架构** | 4 层垂直分层：应用 → 系统调用 → 内核核心 → HAL |
| **移植** | 仅重写 HAL 层，核心代码 100% 复用 |
| **支持架构** | Cortex-M 全系列、RISC-V RV32 (规划中) |
| **首发平台** | RP2040 (Cortex-M0+) |

---

## 📁 目录结构

```
mini-kernel/
├── include/              # 对外头文件
│   ├── os_config.h       # 系统裁剪总配置（用户修改此文件）
│   ├── kernel.h          # 用户态 API 入口
│   └── hal/              # HAL 统一接口（硬件无关）
├── port/                 # 硬件移植层
│   └── rp2040/           # RP2040 实现 (Pico SDK)
│       ├── hal_port.c/h  # HAL 接口实现
│       ├── context_switch.S  # PendSV 上下文切换汇编
│       ├── startup_rp2040.S  # 启动代码
│       └── rp2040.ld     # 链接脚本
├── kernel/               # 内核核心（硬件无关）
│   ├── core/             # 任务、调度、内存、中断
│   ├── syscall/          # 系统调用层 (SVC)
│   └── modules/          # 可裁剪扩展模块
│       ├── periph/       # 共享总线服务
│       ├── shell/        # 命令行终端
│       ├── vfs/          # 虚拟文件系统
│       └── loader/       # 用户程序加载器
├── tests/
│   ├── unit/             # Unity 单元测试 (本地运行)
│   └── integration/      # Renode/QEMU 集成测试
├── cmake/                # CMake 模块 (体积检查等)
├── scripts/              # 构建辅助脚本
├── .github/workflows/    # CI/CD
├── CMakeLists.txt        # CMake 主构建
├── platformio.ini        # PlatformIO 配置
└── README.md
```

---

## 🚀 快速开始

### 1. 环境准备

```bash
# Ubuntu/Debian
sudo apt-get install -y gcc-arm-none-eabi cmake make ninja-build python3

# Pico SDK
export PICO_SDK_PATH=/path/to/pico-sdk
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git $PICO_SDK_PATH
cd $PICO_SDK_PATH && git submodule update --init
```

### 2. 配置裁剪

复制并修改配置文件：
```bash
cp include/os_config.h include/os_config.h.bak
# 编辑 include/os_config.h，按需开关模块
```

关键宏：
```c
#define OS_CFG_PERIPH_SERVICE   1   // GPIO/SPI/I2C/UART 服务
#define OS_CFG_SHELL            1   // 命令行终端
#define OS_CFG_VFS              1   // 虚拟文件系统
#define OS_CFG_FATFS            0   // FatFs (需 VFS=1)
#define OS_CFG_LOADER           0   // 程序加载器 (需 VFS=1)
```

### 3. 构建固件

**CMake + Ninja (推荐)**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=$PICO_SDK_PATH -G Ninja
cmake --build build -j4
```

产物：
- `build/mini_kernel.elf` - 调试用
- `build/mini_kernel.bin` - 直接烧录
- `build/mini_kernel.uf2` - 拖拽进 U 盘模式烧录
- `build/mini_kernel.map` - 内存映射 (体积检查依据)

**PlatformIO**
```bash
pio run -e pico
```

### 4. 烧录运行

**UF2 拖拽法 (最简单)**
1. Pico 按住 BOOTSEL 插入 USB
2. 将 `mini_kernel.uf2` 拖入 `RPI-RP2` 盘符
3. 串口打开 115200 8N1，看到：
```
=== Mini Kernel Boot ===
```

**OpenOCD + GDB**
```bash
openocd -f interface/picoprobe.cfg -f target/rp2040.cfg
# 另开终端
arm-none-eabi-gdb build/mini_kernel.elf
(gdb) target remote localhost:3333
(gdb) load
(gdb) continue
```

---

## 🧪 测试体系

### 单元测试 (本地运行，无硬件)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target unit_tests
./build/unit_tests
```
覆盖：任务管理、内存管理、调度器核心逻辑。

### 集成测试 (Renode 模拟器)
```bash
# 安装 Renode
wget https://github.com/renode/renode/releases/download/v1.14.0/renode_1.14.0.deb
sudo apt install ./renode_1.14.0.deb

# 运行测试
python3 tests/integration/renode_test.py build/mini_kernel.elf
```
验收：任务轮转、内存回收、栈保护、Shell 交互。

### 集成测试 (QEMU 备选)
```bash
sudo apt-get install qemu-system-arm
chmod +x tests/integration/qemu_test.sh
tests/integration/qemu_test.sh build/mini_kernel.elf
```

---

## 🔧 移植新 MCU

仅需实现 `port/<new_mcu>/hal_port.c`，填满 `hal_export` 表：

```c
// 必选 (内核启动依赖)
hal_systick_ops_t   // 系统滴答
hal_console_ops_t   // 调试串口
hal_context_init/switch/yield  // 汇编上下文切换

// 可选 (按 OS_CONFIG 裁剪)
hal_gpio_ops_t
hal_spi_ops_t
hal_i2c_ops_t
hal_uart_ops_t
hal_sdcard_ops_t
```

核心代码 **零修改**，用户应用代码 **跨平台完全兼容**。

---

## 📏 体积红线自检

构建后自动解析 `.map` 文件：
```
Firmware Size: Flash = 8 KB, RAM = 3 KB
Limits:        Flash <= 10 KB, RAM <= 4 KB
✅ Size check passed.
```
超限直接报错阻断 CI。

---

## 📚 开发指南

### 创建任务
```c
#include "kernel.h"

void my_task(void *arg) {
    while (1) {
        k_printf("Hello from task!\r\n");
        k_task_sleep(1000);  // 睡眠 1 秒 (OS_CFG_TICK_HZ=1000)
    }
}

int main(void) {
    k_task_create("my", my_task, NULL, 512, 1);
    // 调度器在 kernel_main 中启动，main 不返回
}
```

### 使用外设服务 (共享总线自动排队)
```c
// 多任务并发访问 I2C，内核自动串行化
k_i2c_write(0, 0x68, reg_addr, 1);  // 任务 A
k_i2c_read(0, 0x68, buf, 6);        // 任务 B (自动等待 A 完成)
```

### Shell 交互
```bash
> help
> tasks        # 查看任务列表
> mem          # 内存状态
> gpio 25 1    # 控制 GPIO
> reboot       # 复位
```

---

## ⚠️ 明确的非目标 (边界红线)

1. **不追求硬实时** - 无优先级抢占，不承诺微秒级响应
2. **不做虚拟内存/MPU 强隔离** - 单地址空间，协作式保护
3. **不做 Linux 式设备树/总线匹配** - 静态配置，零运行时解析
4. **不全量兼容 POSIX** - 仅核心常用接口
5. **不为性能牺牲简洁** - 优先架构清晰、体积可控

---

## 🤝 贡献指南

1. Fork & 分支
2. 遵循编码规范：`clang-format --style=file -i`
3. 通过静态分析：`cppcheck --enable=all kernel/ port/`
4. 单测全过：`./build/unit_tests`
5. 集成测试过：Renode/QEMU
6. 提交 PR

---

## 📄 许可证

MIT License - 详见 [LICENSE](LICENSE)

---

## 🙏 致谢

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) - RP2040 官方 SDK
- [Unity](https://github.com/ThrowTheSwitch/Unity) - 嵌入式单测框架
- [Renode](https://renode.io/) - 强大的模拟器平台
- [FreeRTOS](https://www.freertos.org/) / [Zephyr](https://www.zephyrproject.org/) - 架构参考