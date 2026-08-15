# Mini Kernel · Project Repository（GitHub 仓库根入口）

> **GitHub 仓库地址**：[lsx1156/mini-kernel](https://github.com/lsx1156/mini-kernel)
> **当前版本**：v2.3.6 · 2026-08
> **项目性质**：**轻量分时通用 32 位 MCU 内核**（协作式多任务，非 RTOS；非抢占、无优先级、不承诺硬实时）
>
> 🟢 **v2.3.6 更新：根治 >30 次轮询崩溃（I2C 单事务原子性）**
> · **根因**：VT3 每 10s 对 VT2 做 `task_suspend/resume`，若 VT2 恰在 `i2c_write_timeout_us` 中途被挂起 3s，SDK 的 200ms 超时是"调用起点算起的绝对截止"，恢复后立即判超时 → `HAL_ERR_IO` → 反复 `reinit` 复位外设，累积约 30 轮后触发 HardFault
> · **修复**：用 PRIMASK 临界区包住"单次 I2C 事务"（`VT2` 的 `set_addr`/`write_block`），使 PendSV 上下文切换无法打断一次 START..STOP 事务，VT2 永不带着过期截止指针被挂起（400kHz 下单次最多约 3ms 关中断）
> · 该修复同时消除"屏幕刷新错误随轮询次数递增"的现象
>
> 🟢 **v2.2.7 更新：OLED 底层驱动重构 + 调度权重可调**
> · 重构 0.96" I2C OLED 最底层驱动：命令/数据统一走 16 位 I2C 编码（Co 控制字节），`set_addr` 旧式寻址 + 列偏移，`write_block` 数据块写入，与网上通用 0.96 驱动写法对齐
> · 驱动缓冲全部移出任务栈（BSS 静态），消除栈帧指针错乱导致的 HardFault
> · I2C 事务失败自动重试 + `hal_i2c_init` 外设复位重试，解决 VT3 suspend/resume 打断白帧刷新
> · I2C 时钟 100kHz → 400kHz，刷新提速 4 倍
> · VT2 权重 1 → 8（时间片 5ms → 40ms），整帧在一个时间片内连续跑完，肉眼看瞬间整屏刷新（消除逐行扫描）
> · `help` 新增"调度说明"：解释 weight=时间片倍数、非抢占、无优先级
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
├── mini-kernel/                  📚 【可移植内核库】——对外提供的纯 C + 移植汇编内核
│   │                               （硬件无关代码 100% 在这；RP2040 demo 通过的所有 Bug 修复
│   │                                 全部同步回此目录，rp2040demo/ 不维护任何内核副本）
│   │
│   ├── include/                  对外头文件入口
│   │   ├── os_config.h           ✂️ 内核裁剪宏（开/关功能用这个）
│   │   ├── hal/                  HAL 统一接口（移植新 MCU 时实现）
│   │   │   └── flash_layout.h    Flash 三分区布局宏定义（1MiB FW / 1012KiB MSC / 8KiB Bootscript）
│   │   └── kernel.h              用户态唯一 API 入口（os_task_create / os_time_dly_ms 等）
│   │
│   ├── kernel/                   内核核心 + 可裁剪扩展模块（100% 硬件无关，可移植）
│   │   ├── core/                 kernel.c / sched.c / task.c / mem/heap.c（最底层核心）
│   │   ├── syscall/              SVC 异常契约表（X-Macro 静态生成）
│   │   ├── modules/shell/        交互式 Shell（命令 + 文件系统目录命令）
│   │   ├── modules/fatfs_shim/   FatFs 适配层（含 f_mkfs 实现、Flash HAL 读写、单写者互斥）
│   │   ├── modules/periph/       外设服务（GPIO / SPI / I2C / UART，按 OS_CFG 裁剪）
│   │   └── examples/builtin_demo/ 可选示例任务（LED/心跳/bootscript 执行）
│   │
│   ├── port/                     🔌 硬件移植层（每款 MCU 一个子目录；当前已实现 rp2040/）
│   │   └── rp2040/
│   │       ├── context_switch.S  Cortex-M0+ SVC + PendSV 上下文切换（r4-r11 完整保存）
│   │       ├── hal_impl.c        HAL Flash / Time / Uart 具体实现
│   │       └── msc_usb.c         USB Composite（CDC 串口 + MSC U 盘）描述符 + SCSI 回调
│   │
│   ├── tests/                    单元测试 + 诊断固件（可选）
│   ├── cmake/check_size.cmake    体积红线检查脚本（内核本体 Flash ≤ 20KB / RAM ≤ 8KB）
│   ├── README.md                 📖 **完整详细文档请点这里**（特性表/命令手册/架构/移植指南）
│   └── CMakeLists.txt            mini-kernel 作为 CMake 静态库的构建
│
└── rp2040demo/                   🛠️ 【RP2040 演示平台】——真正烧录到 RP2040 的示例工程
    │                               （用 add_subdirectory(../mini-kernel) 直接引用内核库，
    │                                 + Pico SDK；产生可拖拽的 .uf2 固件）
    ├── src/main.c                应用入口（强符号 main() 覆盖内核的弱 main）
    ├── CMakeLists.txt
    ├── build.bat                 Windows 一键构建（会自动拉 Pico SDK + 生成 uf2）
    └── build/rp2040demo.uf2      构建产物（烧录用）
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

---

## 🗂️ 版本状态速览（与 mini-kernel/README 一致）

| 版本 | 状态 | 关键词 |
|---|---|---|
| v2.2.7 | ✅ **当前稳定**（仓库 HEAD） | 🟢 **OLED 底层驱动重构**（16 位 I2C 编码 + 旧式寻址 + 列偏移）+ 权重驱动刷屏提速 + help 调度说明；⚠️ 已知问题：>30 次轮询有崩溃风险（排查中） |
| v2.2.6 | ✅ 已发布历史 | 🟢 **调度系统完全正常**：boot_setup "复活" Bug 修复 + Shell 栈溢出修复；状态机/队列契约 100% 合规 |
| v2.2.x | ✅ 已发布历史 | v2.2.0~v2.2.4：三分区 Flash / Composite USB(CDC+MSC) / FatFs 目录命令 / Bootscript 固化 / 诊断闪灯 / mkfs 实现 / ALARM3 tick 修复 |
| v2.3 | 📅 规划中（下一版） | GPIO → TFT LCD 驱动 / 目录命令补全（cat 大文件分屏 / cp 复制 / mv 重命名） |
| v3.0 | 📅 远期规划 | VFS 虚拟文件系统抽象层（挂载多路，路径/驱动解耦） |
| v3.5 | 📅 远期规划 | RP2350 多核（SMP）+ RISC-V（CH32V/bl702）移植 |

---

## 🔗 快速跳转

| 文档/文件 | 链接 |
|---|---|
| 📖 完整详细主文档（必看）| [mini-kernel/README.md](./mini-kernel/README.md) |
| 🧩 裁剪宏总开关 | [mini-kernel/include/os_config.h](./mini-kernel/include/os_config.h) |
| 🧠 内核主入口（启动顺序+FatFs自动初始化）| [mini-kernel/kernel/core/kernel.c](./mini-kernel/kernel/core/kernel.c) |
| 🔄 调度器（非抢占 FIFO + 时间片） | [mini-kernel/kernel/core/sched.c](./mini-kernel/kernel/core/sched.c) |
| 🔀 Cortex-M0+ 上下文切换（SVC+PendSV） | [mini-kernel/port/rp2040/context_switch.S](./mini-kernel/port/rp2040/context_switch.S) |
| 💾 Flash 三分区布局宏 | [mini-kernel/include/hal/flash_layout.h](./mini-kernel/include/hal/flash_layout.h) |
| 🔌 USB Composite (CDC+MSC) 描述符 | [mini-kernel/port/rp2040/msc_usb.c](./mini-kernel/port/rp2040/msc_usb.c) |
| 🐚 Shell 文件系统目录命令（ls/cd/pwd/mkdir） | [mini-kernel/kernel/modules/shell/shell_fs.c](./mini-kernel/kernel/modules/shell/shell_fs.c) |
| 🛠️ 一键构建脚本（RP2040 演示固件） | [rp2040demo/build.bat](./rp2040demo/build.bat) |

---

## 🏷️ 为什么叫 "Mini Kernel" 不叫 "Mini RTOS"

就是因为前面那张对比表写死的三条：**无优先级抢占、无硬实时承诺、调度不看优先级**。
RTOS 是有严格工程定义的术语，不能随便贴标签。本项目老老实实做自己的定位——

> **轻量分时通用 32 位 MCU 内核：裸机和 RTOS 之间、够用、干净、带 U 盘 + 文件系统命令的多任务小底座。**
