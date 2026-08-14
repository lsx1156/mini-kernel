# PP CD · Project Repository v0.1

```
project/                          ← 本仓库（单仓）
├── mini-kernel/                  ← 轻量级 RTOS 内核（RP2040 端口）
│   ├── include/                  公共头（include/kernel.h 是用户态唯一入口）
│   ├── kernel/core/              内核核心（sched / task / mem / kernel_main）
│   ├── kernel/syscall/           Syscall 契约表（静态编译期 X-Macro）
│   ├── kernel/modules/shell/     交互式命令行（help/ps/heap/syscalls 等）
│   ├── kernel/modules/periph/    外设服务（GPIO/SPI/I2C/UART，按裁剪宏）
│   ├── examples/builtin_demo/    内置 demo 任务（LED/心跳/内存压力）
│   ├── port/rp2040/              Cortex-M0+ 移植（PendSV / SVC / HAL）
│   ├── tests/                    单元测试 + 诊断固件
│   ├── cmake/check_size.cmake    体积红线检查脚本
│   └── CMakeLists.txt
└── rp2040demo/                   ← 独立应用工程（引用 mini-kernel 作静态库）
    ├── src/main.c                应用入口（强 main() 覆盖内核的弱 main）
    └── CMakeLists.txt
```

## v0.1 里程碑

* Cortex-M0+ 时间片轮转调度（支持权重比例）

* 固定内存池 + 零碎片堆（含临界区保护）

* USB CDC 控制台（硬件 EP1 OUT 轮询旁路）

* 交互式 Shell（含 syscalls 契约表查询命令）

* 完整上下文切换（保存 r4-r11 callee-saved 寄存器）

* Pico SDK 可集成：既可顶层固件，也能被独立工程 add\_subdirectory 引用

* **体积约束**：精简版 Flash ≤ 10KB / RAM ≤ 4KB（不含 Pico SDK）；完整基础版 ≤ 20KB/8KB

## 架构设计

### 分层架构

```
┌─────────────────────────────────────────┐
│         应用层 (Application)             │
│   独立应用 / 内置演示任务                │
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

### 关键特性

* **时间片轮转调度**：固定时间片 + 权重比例，非抢占式

* **零碎片内存**：固定池 + 隐式空闲链表堆

* **临界区保护**：kmalloc/kfree 和任务队列操作均使用 cpsid/cpsie

* **上下文切换**：PendSV 保存/恢复 r4-r11 寄存器

## 构建

### mini-kernel 内置 demo 固件

```bash
cd mini-kernel
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=/path/to/pico-sdk -G Ninja
cmake --build build -j4
```

### rp2040demo 独立应用固件

```bash
cd rp2040demo
cmake -B build -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH=/path/to/pico-sdk -G Ninja
cmake --build build -j4
```

## 烧录

将生成的 `.uf2` 文件拖入 RP2040 的 `RPI-RP2` 盘符即可。

烧录后用 PuTTY / TeraTerm 打开 USB 虚拟串口（115200-8-N-1），看到：

```
============================================================

 === Mini Kernel Boot ===

============================================================
```

输入 `help` / `tasks` / `syscalls` 查看可用命令。

## 版本历史

### v0.1.0 (当前)

* ✅ 基础内核调度器（时间片轮转 + 权重）

* ✅ 任务管理（创建/销毁/睡眠/唤醒）

* ✅ 内存管理（固定池 + 堆，零碎片）

* ✅ Shell 命令行（help/tasks/mem/syscalls）

* ✅ USB CDC 控制台

* ✅ 临界区保护（内存/任务队列操作）

* ✅ 上下文切换（PendSV/SVC，完整寄存器保存）

* ✅ 单仓结构（mini-kernel + rp2040demo）

