# 板卡细节（MuLuoxing RP2350 USB-A 款）

## 来源与资料

- 厂商资料：`../树莓派RP2350开发板USBA款资料/`
  - `2.文档/rp2350-datasheet.pdf`（芯片手册）、`pico-2-datasheet.pdf`（Pico 2 板手册）
  - `2.文档/主板原理图.pdf`（本板原理图，文字层可抽取但网表顺序混乱，按网名读）
  - `1.图片&3D/`：正面/背面丝印图、`PCBA_3D.step`、引脚分布图
  - `Micro python固件/micropython_rp2350.uf2`、例程 `ws2812_rp2350_gpio16.py` + `ws2812.py`

## 已测事实

见 `../README.md` 第 1 节。要点：RP2350A rev3、ARM-S 平台、**16 MB flash（JEDEC 0x68 0x40 0x18，Boya 系）**、
全片可写、150 MHz 默认主频、CDC 端口 `VID_2E8A:0009`。

原理图标的 W25Q32（4 MB）与实际不符——这是必须记下的差异：板级 `PICO_FLASH_SIZE_BYTES` 不能照抄 pico2 板定义。

## 引脚（丝印 + 原理图）

左侧（自上而下）：`VBUS GP0 GP1 GND GP2 GP3 GND GP4 GP5 GND GP6 GP7 GP8 GP9 GND GP10 GP11 GP12 GP13 GND GP14 GP15`
右侧（自上而下）：`VBUS VSYS GND 3V3_EN 3V3 ADC_VREF GP28 GND GP27 GP26 RUN GP22 GND GP21 GP20 GP19 GND GP18 GP17 GP16`

即标准 Pico 2 40 脚序。板载占用：

| 网络 | 引脚 | 说明 |
|---|---|---|
| WS2812 DIN | GPIO16 | 4 颗 WS2812-2020 串联（R21/R22 0R 隔离） |
| USER LED | GPIO25 | 经 470R，另有 JP1 跳线位 |
| VSYS 检测 | GPIO29 / ADC3 | 56K/100K 分压 + Q1(DMG1012T) 开关 |
| BOOT 键 | QSPI_SS | 上电时拉低进 BOOTSEL |
| RUN / RESET | RUN | 复位键 KEY1 |
| SWD | SWDIO / SWCLK / GND | 背面三焊盘，TP1–TP7 为测试点 |
| USB | DM/DP（27R 串阻） | USB-A 公头直插；VBUS→VSYS 经 1N5817W 肖特基 |
| 供电 | VSYS → ME6211C33 → 3V3 | 1 µF×2 输入输出 |

## 待实测清单

1. USER 键实际 GPIO（原理图网名 `USERE_KEY`，需上拉扫描确认；不要照猜）。
2. GPIO29 分压系数实测（写 ADC 读数 ↔ 万用表 VSYS，反推比例）。
3. WS2812 实际颗数与数据极性（厂商例程写 4 颗，需点亮确认）。
4. 3V3 轨最大负载、VSYS 输入范围（USB 5 V 直供 vs. 外部 VSYS）。
5. flash 真实颗粒型号（0x4B Read Unique ID 或 SFDP，比 JEDEC 更确定厂商批次）。

## 与官方 Pico 2 的差异（影响移植）

- flash 16 MB vs 4 MB：链接脚本、文件系统、OTA 分区都要按 16 MB 配。
- 无板载调试器、无 USB-C、无电池充电管理；WS2812/USER 键为本板新增。
- `PICO_BOARD=pico2` 可直接用（引脚与 flash 启动参数兼容），只需覆盖 `PICO_FLASH_SIZE_BYTES`。
