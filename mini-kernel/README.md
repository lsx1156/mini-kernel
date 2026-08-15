# Mini Kernel — 轻量分时通用 32 位 MCU 内核

面向资源受限 32 位 MCU 的极简分时通用内核。
**最小内核 Flash ≤ 10KB / RAM ≤ 4KB** 即可跑调度 + 堆 + 上下文切换；
**完整基础版（Shell + 外设服务 + 指令固化）** Flash \~20KB / RAM \~8KB；
**RP2040 演示版（v2.2，含 Pico SDK + USB Composite(CDC+MSC) + FatFs）** Flash \~111KB / RAM \~20KB。

> **定位**：填补「裸机开发门槛高、RTOS 偏向工业硬实时控制（对非工控场景又太重）、Linux 无法运行」的中间空白地带。
>
> **🛑 重要澄清：本项目 ≠ RTOS** — 不要把 Mini Kernel 叫 RTOS，RTOS 有严格工程定义：
>
> | 判断维度 | RTOS（FreeRTOS/Zephyr/RT-Thread）必须 | 本 Mini Kernel 实际                              |
> | ---- | --------------------------------- | --------------------------------------------- |
> | 调度语义 | **优先级抢占**（高优先级可中途强抢低优先级任务）        | **时间片轮转+权重，非抢占**（仅 yield/睡眠/时间片耗尽时切走）         |
> | 实时性  | **硬实时有界**（最坏延迟微秒级确定值）             | **不承诺硬实时**（延迟受时间片剩余影响，非目标§1明确不追求）             |
> | 调度算法 | 核心必须基于**优先级**                     | **无优先级概念**（就绪队列 FIFO + 时间片权重，不看 priority 做决策） |
>
> 📌 **一句话定义**：轻量分时通用 32 位 MCU 内核 —— 裸机和 RTOS 之间、够用、干净、带 U 盘和文件系统命令的多任务小底座。
>
> 架构 4 层垂直分层：**应用 → 系统调用 → 内核核心 → HAL 移植层**，核心代码 100% 跨平台复用。

> **当前版本**：`2.4.0 🔬 探索` — 🟡 **超频档位 + 多核固化**（v2.4）：默认单核 + 125MHz，通过 `ovclk` 指令设置档位/多核并固化到独立 Flash Config 区，冷启动应用；未固化/损坏则安全回退单核 + 原始频率。多核调度器为脚手架（标志已持久化，`config_mcore_apply` 钩子预留）。
>
> **v2.2 大版本亮点**
>
> * ✨ USB 插电脑 **同时弹出**：CDC 串口（命令行调试 COMx）**+** MSC 可读写 U 盘盘符
>
> * ✨ W25Q16JV 2MB SPI Flash **三分区独立布局**：固件区 / MSC 数据盘 / Bootscript 固化区 互不重叠
>
> * ✨ 真正的 **目录索引文件系统**（FatFs FAT16）：`ls / cd / pwd / mkdir / rmdir / rm / cat` 支持子目录管理，不是内存地址索引
>
> * ✨ 主机 ↔ 本机写互斥保护：`msc mount / eject / status / format` 切换写入所有权避免数据损坏
>
> * ✨ 首启动空片 **自动 f\_mkfs** 建 FAT16，Windows 拷贝的文件 / 子目录 Shell 端直接 cat 可见，完全同一份后端

***

## 📦 仓库结构（project/）

**重要**：本仓库 `project/` 目录下包含两个完全独立的部分，**不要混淆**：

```
project/
├── mini-kernel/          📚 【可移植内核库】——对外提供的纯 C + 移植汇编内核
│   ├── include/          对外头文件 (os_config.h 裁剪宏 + HAL 统一接口 + kernel.h)
│   ├── kernel/           内核核心 + 可裁剪扩展模块（100% 硬件无关，可移植）
│   │   ├── core/         任务 / 调度 / 内存 / 中断管理 (mem.c sched.c task.c kernel.c)
│   │   ├── syscall/      SVC 系统调用层
│   │   └── modules/      可裁剪扩展 (Shell / FatFs / VFS / Loader / ……)
│   │       ├── shell/    Shell 命令行 (bootscript 固化 + 文件系统命令)
│   │       └── vfs/fatfs Elm FatFs R0.15 对接层 (ffconf.h diskio.c fatfs_api.h)
│   ├── port/             硬件移植层（每款 MCU 独立目录）
│   │   └── rp2040/       ⭐ 当前唯一移植：RP2040 (Cortex-M0+ @ Pico SDK)
│   │       ├── hal_port.c/h              HAL 统一接口实现
│   │       ├── context_switch.S          PendSV/SVC 上下文切换 (含栈帧修复)
│   │       ├── msc_blockdev.c/h          USB MSC 与 FatFs 共享 512B 扇区后端
│   │       └── msc_usb.c                 Composite USB 描述符 + SCSI 回调
│   ├── examples/builtin_demo/            示例演示应用（4 个 demo 任务 + 命令注册）
│   ├── cmake/check_size.cmake            体积红线检查脚本
│   ├── tests/                            诊断固件 + 单测
│   └── CMakeLists.txt                    mini-kernel 作为库的 CMake 构建
│
└── rp2040demo/          🛠️ 【RP2040 演示平台】——用户真正烧录的示例工程 (本仓库首发验证平台)
    ├── src/main.c        调用 kernel_main() 起内核 (RP2040 特有的 main 入口)
    ├── CMakeLists.txt    add_subdirectory(../mini-kernel) + 链接 Pico SDK
    └── build.bat         Windows 一键构建（产物 = rp2040demo.uf2）
```

### 🔑 关系说明

