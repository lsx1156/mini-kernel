# Mini Kernel - 轻量分时通用操作内核

面向资源受限 32 位 MCU 的极简内核，**Flash ≤ 10KB / RAM ≤ 10KB** 即可运行，主打跨芯片可移植、类 POSIX 开发范式、零动态内存碎片。

> **定位**：填补「裸机开发门槛高、RTOS 偏向实时控制、Linux 无法运行」的中间空白地带。
>
> **当前版本**：`0.2.0-beta [UNTESTED]`（指令固化 + 出厂重置，**尚未在硬件端完整跑通全链路**，请参考下方 Shell 固化命令验证流程做上电测试）

---

## 🧭 版本路线

| 版本 | 状态 | 关键里程碑 |
|------|------|-----------|
| **v0.1.0** | ✅ Released | 基础内核（调度/堆/上下文）+ Shell/外设（GPIO/I2C/UART/SPI）可用 |
| **v0.2.0-beta** | ⚠️ UNTESTED | 本版本：**指令固化机制** (`!`/`save`/`unsave`/`list`/`boot exec`) + **板载 Flash HAL** + **factory_reset 出厂重置** + 固化命令开机自动执行 |
| v0.2.0 | 📋 待验证 | 硬件跑通 I2C OLED 固化开机自动亮起 + SPI B线路板载 Flash 自检全 PASS |
| v0.3.0 | 🗓️ 规划 | VFS + FatFs (SD Card via SPI) + 简单文件读写命令 |

---

## 🎯 核心特性

| 特性 | 指标 |
|------|------|
| **最小内核** | Flash ≤ 10KB, RAM ≤ 10KB |     
| **完整基础版** | Flash ~76KB, RAM ~9KB (含 Shell + 外设服务 + 固化子系统 + Pico SDK 运行时) |
| **调度策略** | 时间片轮转 + 权重比例，**无优先级抢占**，非硬实时 |
| **内存模型** | 固定内存池 + 简易堆，**零碎片** |
| **架构** | 4 层垂直分层：应用 → 系统调用 → 内核核心 → HAL |
| **移植** | 仅重写 HAL 层，核心代码 100% 复用 |
| **支持架构** | Cortex-M 全系列、RISC-V RV32 (规划中) |
| **首发平台** | RP2040 (Cortex-M0+) + 板载 W25Q16JV (2MB SPI Flash) |
| **✨ v0.2 指令固化** | 双备份扇区 + CRC8 校验；32 × 123B 命令槽；`!` 前缀立即执行+固化；开机自动回放 |
| **✨ v0.2 出厂重置** | `factory_reset confirm` 擦末尾 64KB 清除所有持久化数据，保留 OS 固件本身 |

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
├── examples/
│   └── builtin_demo/     # 内置演示应用（LED/心跳/内存压力任务）
├── tests/
│   ├── unit/             # Unity 单元测试 (本地运行)
│   └── integration/      # Renode/QEMU 集成测试
├── cmake/                # CMake 模块 (体积检查等)
├── scripts/              # 构建辅助脚本
├── CMakeLists.txt        # CMake 主构建
└── README.md
```

---

## 🚀 快速开始

### 1. 环境准备

```bash
# Windows (PowerShell)
# 安装 ARM GCC 工具链 (建议使用 PlatformIO 自带的)
# 或从 https://developer.arm.com/tools-and-software 下载

# Pico SDK
$env:PICO_SDK_PATH = "E:\ppCD\pico-sdk"
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git $env:PICO_SDK_PATH
cd $env:PICO_SDK_PATH && git submodule update --init

# Linux/Ubuntu
sudo apt-get install -y gcc-arm-none-eabi cmake make ninja-build python3
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
#define OS_CFG_PERIPH_SERVICE   0   // GPIO/SPI/I2C/UART 服务
#define OS_CFG_SHELL            1   // 命令行终端
#define OS_CFG_VFS              0   // 虚拟文件系统
#define OS_CFG_FATFS            0   // FatFs (需 VFS=1)
#define OS_CFG_LOADER           0   // 程序加载器 (需 VFS=1)
#define OS_CFG_DEMO_APP         1   // 内置演示应用
```

### 3. 构建固件

**CMake + Ninja (推荐)**
```bash
# Windows
$env:PICO_SDK_PATH = "E:\ppCD\pico-sdk"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=$env:PICO_SDK_PATH -G Ninja
cmake --build build -j4

# Linux/Mac
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=$PICO_SDK_PATH -G Ninja
cmake --build build -j4
```

产物：
- `build/mini_kernel.elf` - 调试用
- `build/mini_kernel.bin` - 直接烧录
- `build/mini_kernel.uf2` - 拖拽进 U 盘模式烧录
- `build/mini_kernel.map` - 内存映射 (体积检查依据)

### 4. 烧录运行

**UF2 拖拽法 (最简单)**
1. Pico 按住 BOOTSEL 插入 USB
2. 将 `mini_kernel.uf2` 拖入 `RPI-RP2` 盘符
3. 打开串口终端（115200 8N1），看到：
```
============================================================

 === Mini Kernel Boot ===

