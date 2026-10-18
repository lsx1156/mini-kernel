# RP2350 超频实测台

**用途**：给 mini-kernel 的 RP2040 超频问题提供同架构（Armv8-M / Cortex-M33）的实测依据——上限在哪、为什么在那、负载怎么改变它。

**对象**：MuLuoxing RP2350A 开发板（QFN-60，30 GPIO，16 MB flash）
`CHIP_ID = 0x30004927`（mfr 0x493 / part 0x04 / rev 3），`UID = 8CEDF8FC0AD7BB65`
**环境**：Pico SDK 2.3.1；ARM GCC 13.4；示波器级时间基准用 nanoDLA（sigrok fx2lafw，8 MHz）
**口径**：下表全部为板上实跑，不是数据手册外推。原始串口日志留在实验机 `logs/`（未随仓库分发，见 §5）。

---

## 0. 一页结论

| 项 | 实测值 |
|---|---|
| 12 路 @600 MS/s + 12 DMA + USB 满负载，10 分钟 | **1,421,000 轮，0 短采样，0 复位**，Tmax 49.6 °C |
| 600 MHz 所需电压（满负载） | **1.80 V**；轻/中负载 1.70 V 即可 |
| 硅片墙 | **612 MHz** —— PLL 能设、能回读 `CFG 612 612`，跑起来必挂 |
| 抬压是否有效 | 无效。1.90 V 下 600 挂，2.00 V 下 588 挂 |
| 相对 150 MHz | 4.0× 主频；动态功耗 9.36×；能效 0.42× |
| 唯一不跟频的指标 | **XIP 16 → 18 MB/s（1.13×）** |
| CoreMark（150 MHz） | M33 **1684.687**、Hazard3 **1705.451**；CM/MHz 恒定 → 超频无架构损耗 |

---

## 1. 电压 × 频率矩阵

`vsweep` 扫描台，每点 10 s LCG-on-SRAM 负载 + 50 轮定点校验（golden CRC `f8171d56`）：

| 电压 | 588 | 600 | 612 |
|---|---|---|---|
| 1.70 V | OK | OK | HANG |
| 1.80 V | OK | OK | HANG |
| 1.90 V | OK | **HANG** | — |
| 2.00 V | **HANG** | — | — |

同一工作点的 30 s 复测（`RES` 行原样）：

```
RES 600 1800 30 650195 f8171d56 OK 50.5     ← 1.80 V，65 万轮，CRC 正确，50.5 °C
RES 600 1700 30 650196 f8171d56 OK 51.5     ← 1.70 V，轮数几乎逐位相同
```

**矩阵的读法**：抬压只买「重负载下的裕量」，不买频率。612 不是稳压器拒绝，也不是 PLL 分频算不出来——回读是对的，是硅片在这个负载下跑不动。

---

## 2. 三条与 RP2040 超频直接相关的结论

### 2.1 XIP 不跟频 —— 375/500 MHz 崩在取指，不在算力

150 → 588 MHz（3.92×），而 flash 取指带宽只从 **16 MB/s 走到 18 MB/s（1.13×）**。
XIP 走 QSPI 时钟域，主频抬上去之后 `clk_sys / clk_flash` 的分频比再大也换不回线性带宽，外围 SPI 时序先到顶。

对应到 RP2040：`375/500` 档崩掉的机制与此一致——**热代码还在 flash 里**。可复现的解法不是加压，而是：

1. 执行热路径的 `time_critical` 段放进 SRAM（链接脚本 section，运行时从 XIP 拷过去）；
2. 中断向量表和 ISR 进 SRAM；
3. 两核不同时从 flash 取指（共享的是 XIP **端口**，不只 16 KB 缓存容量）；
4. 大块搬运交给 DMA/PIO，别让 CPU 自己 copy。

本仓库的 `firmware/sniffer/` 就是这个做法的极端形态：12 路采样程序放在 SRAM 里由 12 个 PIO SM 跑，CPU 只负责配置和搬运。

### 2.2 USB CDC 在高负载下失效 —— 是 `tud_task()` 饥饿，不是 USB 时钟不稳

仓库根 README 记录的现象（`ovclk try 3/4` 后 USB 串口断开）在 RP2350 上同样出现，而且**可以复现、可以修**。

TinyUSB 的设备栈只在 `tud_task()` 里推进。固件一旦进入长时间 CPU 忙等（采样窗口、拷机循环、触发轮询），控制传输（`SET_LINE_CODING` / `SET_CONTROL_LINE_STATE`）就没人应答，主机侧超时 → `ERROR_SEM_TIMEOUT (121)` → 端口句柄进死状态，之后每次 open 都阻塞，看起来就像“USB 挂了”。

判据：**芯片还活着**。SDK 的写路径有界（`PICO_STDIO_USB_STDOUT_TIMEOUT_US = 500000`，500 ms 拿不到空间就丢弃），所以固件不会卡死在 printf；主机拔插后一切正常，说明 USB 外设和时钟都没问题。

修法（`firmware/sniffer/main.c` 已实施）：

