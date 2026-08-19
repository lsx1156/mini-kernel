# Mini Kernel · Project Repository（GitHub 仓库根入口）

> **GitHub 仓库地址**：[lsx1156/mini-kernel](https://github.com/lsx1156/mini-kernel)
> **当前版本**：v2.7.1 · 2026-08
> **项目性质**：**轻量分时通用 32 位 MCU 内核**（协作式多任务，非 RTOS；非抢占、无优先级、不承诺硬实时）
>
> 📖 **指令说明书（Shell 命令手册）**：[指令说明书.md](./指令说明书.md)
>
> 🟢 **v2.7.1 更新：三部分分离 + 时钟树说明**
> · **mini-kernel** —— 可移植纯静态库（只导出 `kernel_core.a` / `shell_module.a`），平台无关、不链接 Pico SDK，可移植到其它 MCU
> · **rp2040demo** —— RP2040 Demo 目标工程（`rp2040_port` 移植层 + 展示任务），**自此版起作为 Demo 版完整体截止，不再更新**
> · **rp2040system** —— RP2040 系统工程：与 demo **完全同命令集**，但**无展示任务**（`OS_CFG_SHOW_DEMO=0`，去掉 OLED FPS / 呼吸 LED / GP15 PWM）
> · 新增 **RP2040 时钟树说明**（见下）：超频只改 `clk_sys`，PWM/节拍等周期功能不受影响
>
> 🟡 **v2.7.1-fix：修复运行时超频崩溃 + USB 稳定性**
> · **根因**：超频切换（`sysclk_apply_mhz`）里新增的"关中断 50ms `busy_wait`"本意是等 core1 的 show 任务暂停，却让 core0 整整 50ms 不服务 USB → 主机判定超时 reset/重枚举 → 中断恢复后 `USBCTRL_IRQ` 处理被破坏的端点状态 → 二次崩（误报"idle PC=0"，实为 ISR 崩溃，`xPSR=0` 露馅）。
> · **修复**：移除该 busy-wait，只置跨核暂停标志后立即切换（flash 分频/`clk_peri` 已提前钳制，show 栈已 4096，I2C 超时/重初兜底）；shell 任务栈 2048→4096（吸收 `ovclk` 深调用链）；故障转储升级为按 `EXC_RETURN` 区分 Handler/Thread，ISR 崩溃时打印真实 MSP 帧 PC。
> · **效果**：`ovclk try 250` 运行时切换不再崩；125/250MHz 稳定可用（含运行时切换），仅 375/500MHz 极限档不稳定（见下已知限制）。
>
> 🟢 **v2.5 更新：两版本分离（内核库纯净化）**（历史）
> · mini-kernel 改为可移植纯静态库；rp2040demo 独立编译 `rp2040_port` 并产出 `.uf2`
> · 两版本各自有独立 `os_config.h`；RP2040 专属功能用总开关 `OS_CFG_PORT_RP2040` 隔离
>
> 🟢 **v2.3.6 更新：根治 >30 次轮询崩溃（I2C 单事务原子性）**
> · **根因**：VT3 每 10s 对 VT2 做 `task_suspend/resume`，若 VT2 恰在 `i2c_write_timeout_us` 中途被挂起 3s，SDK 的 200ms 超时是"调用起点算起的绝对截止"，恢复后立即判超时 → `HAL_ERR_IO` → 反复 `reinit` 复位外设，累积约 30 轮后触发 HardFault
> · **修复**：用 PRIMASK 临界区包住"单次 I2C 事务"（`VT2` 的 `set_addr`/`write_block`），使 PendSV 上下文切换无法打断一次 START..STOP 事务，VT2 永不带着过期截止指针被挂起（400kHz 下单次最多约 3ms 关中断）
> · 该修复同时消除"屏幕刷新错误随轮询次数递增"的现象
>
> ✅ **已知问题状态**：~~应用层轮询连续超过 30 次 HardFault 崩溃风险~~ → **v2.3.6 已修复**（I2C 单事务原子性）
> · 使用教程详见 [mini-kernel/README.md § 完整使用教程 + vtest 案例分析](./mini-kernel/README.md#-完整使用教程linux-man-风格)

---

## ⚠️ 项目定位澄清（先看我，避免误解）

**❌ 本项目 ≠ RTOS（实时操作系统）。**

很多朋友一看到"内核"+"多任务"就归类成 RTOS，这里明确划清边界：

| 类别 | **RTOS 必须满足（FreeRTOS / Zephyr / RT-Thread）** | **本 Mini Kernel** |
|---|---|---|
| 调度方式 | **优先级抢占**：高优先级任务可以在**任意时刻**打断低优先级任务（中途强抢） | **时间片轮转 + 权重，非抢占**：只有任务主动 yield / 睡眠 / IO 等待，或用完整个时间片才切走 |
| 实时性 | **硬实时有界**：最坏中断/任务切换延迟有严格上限（微秒级确定值） | **不承诺硬实时**：延迟受当前时间片剩余影响，非目标第 1 条明确写死不追求 |
| 调度算法核心 | 必须有 **优先级**（位图 O(1)、多级队列等） | **无优先级概念**：就绪队列 FIFO + 时间片权重（sched.c 不看 priority 字段做决策） |

**定位一句话**：填补「裸机开发门槛高（自己写状态机很复杂）、RTOS 偏向工业硬实时控制（对非工控场景功能又太重）、Linux 无法运行在小 RAM MCU」之间的中间空白地带——给玩具、教学、DIY 小项目、非实时外设驱动封装提供一个「够轻、够干净、带文件系统和 U 盘」的多任务底座。

---

## 📦 仓库单仓结构（project/）

```
project/                          ← 本仓库根目录（单仓 · monorepo）
│
├── mini-kernel/                  📚 【可移植纯内核库】——平台无关的通用内核（静态库）
│   │                               （只含纯 C 调度核心 + 基础 Shell；不含任何 RP2040 文件）
│   │
│   ├── include/
│   │   ├── os_config.h           ✂️ 内核裁剪宏（纯库配置：RP2040 功能全 0）
│   │   ├── hal/hal_interface.h   HAL 统一接口（移植新 MCU 时实现 hal_port）
│   │   └── kernel.h              用户态 API 入口
│   │
│   ├── kernel/
│   │   ├── core/                 kernel.c / sched.c / task.c / mem.c（最底层核心）
│   │   ├── syscall/              SVC 异常契约表（X-Macro 静态生成）
│   │   └── modules/shell/        基础 Shell（help/ps/heap/... + 扩展命令注册）
│   │
│   ├── tests/                    单元测试
│   ├── cmake/check_size.cmake    体积红线检查脚本
│   ├── README.md                 详细文档（特性表/架构/移植指南）
│   └── CMakeLists.txt            纯静态库构建（导出 kernel_core / shell_module）
│
└── rp2040demo/                   🛠️ 【RP2040 Demo 目标工程】——真正烧录到 RP2040 的示例固件
    │                               （通过 add_subdirectory(../mini-kernel) 引用内核库 + Pico SDK）
    │                               ⚠️ v2.7.1 起作为 Demo 完整体截止，不再更新
    ├── src/main.c                应用入口（main() → kernel_main()）
    ├── os_config.h               本工程专属裁剪配置（RP2040 功能全开 + OS_CFG_SHOW_DEMO=1）
    ├── rp2040_port/              🔌 RP2040 移植层（全部 RP2040 专属代码，**demo 与 system 共享**）
    │   ├── hal_port.c             HAL 实现 + USB/Timer/GPIO/Flash 驱动
    │   ├── context_switch.S       Cortex-M0+ SVC + PendSV 上下文切换
    │   ├── msc_usb.c / msc_blockdev.c   USB Composite(CDC+MSC) + 块设备
    │   ├── include/hal/           config_store.h / sysclk.h / flash_layout.h
    │   ├── kernel/                config_store.c / diskio.c / FatFs 配置
    │   ├── shell/                 bootscript.c / shell_fs.c / shell_mcore.c / shell_ovclk.c
    │   └── demo/demo_app.c        演示任务（LED/心跳/vtest 等）
    ├── CMakeLists.txt
    └── build.bat                 Windows 一键构建（产出 rp2040demo.uf2）

└── rp2040system/                 🛠️ 【RP2040 系统工程】——无展示版本
    │                               （复用 rp2040demo/rp2040_port，os_config.h OS_CFG_SHOW_DEMO=0）
    │                               · 与 demo 完全同命令集，仅去掉 OLED FPS/呼吸 LED/GP15 PWM 展示
    ├── src/main.c                应用入口（与 demo 相同）
    ├── os_config.h               本工程专属裁剪配置（OS_CFG_SHOW_DEMO=0）
    ├── CMakeLists.txt
    └── build.bat                 Windows 一键构建（产出 rp2040system.uf2）
```

---

## 🚀 从这里开始（3 步烧录）

详细文档在 → [**mini-kernel/README.md**](./mini-kernel/README.md)（包含完整特性表、Flash 分区图、Shell 命令手册、典型使用流程、裁剪宏、架构分层、移植指南）。

**最快上手**：
```powershell
cd rp2040demo
.\build.bat
```
构建成功后把 `rp2040demo/build/rp2040demo.uf2` 拖进 RP2040 的 `RPI-RP2` 盘符即可。烧录后：
- 🖥️ USB 串口（115200-8-N-1）：看启动 banner、打 Shell 命令
- 💾 USB 可移动磁盘（≈ 1012 KiB FAT16 U 盘）：电脑直接读写，Shell 也能 `ls/cd/cat/mkdir`

**💾 直接下载固件（免构建，v2.7.1-fix）**：仓库 [`firmware/`](./firmware) 目录下已放置现成的 `.uf2`，直接下载拖进 `RPI-RP2` 即可：
| 固件 | 命令集 | 展示任务 | 下载 |
|---|---|---|---|
| rp2040demo.uf2 | 完整（fs/msc/ovclk/mcore/ipc/...） | ✅ 有（OLED FPS + 呼吸 LED + GP15 PWM） | [下载](./firmware/rp2040demo.uf2) |
| rp2040system.uf2 | 完整（同上） | ❌ 无展示 | [下载](./firmware/rp2040system.uf2) |

---

## 🗂️ 版本状态速览（与 mini-kernel/README 一致）

| v2.8 | 📅 规划中 | 🟢 **PLC 循环指令引擎**：100+ 梯形图指令 + 10ms 扫描周期 + 4 Flash 槽位持久化 + 在线调试 |
| v2.7.1 | ✅ **当前稳定**（仓库 HEAD） | 🟢 **三部分分离 + 时钟树说明**：mini-kernel / rp2040demo（Demo 完整体截止）/ rp2040system（无展示）；多核 idle 栈修复、FPS 修正、GP15 精确台阶 PWM。🟡 **v2.7.1-fix**：移除超频切换的 USB 阻塞 busy-wait + shell 栈 4096 + 故障转储升级，修复 `ovclk try 250` 运行时崩溃。⚠️ **demo 已知限制**：>250MHz 超频不稳定 + USB CDC 失效，demo 不再修复，后续可能在 system 修 |
| v2.7 | ✅ 已发布历史 | 🟢 展示/稳定性验证：OLED FPS 动画 + 呼吸 LED + GP15 PWM（show 任务） |
| v2.5 | ✅ 已发布历史 | 🟢 两版本分离（内核库纯净化）：mini-kernel 可移植纯静态库；rp2040demo 独立编译 `rp2040_port`；`OS_CFG_PORT_RP2040` 总开关隔离；新增指令说明书 |
| v2.4 | ✅ 已发布历史 | 🟢 超频档位/任意 MHz + 多核基础 + 固化配置 + 开机日志回放 |
| v2.3.x | ✅ 已发布历史 | 🟢 OLED 底层驱动重构 + I2C 单事务原子性（根治 >30 次轮询崩溃）+ 权重刷屏提速 + help 调度说明 |
| v2.2.x | ✅ 已发布历史 | 🟢 调度系统完全正常 / 三分区 Flash / Composite USB(CDC+MSC) / FatFs 目录命令 / Bootscript 固化 / mkfs 实现 / ALARM3 tick 修复 |
| v2.3+ | 📅 规划中 | GPIO → TFT LCD 驱动 / 目录命令补全（cat 大文件分屏 / cp / mv） |
| v3.0 | 📅 远期规划 | VFS 虚拟文件系统抽象层（挂载多路，路径/驱动解耦） |
| v3.5 | 📅 远期规划 | RP2350 多核（SMP）+ RISC-V（CH32V/bl702）移植 |
| v4.0 | 🔮 预告     | **RP2350 (Cortex-M33 + RISC-V) 适配**：双核 SMP 调度、TrustZone 安全、更大 RAM/Flash、更高主频、更多外设、Pico 2 硬件支持 |

---

## ⏱️ RP2040 时钟树（超频对功能的影响）

RP2040 不同外设吃**不同时钟源**，超频（`ovclk`）只改 CPU 计算速度，**周期类功能不受影响**：

| 外设 / 功能 | 时钟源 | 超频到 250MHz 时的行为 |
|---|---|---|
| CPU 指令执行、渲染算术 | `clk_sys` | ✅ 变快（约 2 倍） |
| PWM 载波频率（GP25/GP15）、I2C/SPI/UART 波特率 | `clk_peri` | 不变 —— 超频代码把 `clk_peri` 钳回 125MHz |
| 内核 tick、`task_sleep`、呼吸灯/心跳/台阶间隔 | `clk_ref`（= XOSC 12MHz，固定） | 不变 —— `clk_ref` 固定，不随 `clk_sys` 变 |
| USB | `pll_usb`（48MHz，独立） | 不变 |

> **结论**：超频后 PWM 载波、呼吸灯闪烁、GP15 台阶间隔、心跳周期**都不会变短**，只有纯计算更快。
> 详见 [指令说明书 §〇 时钟树](./指令说明书.md#〇rp2040-时钟树超频相关先看这里)。

> ⚠️ **rp2040demo 版已知限制（v2.7.1 标注，不再修复）**
> **超频到 250MHz 以上（375/500MHz）时系统会不稳定**，且 **USB CDC 控制界面会失效**（`ovclk try 3/4` 或固化 375/500 档后 USB 串口断开）。
> · 原因：高主频下 XIP flash 分频 ≥62.5MHz 有取指风险 + USB 时钟/中断时序在高频下不稳定。
> · 稳定可用档位：**125 / 250MHz**（`ovclk try 250` 运行时切换在 v2.7.1-fix 后不再崩）；375/500MHz 仅作极限测试，可能 USB 失效。
> · **本版（rp2040demo）不再修复 375/500 问题**，作为 Demo 完整体截止。
> · **后续可能在 rp2040system 版修复 USB 不稳定的问题**；届时请以 system 版为准。

---

## 🔗 快速跳转

| 文档/文件 | 链接 |
|---|---|
| 📖 **指令说明书（Shell 命令手册）** | [指令说明书.md](./指令说明书.md) |
| 📖 完整详细主文档（必看）| [mini-kernel/README.md](./mini-kernel/README.md) |
| 🧩 纯库裁剪宏总开关 | [mini-kernel/include/os_config.h](./mini-kernel/include/os_config.h) |
| 🧩 rp2040demo 裁剪宏 | [rp2040demo/os_config.h](./rp2040demo/os_config.h) |
| 🧩 rp2040system 裁剪宏（无展示） | [rp2040system/os_config.h](./rp2040system/os_config.h) |
| 🧠 内核主入口 | [mini-kernel/kernel/core/kernel.c](./mini-kernel/kernel/core/kernel.c) |
| 🔄 调度器（非抢占 FIFO + 时间片） | [mini-kernel/kernel/core/sched.c](./mini-kernel/kernel/core/sched.c) |
| 🔀 Cortex-M0+ 上下文切换（SVC+PendSV） | [rp2040demo/rp2040_port/context_switch.S](./rp2040demo/rp2040_port/context_switch.S) |
| 🔌 USB Composite (CDC+MSC) 描述符 | [rp2040demo/rp2040_port/msc_usb.c](./rp2040demo/rp2040_port/msc_usb.c) |
| 🐚 Shell 文件系统目录命令（ls/cd/pwd/mkdir） | [rp2040demo/rp2040_port/shell/shell_fs.c](./rp2040demo/rp2040_port/shell/shell_fs.c) |
| ⚡ 超频/多核命令 | [rp2040demo/rp2040_port/shell/shell_ovclk.c](./rp2040demo/rp2040_port/shell/shell_ovclk.c) |
| 🛠️ 一键构建脚本（RP2040 演示固件） | [rp2040demo/build.bat](./rp2040demo/build.bat) |
| 🛠️ 一键构建脚本（RP2040 系统固件，无展示） | [rp2040system/build.bat](./rp2040system/build.bat) |
| 🛠️ 纯内核库构建脚本 | [mini-kernel/build.bat](./mini-kernel/build.bat) |

---

## 🏷️ 为什么叫 "Mini Kernel" 不叫 "Mini RTOS"

就是因为前面那张对比表写死的三条：**无优先级抢占、无硬实时承诺、调度不看优先级**。
RTOS 是有严格工程定义的术语，不能随便贴标签。本项目老老实实做自己的定位——

> **轻量分时通用 32 位 MCU 内核：裸机和 RTOS 之间、够用、干净、带 U 盘 + 文件系统命令的多任务小底座。**
