# Mini Kernel Code Wiki

> **版本**: v2.2.7 STABLE | **目标平台**: RP2040 (Cortex-M0+) | **文档生成**: 2026-08-15

---

## 目录

1. [项目概述](#1-项目概述)
2. [整体架构](#2-整体架构)
3. [目录结构](#3-目录结构)
4. [核心内核模块](#4-核心内核模块)
   - 4.1 [任务管理 (task.c/task.h)](#41-任务管理-taskctaskh)
   - 4.2 [调度器 (sched.c/sched.h)](#42-调度器-schedcschedh)
   - 4.3 [内存管理 (mem.c/mem.h)](#43-内存管理-memcmemh)
   - 4.4 [内核主入口 (kernel.c)](#44-内核主入口-kernelc)
5. [硬件抽象层 HAL](#5-硬件抽象层-hal)
   - 5.1 [HAL 统一接口 (hal_interface.h)](#51-hal-统一接口-hal_interfaceh)
   - 5.2 [RP2040 移植层 (hal_port.c)](#52-rp2040-移植层-hal_portc)
   - 5.3 [上下文切换 (context_switch.S)](#53-上下文切换-context_switchs)
6. [系统调用层](#6-系统调用层)
7. [服务模块](#7-服务模块)
   - 7.1 [Shell 命令行](#71-shell-命令行)
   - 7.2 [FatFs / USB MSC](#72-fatfs--usb-msc)
   - 7.3 [Bootscript 固化命令](#73-bootscript-固化命令)
8. [Flash 分区布局](#8-flash-分区布局)
9. [内核防卡死机制详解](#9-内核防卡死机制详解)
10. [关键数据结构](#10-关键数据结构)
11. [依赖关系图](#11-依赖关系图)
12. [构建与运行](#12-构建与运行)
13. [裁剪配置 (os_config.h)](#13-裁剪配置-os_configh)
14. [调试与诊断](#14-调试与诊断)

---

## 1. 项目概述

### 1.1 项目定位

Mini Kernel 是一个面向资源受限 32 位 MCU 的轻量级时间片轮转内核，专为 **RP2040 (Cortex-M0+, 264KB SRAM, 2MB Flash)** 设计。

### 1.2 核心设计目标

| 目标 | 约束值 | 说明 |
|------|--------|------|
| 最小内核 Flash | ≤ 10KB | 仅核心调度 + 任务 + 内存 |
| 最小内核 RAM | ≤ 4KB | 静态结构 + 内核堆 |
| 完整基础版 Flash | ≤ 20KB | + Shell + FatFs + MSC + 外设服务 |
| 完整基础版 RAM | ≤ 8KB | 多任务栈 + TCB + 内存池 |
| 调度策略 | 时间片轮转 | 无优先级抢占，支持权重比例 |
| 内存模型 | 固定池 + 零碎片堆 | 无 MMU，无虚拟内存 |

### 1.3 功能清单

- ✅ 多任务管理（最多 16 任务）
- ✅ 时间片轮转调度 + 权重
- ✅ 任务睡眠/挂起/恢复/销毁
- ✅ 固定内存池 + 简易堆 kmalloc/kfree
- ✅ 栈溢出魔值检测
- ✅ SVC 系统调用层
- ✅ PendSV 上下文切换（保存 r4-r11）
- ✅ Shell 交互式命令行（USB CDC + UART0 双通道）
- ✅ GPIO / SPI / I2C / UART 外设服务
- ✅ Elm FatFs FAT16 文件系统
- ✅ USB 复合设备：CDC（串口）+ MSC（U盘）
- ✅ Bootscript 固化命令（Flash 双备份）
- ✅ LED 诊断闪灯 / HardFault 快闪

---

## 2. 整体架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────────┐
│                    用户应用层 (User App)                      │
│  demo_app.c / rp2040demo/src/main.c                          │
├─────────────────────────────────────────────────────────────┤
│                    系统调用层 (Syscall)                       │
│  syscall_contract.h / syscall_contract.c  (SVC #0 陷入)      │
├─────────────────────────────────────────────────────────────┤
│                    服务模块层 (Modules)                        │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────────┐   │
│  │  Shell   │  │  FatFs   │  │   MSC    │  │ Bootscript│   │
│  └──────────┘  └──────────┘  └──────────┘  └───────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    内核核心层 (Kernel Core)                   │
│  ┌────────────┐  ┌────────────┐  ┌─────────────────────┐    │
│  │ 任务管理   │  │  调度器    │  │   内存管理          │    │
│  │  task.c    │  │  sched.c   │  │   mem.c             │    │
│  └────────────┘  └────────────┘  └─────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    硬件抽象层 (HAL Port)                      │
│  ┌─────────────────┐  ┌────────────────────────────────┐    │
│  │ hal_port.c      │  │ context_switch.S (PendSV/SVC)  │    │
│  │ SysTick/GPIO/   │  │ HardFault/NMI/Invalid IRQ      │    │
│  │ SPI/I2C/UART/USB│  │                                │    │
│  └─────────────────┘  └────────────────────────────────┘    │
├─────────────────────────────────────────────────────────────┤
│                    Pico SDK / TinyUSB / ROM                  │
│  pico_stdlib / hardware_* / TinyUSB CDC+MSC / boot2         │
├─────────────────────────────────────────────────────────────┤
│                    RP2040 硬件 (Cortex-M0+)                  │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 启动流程

```
Reset → boot2 (ROM) → crt0.S (SDK) → runtime_init → main() (弱符号)
  │
  ├─ cpsid i (关中断，冷初始化全程关中断)
  ├─ _led_init() (GPIO25 初始化)
  ├─ kmem_init() (内核堆初始化)
  ├─ task_module_init() (创建 idle 任务)
  ├─ sched_init() (就绪/睡眠队列初始化)
  ├─ task_create("boot_setup", ...) (创建启动任务)
  ├─ cpsie i (开中断)
  ├─ hal_console_init() (USB CDC + UART0 初始化)
  ├─ sched_start()
  │     │
  │     ├─ 取 boot_setup 任务
  │     ├─ msr psp = task->sp (r4-r11 区域)
  │     └─ svc #0 → SVC_Handler
  │           └─ PSP += 32 → 硬件栈帧 → bx EXC_RETURN
  │                 └─ 跳入 _boot_setup_task()
  │                       │
  │                       ├─ hal_systick_init() (ALARM3 启动 tick)
  │                       ├─ 打印 Banner
  │                       ├─ demo_app_init() (创建 shell 任务)
  │                       ├─ fatfs_init_and_mount()
  │                       └─ task_suspend(self) (启动任务结束)
  └─ 调度器开始轮转：shell ↔ idle ↔ (其他用户任务)
```

---

## 3. 目录结构

```
project/mini-kernel/
├── CMakeLists.txt              # 顶层构建脚本（分层编译 + 裁剪 + 体积检查）
├── build.bat                   # Windows 编译/烧录脚本（安全版，带日志+暂停）
├── toolchain-arm-none-eabi.cmake
├── platformio.ini
│
├── include/                    # 【对外头文件】用户应用仅包含此目录
│   ├── kernel.h                # 内核统一 API（系统调用封装）
│   ├── os_config.h             # 裁剪配置宏（体积红线+功能开关）
│   └── hal/
│       ├── hal_interface.h     # HAL 统一接口（ops 表）
│       └── flash_layout.h      # Flash 三分区定义（固件/MSC/Bootscript）
│
├── kernel/
│   ├── core/                   # 【内核核心】必编，不可裁剪
│   │   ├── kernel.c            # 内核主入口 + 启动流程 + tick_hook
│   │   ├── task.c / task.h     # 任务管理：TCB/状态机/创建/销毁/睡眠/挂起
│   │   ├── sched.c / sched.h   # 调度器：RR/权重/就绪队列/睡眠队列
│   │   └── mem.c / mem.h       # 内存管理：固定池 + 隐式链表堆
│   │
│   ├── syscall/                # 系统调用契约表（X-Macro 静态生成）
│   │   ├── syscall_contract.h
│   │   └── syscall_contract.c
│   │
│   └── modules/                # 【可裁剪模块】按 OS_CFG_* 开关编入
│       ├── shell/
│       │   ├── shell.c         # Shell 主逻辑：输入/解析/28+ 条命令
│       │   ├── shell_core.h    # 扩展命令注册 API（shell_register）
│       │   ├── shell_register.c
│       │   ├── shell_fs.c      # FS/MSC 扩展命令：ls/cd/msc mount/format...
│       │   ├── bootscript.c/h  # 固化命令：save/! 双备份 + CRC8 校验
│       │   └── bootscript.h
│       │
│       └── vfs/fatfs/
│           ├── diskio.c/h      # FatFs 磁盘 I/O（对接 Flash MSC 分区）
│           ├── ffconf.h        # FatFs 配置（FF_USE_MKFS=1）
│           └── fatfs_api.h     # f_mkfs / mount / 格式化封装
│
├── port/rp2040/                # 【RP2040 移植层】
│   ├── hal_port.c              # HAL ops 实现：SysTick/Console/GPIO/SPI/I2C/UART/Flash/USB
│   ├── hal_port.h
│   ├── context_switch.S        # ★ 核心：PendSV/SVC/HardFault/NMI/Invalid IRQ
│   ├── startup_rp2040.S        # 备用（当前用 SDK crt0.S，不参与链接）
│   ├── msc_usb.c               # USB 复合设备描述符 + MSC SCSI 回调
│   ├── msc_blockdev.c/h        # MSC 块设备（读 Flash MSC 分区扇区）
│   └── rp2040.ld               # 备用（当前用 SDK memmap_default.ld）
│
├── examples/builtin_demo/
│   └── demo_app.c              # 演示应用入口（创建 Shell 任务 + FatFs 初始化）
│
├── tests/
│   ├── unit/                   # 单元测试（Unity 框架）
│   │   ├── test_mem_mgmt.c
│   │   ├── test_sched.c
│   │   └── test_task_mgmt.c
│   ├── minimal_led_test.c      # 诊断固件 1：纯 SDK 闪灯（排除硬件问题）
│   └── usb_print_test.c        # 诊断固件 2：纯 SDK USB CDC 打印（排除 USB 通路）
│
├── cmake/check_size.cmake      # 构建后体积红线检查（解析 .map）
└── scripts/gen_config_macros.py
```

**关键代码引用**:
- 构建系统: [CMakeLists.txt](file:///e:/ppCD/project/mini-kernel/CMakeLists.txt)
- 编译脚本: [build.bat](file:///e:/ppCD/project/mini-kernel/build.bat)
- Flash 分区: [flash_layout.h](file:///e:/ppCD/project/mini-kernel/include/hal/flash_layout.h)

---

## 4. 核心内核模块

### 4.1 任务管理 (task.c/task.h)

**文件位置**: [task.c](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c) / [task.h](file:///e:/ppCD/project/mini-kernel/kernel/core/task.h)

#### 4.1.1 任务状态机

```
READY ──pick_next──▶ RUNNING ──time_slice=0──▶ (触发 PendSV) ──enqueue──▶ READY
  ▲                      │
  │ task_suspend         │ task_sleep
  │                      ▼
SUSPEND                SLEEP ──ticks_to_sleep=0──▶ READY (sched_sleep_tick)
  │                      ▲
  │ task_resume          │ task_wakeup
  ▼                      │
(任意状态可转为 DEAD 通过 task_destroy)
```

#### 4.1.2 TCB (任务控制块) 结构

| 字段 | 类型 | 偏移 | 说明 |
|------|------|------|------|
| `sp` | `uint32_t*` | 0 | 栈指针，指向 r4-r11 保存区域 |
| `state` | `task_state_t` | 4 | 任务状态：READY/RUNNING/SLEEP/SUSPEND/DEAD |
| `priority` | `uint8_t` | 8 | 优先级/权重（默认=priority） |
| `ticks_to_sleep` | `uint32_t` | 12 | 睡眠剩余 tick |
| `time_slice` | `uint32_t` | 16 | 剩余时间片（默认 5 tick） |
| `weight` | `uint32_t` | 20 | 权重（默认 1） |
| `stack_base` | `void*` | 24 | 栈底（高地址） |
| `stack_size` | `size_t` | 28 | 栈大小（字节） |
| `stack_magic` | `uint32_t` | 32 | 栈尾魔值 `0xDEADBEEF` |
| `next/prev` | `tcb_t*` | 36/40 | 链表指针（就绪/睡眠队列） |
| `name[12]` | `char` | 44 | 任务名 |
| `id` | `uint32_t` | 56 | 任务 ID（自增，idle=0） |
| **合计** | | **60B** | 每个 TCB 约 60 字节 |

#### 4.1.3 关键函数

| 函数 | 原型 | 说明 |
|------|------|------|
| `task_module_init()` | `void task_module_init(void)` | 创建 idle 任务，初始化任务池位图 |
| `task_create()` | `tcb_t* task_create(name, entry, arg, stack_size, priority)` | 分配 TCB+栈 → 初始化栈帧 → 入就绪队列 |
| `task_destroy()` | `void task_destroy(tcb_t *task)` | 从队列移除 → 释放栈+TCB → 释放槽位（**禁止销毁自身**） |
| `task_sleep()` | `void task_sleep(uint32_t ticks)` | 当前任务设 SLEEP → 移出就绪 → 入睡眠队列 → 触发 PendSV |
| `task_wakeup()` | `void task_wakeup(tcb_t *task)` | 从睡眠队列移除 → 设 READY → 入就绪队列 |
| `task_suspend()` | `void task_suspend(tcb_t *task)` | 设 SUSPEND → 从就绪/睡眠队列移除 → 若自挂起则 PendSV |
| `task_resume()` | `void task_resume(tcb_t *task)` | SUSPEND→READY → 入就绪队列 |
| `task_yield()` | `void task_yield(void)` | 触发 PendSV 让出 CPU |
| `idle_task_entry()` | `void idle_task_entry(void *arg)` | 空闲任务：轮询 USB + busy-wait 1ms（**绝对不调用 task_sleep**） |

#### 4.1.4 Idle 任务设计要点

> **【防卡死关键】** idle 任务 **绝对不能调用 task_sleep()**！idle 是就绪队列空时的兜底，若 idle 睡着，则：就绪队列空 + 睡眠队列无到期 → 调度器永久锁死（LED 停在某状态）。
>
> **非 TICKLESS 模式**：idle 100% 占用 CPU，用于：
> 1. 驱动 TinyUSB 状态机（`hal_usb_poll()` 每 1ms 一次）
> 2. 维持 USB CDC OUT 端点数据搬运
> 3. 提供"LED 停闪 = 卡死"的诊断信号（心跳由其他任务负责）

---

### 4.2 调度器 (sched.c/sched.h)

**文件位置**: [sched.c](file:///e:/ppCD/project/mini-kernel/kernel/core/sched.c) / [sched.h](file:///e:/ppCD/project/mini-kernel/kernel/core/sched.h)

#### 4.2.1 调度策略

- **算法**: 时间片轮转 (Round-Robin) + 权重比例
- **无抢占**: 仅时间片到期 / 主动 yield / 睡眠 / 挂起 时触发切换
- **触发源**: `hal_yield_trigger()` → 写 ICSR.PENDSVSET → PendSV 异常

#### 4.2.2 双队列结构

```
g_ready_head (双向循环链表)
  └─ READY 任务按到达顺序排队，FIFO 出队

g_sleep_head (双向链表，按 ticks_to_sleep 升序)
  └─ SLEEP 任务：sched_sleep_tick() 每个 SysTick 递减
     ticks_to_sleep=0 时自动迁移到 g_ready_head
```

#### 4.2.3 关键函数

| 函数 | 说明 | 关键修复点 |
|------|------|-----------|
| `_sched_ready_enqueue()` | 就绪队列入队（内部版本，调用方已关中断） | |
| `sched_ready_enqueue()` | 就绪入队（带 PRIMASK 保存/恢复） | 避免冷初始化阶段意外开中断 |
| `sched_ready_pick_next()` | 取队头并从队列移除 | **★ 关键修复**：remove 后 **不再 enqueue**，RUNNING 任务不在就绪队列；空队列返回 idle（idle 永不入队） |
| `_sched_sleep_enqueue()` | 入睡眠队列（按 ticks_to_sleep 升序插入） | |
| `sched_sleep_tick()` | 每 tick 递减睡眠计数，到期迁移就绪 | 在 SysTick 中断上下文调用 |
| `sched_start()` | 启动首任务：msr PSP → SVC #0 | PSP 指向 r4-r11 区域，SVC 异常帧不覆盖硬件栈帧 |
| **`sched_do_switch()`** | PendSV 内调用：保存 old_sp → 选 next → 返回 new_sp | **★ 核心防卡死**：仅当 `from->state == RUNNING` 时才设 READY 并入队；SLEEP/SUSPEND 由各自 API 管理队列归属，**不覆盖状态** |

#### 4.2.4 调度器防卡死核心契约

> **【sched_do_switch 状态保护】**
> 旧版 bug：无条件把 from 设 READY → 入就绪队列。若 task_sleep() 已把任务设 SLEEP 并移入睡眠队列，此处会覆盖成 READY → 任务同时在两个队列 → sleep 无效 → LED 爆闪。
>
> 修复：`if (from->state == RUNNING)` 才做 READY 转换，其他状态保持不变。

> **【sched_ready_pick_next 防止重复入队】**
> 旧版 bug：remove 后又 enqueue → 选中任务仍在队列 → sched_do_switch 再入队 → 同一任务出现两次 → next/prev 自引用环 → 永远只选它 → LED 爆闪 + ps 乱码。
>
> 修复：pick_next 只 remove 不 enqueue；RUNNING 任务不在就绪队列；切出时 sched_do_switch 再入队（仅当仍 RUNNING）。

---

### 4.3 内存管理 (mem.c/mem.h)

**文件位置**: [mem.c](file:///e:/ppCD/project/mini-kernel/kernel/core/mem.c) / [mem.h](file:///e:/ppCD/project/mini-kernel/kernel/core/mem.h)

#### 4.3.1 两级内存模型

```
┌─────────────────────────────────────────────────┐
│  kmalloc 内核堆 (隐式链表，首部带 block_header) │
│  ┌──────────┬──────┬──────────┬──────┬────────┐ │
│  │ block_1  │ free │ block_2  │ used │ free...│ │
│  └──────────┴──────┴──────────┴──────┴────────┘ │
│  位置：__end__ 之后 8KB (OS_CFG_HEAP_SIZE_BYTES) │
│  用途：任务栈 / TCB / kmalloc 动态分配           │
├─────────────────────────────────────────────────┤
│  mem_pool 固定内存池 (可选，bitmap 管理)          │
│  用途：内核对象（信号量/队列等）固定大小分配     │
└─────────────────────────────────────────────────┘
```

#### 4.3.2 堆块头结构

```c
typedef struct heap_block {
    size_t size;          /* 含头部的总大小；bit0=1 已分配，bit0=0 空闲 */
    struct heap_block *next;  /* 仅空闲块使用，构成空闲单链表 */
} heap_block_t;
```

- **分配算法**: 最先适配 (First Fit)
- **对齐**: 8 字节 (`BLOCK_ALIGN = 8`)
- **合并策略**: 释放时仅与**后向块**合并（前向合并未实现，简化实现）
- **临界区保护**: `kmalloc/kfree` 内部保存/恢复 PRIMASK

#### 4.3.3 关键函数

| 函数 | 说明 |
|------|------|
| `kmem_init(heap_start, heap_size)` | 堆初始化：整块设为一个大空闲块 |
| `kmalloc(size)` | 分配：8 字节对齐 + 头部 → 遍历空闲链 → 找到则分割（若剩余足够） |
| `kfree(ptr)` | 释放：设 FREE → 尝试与后向块合并（**★ 先从空闲链移除 next 再更新 size**）→ 插入空闲链表头 |
| `kmem_free_size()` | 统计当前总空闲字节（遍历空闲链累加） |
| `kmem_max_free_block()` | 最大连续空闲块大小 |

#### 4.3.4 内存防卡死关键修复

> **【kfree 后向合并：先移除再合并】**
> 旧版 bug：合并时只更新 block->size，未把 next 从空闲链表移除 → block 插入链头后，next 仍残留链表 → kmem_free_size 重复计数 → 从已合并区域重复分配 → 内存踩坏。
>
> 修复：合并前先从空闲链表找到并移除 next，再更新 block->size。

---

### 4.4 内核主入口 (kernel.c)

**文件位置**: [kernel.c](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c)

#### 4.4.1 冷启动 vs 热启动

| 阶段 | 执行位置 | 中断状态 | 任务 |
|------|---------|---------|------|
| **冷启动 Cold Init** | `main()` → `kernel_main()` | `cpsid i` (关中断) | kmem_init / task_module_init / sched_init / 创建 boot_setup |
| **热启动 Warm Init** | `_boot_setup_task()` (调度后运行) | `cpsie i` (开中断) | hal_systick_init / 打印 Banner / demo_app_init / FatFs / 自挂起 |

> **【关键架构修复】两阶段启动**
> 旧版崩溃根因：冷初始化 kmem/task 数据结构时就开中断 + USB 枚举 → USBCTRL_IRQ / SDK alarm1 IRQ 在数据结构未完成静态一致时触发 → TinyUSB 状态机与初始化代码非同步内存访问 → 静默数据损坏 → 稍后 HardFault。
>
> 修复：冷初始化全程关中断，创建 boot_setup 任务挂就绪队列，sched_start 启动调度器后再做热初始化。

#### 4.4.2 kernel_tick_hook

每个 SysTick (ALARM3 IRQ) 尾部调用，做两件事：
1. `sched_sleep_tick()`：递减睡眠队列，到期任务迁就绪
2. 当前任务 time_slice--，若归零则 `hal_yield_trigger()` 触发 PendSV

---

## 5. 硬件抽象层 HAL

### 5.1 HAL 统一接口 (hal_interface.h)

**文件位置**: [hal_interface.h](file:///e:/ppCD/project/mini-kernel/include/hal/hal_interface.h)

#### 5.1.1 导出表模式

内核只通过 `hal_export` 指针表调用 HAL 实现，移植新 MCU 只需实现表中 ops。

```c
typedef struct {
    const hal_systick_ops_t *systick;   /* 必选：SysTick 定时器 */
    const hal_console_ops_t *console;   /* 必选：调试串口 */

#if OS_CFG_PERIPH_SERVICE
    const hal_gpio_ops_t   *gpio;       /* 可选：GPIO */
    const hal_spi_ops_t    *spi;        /* 可选：SPI */
    const hal_i2c_ops_t    *i2c;        /* 可选：I2C */
    const hal_uart_ops_t   *uart;       /* 可选：UART */
    const hal_flash_ops_t  *flash;      /* 可选：板载 Flash */
#endif
#if OS_CFG_VFS && OS_CFG_FATFS
    const hal_sdcard_ops_t *sdcard;     /* 可选：SD 卡块设备 */
#endif
} hal_export_t;
```

---

### 5.2 RP2040 移植层 (hal_port.c)

**文件位置**: [hal_port.c](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c)

#### 5.2.1 SysTick 定时器 (ALARM3 + TIMER_IRQ_3)

| 配置项 | 值 | 说明 |
|--------|-----|------|
| Alarm ID | 3 | ALARM3，**不与 SDK alarm_pool (ALARM0) 冲突** |
| IRQ | TIMER_IRQ_3 | 不覆盖 SDK 的 TIMER_IRQ_0 |
| 优先级 | 0xFF (最低) | 不抢占 TinyUSB/SDK 关键中断 |
| 默认频率 | 1000 Hz (1ms) | 可通过 OS_CFG_TICK_HZ 修改 |

> **【★ USB 枚举修复：使用 ALARM3 而非 ALARM0】**
> 旧版致命 bug：`irq_set_exclusive_handler(TIMER_IRQ_0, ...)` 直接覆盖 SDK alarm_pool 已注册好的 ALARM0 handler → TinyUSB CDC/MSC 超时机制全失效 → USB 枚举一半卡死 → Windows 识别不出 COM 口/U盘。
>
> 修复：改用 ALARM3 + TIMER_IRQ_3，SDK 约定 ALARM1/2/3 留给用户自由使用。

> **【★ INTR 寄存器写 1 清】**
> 旧版致命 bug：用 `hw_clear_bits(&timer_hw->intr, mask)` 清中断 → W1C 寄存器写 0 无效 → 中断无限重入 → CPU 卡死在 TIMER_IRQ_3 → HardFault。
>
> 修复：`timer_hw->intr = KTICK_TIMER_BIT;`（直接写 bit 位，W1C 语义）。

#### 5.2.2 USB CDC 接收链路

**问题背景**：内核抢占调度（PendSV 任务切换）使 TinyUSB 内部 `dcd_int_disable()` 后配对的 `dcd_int_enable()` 未走回 → INTE=0 + NVIC ISER.21=0 → USB 数据到达但不处理 → Shell 输入死锁。

**终极解决方案（三层兜底）**：

```
1. 轮询驱动 hal_usb_poll()：
   ├─ 仅当 INTE==0 时恢复（先 dcd_int_enable，失败再写安全掩码 0x0009004D）
   ├─ _snapshot_setup_if_pending() 抓 SETUP 包
   ├─ dcd_int_handler(0) 处理挂起中断（处理 CDC SET_LINE_CODING 等 Control Request）
   ├─ tud_task_ext(0,0) 驱动 TinyUSB 状态机
   └─ _ep1_out_drain_all() 绕过 TinyUSB 接收软件层，直接读 EP1 OUT 硬件端点
        ├─ 读 BUFSTAT.EP1_OUT_AVAIL (bit2)
        ├─ 从 ADDR_ENDP1 → DPRAM 描述符 → 数据偏移
        ├─ 搬到私有 ring buffer (128B)
        └─ 清描述符 bit31 告知硬件"软件已拿走"

2. hal_console_getc_impl()：
   └─ 直接从私有 ring buffer 读字节（完全不依赖 SDK getchar_timeout_us）

3. idle 任务每 1ms 调用一次 hal_usb_poll()，确保 CDC OUT 数据不堆积
```

> **【安全掩码 0x0009004D】** 只用 TinyUSB 有 ack 代码的中断位：
> STALL_STATUS(bit0) | BUFF_STATUS(bit2) | ERROR(bit3) | EP0_SETUP_REQ(bit6) | BUS_RESET(bit16) | RESUME(bit19)
> **禁止** SOF(bit3) 等未处理的位，否则 CPU 100% 跑 USB 中断死循环。

#### 5.2.3 板载 Flash 操作 (W25Q16JV 2MB)

- **擦除粒度**: 4KB 扇区对齐 (`HAL_FLASH_SECTOR_SIZE = 4096`)
- **写入粒度**: 256B 页（非对齐写入自动按页合并，允许 1→0 不擦）
- **运行时限制**: 擦/写期间暂停 XIP + 关中断（SDK ROM 函数要求）
- **CRC8**: SMBUS 多项式 `x^8+x^5+x^4+1 (0x07)`，供 bootscript 校验用

---

### 5.3 上下文切换 (context_switch.S)

**文件位置**: [context_switch.S](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S)

#### 5.3.1 栈帧布局（★ 必须严格匹配）

```
stack_top (高地址) ────────────────────────────────────┐
                                                        │
  [硬件栈帧 32B] ── 8 个寄存器（异常进入/退出时硬件自动 push/pop）
    offset +32: xPSR  (T-bit=1, 0x01000000)
    offset +28: PC    (entry | 1, Thumb LSB=1)
    offset +24: LR    (0 = noreturn 任务不会返回，若返回触发 HardFault)
    offset +20: R12   (0)
    offset +16: R3    (0)
    offset +12: R2    (0)
    offset  +8: R1    (0)
    offset  +4: R0    (arg, entry 的唯一参数)
                                                        │
  [软件栈帧 32B] ── 8 个寄存器（PendSV 手动 push/pop）
    offset +28: R11
    offset +24: R10
    offset +20: R9
    offset +16: R8
    offset +12: R7
    offset  +8: R6
    offset  +4: R5
    offset  +0: R4  ◀── task->sp 指向这里（ctx->sp 初始化值）
                                                        │
stack_bottom (低地址)
```

**总栈帧大小**: 64 字节（r4-r11 32B + 硬件栈帧 32B）

#### 5.3.2 hal_context_init（任务初始栈帧构建）

为新任务构造"异常返回现场"，使首次 PendSV/SVC 切换能直接跳入 `entry(arg)`：

```
输入: r0=ctx, r1=stack_top, r2=entry, r3=arg
操作:
  1. stack_top 8 字节对齐
  2. sp = stack_top - 64  (r4-r11 32B + 硬件栈帧 32B)
  3. 写硬件栈帧:
     [sp+32] = r3 (R0=arg)
     [sp+36..48] = 0 (R1-R12)
     [sp+52] = 0 (LR, noreturn)
     [sp+56] = entry | 1 (PC Thumb LSB=1 ★)
     [sp+60] = 0x01000000 (xPSR T-bit=1)
  4. ctx->sp = sp (指向 r4-r11 起始)
```

> **【PC Thumb 位强制 1】** Cortex-M 无 ARM 状态，PC LSB=0 立即触发 INVSTATE UsageFault → 升级 HardFault。

> **【LR = 0 触发 HardFault】** noreturn 任务若意外返回，LR=0 不是有效地址 → 取指 BusFault → HardFault（5Hz 快闪诊断，非 Lockup）。

#### 5.3.3 PendSV_Handler（任务切换核心）

```
PendSV 进入:
  Step 1 ─ 保存 from 任务 r4-r11:
    mrs r0, psp                  ; PSP = 硬件 pop 后位置（PSP 已 -32）
    subs r0, r0, #32             ; 再 -32 预留 r4-r11
    str r4-r7, [r0+0..12]        ; 保存低 4 callee-saved
    mov r1-r11, str [r0+16..28]  ; 保存高 4 callee-saved

  Step 2 ─ 调 C 调度器:
    mov r4, lr                   ; ★ 先存 EXC_RETURN 到 r4（bl 会覆盖 LR）
    bl sched_do_switch           ; r0 = sched_do_switch(old_sp) → new_sp
                                 ; sched_do_switch: 保存 from->sp, 选 next, 返回 to->sp

  Step 3 ─ 恢复 to 任务 r4-r11:
    ldr/mov r8-r11, [r0+16..28]  ; 恢复高 4 callee-saved
    mov lr, r4                   ; ★ 恢复 EXC_RETURN 到 LR（在覆盖 r4 之前）
    ldr r4-r7, [r0+0..12]        ; 恢复低 4 callee-saved
    adds r0, r0, #32             ; PSP = r4-r11 区域 + 32 = 硬件栈帧位置

  Step 4 ─ 退出异常:
    msr psp, r0
    bx lr                        ; LR = 0xFFFFFFFD: Thread mode + PSP 弹栈
                                 ; 硬件自动 pop r0-r3/r12/lr/pc/xpsr → 跳入 to 任务
```

> **【★ EXC_RETURN 保存到 r4】** `bl sched_do_switch` 会把 LR 覆盖成 bl 的返回地址。进入 PendSV 时 LR=EXC_RETURN(0xFFFFFFFD)，若不保存则 bx lr 无法异常返回，直接跳到 bl 下一条 → PC 乱飞 → HardFault。
>
> 修复：bl 前 `mov r4, lr`，sched_do_switch 遵循 AAPCS 保持 r4 不变；bl 后 `mov lr, r4` 恢复。

> **【★ r4-r11 完整保存/恢复】** 旧版只有 `bx lr`，硬件仅恢复 r0-r3 等 8 个寄存器，Callee-saved r4-r11 全未保存 → 任务切回时局部变量/循环计数 = B 任务的垃圾值 → 随机崩溃/死循环/内存踩坏。修复：严格 AAPCS 保存/恢复 8 个 callee-saved。

#### 5.3.4 SVC_Handler（启动首任务）

```
SVC 进入:
  mrs r0, psp          ; PSP = sched_start 设置的 task->sp（r4-r11 区域）
                       ; ★ SVC 异常帧压到 MSP，不影响 PSP！
  adds r0, r0, #32     ; PSP += 32 → 指向硬件栈帧（r0=arg, pc=entry|1...）
  msr psp, r0
  ldr r0, =0xFFFFFFFD  ; EXC_RETURN
  bx r0                ; 硬件从 PSP pop 8 寄存器 → 跳入首任务
```

> **【SVC PSP 偏移 = +32 不是 +64】** 旧误用 +64 → PSP 越过硬件栈帧 → 弹出垃圾 PC → HardFault。正确：SVC 异常帧压到 MSP，PSP 保持 task->sp，只需 +32 到硬件栈帧。

#### 5.3.5 HardFault / NMI / Invalid IRQ 诊断处理（防卡死第一道防线）

**【关键原则】** 异常处理中**绝不写 IO_BANK0 / PADS_BANK0 / SIO_OE 等 APB 寄存器**，仅写 SIO_OUT_SET/CLR (AHB-Lite 单周期端口)。若总线已异常，写 APB 外设会在 HardFault 压栈阶段再 Fault → CPU 双 Fault **Lockup 状态**（LED 永远灭，连快闪诊断都看不到）。

| 异常 | 处理方式 | LED 模式 | 说明 |
|------|---------|---------|------|
| **HardFault** | 死循环快闪 | **5Hz 100ms on/off** | 典型：栈帧错位、内存踩坏、INVSTATE、总线错 |
| **NMI** | 3 连闪 + 长停顿 循环 | 3×160ms burst + 480ms pause | 调试器/NMI 引脚触发 |
| **Invalid IRQ** | 直接跳 HardFault | 同 5Hz 快闪 | 替换 SDK 默认 `bkpt #0`（bkpt 在 HardFault 嵌套会 Lockup）|
| **Systick 意外返回** | 1s 超慢闪 | 500ms on / 500ms off | sched_start 意外返回，与 HardFault 5Hz 区分 |

> **【★ 向量命名用 SDK 的 isr_xxx，不用 CMSIS 别名 .set】**
> 旧版用 `.set isr_pendsv, PendSV_Handler` 做符号别名，GAS 不保证把 PendSV_Handler 的 Thumb LSB=1 传递给 isr_pendsv → 向量表 bit0=0 → CPU 以为切 ARM 状态 → INVSTATE → HardFault → HardFault 向量也 LSB=0 → **双 Fault Lockup**。
>
> 修复：主标签**直接就是** SDK 的 `isr_pendsv / isr_svcall / isr_hardfault / isr_nmi / isr_invalid`，并在其身上贴 `.thumb_func` + `.type %function` → 汇编器 100% 给 LSB=1；CMSIS 名作为紧跟其后的同址别名标签。

---

## 6. 系统调用层

**文件位置**: [syscall_contract.h](file:///e:/ppCD/project/mini-kernel/kernel/syscall/syscall_contract.h)

X-Macro 静态编译期生成的契约表，记录每个 syscall 的元信息（ID/名称/参数/返回类型/签名）。

### 系统调用号列表

| ID | 枚举 | 名称 | 参数 | 说明 |
|----|------|------|------|------|
| 0 | `SYS_TASK_CREATE` | task_create | 5 | 创建任务 |
| 1 | `SYS_TASK_DESTROY` | task_destroy | 1 | 销毁任务 |
| 2 | `SYS_TASK_YIELD` | task_yield | 0 | 让出 CPU |
| 3 | `SYS_TASK_SLEEP` | task_sleep | 1 | 睡眠 N tick |
| 4 | `SYS_TASK_SUSPEND` | task_suspend | 1 | 挂起任务 |
| 5 | `SYS_TASK_RESUME` | task_resume | 1 | 恢复任务 |
| 10 | `SYS_KMALLOC` | kmalloc | 1 | 堆分配 |
| 11 | `SYS_KFREE` | kfree | 1 | 堆释放 |
| 20 | `SYS_CONSOLE_PUTC` | console_putc | 1 | 控制台输出单字符 |
| 21 | `SYS_CONSOLE_GETC` | console_getc | 1 | 控制台输入单字符 |
| 30-33 | GPIO 系列 | gpio_init/write/read/toggle | 1-3 | GPIO 操作 |
| 40 | SPI_XFER | spi_xfer | 4 | SPI 收发 |
| 50-51 | I2C TX/RX | i2c_write/read | 4 | I2C 主模式收发 |
| 60-61 | UART RD/WR | uart_write/read | 4 | UART 异步读写 |

> **当前实现说明**：内核运行在特权级，任务与内核共享同一地址空间，系统调用通过函数直接调用（非 SVC 指令陷入）。SVC #0 仅用于 `sched_start()` 启动首任务的首次切换。

---

## 7. 服务模块

### 7.1 Shell 命令行

**文件位置**: [shell.c](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c) / [shell_core.h](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell_core.h)

#### 7.1.1 Shell 架构

```
Shell 任务 (task_stack=2048B, weight=1)
  │
  ├─ 主循环:
  │    hal_console_getc() (内含 hal_usb_poll + EP1 OUT drain)
  │      │
  │      ├─ 普通字符 → g_shell_line[g_shell_pos++]
  │      ├─ \b → 退格 (pos--, 输出 \b \033[K)
  │      └─ \r\n → 解析执行 + 恢复提示符
  │
  ├─ 解析: strtok 按空格切 argv (最多 SHELL_MAX_ARGS=16)
  ├─ 查找: 先扫静态命令表 (~20条) → 再扫扩展命令表 (~8条: msc/ls/cd...)
  └─ 执行: cmd_handler(argc, argv) → 打印结果
```

#### 7.1.2 核心命令表

| 命令 | 参数 | 说明 |
|------|------|------|
| `help` | [cmd] | 列出所有命令或指定命令帮助 |
| `ps` | - | 任务列表：ID/Name/State/TicksLeft/StackBase/StackSize |
| `heap` | - | 堆状态：总空闲/最大空闲块 |
| `tick` | - | 当前 tick 计数 |
| `version` | - | 内核版本号 |
| `suspend` | <id> | 挂起指定任务 |
| `resume` | <id> | 恢复指定任务 |
| `kill` | <id> | 销毁指定任务（Linux 风格任务控制）|
| `jobs` | - | 列出后台任务 |
| `clear` | - | 清屏（VT100 序列）|
| `led` | on/off/blink | 板载 LED 控制 |
| `gpio` | init/write/read/toggle | GPIO 操作（含 help 子命令）|
| `i2c` | init/scan/write/read/cmds/fill | I2C 总线操作（SSD1306 OLED 支持）|
| `syscalls` | - | 遍历系统调用契约表 |
| `vtest` | status/start | 三任务嵌套调度稳定性验证工具 |
| **bootscript 固化** | | |
| `save` | <cmd...> | 固化命令到 Flash（开机自动执行）|
| `unsave` | <index> | 删除第 N 条固化命令 |
| `list` / `!` | - | 列出所有固化命令 / 立即全部重放 |
| `boot status` | - | 启动回放结果快照 |
| `factory_reset` | confirm | 擦除所有固化命令（两步确认）|
| **FatFs / MSC** | | |
| `msc mount` | - | 主机 USB 可写 U 盘（Shell 只读）|
| `msc eject` | - | 主机显示"无介质"（Shell 可读写）|
| `msc status` | - | MSC 分区状态 |
| `msc format` | confirm | 两步确认后重建 FAT16 |
| `ls` | [path] | 列出目录（真实 FAT16 目录，非地址索引）|
| `cd` | <path> | 切换当前目录 |
| `pwd` | - | 打印当前工作目录 |
| `mkdir` | <name> | 创建子目录 |
| `rmdir` | <name> | 删除空目录 |
| `rm` | <file> | 删除文件 |
| `cat` | <file> | 查看文件内容 |

#### 7.1.3 Readline 风格输入保护（v2.2.5 新增）

后台任务输出会打断用户输入行。Shell 提供 `shell_async_enter() / shell_async_exit()` 保护：
- 进入：`\r` + `\033[K` 清除当前行
- 退出：恢复 `mk> ` 提示符 + 用户已输入的字符

---

### 7.2 FatFs / USB MSC

#### 7.2.1 USB 复合设备

- **Interface 0**: CDC ACM（虚拟串口）
- **Interface 1**: MSC SCSI（可移动磁盘）
- **描述符**: [msc_usb.c](file:///e:/ppCD/project/mini-kernel/port/rp2040/msc_usb.c) 提供 Composite 描述符，覆盖 SDK 单 CDC 默认描述符

#### 7.2.2 FatFs 关键配置

| 宏 | 值 | 说明 |
|----|-----|------|
| `FF_FS_READONLY` | 0 | 读写 |
| `FF_USE_MKFS` | 1 | 支持 f_mkfs（格式化 FAT16）|
| `FF_MAX_SS` | 512 | 扇区大小（与 MSC 标准一致）|
| `FF_VOLUMES` | 1 | 单卷（Flash MSC 分区）|

#### 7.2.3 写互斥模式

主机 USB (MSC) 与 Shell (FatFs) 不能同时写，否则 FAT 表损坏。通过 `msc` 命令切换：

| 模式 | Shell 访问 | 主机 USB 访问 | 触发命令 |
|------|-----------|--------------|---------|
| **Shell 独占** (默认) | 可读写（mkdir/rm/cat 全可用）| 显示"无介质" | `msc eject` |
| **主机可写** | 只读（ls/cat 可用，mkdir/rm 拒绝）| 完整读写（U 盘盘符）| `msc mount` |

---

### 7.3 Bootscript 固化命令

**文件位置**: [bootscript.c](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/bootscript.c) / [bootscript.h](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/bootscript.h)

#### 7.3.1 Flash 双备份结构

- **SEC_A**: `0x1FE000` (2MiB - 8KB)
- **SEC_B**: `0x1FF000` (2MiB - 4KB)

写入流程：先擦 SEC_A → 写 SEC_A → 校验 CRC8 通过 → 再写 SEC_B 冗余备份。
读取流程：读 SEC_A → CRC 正确则用；否则回退 SEC_B。

每条命令存储格式：
```
[len:1B][flags:1B][crc8:1B][payload:len 字节]
len=0xFF 表示数组结束
最多 32 条命令
```

#### 7.3.2 Boot 回放时机

`shell_start()` 创建 Shell 任务前，**同步调用** `bootscript_run_all()` 跑完所有固化命令 → 回放结果写入 RAM 的 boot status 缓冲区 → 再进入 Shell 交互模式。即使开机时没接串口，用户事后打开终端也能通过 `boot status` 查看启动回放结果。

---

## 8. Flash 分区布局

**文件位置**: [flash_layout.h](file:///e:/ppCD/project/mini-kernel/include/hal/flash_layout.h)

```
2 MiB (0x00200000) ───────────────────────────────────────────────┐
                                                                    │
  Bootscript SEC_B (4 KiB)          offset: 0x001FF000  ←─────────┤
  (备份：CRC8 校验失败时回退)                                       │
  0x001FF000 ~ 0x001FFFFF                                          │
                                                                    │
  Bootscript SEC_A (4 KiB)          offset: 0x001FE000  ←─────────┤
  (主区：每次 save 先写这里)                                        │
  0x001FE000 ~ 0x001FEFFF                                          │
                                                                    │
  ┌─────────────────────────────────────────────────────────────┐ │
  │  MSC 数据盘 (FAT16)                                           │ │
  │  大小: 2032 sectors × 512B/sector = 1,040,384 Bytes          │ │
  │  扇区范围: LBA 0 ~ LBA 2031                                   │ │
  │  offset: 0x00100000 ~ 0x001FDFFF  (恰好对齐 SEC_A 开始)       │ │
  │  Windows/Mac/Linux 作为"可移动磁盘"读写                       │ │
  │  Shell: msc mount 前可读写，之后只读                          │ │
  └─────────────────────────────────────────────────────────────┘ │
                                                                    │
  Firmware 固件区 (1 MiB)            offset: 0x00000000  ←─────────┤
  0x00000000 ~ 0x000FFFFF                                          │
  XIP 执行：mini-kernel + demo_app + Shell + FatFs + TinyUSB       │
  (当前固件约 195KB，1MiB 充足，未来翻倍也够)                        │
                                                                    │
0x00000000 ───────────────────────────────────────────────────────┘
```

**边界校验（编译期断言）**:
- `MSC_END_EXCLUSIVE == BOOTSCRIPT_A_START`：严丝合缝，无重叠无间隙
- `SEC_A + 4KB == SEC_B`：相邻
- `SEC_B + 4KB == FLASH_END`：占用最后 2 个扇区
- `MSC_OFFSET % 4KB == 0`：扇区擦除对齐

---

## 9. 内核防卡死机制详解

这是 Mini Kernel 设计中**投入最大、修复最彻底**的部分，涵盖启动阶段、运行阶段、异常阶段三层防护，共 20+ 处经过线上验证的防卡死设计点。

### 9.1 LED 诊断闪灯系统（第一层：肉眼可定位）

| LED 模式 | 节奏 | 含义 | 可能原因 | 处理建议 |
|---------|------|------|---------|---------|
| **500ms 心跳闪** | 250ms on / 250ms off | ✅ **系统正常运行** | task_led 在跑，调度器轮转正常 | 正常操作 |
| **常亮或常灭** | 不变 | ❌ **早期初始化失败** | main() 进入前 / _led_init 之前崩；或 CPU Lockup | 检查电源；断电重插；把 `MK_BOOT_DIAG_LED=1` 重新编译看 stage 闪 |
| **5Hz 快闪** | 100ms on / 100ms off (爆闪) | ❌ **HardFault** | 栈帧错位、PC 乱飞、内存踩坏、INVSTATE、总线错 | 读反汇编 `.disasm`；检查栈帧偏移；查看 PSP/PC/LR |
| **1Hz 超慢闪** | 500ms on / 500ms off | ⚠️ **调度器未启动** | main() / sched_start() 意外返回；boot_setup 任务未被调度到 | 检查 TCB 初始化；检查 sched_ready_pick_next 是否正确返回 boot_setup |
| **3 连闪 + 停顿** | 3×160ms burst + 480ms pause | ⚠️ **NMI 触发** | 调试器连接 / SWD NMI 引脚 | 正常调试行为，无需处理 |
| **启动阶段 1~8 闪** | N×250ms + 1s 停顿 (MK_BOOT_DIAG_LED=1) | 🔍 **启动诊断** | 崩溃在冷初始化的哪个阶段 | 数闪次数对应 kernel.c 的 _led_stage(N) |

### 9.2 启动阶段防卡死（冷初始化 → 热初始化 两阶段）

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.2.1 | **冷初始化全程关中断 (cpsid i)** | [kernel.c#L210](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L210) | USBCTRL_IRQ / SDK alarm 在 kmem/task 数据结构未初始化时触发 → 静默数据损坏 → 稍后 HardFault |
| 9.2.2 | **Boot_setup 任务化** | [kernel.c#L256](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L256) | 热初始化（打印/Shell/FatFs）在调度器运行后执行，核心状态已稳定 |
| 9.2.3 | **boot_setup task_suspend 后不再复活** | [kernel.c#L193-L194](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L193-L194) | 旧版 suspend+sleep 组合导致 SUSPEND→SLEEP→被 tick 唤醒→再次执行 demo_app_init→重复创建任务→TCB 池指针覆盖→悬空 TCB→ps 乱码→LED 爆闪。修复：suspend 后 while(1) 空转等 PendSV |
| 9.2.4 | **ALARM3 而非 ALARM0** | [hal_port.c#L67-L69](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L67-L69) | 不覆盖 SDK alarm_pool handler → USB 枚举正常完成 → 不出现 COM 口完全不识别 |
| 9.2.5 | **stdio_init_all 在 MSP+开中断后调用** | [kernel.c#L270-L275](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L270-L275) | USB CTRL IRQ 注册 + 枚举依赖正确上下文，在 PSP 任务中调用会导致 USB 不枚举 |
| 9.2.6 | **PICO_STDIO_USB_STDOUT_TIMEOUT_US=0** | [CMakeLists.txt#L159](file:///e:/ppCD/project/mini-kernel/CMakeLists.txt#L159) | CDC FIFO 满不阻塞 60s/字符 → 不阻塞在 banner 打印 → sched_start 能及时执行 |
| 9.2.7 | **PICO_STDIO_USB_CONNECTION_WITHOUT_DTR=1** | [CMakeLists.txt#L151](file:///e:/ppCD/project/mini-kernel/CMakeLists.txt#L151) | 兼容 PuTTY 等不拉 DTR 的终端 → 不会丢弃所有输出 |
| 9.2.8 | **删除 5s USB 枚举忙等** | [kernel.c#L289-L291](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L289-L291) | 50M nop 循环 ≈ 5s 完全浪费 CPU；USB 枚举由中断驱动，不需要忙等，启动时间从 10s+ → <500ms |

### 9.3 调度器防卡死（任务不饿死、队列不损坏）

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.3.1 | **RUNNING 任务不在就绪队列** | [sched.c#L53-L75](file:///e:/ppCD/project/mini-kernel/kernel/core/sched.c#L53-L75) | pick_next 只 remove 不 enqueue，防止同一任务出现两次 → 链表自引用环 → 永远只选一个任务 → LED 爆闪 |
| 9.3.2 | **idle 永不入就绪队列** | [task.c#L61-L67](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c#L61-L67) | idle 是空队列兜底；若入队则被 pick_next 选为 RUNNING 移除后丧失兜底语义 |
| 9.3.3 | **sched_do_switch 仅 RUNNING 时 READY** | [sched.c#L205-L213](file:///e:/ppCD/project/mini-kernel/kernel/core/sched.c#L205-L213) | SLEEP/SUSPEND 状态不被覆盖，否则 task_sleep 无效 → 任务 tight loop → LED 爆闪 |
| 9.3.4 | **idle 绝对不调用 task_sleep** | [task.c#L288-L313](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c#L288-L313) | 所有用户任务都睡眠时 idle 兜底；若 idle 也睡则无任何就绪任务 → 调度器永久锁死 |
| 9.3.5 | **PendSV 优先级最低 (SHPR3[22]=0xFF)** | [hal_port.c#L108](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L108) | 不抢占 TIMER_IRQ_3 tick 中断，避免调度器重入 + 队列损坏 |
| 9.3.6 | **tick_hook 不跳过 idle** | [kernel.c#L316-L328](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L316-L328) | 旧版若 current=idle 则跳过 tick 递减 → idle time_slice 永远不归零 → 其他任务永远抢不到 CPU → 表现同卡死 |
| 9.3.7 | **task_destroy 禁止销毁自身** | [task.c#L134](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c#L134) | TCB/栈释放后调度器仍访问 → use-after-free → 链表随机崩溃 |
| 9.3.8 | **task_destroy 从对应队列移除** | [task.c#L147-L160](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c#L147-L160) | 旧版只从就绪队列移除，SLEEP 任务仍在睡眠队列 → sched_sleep_tick 递减已释放内存 → 归零时把悬空 TCB 入就绪队列 → 链表损坏 |

### 9.4 上下文切换 / 异常防卡死（栈帧精确 + 不 Lockup）

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.4.1 | **栈帧总大小 64B** | [context_switch.S#L72-L73](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L72-L73) | r4-r11 (32B) + 硬件栈帧 (32B) = 64B，缺一不可 |
| 9.4.2 | **task->sp 指向 r4-r11 起始** | [context_switch.S#L95-L96](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L95-L96) | PendSV pop r4-r11 时从正确位置读；to->sp + 32 = 硬件栈帧位置 |
| 9.4.3 | **PC Thumb LSB 强制 1** | [context_switch.S#L79-L81](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L79-L81) | Cortex-M 无 ARM 状态，LSB=0 → INVSTATE → HardFault |
| 9.4.4 | **LR = 0（noreturn 守卫）** | [context_switch.S#L85-L90](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L85-L90) | 任务意外返回时 LR=0 → 取指错 → HardFault（5Hz 快闪，非 Lockup）|
| 9.4.5 | **xPSR T-bit = 1** | [context_switch.S#L92-L93](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L92-L93) | 异常返回后进入 Thumb 模式 |
| 9.4.6 | **SVC PSP 偏移 +32（不是 +64）** | [context_switch.S#L263-L265](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L263-L265) | SVC 异常帧压 MSP 不影响 PSP；正确偏移 = 只跳 r4-r11 区域 |
| 9.4.7 | **PendSV 先存 EXC_RETURN 到 r4** | [context_switch.S#L188-L189](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L188-L189) | `bl sched_do_switch` 覆盖 LR，不保存则异常返回用错误 LR → PC 乱飞 |
| 9.4.8 | **先恢复高 r8-r11，再恢复低 r4-r7，最后设 LR** | [context_switch.S#L192-L206](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L192-L206) | 在覆盖 r4 之前必须先把 EXC_RETURN 从 r4 搬到 LR，顺序错一个就 HardFault |
| 9.4.9 | **r4-r11 完整保存/恢复（8 个 callee-saved）** | [context_switch.S#L165-L178](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L165-L178) | 旧版漏保存 → 任务切回时 r4-r11=别人的垃圾值 → 随机崩溃/死循环 |
| 9.4.10 | **AAPCS 兼容：hal_context_init 保存/恢复 r4** | [context_switch.S#L44/L98](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L44) | 旧版用 r4 作 scratch 不保存 → 覆盖调用者 g_idle_task 指针 → idle.sp=0 → HardFault |
| 9.4.11 | **向量命名用 isr_xxx (SDK 名) + .thumb_func** | [context_switch.S#L157-L163](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L157-L163) | 旧 .set 别名不传递 Thumb LSB → 向量表 bit0=0 → INVSTATE → HardFault 向量也 LSB=0 → **双 Fault Lockup（LED 全灭）** |
| 9.4.12 | **HardFault 只写 SIO_OUT_SET/CLR** | [context_switch.S#L291-L303](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L291-L303) | 不碰 IO_BANK0/PADS_BANK0 等 APB 寄存器，避免 HardFault 阶段再总线错 → Lockup |
| 9.4.13 | **Invalid IRQ = HardFault 同处理** | [context_switch.S#L356-L361](file:///e:/ppCD/project/mini-kernel/port/rp2040/context_switch.S#L356-L361) | 替换 SDK 默认 `bkpt #0`（bkpt 嵌套 HardFault 会 Lockup）|

### 9.5 USB 接收链路防卡死（三层兜底）

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.5.1 | **条件式 INTE 恢复：仅 INTE==0 才干预** | [hal_port.c#L354-L372](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L354-L372) | 不覆盖 TinyUSB 自身正常管理的 INTE 值，避免竞争；只有 dcd_int_disable() 被抢占未配对恢复时才兜底 |
| 9.5.2 | **安全掩码 0x0009004D，不含 SOF** | [hal_port.c#L312-L316](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L312-L316) | 避免 SOF 等未处理中断位 → CPU 100% 跑 USB IRQ 死循环 → 其他任务抢不到时间片 → 启动画面截断卡死 |
| 9.5.3 | **SETUP/Control Request 必须同步处理** | [hal_port.c#L293-L310](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L293-L310) | PuTTY 打开串口时 Windows 发 SET_LINE_CODING + SET_CONTROL_LINE_STATE → 若不应答则 CDC 不就绪 → **绝不向 EP1 OUT 发任何数据包** → Shell 输入永远空 |
| 9.5.4 | **绕过 TinyUSB 接收层，直接读 EP1 OUT 硬件端点** | [hal_port.c#L234-L289](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L234-L289) | 终极兜底：即使 TinyUSB 内部状态机完全异常，也能从 DPRAM → 私有 ring buffer 取字节 |
| 9.5.5 | **私有 ring buffer 128B + 临界区保护** | [hal_port.c#L221-L289](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L221-L289) | 生产者 (EP1 drain) 和消费者 (getc) 多任务下 head/tail 不原子 → 关中断保护，防止数据丢失 |
| 9.5.6 | **idle 任务每 1ms 调 hal_usb_poll()** | [task.c#L302-L309](file:///e:/ppCD/project/mini-kernel/kernel/core/task.c#L302-L309) | 所有用户任务睡眠时，idle 独占 CPU 驱动 USB 状态机，不丢 CDC OUT 包 |
| 9.5.7 | **NVIC ISER bit21 同步兜底** | [hal_port.c#L369-L371](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L369-L371) | dcd_int_enable 可能因抢占未设置 NVIC 位，手动写 ISER.21=1 确保 USBCTRL_IRQ 能被 NVIC 响应 |

### 9.6 内存防卡死

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.6.1 | **kmalloc/kfree 保存/恢复 PRIMASK** | [mem.c#L117-L122](file:///e:/ppCD/project/mini-kernel/kernel/core/mem.c#L117-L122) | 不无条件 cpsie i；冷初始化（已关中断）调用 kmalloc 时不会意外开中断 → 初始化中数据结构不被 IRQ 打断 |
| 9.6.2 | **kfree 后向合并：先从空闲链移除 next** | [mem.c#L141-L152](file:///e:/ppCD/project/mini-kernel/kernel/core/mem.c#L141-L152) | 旧版不移除 → kmem_free_size 重复计数 → 从已合并区域重复分配 → 内存踩坏 → 随机 HardFault |
| 9.6.3 | **栈尾魔值 0xDEADBEEF + task_stack_check()（v2.2.7 已启用）** | [task.h#L73-L85](file:///e:/ppCD/project/mini-kernel/kernel/core/task.h#L73-L85) + [kernel.c#L327-L337](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L327-L337) | 每个 SysTick 检查当前任务栈底魔值；溢出即打印诊断 + 挂起该任务，防止踩坏扩散为 HardFault（此前检查函数存在却从未被调用） |

### 9.7 Shell 输入防卡死

| 编号 | 机制 | 代码位置 | 防止的卡死场景 |
|------|------|---------|--------------|
| 9.7.1 | **hal_console_getc_impl 内含 usb_poll** | [hal_port.c#L413-L429](file:///e:/ppCD/project/mini-kernel/port/rp2040/hal_port.c#L413-L429) | Shell 等待输入的每一步都在驱动 USB 状态机，不出现"开中断但不 poll → USB 死锁" |
| 9.7.2 | **SHELL_MAX_ARGS=16 + 溢出警告** | [shell.c#L55-L58](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L55-L58) | 旧版 10 → 静默截断 `i2c cmds 0 0x3C + 8 字节` 等长命令 → 参数丢失 → I2C 命令异常 |
| 9.7.3 | **自动跳过前缀提示符 mk> / shell> / >** | Shell 解析逻辑 | 防止复制粘贴命令时把 `mk> ` 当作第一个参数，导致 "Unknown command 'mk>'" |

### 9.8 vtest 任务拆除防卡死（v2.2.7 修复）

**现象**：`vtest start` 后执行 `vtest stop`（或 Ctrl+C），VT3 打印中途输出错乱（与 Shell 输出逐字符交错），随后 LED 5Hz 爆闪 = HardFault。

**根因链**（三层叠加）：
1. **堆 93% 满载**：8KB 堆中 VT1/VT2/VT3 三任务栈+TCB 只剩 ~792B，堆块紧密相邻——VT3 栈底紧贴 VT2 的 TCB。
2. **VT3 栈 1024B 不足**：停止时 VT3 走进最深打印链路 `vt_task_ctrl → shell_async_enter → sh_puts → hal_console_putc → putchar_raw → stdio_usb_out_chars → tud_cdc_n_write`，叠加 PendSV 64B + IRQ 嵌套压栈，越过 1024B 即写穿下方 VT2 的 TCB（sp/state/next/prev 在头 72B）。
3. **vtest_stop_all 盲等 5ms 不是握手**：被唤醒任务 5ms 内根本停不下来——VT2 恢复在 I2C chunk 循环里还剩 ~90ms 传输；VT3 恢复在 task_sleep(3000) 里还会 `task_resume(g_vt2)`（可能读已释放 TCB）再睡 7s。destroy 与 VT3 垂死代码形成 use-after-free 竞态窗口。

| 编号 | 修复 | 代码位置 | 说明 |
|------|------|---------|------|
| 9.8.1 | **内核堆 8KB → 16KB** | [os_config.h#L53](file:///e:/ppCD/project/mini-kernel/include/os_config.h#L53) | 消除 93% 满载，栈与 TCB 不再紧贴（RP2040 256KB SRAM 充足） |
| 9.8.2 | **VT1 栈 384→512，VT3 栈 1024→2048** | [shell.c#L1823-L1825](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1823-L1825) | 覆盖 USB 控制台深链路 + PendSV 64B + IRQ 嵌套的最坏栈深 |
| 9.8.3 | **stop_all 盲等 5ms → 等停靠（上限 200ms）** | [shell.c#L1713-L1731](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1713-L1731) | 轮询等三任务全部 SUSPEND 才 destroy；超时放弃 destroy 并提示重试（宁可泄漏 TCB 也不踩 freed 内存） |
| 9.8.4 | **VT2 chunk 循环 / VT3 resume 块加 flag 复查** | [shell.c#L1562-L1565](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1562-L1565) + [shell.c#L1677-L1681](file:///e:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1677-L1681) | 被唤醒后尽快停靠：VT2 不再发剩余 I2C chunk；VT3 不再 resume 可能已销毁的 VT2 |
| 9.8.5 | **tick_hook 启用栈金丝雀** | [kernel.c#L327-L337](file:///e:/ppCD/project/mini-kernel/kernel/core/kernel.c#L327-L337) | 每 tick 检查当前任务栈底魔值；溢出 → 打印 `[KERNEL] Stack overflow in task 'xxx'` + 挂起该任务，问题可见且不再扩散为 HardFault |

**设计教训**：
- 多任务拆除必须**等停靠**，不能靠定时盲等——被唤醒任务的恢复点可能在任意长阻塞操作内部；
- 长循环任务必须在每个 chunk/阶段边界**复查停止 flag**；
- 栈预算必须按最深调用链 + PendSV 64B + IRQ 嵌套计算，"差一点够"就是"溢出"；
- 堆利用率逼近 100% 时，任何栈溢出都直接踩坏相邻 TCB/堆元数据——留余量是稳定性的一部分。

---

## 10. 关键数据结构

### 10.1 总览

| 结构体 | 大小 (约) | 定义位置 | 存储位置 |
|--------|----------|---------|---------|
| `tcb_t` (TCB) | 60B | task.h | 内核堆 kmalloc (idle 任务静态 .bss) |
| `heap_block_t` | 8B | mem.c | 内核堆内部（块头部）|
| `mem_pool_t` | 16B | mem.h | 用户指定 (通常静态) |
| `hal_context_t` | 4B | hal_interface.h | TCB 内部 sp 字段 |
| `hal_export_t` | 8~48B | hal_interface.h | Flash (const) |
| `syscall_entry_t` | ~16B | syscall_contract.h | Flash (const X-Macro 表) |
| `uart_rx_ctx_t` | 134B | hal_port.c | .bss (静态，UART0/1 各一) |
| USB EP1 RX Ring | 130B | hal_port.c | .bss (静态) |

### 10.2 静态资源使用

| 资源 | 大小 | 说明 |
|------|------|------|
| 内核堆 | 8KB (`OS_CFG_HEAP_SIZE_BYTES`) | 7 个任务栈(合计 ~4KB) + 7×TCB(420B) + kmalloc 碎片缓冲 |
| idle 任务栈 | 256B (`OS_CFG_IDLE_STACK_SIZE`) | 轮询 USB 不需要大栈 |
| 任务池 | 16 槽 (`OS_CFG_MAX_TASKS`) | 16 个 TCB 指针 + 1 字节位图（含 idle 占槽 0）|

---

## 11. 依赖关系图

```
用户应用 (demo_app.c / rp2040demo main.c)
    │
    ├──▶ kernel.h (用户态 API)  ←── os_config.h (裁剪宏)
    │       │
    │       └──▶ syscall_contract.h (契约表)
    │
    ▼
kernel_core (task.o + sched.o + mem.o + kernel.o + syscall_contract.o)
    │
    ├──▶ task.h ──▶ hal_interface.h ──▶ os_config.h
    ├──▶ sched.h ──▶ task.h
    └──▶ mem.h ──▶ os_config.h
    │
    ▼
hal_port (hal_port.o + context_switch.o + msc_usb.o + msc_blockdev.o)
    │
    ├──▶ hal_interface.h
    ├──▶ Pico SDK: pico_stdlib / hardware_gpio / hardware_timer
    │              hardware_uart / hardware_spi / hardware_i2c
    │              pico_time / hardware_irq
    │
    └──▶ TinyUSB (通过 Pico SDK):
           tusb / dcd_* / tud_task_ext / CFG_TUD_MSC=1
    │
    ▼
shell_module (shell.o + shell_register.o + shell_fs.o + bootscript.o)
    │
    ├──▶ shell_core.h ──▶ fatfs_api.h (shell_fs_register)
    └──▶ kernel_core (g_task_pool / g_current_task / kmalloc)
    │
    ▼
fatfs_module (diskio.o + SDK ff.o + SDK ffunicode.o + ffconf shim)
    │
    ├──▶ fatfs_api.h ──▶ os_config.h
    ├──▶ diskio.h ──▶ msc_blockdev.h ──▶ flash_layout.h
    └──▶ hal_interface (hal_flash_*)
```

---

## 12. 构建与运行

### 12.1 前置环境 (Windows)

| 工具 | 路径 (build.bat 默认) | 说明 |
|------|---------------------|------|
| Pico SDK | `E:\ppCD\pico-sdk` | 已在仓库中，含 TinyUSB + FatFs |
| GCC 交叉编译器 | `C:\Users\master\.platformio\packages\toolchain-gccarmnoneeabi\bin` | arm-none-eabi-gcc 10+ |
| CMake | `C:\Program Files\CMake\bin` | 3.13+ |
| Ninja | `C:\Users\master\AppData\Local\Programs\Python\Python312\Scripts` | 构建后端 |

### 12.2 build.bat 命令

```cmd
cd e:\ppCD\project\mini-kernel

build.bat                    增量编译
build.bat clean              清空 build/ 后全量重编
build.bat flash              编译 + 自动烧录 (Pico 需在 BOOTSEL 模式)
build.bat just-flash         跳过编译，直接烧录已有 UF2
build.bat ... /nowait        结束不暂停 (用于脚本化 CI)
```

### 12.3 烧录操作

1. **Pico 进入 BOOTSEL 模式**：按住 BOOTSEL 键 → 拔插 USB
2. 此时 Windows 会弹出一个"可移动磁盘"(RPI-RP2，约 128MB)
3. 执行 `build.bat flash` → 脚本自动检测盘符 → copy UF2 过去
4. Pico 自动重启，开始执行固件

> **注意**：旧固件调度器 bug 可能导致 Windows USB 驱动死锁，此时无法识别 COM 口，必须**物理断电**后再按 BOOTSEL，否则 BOOTSEL 模式也进不去。

### 12.4 连接串口

- **USB CDC (推荐)**: 插上 USB 线后，设备管理器出现 COMx，PuTTY 打开，波特率任意（USB CDC 不使用波特率，但习惯填 115200）
- **物理 UART0**: GPIO0(TX) / GPIO1(RX)，115200-8N1，无流控
- 双通道同时启用，两边都能输入命令，输出在两边同步出现

### 12.5 初始验证

打开串口后看到 `mk> ` 提示符，输入以下命令验证：

```
help           → 列出所有命令
ps             → 列出 7+ 个任务 (idle, boot_setup, shell 等)
heap           → 堆空闲字节
led on         → 板载 LED 亮
led off        → 板载 LED 灭
tick           → 当前 tick 数 (1000/s)
version        → v2.2.6 STABLE
```

### 12.6 诊断固件

若主固件完全不工作（不亮灯、不出串口），用诊断固件排除硬件：

```cmd
# 在 build 目录下手动编译诊断固件
cmake --build build --target minimal_led_test
cmake --build build --target usb_print_test
```

- **minimal_led_test.uf2**：纯 SDK API 让 LED 闪烁 → 不亮 = 硬件/烧录问题
- **usb_print_test.uf2**：纯 SDK USB CDC 持续打印 → 无串口输出 = USB 通路问题

---

## 13. 裁剪配置 (os_config.h)

**文件位置**: [os_config.h](file:///e:/ppCD/project/mini-kernel/include/os_config.h)

### 13.1 体积红线（刚性约束，超限 CMake 编译报错）

```c
#define OS_CFG_MIN_KERNEL_FLASH_KB   10   /* 最小内核 Flash 上限 */
#define OS_CFG_MIN_KERNEL_RAM_KB      4   /* 最小内核 RAM 上限 */
#define OS_CFG_FULL_BASE_FLASH_KB    20   /* 完整基础版 Flash 上限 */
#define OS_CFG_FULL_BASE_RAM_KB       8   /* 完整基础版 RAM 上限 */
```

### 13.2 功能开关（关闭=零代码，不编入）

| 宏 | 默认值 | 关闭影响 |
|----|--------|---------|
| `OS_CFG_TASK_MODULE` | 1 | 必选，不可关 |
| `OS_CFG_SCHED_RR` | 1 | 必选，时间片轮转 |
| `OS_CFG_SCHED_WEIGHT` | 0 | 关闭权重比例（时间片都相等） |
| `OS_CFG_MEM_POOL` | 1 | 固定内存池（内核对象分配） |
| `OS_CFG_KERNEL_HEAP` | 1 | kmalloc/kfree（关闭后任务栈用静态）|
| `OS_CFG_STACK_OVF_CHECK` | 1 | 栈尾魔值检测 |
| `OS_CFG_SYSCALL` | 1 | 系统调用层 |
| `OS_CFG_PARAM_CHECK` | 1 | 系统调用参数校验 |
| `OS_CFG_PERIPH_SERVICE` | 1 | GPIO/SPI/I2C/UART 总线服务 Shell 命令 |
| `OS_CFG_SHELL` | 1 | 交互式命令行（关闭后无控制台交互）|
| `OS_CFG_FATFS` | 1 | Elm FatFs + USB MSC U 盘 |
| `OS_CFG_DEMO_APP` | 1 | 启动演示 Shell 任务（独立应用置 0）|
| `OS_CFG_VFS` | 0 | VFS 抽象层（暂未使用，v2.2 直接对接 FatFs）|
| `OS_CFG_LOADER` | 0 | 用户程序加载器（需 VFS=1）|

### 13.3 资源上限

| 宏 | 默认 | 说明 |
|----|------|------|
| `OS_CFG_MAX_TASKS` | 16 | 最大任务数（含 idle）|
| `OS_CFG_HEAP_SIZE_BYTES` | 16384 | 内核堆大小（v2.2.7：8K→16K，支撑 vtest 三任务；裁剪可调小）|
| `OS_CFG_IDLE_STACK_SIZE` | 256 | 空闲任务栈 |
| `OS_CFG_DEFAULT_TASK_STACK` | 512 | 默认任务栈 |
| `OS_CFG_TICK_HZ` | 1000 | 系统滴答频率 (1ms) |
| `OS_CFG_TIME_SLICE_TICKS` | 5 | 默认时间片 (5ms) |

### 13.4 编译期体积自检

```c
#if (OS_CFG_MIN_KERNEL_FLASH_KB > 10) || (OS_CFG_MIN_KERNEL_RAM_KB > 4)
#error "最小内核体积超标，请精简配置或调整红线"
#endif
```
链接后 CMake 还会执行 `cmake/check_size.cmake` 解析 .map 文件做二次验证。

---

## 14. 调试与诊断

### 14.1 LED 故障快速排查

```
LED 状态 → 行动树：

  完全灭/常亮
    ├─ 物理断电 → 按 BOOTSEL → 重插 → 烧 minimal_led_test
    │   ├─ 仍不亮 → 硬件坏 / USB 线 / BOOTSEL 操作
    │   └─ 亮 → 烧 usb_print_test
    │       ├─ 无串口 → USB 通路问题（换线换口换电脑）
    │       └─ 有串口 → 烧 mini_kernel.uf2，MK_BOOT_DIAG_LED=1
    │           └─ 数闪次数 → 对应 kernel.c _led_stage(N)
    │               stage1=main  / stage2=kernel_main / stage3=kmem_init
    │               stage4=task  / stage5=boot_setup_created / stage6=cpsie
    │               stage7=console / stage8=sched_start
    │
  5Hz 快闪 (HardFault)
    ├─ 检查 objdump: arm-none-eabi-objdump -d mini_kernel.elf > disasm.txt
    ├─ 重点看: PendSV_Handler / SVC_Handler / hal_context_init
    └─ 核对 PSP 偏移：task->sp + 32 是否 = 硬件栈帧

  1Hz 慢闪
    └─ sched_start 未正常切换 → 检查 boot_setup TCB 状态

  启动后停闪 (调度器卡死)
    ├─ ps 命令看：是否所有任务都 SUSPEND/SLEEP
    ├─ 检查 tick_hook: g_current_task->time_slice 是否递减
    └─ 确认 idle 任务未被加入睡眠队列
```

### 14.2 Shell 诊断命令

| 命令 | 输出示例 | 诊断含义 |
|------|---------|---------|
| `ps` | ID/Name/State/StackBase | 看是否有任务状态异常（如 RUNNING 任务不在链表、DEAD 仍在队列）|
| `heap` | heap_free=4567B max_block=3072B | 堆泄漏检测：运行几小时后 free 是否与初始差值巨大 |
| `tick` | tick=1234567 | tick 是否单调递增；不递增 = TIMER_IRQ_3 未触发 |
| `syscalls` | 列出所有 syscall | 契约表是否正确生成，外设开关是否生效 |

### 14.3 USB 专用诊断接口（hal_port.c 导出）

这些函数在 Shell 中可扩展为命令（目前 v2.2.6 稳定版不打印，必要时在 heartbeat 里加）：

```c
uint32_t hal_usb_diag_setup_count(void);      /* SETUP 包计数（SET_LINE_CODING 是否到达）*/
uint32_t hal_usb_diag_setup_w0(void);         /* 最近一次 SETUP 前 4B (bmRequestType/bRequest/wValue/wIndex) */
uint32_t hal_usb_diag_setup_w1(void);         /* 最近一次 SETUP 后 4B (wLength) */
uint32_t hal_usb_diag_mask_write_count(void); /* 安全掩码兜底次数（异常时 >0 正常 =0） */
uint32_t hal_usb_diag_dcd_handler_count(void);/* dcd_int_handler 调用计数 */
uint32_t hal_usb_diag_ep1_ring_count(void);   /* 私有 ring buffer 可读字节数 */
uint32_t hal_usb_diag_ep1_hw_avail(void);     /* BUFSTAT.EP1_OUT_AVAIL 硬件标志 */
```

> 正常情况下：`s_setup_count >= 2`（Windows 打开 PuTTY 后有 SET_LINE_CODING + SET_CONTROL_LINE_STATE 两个 SETUP），`s_poll_mask_write_count = 0`（INTE 未被抢占清零）。

### 14.4 构建日志

`build.bat` 每次运行把完整 stdout/stderr 写到 `build/build.log`，失败时自动在终端显示最后 25 行 tail。

```
Log file: e:\ppCD\project\mini-kernel\build\build.log
失败排查：
  1. 查看 PICO_SDK_PATH / TOOLCHAIN_BIN 是否有效
  2. 看 CMake configure 阶段 OS_CFG_DEFINES 是否正确解析
  3. 链接错误 multiple definition → 通常是 startup_rp2040.S 被错误链接（已 EXCLUDE）
  4. UF2 文件体积：~195KB 为正常完整基础版
```

---

**文档结束**

> **总结**：Mini Kernel v2.2.7 的防卡死设计覆盖了"从 reset 到 HardFault"全生命周期共 9 大类 65+ 个设计点，其中 25+ 处是从真实 HardFault / Lockup / USB 死锁 / LED 爆闪的血泪 bug 中总结修复的经验（含 v2.2.7 的 vtest 拆除竞态修复），每一处都附带了旧版根因 + 修复方案，确保 RP2040 这款 4 美元 MCU 能稳定运行多任务调度 + USB 复合设备 + FatFs 文件系统，而不是隔三差五就需要物理断电重启。
