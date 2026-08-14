# 调试会话：USB CDC 输入无响应 [OPEN]
- **Session ID**: usb-cdc-input
- **创建时间**: 2026-08-13
- **症状**: 用户能看到串口输出（启动banner、mk>提示符），但无法输入任何字符。输出（IN）正常，输入（OUT）完全无响应。
- **已知事实**:
  1. 硬件通路 100% OK：`usb_print_test` 纯 SDK 固件可双向通信（CDC_RX 正常）
  2. Shell 任务正常运行：`mk>` 提示符每 3 秒重打一次
  3. boot_setup / led / heartbeat / mem 任务都正常工作（心跳日志持续输出）

## 可证伪假设
| # | 假设 | 证伪方式 |
|---|------|----------|
| H1 | `PICO_STDIO_USB_SUPPORT_CHARS_AVAILABLE` 未定义 → `stdio_usb_in_chars` 永远返回 0，`getchar_timeout_us` 读不到 | 在 shell 循环同时打印 `getchar_timeout_us(0)` 返回值和 `tud_cdc_n_available(0)` 返回值，两者都为 0 则确认 |
| H2 | 调度器启动后 USBCTRL_IRQ 被意外屏蔽（NVIC ISER 被改）→ 即使主机发数据，USB 中断不触发，`tud_task_ext` 读不到端点 | 打印 `NVIC->ISER[0]`、`USBCTRL->INTE`、`USBCTRL->SIE_STATUS` 寄存器值，对比 usb_print_test 预期 |
| H3 | `tud_cdc_rx_cb` 回调没注册成功（`stdio_init_all` 在 `kernel_main` 中从 PSP 上下文调用，SDK 内部注册失败）→ `tud_task` 处理 OUT 数据后没人搬入 SDK 缓冲区 | 同时打印 `getchar_timeout_us` 和 `tud_cdc_n_read` 返回值：若后者有数据而前者没有 → 回调未注册 |
| H4 | 内核接管 TIMER_IRQ_0 破坏了 SDK `alarm_pool` → `stdio_usb` 依赖的 `async_context` 不能正常 poll → 即使 `tud_task_ext` 跑了，上层状态机异常 | 在 shell 空闲循环直接打印心跳计数器递增 + 当前任务名，确认调度器没乱 |
| H5 | `tud_task_ext(0,0)` 参数错误 / 实际没处理 OUT | 换用 `tud_task()`（如果能链接）或 `tud_task_ext(0,1)`，或打印 TinyUSB 设备状态寄存器 |

## 插桩计划
在 shell.c 主循环的 idle 分支（prompt 重打处），每次重打提示符时追加诊断行：
```
[DBG] iter=N gtc_ret=X cdc_avail=Y tud_rdy=A cdc_conn=B
  NVIC_ISER=0x... USB_INTE=0x... USB_SIE=0x...
```
每 ~3 秒一次，不影响正常输入路径。

## 证据日志（beat#1 运行时）
用户输出关键片段：
```
[HEART ] beat #1 | tick=0 | ...
[USBDIAG] A.gtc=0 B.av=0 B.rd=0 SIE=0x0x40050005    ← 注意：SIE 多打了一个 0x 前缀
[USBDIAG] gtc_byte=0
```

### 证据分析
1. **SIE=0x40050005** → bit0(VBUS)=1, bit2(RESET_RECV)=1, bit16(CONNECTED_SPEED_HI)=0x4005 bit0=1, bit17(ENUM_OK)=0x4005 bit1=1 → **USB 枚举完成，连接正常** ✅
2. **A.gtc=0 而非 TO（-1/0xFFFFFFFF）** → `getchar_timeout_us(0)` 超时返回值不是 PICO_ERROR_TIMEOUT，而是 **0**
   - **直接证实 H1 变种**：某些 SDK 版本 timeout_us=0 是立即返回假值 0，不检查输入
3. **B.av=0 B.rd=0** → beat#1 时刻用户还没输入字符 → 合理（用户只看到启动输出）
4. 后续 `beat #2`/`[USBDIAG]` 未出现在用户输出中：用户只截图了启动时刻

## 根因（最终确认 - 2026-08-13）
**RP2040 timer ARMED 寄存器是 write-1-to-clear，代码误用为 write-1-to-set**

证据链：
1. `beat #1 | tick=0` → tick 永远是 0，说明 `systick_irq_handler` 从未被调用
2. 即使完全去掉 USB 诊断代码，heartbeat 只做 `print + task_sleep(300)`，beat#2 仍不出现
3. `ISER bit0=1` → TIMER_IRQ_0 在 NVIC 已使能，`cpsie i` 也加了，中断还是不触发
4. LED 闪烁正常 → idle 任务 busy_wait 工作正常（不依赖中断）

根因：`hal_systick_init_impl` 和 `systick_irq_handler` 中都有这行：
```c
hw_set_bits(&timer_hw->armed, bit);  // 以为是设置 armed，实际是清除！
```

RP2040 timer 硬件：
- 写 `alarm[n]` 寄存器 → **自动 armed** 该 alarm（开始倒计时）
- `ARMED` 寄存器是 **write-1-to-clear**（写 1 清除 armed 状态，写 0 无效）

代码在写 `alarm[n]`（自动 armed）之后，又写 `ARMED` 寄存器（清除 armed）→ alarm 被解除武装 → 永远不触发中断 → tick 永远是 0 → task_sleep 永远不唤醒

## 修复
1. `hal_systick_init_impl`：把 `hw_set_bits(&timer_hw->armed, bit)` 移到写 alarm 之前（清除旧状态），写 alarm 后不再碰 ARMED
2. `systick_irq_handler`：删除 `hw_set_bits(&timer_hw->armed, bit)`，写 alarm 自动 armed

## 修复（v3 固件，已编译烧录待验证）
### 变更 1：shell.c 输入路径
- 优先：`tud_task_ext` + `tud_cdc_n_available(0)` + `tud_cdc_n_read(0, &b, 1)`（TinyUSB 直读，与 usb_print_test 完全一致）
- 回退：`getchar_timeout_us(100)`（timeout=100us 避免假值，且只接受 `>0` 的值）

### 变更 2：demo_app.c heartbeat 诊断增强
- 多次 `tud_task_ext(0,0)`（3 次）确保状态机推进
- 额外诊断 NVIC_ISER（bit21=USBCTRL_IRQ）、USB_INTE 寄存器，验证中断没被关
- 读到字符时**直接回显**给用户，让用户即时看到字符出现在终端

### 变更 3：hal_port.c 同步方案
- `hal_console_getc_impl` 仍然使用 `tud_task_ext + getchar_timeout_us(0)` 作为 HAL 通用接口
- 但 shell 任务绕开 HAL，直接读 TinyUSB → 修复了之前误判断 0 为真字符的问题