- 长循环里每 1 ms 调一次 `tud_task()`；
- 短窗口（<1 ms）仍用精确 `sleep_us`，不牺牲时序；
- 输出前查 `stdio_usb_connected()`，主机没开端口就不往里灌。

对照实验：跑 120 s 满负载，第 18 s 强行杀掉占端口的主机进程，立刻重开 —— **修复前 ~30 s 后失败，修复后 0.0 s 成功**，且拷机跑完自动恢复，无需拔插。

> 对 RP2040 版的启示：`250 MHz 以上 USB 失效` 未必是 PLL/USB 时序问题，先查固件里有没有长忙等不喂 `tud_task()`。RP2040 的 `clk_usb` 来自独立 PLL，主频抬高本身不该影响 USB。

### 2.3 裕量取决于负载，不取决于核心

同一颗芯片、同一个 588 MHz：

| 负载 | 1.70 V | 1.80 V |
|---|---|---|
| 单核 LCG（`vsweep`） | 30 s 稳定 | 30 s 稳定 |
| 12 SM + 12 DMA + USB 满速 | **立刻挂** | 30/30 通过 |

差别在瞬时电流：12 路 PIO + 12 通道 DMA 同时拉总线时，板级 IR drop 把核心实际电压压到阈值以下。所以「这颗芯片能超到多少」这个问题，**必须带负载条件回答**。旧报告里“1.70 V → 588 是上限”只在轻/中负载成立。

---

## 3. 复现

```bash
export PICO_SDK_PATH=D:/project/pico-sdk
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
      -DPICO_PLATFORM=rp2350 -DPICO_NO_PICOTOOL=1 \
      -DPICO_FLASH_SIZE_BYTES=16777216 -DALLOW_OV=1 -DMAX_MHZ=600 -DVOLT_MV=1800
cmake --build build
```

**`vsweep`**（电压×频率扫描台，串口命令）：

| 命令 | 作用 |
|---|---|
| `S <mhz> <mv> <sec>` | 设频+设压，跑 <sec> 秒 LCG 负载，打印 `RES <mhz> <mv> <sec> <轮数> <CRC> <OK/FAIL> <温度>` |
| `C` | 回读当前频率/电压（`CFG <设> <读>`） |
| `B` | 进 BOOTSEL，远程刷机不用拔线 |

**`sniffer`**（12 路协议嗅探器，验证重负载）：`C <us> <mode>` 抓取、`T <post_us> <ch> <edge>` 环形预录触发、`X <sec> <dump_every>` 满负载拷机、`V` 回读工作点。

> 1.30 V 以上需要写 `POWMAN_VREG_CTRL` 的 `DISABLE_VOLTAGE_LIMIT`（bit8，配合 `0x5afe0000` + bit13 UNLOCK），`firmware/vsweep/main.c` 里有实现。

---

## 4. 目录

```
rp2350-overclock/
├── docs/                  实测报告（性能汇总、588 报告、频率扫描、PIO/DMA/SRAM/HSTX、XIP 干扰）
├── firmware/
│   ├── vsweep/            电压×频率扫描台（本文档 §1 的数据来源）
│   ├── sniffer/           12 路并发协议嗅探器（重负载来源，含 PIO 程序与拷机）
│   ├── coremark/          CoreMark 移植（M33 / Hazard3）
│   ├── bench/ mc/ mixed/ render/      算力与算力混合基准
│   ├── srambench/ dmabench/ piobench/ hstxbench/ cacheprobe/
├── host/                  主机侧工具：解码、抓取、nanoDLA 交叉验证
└── tools/                 bin→uf2、链接脚本展平、串口守护式抓取
```

`docs/SUMMARY.md` §0 的「600 MHz 热限制不可用」一句已被本文 §1 覆盖——那是 1.70 V 单负载阶段的结论。

---

## 5. 方法学与口径

- **CoreMark 必须满足 EEMBC ≥10 s 规则**，否则不得对外引用；表中数字均为满足口径的跑分。
- 架构对比尽量同 GCC 版本、同 `-O2`、同频率；否则只作趋势参考。
- 频率点为 12 MHz 栅格：150 / 336 … 588 / 600 / 612。
- 每个工作点都跑 **50 轮定点校验**（golden CRC）抓静默算错——超频最危险的失效是算错而不是崩。
- 断电复位与看门狗复位分开记录；空日志也留档，不删改。

## 6. 电气边界（先读这条）

RP2350 数据手册：DVDD 推荐最大 **1.16 V**、绝对最大 **1.21 V**。
本仓库中的 1.70–2.00 V 是**超出规格的破坏性实验**，靠 `DISABLE_VOLTAGE_LIMIT` 绕过保护，只在加了散热片、且明确接受芯片可能永久损伤的前提下做过。

**不要照抄电压值。** 换一颗芯片、换一块板、换一个散热条件，同一个电压的结论都可能不成立。可移植的是方法：先用 `vsweep` 找到自己板子的 `电压 × 负载 × 频率` 曲面，再取工作点。