| 项目               | 角色                                       | 输出                                                                             |
| ---------------- | ---------------------------------------- | ------------------------------------------------------------------------------ |
| **mini-kernel/** | 可移植内核库                                   | 静态库 `libkernel_core.a / libhal_port.a / libshell_module.a / libfatfs_module.a` |
| **rp2040demo/**  | RP2040 演示应用（消费 mini-kernel 库 + Pico SDK） | **`rp2040demo.uf2`** — 实际烧录固件 (241 KB)                                         |

> **v2.2 内核同步说明**：
> 所有 RP2040 demo 验证通过的内核 Bug 修复（上下文栈帧、调度器队列、LED 固化覆盖、启动速度优化、FatFs 自动初始化等）**全部已同步回** **`mini-kernel/`** **可移植目录**，`rp2040demo/` 本身**不维护任何内核代码副本**。

***

## 🧭 版本路线

| 版本                 | 状态         | 关键里程碑                                                                                                                                                                                                                                                                                  |
| ------------------ | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **v0.1.0**         | ✅ Released | 基础内核（调度/堆/上下文）+ Shell/GPIO/I2C/UART/SPI 外设服务（RP2040 首发验证）                                                                                                                                                                                                                              |
| **v0.2.0-beta**    | ✅ Archived | **指令固化机制** (`!`/`save`/`unsave`/`list`/`boot exec`) + hal\_flash + `factory_reset` + 开机自动回放                                                                                                                                                                                            |
| **v0.2.0-hotfix1** | ✅ Archived | **修复 led on 固化后被 \_led\_stage(13) 覆盖**；新增 `boot status` RAM 常驻回放结果；`Unknown command` 误报修复                                                                                                                                                                                              |
| **v0.2.1**         | ✅ Archived | **启动速度优化**：10s+ → <500ms；新增 `MK_BOOT_DIAG_LED` 宏（默认 0）；删除 5s USB 枚举忙等                                                                                                                                                                                                                  |
| **v2.2.0**         | ✅ Released | **大版本**：USB Composite (CDC+MSC) + FatFs FAT16 + Shell 目录命令 + 三分区 Flash 布局                                                                                                                                                                                                              |
| **v2.2.1**         | ✅ Released | README 彻底重写（补全特性表/分区图/命令手册/架构图）+ 可移植内核独立 FatFs 初始化（不依赖 demo\_app.c）                                                                                                                                                                                                                    |
| **v2.2.2**         | ✅ Released | 🛑 **定位澄清：≠RTOS**（两个 README 新增 RTOS vs 分时内核对比表；仓库首页 project/README 从 v0.1 同步到 v2.2，删除错误"轻量级 RTOS 内核"命名）                                                                                                                                                                                |
| **v2.2.3**         | ✅ Released | 🐛 **修复首启动死机 v2.2.1 回归**：① boot\_setup\_task 栈 1024 → 2048（f\_mkfs 格式化最坏栈溢出写坏 kmem/TCB → HardFault）；② fatfs\_init\_and\_mount() 从 demo\_app\_init 之前挪到之后（恢复 v2.2.0 时序：shell\_start → mount；OS\_CFG\_DEMO\_APP=0 空桩后仍独立执行）；③ demo\_app.c 默认 KERNEL\_VERSION\_STR 从 2.2.0 UNTESTED 同步    |
| **v2.2.4**         | ✅ Released | 🐛 **修复烧录后看不到串口 / U 盘 Bug**（v2.2.0\~v2.2.3 全都存在）：内核 tick 使用 ALARM0 + TIMER\_IRQ\_0，直接覆盖 Pico SDK 默认 alarm\_pool handler → TinyUSB 的 CDC/MSC 端点握手定时 / SCSI 超时全部失效 → USB 枚举中途卡死 → Windows 识别不出 COM 口和盘符。✅ 修复：改用 **ALARM3 + TIMER\_IRQ\_3**（Pico SDK 保留给用户用，不冲突）。FAQ 新增 Q1\~Q5 3 步硬件自检流程。 |
| **v2.2.5**         | ✅ Released | 🟢 **调度系统完全正常（收尾修复）**：① **boot\_setup 任务状态机 Bug**：`task_suspend(g_current_task)` 后又执行 `while(1) task_sleep(1000)` → task\_sleep 不检查 state，强制把 SUSPEND 覆盖成 SLEEP 并塞入睡眠队列 → 1000 tick 后 boot\_setup "复活" → 再次执行 demo\_app\_init 重复创建已存在的任务 → g\_task\_pool[] 指针被新 TCB 覆盖，旧 TCB 悬空 → `ps` 输出 ID=0x20000xxx 乱码 / LED 任务 tight loop 爆闪。修复：task\_suspend 后空 while(1) 等待 PendSV 切换（SUSPEND 永久不调度）。② **Shell 栈溢出踩坏 TCB**：`SHELL_TASK_STACK=768B`，`cmd_i2c fill` 用 `uint8_t block[256]`、`cmd_i2c rd` 用 `buf[256]`，加上 I2C 驱动调用栈和 PendSV 中断栈帧（64B）→ 768B 不够 → 溢出写坏相邻堆内存（g\_task\_pool[] 的指针）。修复：SHELL\_TASK\_STACK → 2048B。③ 验证：`ps` 输出 ID=0 idle / 1 boot\_setup(SUSPEND) / 2 shell(RUNNING)，所有任务状态与队列归属契约 100% 符合设计，OLED I2C 初始化 + fill 1024B 分块发送后 `ps` 依然干净。 |
| **v2.2.6**         | ✅ **当前稳定** | 🟢 **Linux 风格任务控制 + vtest 三任务嵌套调度验证器**：① **vtest 命令**：VT1(LED 心跳 500ms) + VT2(OLED 每 2s 刷 1024B GDRAM) + VT3(每 10s 堆压力 alloc/free + 嵌套 suspend/resume VT2)，一次性验证 5 大调度器契约（时间片轮转 / SLEEP 队列 / SUSPEND 隔离 / 堆零碎片 / TCB 队列不损坏）。② **Ctrl+C 中断**：PuTTY 按 Ctrl+C → 停止 vtest + 清空输入行，类似 Linux 信号中断。③ **jobs 命令**：列出用户创建的后台任务（跳过 idle/boot_setup/shell）。④ **readline 输入行保护**：后台任务输出前清当前行 + 输出后恢复 `mk> ` + 用户已输入内容，解决 VT3 刷屏打断输入的问题。⑤ **Bug 修复**：VT3 `void *p[8]` 未初始化 → kmalloc 失败 break 后 kfree 野指针 → HardFault（修复：`= {0}` 强制清零）；任务退出循环后函数 return → LR=0 → HardFault 爆闪（修复：`task_suspend(self)` 自挂起）；VT2 栈 768B 溢出踩坏 heap header（修复：1536B）。 |
| **v2.2.7**         | ✅ Released | 🟢 **0.96" I2C OLED 底层驱动重构 + 调度权重可调**：① 底层驱动抽离——命令/数据统一走 16 位 I2C 编码（Co 控制字节），`set_addr` 改旧式寻址 + 列偏移，`write_block` 数据块写入，与网上通用 0.96 驱动写法对齐，解决花屏/寻址错位。② 驱动缓冲全部从任务栈移出到 BSS 静态区，消除 GCC nested function trampoline 与栈帧指针错乱导致的 HardFault。③ I2C 事务失败自动重试 + `hal_i2c_init` 外设复位重试，解决 VT3 suspend/resume 打断白帧刷新导致 page 1~7 全挂。④ I2C 时钟 100kHz → 400kHz，刷新提速 4 倍。⑤ VT2 权重 1 → 8（时间片 5ms → 40ms），整帧 25ms 在一个时间片内跑完 → 肉眼瞬间整屏切换（消除逐行扫描）。⑥ `help` 新增"调度说明"：weight=时间片倍数、非抢占、无优先级。⚠️ 曾标注已知问题：应用层轮询连续 >30 次 HardFault 崩溃风险 → **已在 v2.3.6 根治**。 |
| **v2.3.6**         | ✅ **当前稳定**（HEAD） | 🟢 **根治 >30 次轮询崩溃（I2C 单事务原子性）**：**根因**——VT3 每 10s 对 VT2 做 `task_suspend/resume`，若 VT2 恰在 `i2c_write_timeout_us` 中途被挂起 3s，SDK 的 200ms 超时是"调用起点算起的绝对截止"，恢复后立即判超时 → `HAL_ERR_IO` → 反复 `reinit`（`i2c_init` 复位外设）→ 累积约 30 轮后 I2C 外设状态/堆被反复复位打乱 → HardFault；同时表现为"屏幕刷新错误随轮询递增"。**修复**——用 PRIMASK 临界区包住 VT2 的"单次 I2C 事务"（`vt2_oled_tx_cmds` / `vt2_oled_write_block` 内的 `hal_i2c_tx`），使 PendSV 上下文切换无法打断一次 START..STOP 事务（400kHz 下单次约 6B~133B，最多 ~3ms 关中断）；VT3 的 suspend 在 VT2 完成本次事务、重新开中断后才生效，VT2 永不带着过期截止指针被挂起。 |
| v2.3               | 📋 规划      | RP2040 板载 TFT (SPI 线路 B 剩余) + 图形 API 演示                                                                                                                                                                                                                                                |
| **v2.4.0**         | 🔬 探索（HEAD） | 🟡 **超频档位 + 多核固化**：默认单核 + 125MHz；`ovclk list/set/mcore/get/save/unsave` 指令设置档位（125/150/200/250/270/300MHz，后两档隐藏）与多核标志，固化到 MSC 尾部新划的独立 **Flash Config 区**（4KB，魔数+版本+CRC32 保护）；冷启动 `config_apply()` 在 `sched_start` 前应用，未固化/损坏安全回退单核 + 125MHz。超频切换 SYS PLL + CLK\_PERI 钳回 125MHz + SSI/XIP 分频缩放 + vreg 升压。多核调度器为脚手架（标志持久化 + `config_mcore_apply` 弱钩子，实装待 v3.5）。                                                                                                                                                                                                                                                |
| v3.0               | 🗓️ 规划     | VFS 抽象层 + 多分区 + SD Card (SPI1)                                                                                                                                                                                                                                                         |
| v3.5               | 🗓️ 规划     | 多核调度 (RP2040 Core 1 唤醒) + RISC-V RV32 移植                                                                                                                                                                                                                                               |

***

## 🎯 核心特性表

| 类别                         | 特性                        | 指标 / 说明                                                                        |
| -------------------------- | ------------------------- | ------------------------------------------------------------------------------ |
| **体积**                     | 最小内核（仅调度/堆/上下文）           | **Flash ≤ 10KB, RAM ≤ 4KB**                                                    |
| <br />                     | 完整基础版（Shell + 外设 + 固化子系统） | Flash \~76KB, RAM \~9KB（不含 Pico SDK）                                           |
| <br />                     | RP2040 演示版 (v2.2)         | Flash \~111KB, RAM \~20KB（含 Pico SDK + USB + FatFs）                            |
| **调度**                     | 策略                        | 时间片轮转 + 权重比例（**无优先级抢占**，非硬实时）                                                  |
| <br />                     | 防重入修复                     | `sched_ready_pick_next` 不复入队；`sched_do_switch` 仅 `RUNNING` 状态重入队；**无自引用链表死循环** |
| <br />                     | 🟢 v2.2.5 状态机验证              | boot\_setup(SUSPEND) / shell(RUNNING) / idle(READY) 状态+队列契约 100% 合规；OLED I2C fill 1024B 后 `ps` 仍干净；长时间无爆闪/乱码 |
| **内存**                     | 模型                        | 固定内存池（内核对象）+ 简易堆（首次适配），**零碎片**                                                 |
| <br />                     | 临界区                       | `kmalloc/kfree` / `task_sleep` / 就绪队列均 `mrs/msr PRIMASK` 保护（Cortex-M 通用）       |
| **移植**                     | 分层                        | 4 层垂直：应用 → Syscall → Kernel Core → HAL                                         |
| <br />                     | 工作量                       | 仅重写 `port/<mcu>/`（HAL 接口 + 上下文切换汇编），核心 100% 复用                                 |
| **首发平台**                   | MCU                       | RP2040 (Cortex-M0+, 双核但当前 v2.2 仅单核 Core 0)                                     |
| <br />                     | Flash                     | W25Q16JV 2MB SPI Flash（板载）                                                     |
| **✨ 指令固化 (v0.2)**          | 存储                        | 末尾 2×4KB 双备份扇区 + CRC8 校验                                                       |
| <br />                     | 容量                        | 32 × 123B 命令槽 (slot)                                                           |
| <br />                     | 用法                        | `! cmd` 立即执行+固化；`save cmd` 仅固化；开机自动回放                                          |
| **✨ 出厂重置 (v0.2)**          | `factory_reset confirm`   | 两步确认擦 bootscript + 末尾 64KB 保留区，保留 OS 固件本身                                      |
| **✨ USB Composite (v2.2)** | 双接口                       | **CDC（串口调试 COMx）+ MSC（可读写 U 盘）** 同时枚举                                          |
| <br />                     | VID/PID                   | 沿用 Pi Foundation (0x2E8A / 0x0005)，Windows 自动识别驱动                              |
| **✨ Flash 三分区 (v2.2)**     | 固件区                       | `0x000000` – `0x0FFFFF` (**1 MiB**) — kernel + app + spiflash 驱动               |
| <br />                     | MSC 数据盘                   | `0x100000` – `0x1FEFFF` (**1012 KiB, 2032 × 512B**) — U 盘 & FatFs 共用           |
| <br />                     | Bootscript                | `0x1FF000` / `0x1FF000+4096` (**2 × 4 KiB**) — 固化指令双备份扇区                       |
| **✨ FatFs 目录 (v2.2)**      | 文件系统                      | Elm FatFs R0.15 — **FAT16**，首启动空片自动 `f_mkfs`                                   |
| <br />                     | 目录命令                      | `ls / cd / pwd / mkdir / rmdir / rm / cat`（**真正目录树，非地址索引**）                    |
| <br />                     | 代码页                       | 437（英文 OEM）；仅 8.3 短名（FF\_USE\_LFN=0，最省 RAM）                                    |
| **✨ 写互斥 (v2.2)**           | 原则                        | USB MSC 与 Shell 写**不同时进行**，通过 `ejected` 状态切换                                   |
| <br />                     | `msc eject`               | ejected=true：电脑显示"无介质"；Shell 可读可写 `mkdir/rm/cat`                               |
| <br />                     | `msc mount`               | ejected=false：电脑 U 盘可读写；Shell 仅读（`ls/cat`）                                     |
| <br />                     | `msc format`              | 强制 ejected=true → `f_mkfs FAT16` → 回到 ejected=true                             |
| **✨ 诊断闪灯 (v0.2)**          | 心跳 500ms                  | 系统正常运行                                                                         |
| <br />                     | 1s 超慢闪                    | 卡在 idle（所有任务睡眠）                                                                |
| <br />                     | 5Hz 快闪                    | HardFault（50ms 爆闪）                                                             |
| <br />                     | 常亮/灭                      | 早期初始化崩溃（可 `MK_BOOT_DIAG_LED=1` 定位阶段）                                           |

***

## 💾 RP2040 演示版：Flash 分区布局（v2.2）

```
W25Q16JV (2 MiB = 0x200000 = 2,097,152 B)
┌────────────────────────────────────────────────────────────┐ 0x000000
│  【固件区 Firmware】1 MiB = 256 × 4KB sectors              │
│  mini_kernel + rp2040demo app + Pico SDK boot2             │
│  + hal_flash driver + Shell dispatch table                 │
│  👉 被电脑写 U 盘时 100% 不会碰这里！                        │
├────────────────────────────────────────────────────────────┤ 0x100000
│  【MSC 数据盘 Mass Storage】1012 KiB = 2032 × 512B         │
│  FatFs FAT16 分区，盘符在 Windows 下"可移动磁盘"            │
│  · 512B/sector（MSC 标准）                                 │
│  · 底层通过 msc_blockdev 4KB RMW（读 4KB→改 512B→擦→写回） │
│  · Shell (ejected=true) 与 主机 USB (ejected=false) 互斥写  │
├────────────────────────────────────────────────────────────┤ 0x1FE000
│  【Bootscript A】4 KiB = 1 × 4KB sector                    │
│  固化条目 slot 0..31 (双备份主)                             │
├────────────────────────────────────────────────────────────┤ 0x1FF000
│  【Bootscript B】4 KiB = 1 × 4KB sector                    │
│  固化条目 slot 0..31 (双备份副 / 对比恢复)                  │
└────────────────────────────────────────────────────────────┘ 0x200000 (2MB 结束)
```

**分区宏定义文件**：[flash\_layout.h](file:///E:/ppCD/project/mini-kernel/include/hal/flash_layout.h)
（所有分区起始 / 大小 / 对齐检查统一集中在此，修改此处即可全局生效）

***

## 🚀 快速开始：构建 + 烧录 RP2040 演示版

### 1. 环境准备 (Windows)

```powershell
# ARM GCC 工具链 + CMake + Ninja（确保在 PATH）
$env:PICO_SDK_PATH = "E:\ppCD\pico-sdk"

# Pico SDK（包含 TinyUSB + FatFs 源码子模块）
git clone --depth 1 https://github.com/raspberrypi/pico-sdk.git $env:PICO_SDK_PATH
Set-Location $env:PICO_SDK_PATH
git submodule update --init   # 必须：获取 tinyusb / lib/fatfs / btstack 等子模块
```

### 2. 一键构建 rp2040demo (v2.2 固件)

```powershell
Set-Location E:\ppCD\project\rp2040demo
.\build.bat          # 构建；完成后自动 pause 看体积检查（默认）
# 或无等待（CI）
.\build.bat /nowait
```

**产物路径**（真正烧录的文件）：

```
E:\ppCD\project\rp2040demo\build\rp2040demo.uf2   ← 拖拽烧录
E:\ppCD\project\rp2040demo\build\build.log         ← 编译日志（失败看 tail）
```

> 如果你只想单独构建 mini-kernel 静态库（不烧录）：
>
> ```powershell
> Set-Location E:\ppCD\project\mini-kernel
> .\build.bat /nowait
> ```
>
> 输出静态库（给你的独立应用用）：`build/libkernel_core.a / libhal_port.a / libshell_module.a / libfatfs_module.a`

### 3. 烧录运行（UF2 拖拽法 · 最简单）

1. **按住 Pico 的 BOOTSEL 键** → 插入电脑 USB 线（不要松手）→ 松开按键
2. 资源管理器出现 **`RPI-RP2`** 盘符（约 128MB 烧录模式 U 盘）
3. 把 `rp2040demo.uf2` **复制到** `RPI-RP2` 盘符 → Pico 自动重启 → 启动 v2.2 固件

### 4. 打开两个 Windows 设备 🎉

烧录成功后 Pico 会重新枚举 USB，设备管理器出现：

* **端口 (COM & LPT)** → `USB Serial Device (COMx)` — 命令行调试通道

* **磁盘驱动器** → 新的 **可移动磁盘**（容量 \~992 KB，FAT16）— MSC 数据盘

> **注意**：首启动默认 `ejected=true`（Shell 独占写模式），所以"可移动磁盘"会显示"请插入磁盘"。
> 想要电脑真正看到 U 盘，先打开串口终端，输入 `msc mount`。

### 5. 串口终端推荐设置

* 波特率：115200

* 数据位：8

* 停止位：1

* 校验：无

* 流控：无（Pico SDK stdio\_usb 是纯 USB，波特率仅形式参数，可任意值）

推荐工具：PuTTY / TeraTerm / VSCode Serial Monitor。连接后看到：

```
============================================================

 === Mini Kernel Boot ===

============================================================

========================================
  Mini Kernel Demo Platform Ready
========================================
Kernel version: 2.2.5 ✅ STABLE
Config: MAX_TASKS=16  HEAP=8192B  TICK_HZ=1000  TIME_SLICE=5
PeriphService=ON  Shell=ON  VFS=OFF  FatFs=ON
========================================

[BOOT  ] Mode: Sync-Interactive (commands only, no async logs)
[BOOT  ] Supported buses: GPIO SPI I2C UART
[BOOT  ] Creating interactive shell task...
[BOOT  ] Pre-init: GPIO25 (LED) OUT, LOW (ready for bootscript)
[BOOT  ] Entering persistent boot command playback (shell_start → bootscript_run_all)...

==============================================================
=====         BOOTSCRIPT START (persistent cmd playback)        =====
==============================================================
[BOOT ] No persistent commands saved yet. Use 'save <cmd>' or '!<cmd>'.
[BOOT ] GPIO25 (LED) level after bootscript = LOW
[BOOT ] Summary: 0 ok / 0 failed (total=0)
==============================================================
=====         BOOTSCRIPT DONE                                   =====
==============================================================
[BOOT  ] MSC: Blank flash → auto-f_mkfs FAT16 (1012KiB data partition).
[BOOT  ] MSC: FatFs mounted OK. Mode = SHELL-EXCLUSIVE (ejected=true).
[BOOT  ] MSC: To expose drive to host PC, run:  msc mount
[BOOT  ] MSC: Shell commands (mkdir/rm/cat) work now; no `msc eject` needed.

[BOOT  ] Shell ready. Connect USB CDC COMx and type 'help' or 'gpio help'.

========================================
  Mini Kernel Interactive Shell Ready
  Version: 2.2.4 ✅ STABLE
  Type 'help' to list commands.
========================================

mk>
```

***

## 📘 Shell 命令手册 (v2.2)

### 1. 基础内核命令

| 命令                      | 说明                                                               |
| ----------------------- | ---------------------------------------------------------------- |
| `help`                  | 列出所有已注册命令（含 GPIO/I2C/SPI/MSC/FS 子命令）                             |
| `ps`                    | 所有任务列表（ID / Name / State / Ticks to sleep / Stack used / Weight） |
| `heap`                  | 内核堆空闲字节 / 最大连续空闲块                                                |
| `version`               | 打印版本号（含当前宏：MAX\_TASKS / HEAP / TICK\_HZ / TIME\_SLICE / 各模块开关）   |
| `led on / off / toggle` | 板载 LED（GPIO25）开关翻转                                               |

### 2. 指令固化 & Boot 控制 (v0.2+)

| 命令                      | 说明                                                                   |
| ----------------------- | -------------------------------------------------------------------- |
| `! <cmd>`               | **立即执行** **`<cmd>`**，成功后自动追加写入 Flash Bootscript 双备份（带剩余 slots 汇报）    |
| `save <cmd>`            | **不立即执行**，仅把 `<cmd>` 追加写入 Flash（下次开机自动跑）                             |
| `list`                  | 列出所有固化条目（`#0: cmd` / `#1: cmd` …），汇报 `used N / free 32-N / 32 slots` |
| `unsave <id>`           | 删除第 `id` 条（0-based）                                                  |
| `unsave all`            | 一键清空所有固化条目                                                           |
| `boot exec`             | 立刻同步回放所有固化（不等下次开机），刷新 RAM 常驻 `boot status`                           |
| `boot status`           | 查看本次开机 bootscript 回放结果（OK / FAIL / RC / 每条命令展开）+ GPIO25 level 验证     |
| `boot flash_test`       | SPI Flash HAL 自检：双备份扇区 A/B → 擦 → 写伪随机 → 读 → CRC → 一致性对比              |
| `factory_reset`         | 仅打印警告与影响范围（安全，不真正擦除）                                                 |
| `factory_reset confirm` | **真正执行**：擦 Bootscript 双备份 + 末尾 64KB 保留区（保留固件本身），完成后建议重新上电            |

### 3. 外设子命令（GPIO / I2C / SPI / UART）

```
mk> gpio help              # 子命令：init/read/write/toggle
mk> i2c help               # 子命令：init/scan/read/write/cmds/fill
mk> spi help               # 子命令：init/xfer/flash_id/flash_read
mk> uart help              # 子命令：init/write/read
```

### 4. ✨ MSC U 盘管理命令 (v2.2 新增)

```
mk> msc help               # 子命令一览
mk> msc status             # 分区总览：总容量 / 已用 / 空闲 / 介质 ejected? / FatFs mounted?
mk> msc mount              # ejected=false → 电脑显示可移动磁盘（电脑可写；Shell 只能读 ls/cat）
mk> msc eject              # ejected=true  → 电脑显示"请插入磁盘"（Shell 可 mkdir/rm/cat 写）
mk> msc format             # 两步确认：ejected=true → f_mkfs(FAT16, 1012KiB) → 重新挂载
```

### 5. ✨ 文件系统 & 目录命令 (v2.2 新增 · 真正目录树，非地址索引)

> 前提：**写入类命令要求 ejected=true**（先 `msc eject`，否则拒绝）。只读类命令任何状态都 OK。

```
mk> pwd                    # 打印当前工作目录（绝对路径，以 / 开头）
mk> ls [path]              # 列出目录（文件 + <DIR> 子目录 + 大小）
mk> cd <path>              # 切换目录（支持 绝对 / 相对 / .. / 父目录）
mk> mkdir <name>           # 创建子目录（只能在 ejected=true 下写）
mk> rmdir <name>           # 删除空目录
mk> rm <file>              # 删除文件
mk> cat <file>             # 以十六进制 + ASCII dump 方式查看文件内容（只读）
```

***

## 🔄 典型使用流程：电脑 ↔ Shell 文件共享（v2.2）

### 场景 A：电脑拷贝文件进 U 盘 → Shell 端查看

```
# Step 1: Shell 端先让出写权限给电脑
mk> msc mount
OK: MSC media MOUNTED (ejected=false). Host PC can now write to the removable drive.
⚠️  Shell is now READ-ONLY. To modify files, run:  msc eject

# Step 2: 电脑端往可移动磁盘（U 盘）里拷贝 / 创建子目录
#         例如拷贝 hello.txt 和建 docs/ 子目录

# Step 3: 电脑端安全弹出 U 盘后，Shell 端收回写权限
mk> msc eject
OK: MSC media EJECTED (ejected=true). Host PC now shows "No Media".
Shell can now mkdir / rm / cat (no concurrent writes).

# Step 4: Shell 端验证
mk> ls
-rwxrwxrwx       18  hello.txt
drwxrwxrwx        0  docs/
2 files, 1 dirs (free 967 KB)

mk> cat hello.txt
00000000  48 65 6C 6C 6F 2C 20 50  69 63 6F 20 4D 69 6E 69  |Hello, Pico Mini|
00000010  20 4B 65 72 6E 65 6C 21  0A                       | Kernel!.|
OK: 18 bytes read.
```

### 场景 B：Shell 端创建文件 / 子目录 → 电脑端读取

```
# Step 1: 确保 ejected=true（首启动默认如此；若已 mount 先 eject）
mk> msc status
MSC Partition (0x100000..0x1FEFFF, 1012 KiB)
  Media ejected  : YES (Shell can write)
  FatFs mounted  : YES
  Used / Free    : 28 KB / 984 KB

# Step 2: Shell 创建子目录 + 固化命令（实际写文件可用 future f_write API，
#         当前 v2.2 目录 API 仅 Shell 侧 mkdir/rm；写文件一般走电脑 U 盘拷贝路径）
mk> mkdir configs
mk> ls
drwxrwxrwx        0  configs/
drwxrwxrwx        0  docs/
-rwxrwxrwx       18  hello.txt

# Step 3: 交给电脑读取
mk> msc mount
# 电脑端打开可移动磁盘 → configs/ docs/ hello.txt 全部可见
```

***

## 🎓 完整使用教程（Linux man 风格）

本章节按 Linux man 手册风格组织，按使用场景分模块讲解。每个命令包含**语法 / 参数 / 返回 / 示例 / 错误 / 注意事项**六段式说明。

### man 1: 系统启动 & 交互模式

#### 启动流程

```
上电 → boot2 → crt0 → main() → kernel_main()
                                   │
                                   ├─ kmem_init / task_module_init / sched_init (关中断)
                                   ├─ 创建 boot_setup 任务 (SUSPEND 后永久挂起)
                                   ├─ hal_systick_init (ALARM3 + TIMER_IRQ_3, 1kHz)
                                   └─ sched_start → SVC 切到第一个任务 (boot_setup)
                                              │
                                              ├─ hal_systick_init → cpsie i
                                              ├─ 打印 banner
                                              ├─ demo_app_init → 创建 shell 任务
                                              │   └─ shell_start → bootscript_run_all (回放 save/! 固化命令)
                                              │                   → 进入 shell 主循环
                                              ├─ fatfs_init_and_mount (首启动自动 f_mkfs FAT16)
                                              └─ task_suspend(self) → 永久 SUSPEND (不再调度)
```

#### 交互模式特点（v2.2 起）

* **纯同步交互式**：用户输入命令 → 解析 → 执行 → 打印结果 → 等下一条命令
* **无异步后台任务刷屏**：心跳/内存压力/ctrl 任务不再创建，避免打断用户输入
* **唯一持续运行的任务**：`shell` (ID=2)。idle 任务作为调度器兜底，不主动打印
* **特殊场景**：`vtest` 验证命令会临时创建后台任务并打印日志（用户主动触发）

#### 提示符与命令格式

```
mk> <command> [arg1] [arg2] ...
```

* `mk>` = Mini Kernel 提示符
* 命令大小写敏感（`PS` 不识别，必须 `ps`）
* 参数用空格分隔；十六进制需 `0x` 前缀（`0x3C` ≠ `3C`）
* 退格 / DEL 支持，方向键不支持
* 命令最大长度 128 字节，最多 16 个参数
* 行结束支持 `\r` / `\n` / `\r\n`，跨平台兼容

### man 2: 任务管理命令

#### ps — 列出所有任务

**语法**：`ps`

**输出列**：

| 列名 | 含义 |
|---|---|
| ID | 任务 ID（创建顺序，0=idle, 1=boot_setup, 2+=用户任务）|
| Name | 任务名（task_create 时传入，最长 12 字符）|
| State | READY / RUNNING / SLEEP / SUSPEND / DEAD |
| TicksLeft | 若 SLEEP 状态，剩余唤醒 tick 数；其他状态为 0 |
| StackBase | 任务栈顶地址（堆分配），用于调试 |
| StackSize | 栈大小（字节）|
| StackOK | Y=栈未溢出；N=栈溢出（应立即排查）|

**示例**：

```
mk> ps

--- Task List ---
ID  Name        State     TicksLeft  StackBase  StackSize  StackOK
0   idle        READY    0 0x200027E8 256 Y
1   boot_setup  SUSPEND  0 0x200058A8 2048 Y
2   shell       RUNNING  0 0x200060F0 2048 Y
Cur: shell
-----------------
```

**注意事项**：
* `Cur: <name>` 显示当前正在运行的任务
* `StackOK=N` 说明栈溢出，需检查 task_create 的 size 参数
* ID 出现 `536883988` 等乱码值 → TCB 损坏（已在 v2.2.5 修复）
* `boot_setup` 永远是 SUSPEND，不会复活（v2.2.5 保证）

#### suspend / resume / kill — 任务控制

**语法**：

```
suspend <id>     # 挂起指定任务（移出所有队列，不参与调度）
resume  <id>     # 恢复挂起的任务（重新进入就绪队列）
kill   <id>      # 销毁任务（TCB + 栈内存回收，不可恢复）
```

**示例**：

```
mk> ps                          # 假设有 ID=3 的 vt_led 任务
mk> suspend 3                   # 挂起 vt_led → LED 停止闪烁
mk> ps                          # vt_led 的 State = SUSPEND
mk> resume 3                    # 恢复 vt_led → LED 继续闪烁
mk> kill 3                      # 销毁 vt_led（不可恢复）
```

**注意事项**：
* `suspend` 和 `resume` 必须配对，挂起后任务不会被任何 tick 唤醒
* `kill` 会立即回收 TCB 和栈内存，再次 `ps` 该任务消失
* 不能 `kill 0`（idle）或 `kill 1`（boot_setup），会返回错误
* 自挂起（`suspend <self_id>`）会触发 PendSV 立即切换到下一个任务

### man 3: 系统诊断命令

#### heap / tick / version / syscalls — 系统信息

**语法**：

```
heap              # 内核堆空闲字节 + 最大连续空闲块
tick              # 当前系统 tick（@1kHz = 自启动毫秒数）
version           # 内核版本 + 编译宏配置
syscalls          # SVC 系统调用契约表（开发者参考）
```

**示例**：

```
mk> heap
Heap free     = 3952 B
Heap max blk  = 3952 B          ← 零碎片：free == max_block 说明堆完全连续

mk> tick
Tick = 465249  (@1000 Hz)       ← 系统已运行 465 秒

mk> version
Kernel version: 2.2.5 ✅ STABLE
Config: MAX_TASKS=16  HEAP=8192B  TICK_HZ=1000  TIME_SLICE=5
PeriphService=ON  Shell=ON  VFS=OFF  FatFs=ON
```

**解读技巧**：
* `heap free` 与 `max blk` 相等 = 零碎片；差值越大碎片越严重
* `tick` 长时间不增长 = 调度器卡死（HardFault 或死循环）
* `version` 显示编译时的 `OS_CFG_*` 宏，用于确认裁剪配置

### man 4: 持久化指令（Bootscript）

Bootscript 是 Mini Kernel 的"开机自启"机制，类似 Linux 的 `/etc/rc.local` 或 systemd unit。

#### save / ! / list / unsave — 固化命令

**语法**：

```
! <command> [args...]     # 立即执行 + 固化到 Flash（双备份 + CRC8）
save <command> [args...]  # 仅固化，不立即执行
list                      # 列出所有固化条目
unsave <id>               # 删除指定序号条目（0-based）
unsave all                # 清空所有固化条目
```

**存储规格**：
* 容量：32 条命令 × 123 字节
* 位置：Flash 末尾 `0x1FF000` / `0x1FF000+4096` 双备份扇区
* 校验：CRC8，开机自动对比 A/B 两份，损坏的扇区自动恢复

**示例（OLED 初始化场景）**：

```
# 一键保存 17 条 OLED 初始化命令到 Flash
mk> save i2c init 0 4 5 100000
mk> save i2c cmds 0 0x3C 0xAE
mk> save i2c cmds 0 0x3C 0xD5 0x80
# ... 共 17 条
mk> list
Persistent commands (used 17, free 15/32 slots, Flash 2x backup):
  #0: i2c init 0 4 5 100000
  #1: i2c cmds 0 0x3C 0xAE
  ...

# 立即执行 + 固化（! 前缀）
mk> ! led on                # LED 立刻亮 + 固化"led on"到下次开机

# 删除第 5 条
mk> unsave 5

# 全部清空（回到出厂状态）
mk> unsave all
```

**注意事项**：
* `save` 不立即执行，需重启或 `boot exec` 才跑
* `!` 立即执行 + 固化（原子操作）
* 固化命令过长（>123B）会被静默截断，建议拆分
* `factory_reset` 会擦除 Bootscript + 末尾保留区，但保留固件本身

#### boot exec / boot status / boot flash_test — 启动控制

```
boot exec         # 立即同步回放所有固化命令（不等下次开机）
boot status       # 查看本次开机 bootscript 回放结果（OK / FAIL / 每条展开）
boot flash_test   # SPI Flash HAL 自检：A/B 扇区擦写读 CRC 一致性
```

**使用场景**：
* 刚 `save` 完一批命令，不想重启 → `boot exec` 立即跑
* 开机时 banner 没看清 → `boot status` 看 RAM 常驻回放结果
* 怀疑 Flash 硬件故障 → `boot flash_test` 自检

### man 5: 外设子命令（GPIO / I2C）

#### gpio — 通用 IO 控制

```
gpio help                                 # 子命令总览
gpio init <pin> [in|out] [0|1]            # 初始化引脚（默认 out, 0）
gpio read <pin>                           # 读取引脚电平
gpio write <pin> 0|1                      # 写引脚
gpio toggle <pin>                         # 翻转引脚
```

**示例**：

```
mk> gpio init 25 out 1         # GPIO25 (LED) 输出高
mk> gpio toggle 25             # 翻转 LED
mk> gpio read 15               # 读 GP15 电平
```

#### i2c — I2C 主机控制

```
i2c help                                           # 子命令总览
i2c init <bus> <sda> <scl> <hz>                    # 初始化 I2C 总线
i2c scan <bus>                                     # 扫描 7-bit 地址 0x08..0x77
i2c wr  <bus> <addr> <byte0> [byte1] ...           # 主机写（最多 16 字节）
i2c rd  <bus> <addr> <len>                         # 主机读 len 字节
i2c cmds <bus> <addr> <cmd0> [cmd1] ...            # 发命令序列（如 OLED 寄存器配置）
i2c fill <bus> <addr> <byte> <count>               # 填充 count 字节相同数据（用于 GDRAM 刷新）
i2c memwr <bus> <addr> <reg> <data>                # 写寄存器（先发 reg 再发 data）
i2c memrd <bus> <addr> <reg> <len>                 # 读寄存器
```

**地址格式**：必须 `0x` 前缀，如 `0x3C`（7-bit 地址，不含 R/W 位）

**示例（OLED SSD1306 完整初始化）**：

```
# 1. 初始化 I2C0：SDA=GP4, SCL=GP5, 100kHz
mk> i2c init 0 4 5 100000

# 2. 扫描设备
mk> i2c scan 0                  # 期望看到 0x3C

# 3. 发送 SSD1306 初始化序列（17 条命令）
mk> i2c cmds 0 0x3C 0xAE        # display off
mk> i2c cmds 0 0x3C 0xD5 0x80   # display clk div
mk> i2c cmds 0 0x3C 0xA8 0x3F   # multiplex 1/64
mk> i2c cmds 0 0x3C 0xD3 0x00   # display offset
mk> i2c cmds 0 0x3C 0x40        # display start line
mk> i2c cmds 0 0x3C 0x8D 0x14   # charge pump enable
mk> i2c cmds 0 0x3C 0x20 0x00   # horizontal addressing mode
mk> i2c cmds 0 0x3C 0xA1        # segment remap
mk> i2c cmds 0 0x3C 0xC8        # COM scan direction
mk> i2c cmds 0 0x3C 0xDA 0x12   # COM pins config
mk> i2c cmds 0 0x3C 0x81 0xCF   # contrast
mk> i2c cmds 0 0x3C 0xD9 0xF1   # pre-charge period
mk> i2c cmds 0 0x3C 0xDB 0x40   # VCOMH deselect
mk> i2c cmds 0 0x3C 0xA4        # resume display
mk> i2c cmds 0 0x3C 0xA6        # normal display
mk> i2c cmds 0 0x3C 0xAF        # display on

# 4. 测试：全屏亮 → 全屏黑
mk> i2c fill 0 0x3C 0xFF 1024   # 128×64 = 1024B 全亮
mk> i2c fill 0 0x3C 0x00 1024   # 全黑
```

**一键持久化**（开机自动初始化 OLED）：

```
mk> save i2c init 0 4 5 100000
mk> save i2c cmds 0 0x3C 0xAE
# ... 17 条
# 下次开机会自动按顺序回放
```

### man 6: vtest 三任务嵌套调度验证（v2.2.5 新增）

`vtest` 是验证调度系统完全正常的核心工具。创建 3 个并发任务，通过嵌套依赖关系一次性验证时间片轮转、SLEEP 队列、SUSPEND 状态机、堆零碎片、TCB 队列契约。

**语法**：

```
vtest start [bus] [addr]    # 启动 3 任务验证（默认 OLED bus=0, addr=0x3C）
vtest status                # 查看三任务计数器 + 堆快照
vtest stop                  # 停止验证 + 销毁 3 任务 + 灭 LED
```

**快速启动**：

```
mk> vtest start             # 用默认 OLED 配置启动
mk> ps                       # 看到 6 个任务
mk> vtest status             # 查看计数器
mk> vtest stop               # 停止验证
```

详见下方【vtest 案例分析】章节。

### man 7: 文件系统 & U 盘管理

#### msc — U 盘介质控制

```
msc help                    # 子命令总览
msc status                  # 分区总览：容量 / 已用 / 空闲 / ejected / mounted
msc mount                   # 让电脑可写（Shell 只读）
msc eject                   # 让 Shell 可写（电脑显示无介质）
msc format                  # 两步确认 → f_mkfs FAT16 → 重新挂载
```

**互斥原则**（重要）：

| 状态 | 电脑 U 盘 | Shell 文件操作 |
|---|---|---|
| ejected=true | "请插入磁盘" | 可 mkdir/rmdir/rm/cat（读写）|
| ejected=false | 可读写 U 盘 | 只能 ls/cat（只读）|

#### ls / cd / pwd / mkdir / rmdir / rm / cat — 目录命令

```
pwd                         # 当前工作目录（绝对路径）
ls [path]                   # 列目录（文件 + <DIR> + 大小）
cd <path>                   # 切换目录（支持 / .. 相对路径）
mkdir <name>                # 创建子目录（需 ejected=true）
rmdir <name>                # 删除空目录
rm <file>                   # 删除文件
cat <file>                  # 十六进制 + ASCII dump 查看文件
```

**示例**：

```
mk> pwd
/

mk> mkdir docs              # 需先 msc eject
mk> ls
drwxrwxrwx        0  docs/

mk> cd docs
mk> pwd
/docs

mk> cat hello.txt
00000000  48 65 6C 6C 6F 2C 20 50  69 63 6F 20 4D 69 6E 69  |Hello, Pico Mini|
00000010  20 4B 65 72 6E 65 6C 21  0A                       | Kernel!.|
OK: 18 bytes read.
```

### man 8: 系统维护命令

#### factory_reset — 出厂重置

```
factory_reset               # 仅打印警告（安全，不执行）
factory_reset confirm        # 真正执行：擦 Bootscript A/B + 末尾 64KB
```

**影响范围**：Bootscript 固化区 + 保留区；**保留**固件本身（kernel + app）。

**使用场景**：
* 固化了错误命令导致启动异常
* 想把设备完全恢复到出厂状态
* Flash 数据分区损坏需重建

#### clear / led — 辅助命令

```
clear | cls                 # 清屏 + 光标左上角（VT100）
led on | off | toggle       # 控制 GPIO25 LED
```

***

## 🧪 vtest 三任务嵌套调度验证案例分析

vtest 是 v2.2.5 调度系统完全正常的核心证明工具。本章节深度剖析其设计原理、嵌套关系、代码实现、能验证什么。

### 设计目标

一次性验证调度系统的 **5 大契约**：

| 契约 | 验证机制 |
|---|---|
| 1. 时间片轮转（非抢占）| VT2 阻塞 I2C 期间 VT1 继续跳 |
| 2. SLEEP 队列正确轮转 | VT1/VT2/VT3 各自 task_sleep 后能按时唤醒 |
| 3. SUSPEND 状态隔离 | VT3 挂起 VT2 时，VT1 不受影响 |
| 4. 堆零碎片 | VT3 每 10s alloc 8 块乱序 free，free 不变 |
| 5. TCB 队列不损坏 | 长跑 5min+ 后 ps 仍显示干净任务列表 |

### 三个任务的角色与嵌套关系图

```
       ┌─────────────────────────────────────────────────┐
       │            vtest start 一条命令                  │
       └────────────────────┬────────────────────────────┘
                            │ task_create × 3
            ┌───────────────┼───────────────┐
            ▼               ▼               ▼
    ┌─────────────┐ ┌─────────────┐ ┌──────────────┐
    │ VT1 (vt_led)│ │VT2(vt_oled) │ │VT3 (vt_ctrl) │
    │  stack=384  │ │  stack=768  │ │  stack=768   │
    │  weight=1   │ │  weight=1   │ │  weight=2    │
    └──────┬──────┘ └──────┬──────┘ └──────┬───────┘
           │               │               │
           │ 独立循环       │ 独立循环       │ 独立循环
           │ LED 翻转      │ 刷 OLED       │ alloc/free
           │ task_sleep    │ 1024B I2C     │ + 操作 VT2 状态
           │ (500ms)       │ task_sleep    │ task_suspend(VT2)
           │               │ (2s)          │ task_sleep(3s)
           │               │               │ task_resume(VT2)
           │               │               │ task_sleep(7s)
           │               │               │
           ▼               ▼               ▼
    LED 心跳 ~2Hz      OLED 每 2s      每 10s 一轮 Round
    (验证轮转)         换图案          (验证堆零碎片 +
                                      嵌套 suspend/resume)
           │               │  ▲           │
           │               │  │resume     │suspend
           │               │  │           │
           │     ┌─────────┴──┴───────────┘
           │     │  VT3 → VT2 嵌套控制
           │     │  (VT3 修改 VT2 的状态)
           ▼     ▼
      现象：每 13s OLED 冻结 3s，但 LED 继续跳
```

**嵌套关系总结**：
* VT3 操作 VT2 的状态（suspend/resume）→ 嵌套依赖 #1
* VT2 被 VT3 挂起期间，VT1 不受影响 → 嵌套依赖 #2
* VT3 自己 task_sleep 让出 CPU 给 VT1/VT2 → 嵌套依赖 #3

### VT1 代码剖析：为什么 LED 心跳能证明轮转正常

[shell.c: vt_task_led](file:///E:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1481-L1498)

```c
static void vt_task_led(void *arg) {
    register const uint32_t SIO_BASE = 0xD0000000u;
    register const uint32_t MASK25   = 0x02000000u;
    uint8_t on = 0;
    while (g_vtest_running) {
        on = !on;
        if (on) *(volatile uint32_t *)(SIO_BASE + 0x014) = MASK25;  // OUT_SET
        else    *(volatile uint32_t *)(SIO_BASE + 0x018) = MASK25;  // OUT_CLR
        g_vt1_beats++;
        task_sleep(500);   // ← 关键：500ms SLEEP
    }
}
```

**为什么能证明轮转正常**：

1. `task_sleep(500)` 把 VT1 状态改为 SLEEP，移入睡眠队列
2. 500 tick 后 `sched_sleep_tick` 把 VT1 移回就绪队列
3. `sched_do_switch` 选中 VT1 → 翻转 LED → 再次 sleep
4. **如果 VT2 正在阻塞 I2C，VT1 仍能每 500ms 准时翻转** → 证明 tick 中断（1ms）照样进 → kernel_tick_hook 照样递减 → 时间片耗尽照样 PendSV 切换

**失败现象**：LED 停跳或变慢 → 调度器卡死或 VT2 阻塞太久未切走

### VT2 代码剖析：I2C 阻塞期间调度器如何切走

[shell.c: vt_task_oled](file:///E:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1506-L1557)

```c
static void vt_task_oled(void *arg) {
    uint8_t block[256];
    block[0] = 0x40;  // D/C=1: 后续为 GDRAM 数据

    while (g_vtest_running) {
        // 5 chunks × 255B = 1275B (截断到 1024B 有效)
        for (uint8_t c = 0; c < 5; c++) {
            // 动态生成棋盘格图案
            for (uint16_t i = 1; i <= 255; i++) {
                block[i] = (pat_row & 1) ?
                          (0x55 + (frame & 0xFF)) :
                          (0xAA ^ (frame & 0xFF));
                // ...
            }
            hal_err_t r = hal_i2c_tx(bus, addr, block, chunk_sz + 1);
            // hal_i2c_tx 是忙等 I2C 硬件，不主动 yield
            // 但 tick 中断照样进 → 时间片耗尽 → PendSV 切走
        }
        g_vt2_frames++;
        task_sleep(2000);  // 2s SLEEP
    }
}
```

**关键洞察**：`hal_i2c_tx` 是**忙等阻塞**（不主动 yield），但这不影响其他任务：

1. RP2040 1kHz tick 中断（TIMER_IRQ_3）**总是能进**（中断优先级 > 任务）
2. `kernel_tick_hook` 递减 VT2 的 time_slice
3. time_slice 归零 → `hal_yield_trigger` 设 PENDSVSET
4. PendSV 在 TIMER_IRQ 退出后立即触发 → 切到 VT1 或 shell
5. VT1 跑 500ms 后 sleep → 又切回 VT2 继续 I2C 发送

**这就是"时间片轮转非抢占"的正确性证明**：即使一个任务不主动让出 CPU，调度器也能通过 tick 中断强制切走它。

### VT3 代码剖析：嵌套 suspend/resume + 堆压力测试

[shell.c: vt_task_ctrl](file:///E:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c#L1569-L1627)

```c
static void vt_task_ctrl(void *arg) {
    size_t sizes[8] = { 32, 64, 128, 96, 256, 48, 160, 80 };
    int    order[8] = { 3,0,7,2,5,1,6,4 };  // 乱序释放

    while (g_vtest_running) {
        g_vt3_rounds++;

        // ① 堆压力测试：alloc 8 块 → 写数据 → 乱序 free
        size_t free_before = kmem_free_size();
        for (int i = 0; i < 8; i++) {
            p[i] = kmalloc(sizes[i]);
            memset(p[i], 0x33 + i, sizes[i]);
        }
        for (int i = 0; i < 8; i++) kfree(p[order[i]]);
        size_t free_after = kmem_free_size();
        // 验证 free_before == free_after（零碎片）

        // ② 嵌套 suspend/resume VT2
        if (g_vt2) {
            task_suspend(g_vt2);     // VT2 → SUSPEND, OLED 冻结
            task_sleep(3000);         // 3s 内 VT1 继续跳, VT2 不动
            task_resume(g_vt2);      // VT2 → READY, OLED 恢复
        }

        task_sleep(7000);            // 下一轮（总周期 ~10s）
    }
}
```

**为什么能验证 SUSPEND 状态隔离**：

1. `task_suspend(g_vt2)` 把 VT2 移到 SUSPEND 状态（不在任何队列）
2. VT3 自己 `task_sleep(3000)` 进入 SLEEP
3. 这 3s 内，调度器只能从 VT1 + idle 中选 → VT1 继续每 500ms 翻转 LED
4. **VT2 被 SUSPEND 完全不影响 VT1** → 证明状态机隔离正确

**为什么能验证堆零碎片**：

1. 每 10s 分配 8 块不同大小（32~256B）
2. 按乱序释放（模拟真实使用场景）
3. `free_before == free_after && max_before == max_after` → 零碎片
4. 如果堆有碎片，差值会累积增长

### 7 项验证清单详解

| # | 验证项 | 现象 | 失败说明 |
|---|---|---|---|
| 1 | LED 永不停跳 | GPIO25 约 2Hz 稳定翻转 | 调度器卡死 / PendSV 未触发 |
| 2 | OLED 每 2s 换图案 | 屏幕整体换棋盘格 | VT2 未被调度 / I2C NACK |
| 3 | 每 13s OLED 冻结 3s | VT3 Round 时 OLED 停 | suspend 未生效 |
| 4 | 冻结期间 LED 继续跳 | VT1 不受 VT2 SUSPEND 影响 | 状态机隔离破坏 |
| 5 | `vtest status` 计数器单调递增 | beats/frames/rounds 都增长 | 某任务卡死 |
| 6 | `ps` 显示 6 个干净任务 | ID=0~5, State 合法 | TCB 损坏 / 队列重复入队 |
| 7 | `vtest stop` 后 heap 恢复 | 等于 start 前 | 内存泄漏 / TCB 未回收 |

### 通过 vtest 学到的调度器知识

1. **时间片轮转 = tick 中断驱动**：即使任务不主动 yield，tick 也会强制切换
2. **SLEEP 队列 = 有序链表**：`ticks_to_sleep` 递增排序，O(n) 插入
3. **SUSPEND = 完全隔离**：不在就绪/睡眠队列，不会被任何 tick 唤醒
4. **状态机契约**：每个状态对应唯一队列归属，跨队列操作必崩
5. **堆零碎片**：首次适配 + 合并相邻空闲块，乱序 free 不产生碎片
6. **TCB 池保护**：`g_task_pool[]` 指针必须干净，重复创建会覆盖悬空

***



## 🛠️ 配置裁剪总开关 ([os\_config.h](file:///E:/ppCD/project/mini-kernel/include/os_config.h))

> 修改 mini-kernel/include/os\_config.h，cmake 自动解析并传递编译宏。

| 宏                                        | 默认值    | 说明                                                                 |
| ---------------------------------------- | ------ | ------------------------------------------------------------------ |
| `OS_CFG_MIN_KERNEL_FLASH_KB / RAM_KB`    | 10 / 4 | 体积红线：最小内核规格                                                        |
| `OS_CFG_FULL_BASE_FLASH_KB / RAM_KB`     | 20 / 8 | 体积红线：完整基础版规格                                                       |
| `OS_CFG_TASK_MODULE`                     | 1      | 任务管理（task\_create / sleep / suspend）                               |
| `OS_CFG_SCHED_RR`                        | 1      | 时间片轮转调度器（必开；权重比例开 `OS_CFG_SCHED_WEIGHT`）                           |
| `OS_CFG_MEM_POOL` + `OS_CFG_KERNEL_HEAP` | 1 / 1  | 固定内存池 + 简易堆（kmalloc/kfree）                                         |
| `OS_CFG_PERIPH_SERVICE`                  | 1      | GPIO/SPI/I2C/UART 共享总线服务（Shell 命令底层）                               |
| `OS_CFG_SHELL`                           | 1      | 命令行终端（bootscript 固化 + msc + 文件系统命令）                                |
| `OS_CFG_VFS`                             | 0      | ⚠️ 虚拟文件系统抽象层（**v2.2 暂未实现**；FatFs 直接对接 diskio，不开）                   |
| `OS_CFG_FATFS`                           | **1**  | Elm FatFs FAT16（MSC U 盘 + Shell 目录命令；**v2.2 必开**）                  |
| `OS_CFG_LOADER`                          | 0      | 用户程序加载器（规划中，需 VFS=1）                                               |
| `OS_CFG_DEMO_APP`                        | 1      | examples/builtin\_demo/demo\_app.c（心跳 / LED / 内存压力 / ctrl 4 个示例任务） |
| `OS_CFG_MAX_TASKS`                       | 16     | 最大并发任务数（含 idle / boot\_setup）                                      |
| `OS_CFG_TICK_HZ`                         | 1000   | 系统滴答 Hz（1 tick = 1 ms）                                             |
| `OS_CFG_TIME_SLICE_TICKS`                | 5      | 默认时间片（5 ms × weight）                                               |
| `OS_CFG_TICKLESS`                        | 0      | （关）Tickless idle：防止 WFI 导致 CPU 永等中断                                |
| `OS_CFG_SHELL_HISTORY`                   | 8      | Shell 历史条目数（简单环形缓冲）                                                |

> **额外性能宏（非 OS\_CFG 命名）**：在 kernel.c 顶部
>
> * `MK_BOOT_DIAG_LED`（默认 `0` = 发布版，跳过 12 阶段 LED 诊断闪烁，启动 < 500ms；崩溃定位时改为 `1`）

***

## 🏗️ 架构设计 & 移植指南

### 分层架构图

```
┌──────────────────────────────────────────────────────┐
│  应用层 Application                                   │
│  rp2040demo / 用户任务 / demo_app.c tasks             │
├──────────────────────────────────────────────────────┤
│  系统调用层 Syscall                                    │
│  SVC 异常 + X-Macro 契约表 (syscall_contract.c)       │
├──────────────────────────────────────────────────────┤
│  内核核心 Kernel Core (100% 硬件无关，可移植)          │
│  task.c sched.c mem.c kernel.c modules/*              │
├──────────────────────────────────────────────────────┤
│  硬件抽象 HAL 统一接口 (include/hal/)                  │
│  hal_interface.h  hal_flash.h  flash_layout.h         │
├──────────────────────────────────────────────────────┤
│  HAL 移植实现 port/<mcu>/ (硬件相关)                   │
│  RP2040: hal_port.c  context_switch.S  msc_usb.c ...  │
├──────────────────────────────────────────────────────┤
│  硬件 Hardware                                         │
│  RP2040 Cortex-M0+ / W25Q16JV / GPIO / I2C / SPI / USB│
└──────────────────────────────────────────────────────┘
```

### 关键内核 Bug 修复总结（已同步到可移植 mini-kernel）

| Bug                | 表现                                               | 根因                                                   | 修复位置                                                                                                             |
| ------------------ | ------------------------------------------------ | ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| 上下文栈帧              | SVC 返回后 PC 乱飞 → HardFault 5Hz 爆闪                 | SVC 异常帧压 MSP，PSP 未被污染，PSP 应 = task->sp + 32          | [context\_switch.S](file:///E:/ppCD/project/mini-kernel/port/rp2040/context_switch.S) L264                       |
| 调度器队列自引用           | LED 疯狂闪烁（tight loop）；ps 命令输出乱码                   | pick\_next 后又 enqueue，同一任务在队列两次                      | [sched.c](file:///E:/ppCD/project/mini-kernel/kernel/core/sched.c) L72-L74                                       |
| SLEEP 任务被抢回        | task\_sleep 不生效，仍被调度 RUNNING                     | `sched_do_switch` 无条件把 from 设为 READY 并入队             | [sched.c](file:///E:/ppCD/project/mini-kernel/kernel/core/sched.c) L207-L212                                     |
| 临界区嵌套关闭中断          | PendSV 中途进入导致链表损坏                                | 用 `mrs/msr PRIMASK` 保存恢复，而非 `cpsid/cpsie i` 直接关开     | task.c / mem.c / sched.c 所有公共 API                                                                                |
| Bootscript LED 被覆盖 | 固化 `led on` 启动后 LED 仍灭                           | `_led_stage(13)` 在 demo\_app\_init 之后又 SIO\_OUT\_CLR | [kernel.c](file:///E:/ppCD/project/mini-kernel/kernel/core/kernel.c) — 阶段 13 合并到 12，demo\_app\_init 后永不再写 GPIO25 |
| 启动慢（10s+）          | 开机十几秒 LED 才心跳                                    | 12 阶段 \_led\_stage + 50M nop USB 忙等                  | `MK_BOOT_DIAG_LED=0` 默认关；删除 5s 忙等                                                                                |
| save 失败误报 Unknown  | `save cmd` 失败后，Shell 又打印 "Unknown command: save" | dispatch 按 rc==1 判"未知命令"，但 cmd\_save 返回 1 表示失败       | shell.c L1400 `bool found` 机制独立跟踪                                                                                |
| FatFs f\_mkfs 未定义  | 链接报错 `undefined reference to f_mkfs`             | SDK ff.h 的 "" include 优先 SDK 自带 ffconf.h（MKFS=0）     | CMakeLists 生成 fatfs\_shim：复制 SDK ff.h + ff.c + ffunicode.c + 自定义 ffconf.h 到同目录                                   |
| 🟢 boot\_setup "复活"（v2.2.5 修复） | ps 输出 ID=0x20000xxx 乱码、TCB 悬空、LED 爆闪 | task\_suspend 后紧跟 task\_sleep，SUSPEND 被覆盖 SLEEP 入睡眠队列 → tick 唤醒 → 重复 demo\_app\_init | [kernel.c](file:///E:/ppCD/project/mini-kernel/kernel/core/kernel.c) — task\_suspend 后空 while(1) 等待 PendSV，不再调用 task\_sleep |
| 🟢 Shell 栈溢出踩 TCB（v2.2.5 修复） | 执行 i2c fill/rd 后 ps 乱码、HardFault、LED 爆闪 | SHELL\_TASK\_STACK=768B 放不下 256B block[] + 256B buf[] + 调用栈 + 64B 中断栈帧 → 溢出写坏堆上相邻 g\_task\_pool[] | [shell.c](file:///E:/ppCD/project/mini-kernel/kernel/modules/shell/shell.c) — SHELL\_TASK\_STACK 768 → 2048 |

### 移植到新 MCU

仅需在 `port/<new_mcu>/` 目录下实现以下文件，核心零修改：

**必选（内核启动硬依赖）**

* `hal_port.c`：`hal_export` 全局表填满 `hal_systick_ops_t / hal_console_ops_t`

* `context_switch.S`：`PendSV_Handler`（save/restore r4-r11 + call `sched_do_switch`）和 `SVC_Handler`（首任务 PSP 切换 + bx EXC\_RETURN）

* 启动代码：链接脚本（定义 `__end__` 堆起始、Flash 大小）+ Reset\_Handler（最终调 kernel\_main）

*可选（按裁剪宏 OS\_CFG\_* 决定是否编译）\*

* GPIO / SPI / I2C / UART 操作实现（OS\_CFG\_PERIPH\_SERVICE=1 时）

* hal\_flash（erase\_sector / program / map\_read）— 指令固化 + MSC + FatFs 需要

* MSC + USB 描述符回调（v2.2 若要复用 USB Composite 模式）

***

## 📏 体积红线（RP2040 演示版构建后自动解析 .map）

```
-- Firmware Size: Flash = 111 KB, RAM = 20 KB
-- Limits:        Flash <= 20 KB, RAM <= 8 KB
CMake Warning at ... check_size.cmake:80
  FLASH SIZE EXCEEDS LIMIT: 111 KB > 20 KB (含 SDK 运行时)
CMake Warning at ... check_size.cmake:83
  RAM SIZE EXCEEDS LIMIT: 20 KB > 8 KB (含 SDK 运行时)
-- Size check done (warning-only).
BUILD OK - UF2 247296 bytes @ ...
```

> **说明**：v2.2 RP2040 演示版含 Pico SDK 运行时（boot2 / crt0 / stdio / TinyUSB / hardware\_\*）+ FatFs + MSC，体积远超"最小内核"和"完整基础版"的红线是预期的。
> 红线警告仅做 warning 不中断构建。最小内核和完整基础版体积请以关闭 demo\_app + Pico SDK 静态库单独统计为准。

***

## ⚠️ 明确的非目标（边界红线）

1. **不追求硬实时** — 无优先级抢占，不承诺微秒级确定性响应
2. **不做虚拟内存 / MPU 强隔离** — 单地址空间，协作式保护
3. **不做 Linux 式设备树 / 总线匹配** — 静态配置，零运行时解析
4. **不全量兼容 POSIX** — 仅核心常用 API subset
5. **不因性能牺牲可读性 / 体积可控性** — 优先架构清晰、代码精简

***

## ❓ 常见问题 FAQ

### 💡 Q1: 烧录后「完全看不到串口 / 设备管理器里没有 COM 口 / 也没有 U 盘」？（v2.2.4 修复）

这是最常见的用户报告症状。**请严格按 3 步自检，90% 的情况是第 1 步或第 3 步**，剩下 10% 是 v2.2.3 及之前版本的内核 tick alarm 冲突 Bug（已在 v2.2.4 修复）：

```
┌─ Step 1：先确认硬件没死（排除 UF2 没烧录成功 / 线坏了）─────────────────────┐
│                                                                              │
│  按住板子上的【BOOTSEL】按钮不放 → 按一下 RUN / 复位键（或拔插 USB）→        │
│  立刻进入 USB Mass Storage 模式 → Windows 出现 【RPI-RP2】盘符（≈128MB）。    │
│                                                                              │
│  ✅ 能看到 RPI-RP2 盘 → 硬件 / USB 线完全 OK → 进入 Step 2。                 │
│  ❌ 完全看不到 RPI-RP2 → 不是固件问题：                                       │
│     · 换一根能传数据的 USB 线（很多充电线只有电源没有 D+/D-！）               │
│     · BOOTSEL 按钮有没有弹起来卡死？按 RUN 键重试。                           │
│     · 板子有没有短路 / 供电不够？                                            │
└──────────────────────────────────────────────────────────────────────────────┘
                                  ↓
┌─ Step 2：重新烧录正确的 uf2 ────────────────────────────────────────────────┐
│                                                                              │
│  把项目里的 【rp2040demo/build/rp2040demo.uf2】拖进【RPI-RP2】盘符根目录。     │
│  → 几秒后 RPI-RP2 盘会自动消失 → RP2040 自重启，开始跑固件。                  │
│  → 【‼️ 关键】：RPI-RP2 盘符消失 ≠ 烧录成功后 Windows 立刻能识别 COM 口！    │
│     因为 Pico ROM 里的 bootloader 把 USB 重置了一次，Windows 需要重新枚举。  │
└──────────────────────────────────────────────────────────────────────────────┘
                                  ↓
┌─ Step 3：强制 Windows 重新枚举 USB ─────────────────────────────────────────┐
│                                                                              │
│  【‼️ 这步 90% 的用户都没做！】烧录成功后 RP2040 自重启 → **拔插一次 USB 线**│
│  （或者设备管理器 → 操作 → 扫描检测硬件改动）。                               │
│                                                                              │
│  正常情况下 1-2 秒后：                                                        │
│    · 设备管理器【端口 (COM 和 LPT)】出现一个新 COM 口（USB Serial Device）   │
│    · 【此电脑】下同时出现一个 ≈1012 KiB 的可移动磁盘（Mini Kernel U 盘）     │
│                                                                              │
│  ❌ 还是看不到串口？→ 是固件软件 Bug，请用 v2.2.4 或更高版本的 uf2：         │
│     · 🔴 v2.2.3 及以前的 Bug：内核 tick 使用 ALARM0 + TIMER_IRQ_0，           │
│       直接覆盖 Pico SDK 默认 alarm_pool（TinyUSB / stdio_usb 用它来跑        │
│       CDC 端点握手定时 / MSC SCSI 超时）→ USB 枚举到一半卡死 → 无 COM 无 U 盘│
│     · ✅ v2.2.4 修复：内核 tick 改用 ALARM3 + TIMER_IRQ_3，SDK alarm_pool    │
│       完整保留 → USB 枚举正常（hal_port.c systick 部分）。                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 💡 Q2: 烧录后串口能看到 COM 口，但 PuTTY 打开后一行字都没有？

* 串口参数一定设对：**115200 波特率 / 8 数据位 / 无校验 / 1 停止位** (115200-8-N-1)

* 如果还是没输出 → 打开 PuTTY 的【Serial → line discipline options → "Implicit CR in every LF" + "Implicit LF in every CR"】勾上

* 按一下回车，看 Shell 提示符 `mk>` 是否出现

* 如果提示符没出现，但板子 LED 在有规律闪烁（4Hz 心跳）→ 固件在跑但 banner 被 USB 枚举时序吃了 → 用 `boot status` 命令看 RAM 常驻启动回放结果

### 💡 Q3: U 盘盘符出现了，但 Windows 提示"需要格式化"？

* 这是首启动空片未格式化的正常过渡现象。如果 v2.2.0+ 的固件首启动时应已自动 `f_mkfs` FAT16，若提示格式化通常是 ejected=false（主机占用 USB）+ 首启动时自动格式化被跳过。

* **两步解决**：① 打开串口，打 `msc eject`（让 Shell 侧独占写）② 再打 `msc format` → 两步确认后重新 mkfs → 完成后打 `msc mount` → 拔插 USB 就看到正常 U 盘了。

### 💡 Q4: 为什么 mini-kernel 内核本体 Flash ≤ 20KB，实际 rp2040demo.uf2 有 247KB？

* 因为 `rp2040demo.uf2` 包含了完整的 **Pico SDK + TinyUSB + FatFs + startup ROM 引导头 + 中断向量表**，内核代码只占其中约 18KB。详细体积拆解见 `project/mini-kernel/README.md` §【体积红线说明】。

### 💡 Q5: 固化了一条错的 `! led on xxxx` 导致开机 LED 不对 / 系统启动错？

* 串口输入 `factory_reset` → 回车 → 两次确认后擦除 Bootscript A/B 双备份扇区（8KiB）→ Power-Cycle 即可回到出厂状态。

***

## 🤝 贡献指南

1. Fork & 分支
2. 编码规范：`clang-format --style=file -i`
3. 静态分析：`cppcheck --enable=all kernel/ port/`
4. 单测全过：`./build/unit_tests`
5. 演示平台固件必须 build 通过：`rp2040demo/build.bat` 返回 `===== BUILD SUCCESS =====`
6. 提交 PR + 说明修复 / 新增点 + 影响范围

***

## 📄 许可证

MIT License

***

## 🙏 致谢

* [Pico SDK](https://github.com/raspberrypi/pico-sdk) — RP2040 官方 SDK（含 TinyUSB / FatFs）

* [TinyUSB](https://github.com/hathach/tinyusb) — 超棒的嵌入式 USB 协议栈（Composite CDC+MSC 模板参考）

* [Elm FatFs](http://elm-chan.org/fsw/ff/) — Elm Chan 的 FatFs R0.15（小巧精简、零依赖）

* [Unity](https://github.com/ThrowTheSwitch/Unity) — 嵌入式 C 单测框架

* [Renode](https://renode.io/) — 强大的 MCU 模拟器平台（集成测试用）

* [FreeRTOS](https://www.freertos.org/) / [Zephyr](https://www.zephyrproject.org/) / [RT-Thread](https://www.rt-thread.org/) — 架构分层/API 设计参考（**仅参考，不兼容上述项目 API 或行为；本项目也不是 RTOS**）

