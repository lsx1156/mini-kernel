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
│   ├── build.bat                 一键构建/烧录脚本
│   └── CMakeLists.txt
└── rp2040demo/                   ← 独立应用工程（引用 mini-kernel 作静态库）
    ├── src/main.c                应用入口（强 main() 覆盖内核的弱 main）
    ├── build.bat                 一键构建/烧录脚本
    └── CMakeLists.txt
```

## v0.1 里程碑

- Cortex-M0+ 时间片轮转调度（支持权重比例）
- 固定内存池 + 零碎片堆
- USB CDC 控制台（硬件 EP1 OUT 轮询旁路）
- 交互式 Shell（含 syscalls 契约表查询命令）
- Pico SDK 可集成：既可顶层固件，也能被独立工程 add_subdirectory 引用
- **体积约束**：精简版 Flash ≤ 10KB / RAM ≤ 4KB（不含 Pico SDK）；完整基础版 ≤ 20KB/8KB

## 构建

```bat
REM mini-kernel 内置 demo 固件
cd mini-kernel && build.bat

REM rp2040demo 独立应用固件
cd rp2040demo && build.bat
```

烧录后用 PuTTY / TeraTerm 打开 USB 虚拟串口（115200-8-N-1，无需实际波特率参数，USB CDC），输入 `help` / `syscalls` 查看。
