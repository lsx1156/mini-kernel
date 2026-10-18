# HSTX 吞吐实测（RP2350）

日期：2026-10-18
固件：`hstxbench/diag.c`（TMDS 位映射：N_SHIFTS=5、SHIFT=2，CPU 按 FIFO FULL 节流推送）
条件：150 MHz / 1.10 V，HSTX 引脚 GPIO12–19

## 结构与已知量

- HSTX FIFO 深度 **8 字**（填 64 字后 `LEVEL=8 FULL=1 WOF=1`）。
- TMDS 配置下**每个 32 位字 = 3 条 10 bit TMDS 符号 = 1 个像素**，每 5 个 HSTX 时钟消费一个字。

## 实测

| clk_hstx | 实测 | 理论 clk/5 | 判定 |
|---|---|---|---|
| **48 MHz** | **9.60 Mword/s** | **9.60** | ✅ 完全吻合 |
| 24 MHz | 9.60 | 4.80 | 分频未生效（见下） |
| 12 MHz | 9.60 | 2.40 | 分频未生效 |
| 150 MHz | 24.81 | 30.00 | **CPU 供数上限** |

结论：**行的消费速率 = clk_hstx / 5**，48 MHz 这一档逐位吻合（2.50 字/时钟实测 vs 2.50，误差 <0.1%）。

## 两个实测约束

1. **CPU 喂不满 HSTX**：150 MHz 下 CPU 只能推 **24.81 Mword/s**，是 30 Mword/s 天花板的 **83%**。要跑满必须用 DMA（DMA 侧 555 Mword/s，余量 18×）。
2. **`clock_configure(clk_hstx, ...)` 的分频没有生效**：设为 24/12 MHz 后 `clock_get_hz()` 回读正确，但实测消费速率仍是 48 MHz 档的 9.60 Mword/s。即 HSTX 仍在 48 MHz 上跑。写死分频需直接操作 `CLOCKS_CLK_HSTX_CTRL_DIV` 或复位后再配置，当前实现（`csr=0` → `clock_configure` → `csr=EN`）不足以切换。

## 换算

像素率 = clk_hstx / 5；每 lane 的 TMDS 比特率 = 2 × clk_hstx；SRAM 侧每像素 4 B（预编码）或 2 B（命令扩展器做 TMDS 编码）。

| clk_sys | 像素率 | 可跑 16:9/4:3 上限 | SRAM 带宽（4 B/pix） |
|---|---|---|---|
| 150 MHz | **30.0 Mpix/s** | 640×480@60（25.2）✅；800×600@60（40）❌ | 120 MB/s |
| 336 MHz | **67.2 Mpix/s** | 1024×768@60（65.0）✅ 勉强 | 269 MB/s |
| 396 MHz | 79.2 Mpix/s | 1280×720@30（37.1）✅；720p60（74.25）✅ | 317 MB/s |
| 588 MHz | **117.6 Mpix/s** | 720p60（74.25）✅；1080p30（74.25）✅；1080p60（148.5）❌ | 470 MB/s |

对掌机目标（480×272@60 = 7.83 Mpix/s）：**HSTX 只需 39.2 MHz 时钟**，150 MHz 主频下占用 26%。


## 588 MHz 实测与规格边界

数据手册 p.1196 原文：**"The maximum frequency for the HSTX clock is 150 MHz, the same as the system clock."**

实测（`build_588`，clk_hstx = clk_sys = 588 MHz，CPU 按 FIFO FULL 节流推送）：

| clk_sys | clk_hstx | 实测推送 | 说明 |
|---|---|---|---|
| 150 MHz | 150 MHz | 24.81 Mword/s | CPU 供数上限（天花板的 83%） |
| **588 MHz** | **588 MHz** | **97.24 Mword/s** | CPU 供数上限，3.92× 线性 |

97.24 与 24.81×3.92 = 97.3 完全一致，且全程 FIFO 未被填满 → **HSTX 在 clk_hstx=588 MHz 下确实按 clk_sys 跟频，消费速率 ≥97.24 Mword/s**，即超出数据手册 150 MHz 上限约 3.9 倍仍在工作。

但两点必须保留：
- 117.6 Mpix/s 仍是 **clk/5 外推**，片上没有能超过 97 Mword/s 的供数者（CPU 97，DMA 不遵守 FULL 标志），**上限未直接测到**。
- 每 lane 引脚速率 = 2×588 = **1.176 Gbit/s**，远超焊盘与走线的合理范围，信号完整性无保证。**能算不等于能出画面。**
