# PIO 吞吐实测（RP2350）

日期：2026-10-18
固件：`piobench/`（`out pins, 32` / `in pins, 1` 单指令循环 + DMA 供数/取数，clkdiv=1）
条件：150 MHz / 1.10 V，源码在 SRAM 外（XIP），DMA 32 位传输

## TX：`out pins, 32` + autopull

| 配置 | 总量 | 每 SM | 说明 |
|---|---|---|---|
| 1 SM，DMA 重复读同一字 | **116.57 Mword/s** | 116.57 | 1.287 clk/word |
| 1 SM，DMA 从 SRAM 64 KB 流式读 | 116.40 Mword/s | 116.40 | 与重复读几乎相同 → SRAM 不是瓶颈 |
| 4 SM（1 个 PIO 块） | **141.64 Mword/s** | 35.41 | **已饱和** |
| 8 SM（2 块） | 141.64 Mword/s | 17.71 | 无提升 |
| 12 SM（3 块） | 141.64 Mword/s | 11.80 | 无提升 |

- 聚合上限 **141.64 Mword/s = 566.6 MB/s = 4.53 Gbit/s（32 位宽）**
- 折算 **0.944 word/clk**，与数据手册“DMA 可持续 1 word/clk”一致 → 瓶颈是 DMA/总线，不是 PIO
- 单 SM 的 1.287 clk/word 说明 **autopull 每字多花约 0.29 周期**

## RX：`in pins, 1`（1 bit = 1 采样，1 采样/周期）

| 配置 | 数据率 | 采样率 | clk/sample |
|---|---|---|---|
| 1 SM | 149.99 Mbit/s | **149.99 Msample/s** | 1.000 |
| 4 SM | 599.73 Mbit/s | 599.73 Msample/s | 1.000 |
| 12 SM | **1792.82 Mbit/s** | **1792.82 Msample/s** | 1.004 |

- 每 SM 都跑满 **1 采样/周期**，12 SM 线性叠加到 **1.79 Gsample/s**
- RX 侧不存在 TX 那种饱和：每 32 采样才产生一个 FIFO 字，12 SM 合计只有 56 Mword/s

## 结论

1. **TX 的墙在总线，不在 PIO**：4 个状态机就把 566 MB/s 吃满，再加 SM 无效。要更高只能加宽（32 位已是单次上限）或分散到不同主机（DMA 通道本身也受总线约束）。
2. **RX 没有墙**：12 个 SM 各自 1 采样/周期的能力是真的，适合做 12 通道逻辑分析仪。
3. **对本项目（480×272@60 = 15.7 MB/s）**：TX 上限有 **36 倍余量**，显示输出完全不是问题。
4. 单 SM TX 想再快，需要减少 autopull 停顿（提前预取、或改用 `mov`/`out` 分离），但只有在单 SM 独占总线时才有意义。

## 588 MHz / 1.70 V 复测

同一固件加 `oc_apply(588, 1700)`，其余不变（`build_588`，`-DMAX_MHZ=588 -DVOLT_MV=1700`）：

| 测试 | 150 MHz | 588 MHz | 比值 |
|---|---|---|---|
| TX 1 SM（重复源） | 116.57 Mword/s | **456.90 Mword/s** | 3.92× |
| TX 1 SM（SRAM 源） | 116.40 | 456.35 | 3.92× |
| TX 4 SM | 141.64 | **555.17** | 3.92× |
| TX 8 SM | 141.64 | 555.24 | 3.92× |
| TX 12 SM | 141.64 | **555.27** | 3.92× |
| RX 1 SM | 149.99 Msample/s | **587.93 Msample/s** | 3.92× |
| RX 4 SM | 599.73 | 2350.90 | 3.92× |
| RX 12 SM | 1792.82 | **7028.58** | 3.92× |
| clk/word（单 SM） | 1.287 | **1.287** | 不变 |
| clk/sample（RX） | 1.000 | **1.000** | 不变 |
| TX 聚合上限 | 566.6 MB/s | **2.22 GB/s** | 3.92× |

启动温度 41.2 °C，585 秒内跑完全部九项，无复位、无校验异常。

结论：**PIO/DMA 在 588 MHz 下完全线性，没有任何效率损失**；TX 仍然是 0.944 word/clk、RX 仍然是 1 sample/clk。饱和点也仍是 4 个 SM。

两个顺带的发现：

1. TX 聚合 2.22 GB/s 是**总线**上限（0.944 word/clk × 4 B × 588 MHz），不是 SRAM 上限。
2. `src=sram` 那一项实测 **1.83 GB/s**（456 Mword/s × 4 B），与早先 `bench` 里记的 "SRAM 587 MB/s @588" 不一致 —— 后者应是复合读写循环的统计口径，待复核。SRAM 单端口读的上限更接近 2.35 GB/s（1 word/clk）。



## 复现

```
cd E:/build/002/rp2350-lab/piobench
cmake -B build_150 -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
  -DPICO_PLATFORM=rp2350 -DPICO_NO_PICOTOOL=1 -DPICO_SDK_PATH=D:/project/pico-sdk \
  -DPICO_FLASH_SIZE_BYTES=16777216
cmake --build build_150
python ../tools_flash_capture.py build_150/piobench.uf2 55
```

原始日志：`logs/*_piobench150e.log`