============================================================
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
Firmware Size: Flash = 56 KB, RAM = 8 KB  (含 Pico SDK + Shell + Demo)
Limits:        Flash <= 20 KB, RAM <= 8 KB
```

> **注意**：以上为包含完整 Pico SDK（USB CDC、TinyUSB、C 库等）的完整版本。精简内核（最小配置）仅需 ~10KB Flash。

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

### Shell 交互 + 指令固化 (v0.2 新增)
```bash
# 普通执行
mk> help                       # 查看全部命令
mk> led on                     # 板载 LED 亮
mk> i2c init 0 4 5 100000      # I2C0 @ GP4/5 100kHz

# ────────── ✨ v0.2: 指令固化（写入板载 SPI Flash，掉电仍在）──────────
# 方式 A：语法糖 —— 先执行，成功后自动固化
mk> ! led on                   # 立即执行 led on + 追加写入 Flash
                               # 输出: OK + Remaining: 31/32 slots

# 方式 B：先 save，下次开机才执行（不立即执行）
mk> save i2c init 0 4 5 100000
mk> save i2c cmds 0 0x3C 0xAE 0x20 0x00 0xC8 0x40 0x81 0x7F   # OLED init
mk> list                       # 列出全部固化条目 + 汇报剩余 slots
Persistent commands (used 3, free 29/32 slots, Flash 2x backup):
  #0: led on
  #1: i2c init 0 4 5 100000
  #2: i2c cmds 0 0x3C 0xAE 0x20 0x00 0xC8 0x40 0x81 0x7F

mk> boot exec                  # 立刻跑一遍所有已固化指令 (不等开机)
mk> unsave 0                   # 删掉第 0 条
mk> unsave all                 # 一键清空所有固化条目

# 下次按 RUN / 拔插 USB：[BOOT ] banner 之后会自动回放已存的 #0..#n 条，再进 mk>
```

```bash
# ────────── ✨ v0.2: 出厂重置（保留 OS 内核 + demo_app 固件）──────────
mk> factory_reset              # 仅打印警告 + 影响范围（安全，不真正擦除）
****************************************************************
*                    FACTORY RESET WARNING                     *
****************************************************************
  此命令将把系统恢复到刚刷完固件的出厂状态：
   ✓ 保留：内核 + demo_app 固件本身（操作系统完整保留）
   ✗ 清除：所有固化指令 (save / ! 保存的 bootscript 全部条目)
   ✗ 清除：板载 SPI Flash 最后 64KB 保留区 (16 sectors)
   ✗ 清除：任何应用层写入的持久化用户数据
  要继续，请输入:  factory_reset confirm
****************************************************************

mk> factory_reset confirm      # 真正执行：擦 bootscript 双备份 + 末尾 16 个扇区
                               # 完成后请重新上电
```

```bash
# ────────── 📡 B线路 SPI Flash 自检（v0.2 新增）──────────
mk> boot flash_test            # 双备份扇区 A/B 擦→写(伪随机)→读→CRC→一致性对比
                               # PASS=板载 SPI + hal_flash 驱动链路 OK
```

```bash
# 其它常用
mk> ps                         # 任务列表
mk> heap                       # 堆空闲字节 / 最大块
mk> gpio help | i2c help       # 外设子命令帮助
```

---

## 🏗️ 架构设计

### 分层架构
```
┌─────────────────────────────────────────┐
│         应用层 (Application)             │
│   用户代码 / 演示任务                    │
├─────────────────────────────────────────┤
│         系统调用层 (Syscall)             │
│   SVC 异常 / X-Macro 契约表             │
├─────────────────────────────────────────┤
│         内核核心 (Kernel Core)           │
│   调度器 / 任务 / 内存 / 中断管理        │
├─────────────────────────────────────────┤
│         硬件抽象层 (HAL)                │
│   统一接口 / 平台相关实现               │
├─────────────────────────────────────────┤
│         硬件 (Hardware)                 │
│   Cortex-M0+ / RP2040                   │
└─────────────────────────────────────────┘
```

### 上下文切换 (Cortex-M0+)
```
PendSV Handler:
  1. 保存 r4-r11 (callee-saved) 到当前任务栈
  2. 保存 PSP 到当前任务 TCB
  3. 调用 sched_do_switch() 选择下一任务
  4. 从新任务 TCB 恢复 PSP
  5. 恢复 r4-r11
  6. 切换到新任务
```

### 内存管理
- **固定内存池**：内核对象从静态池分配，零碎片
- **简易堆**：隐式空闲链表，首次适配分配
- **临界区保护**：kmalloc/kfree 使用 cpsid/cpsie 保护

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
5. 提交 PR

---

## 📄 许可证

MIT License

---

## 🙏 致谢

- [Pico SDK](https://github.com/raspberrypi/pico-sdk) - RP2040 官方 SDK
- [Unity](https://github.com/ThrowTheSwitch/Unity) - 嵌入式单测框架
- [Renode](https://renode.io/) - 强大的模拟器平台
- [FreeRTOS](https://www.freertos.org/) / [Zephyr](https://www.zephyrproject.org/) - 架构参考