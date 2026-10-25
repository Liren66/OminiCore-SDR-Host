# PPS 时钟频率监控（OCH 0.3.4）

PPS 只用于监测 AD9361 数字接口的 DATA_CLK 频率，没有频率调节、锁相、UTC 时间、IQ 硬件时间戳或定时收发功能。监控默认关闭；不需要外部 PPS 也可以正常启动和收发。

## 使用

```powershell
och get pps.input-enabled
och set pps.input-enabled on
och get pps.status
och get pps.status --json
och set pps.input-enabled off
```

SDK 原指令：`pps_input_en?`、`pps_input_en=0|1`、`pps_status?`。`system_status?` 也包含 PPS 状态。`system_info?` 的 `pps_monitor=1` 表示 SDK 已连接支持 PPS 的 FPGA；`pps_adjustment=0` 表示没有调节功能。旧固件/旧比特流会返回不支持，不伪造有效测量。

开启只解除 PPS 中断屏蔽并启动统计，关闭时屏蔽中断并清空本次统计；FPGA 内部输入采样与计数模块仍存在。打开或关闭监控不暂停 IQ 流、不改采样率、不切参考源。选择在关闭/重新打开 OCH 后保留，在 `rf_reinit!` 后保留；SDK 重启恢复关闭。

## 状态和数据含义

| 字段/状态 | 含义 |
|---|---|
| `disabled` | 监控已关闭 |
| `waiting` | 开启或重新配置后等待两个连续有效脉冲 |
| `valid` | 最近完整 PPS 间隔产生有效频率 |
| `no_signal` | 开启后或最后脉冲之后超过 1.5 秒未收到新脉冲 |
| `invalid_interval` | 脉冲间隔或计数异常，包括丢脉冲后第一次恢复 |
| `clock_unavailable` | RF 时钟未就绪，监控选择保留，测量暂停 |
| `signal_present` | 最近 1.5 秒收到过脉冲，不等于合格的 1 Hz 信号 |
| `frequency_valid` | 必须为 1 才能使用频率和频偏 |
| `nominal_hz` | 根据当前时钟配置推算的名义 DATA_CLK，非测量值 |
| `measured_hz` | 假设外部脉冲为准确 1 Hz，由 FPGA 周期计数获得的 DATA_CLK 频率 |
| `error_ppm` | `(measured_hz / nominal_hz - 1) × 10^6`；无效时协议返回 `unknown` |
| `pulse_count` / `invalid_count` | 本次启用期间收到的 IRQ 数/被拒绝的间隔数，32 位计数可回绕 |
| `age_ms` | 距最后脉冲的毫秒数；4294967295 表示开启/重新测量后尚无脉冲 |
| `raw_counter` | 有效时的 FPGA 原始计数，修正零起点后 `measured_hz = raw_counter + 1` |
| `hardware_timeout` | HDL 自带超时标志，仅作诊断；门限依赖 DATA_CLK，软件状态采用固定 1.5 秒 |

当前物理接口固定为 2R2T LVDS，DATA_CLK = 4 × 每通道采样率。30.72 MSPS 对应名义 122.88 MHz，61.44 MSPS 对应 245.76 MHz；USB 只选一个 RX 通道不会改变这个关系。监测值不是板上 40 MHz 参考时钟的直接读数。

首次 PPS 只建立起点，不发布频率。PS 定时器只检查 IRQ 间隔是否在 0.9–1.1 秒，硬件计数还须在名义频率 ±10% 内；PS 接收时刻不用于精确算频率。下一次连续合格 PPS 才有效。丢失输入时立即停止发布过期频率，`measured_hz=0`、`error_ppm=unknown`；零表示无效占位，不表示真的测得 0 Hz。改采样率、FIR/时钟重配和 RF 重初始化后重新等待两个脉冲。

板上 PPS 接口是 J6 的 PPS_EXT（原理图连接器文字为 10M_EXT），10 MHz 外部频率参考则接 J3。输入电平必须与实板 Bank 供电匹配。OCH 不改变 FPGA 的 LVCMOS25 约束或板上电压。

## OCH 的 LOCK 显示

从 0.3.4 起，参考状态、PPS 状态及系统状态统一使用 PPS 测频判定 `locked`，并标注 `lock_source=PPS`、`lock_tolerance_ppm=1`。

| 监控情况 | LOCK 显示 |
|---|---|
| 已开启，最近有效频率与名义 DATA_CLK 相差不超过 ±1 ppm（包含边界） | `locked=1` |
| 已开启，测量有效但超出 ±1 ppm | `locked=0` |
| 关闭、无输入、等待两个有效脉冲、无效间隔、时钟不可用、超时或查询失败 | 隐藏 LOCK |

判定使用整数频率差，避免 ppm 显示舍入影响边界；每次有效测量更新，不累计多秒确认。`frequency_valid=1` 的 SDK 校验范围是 ±10%，不能直接当作锁定。

这是相对于输入 PPS 的频率容差标志，不是相位锁定、ADF4002 锁定反馈或绝对频率精度保证。只开启外部 10 MHz 时不需要 PPS，ADF4002 实际锁定仍需观察 D5。监控不调节时钟。

CLI 的普通输出及 JSON `reply` 隐藏无法观测的硬件 lock/LED 字段；规范命令 JSON `raw_reply` 和底层 `och_command` 保留原始 SDK 回复，供协议诊断。界面应使用规范 `reply` 或 C API 的 `lock_known/locked`，不要显示原始 SDK 的 `locked=-1`。

## C/C++ 接口

```cpp
och::device device;
device.set_pps_enabled(true);
auto status = device.pps_status();
if (status.frequency_valid) {
    // status.measured_hz 和 status.error_ppm 有效。
}
if (status.lock_known) {
    // 显示 status.locked：1 = 频率在 ±1 ppm 内，0 = 超出。
} else {
    // 隐藏 LOCK。
}
device.set_pps_enabled(false);
```

C ABI：`och_set_pps_enabled(device, 0|1)`，`och_get_pps_status(device, &status)`；`och_pps_status.struct_size` 必须设置。无效频率在 C 结构中以 `frequency_valid=0` 和零数值表示。0.3.4 在原保留槽加入 `lock_known/locked`，`och_pps_status` 仍为 96 字节，`error_ppm` 偏移仍为 56，保持 ABI 1。`lock_known=0` 时 `locked=-1`，界面必须隐藏。`och_reference_status.lock_known/locked` 同样表示 PPS 频率监控结果；参考 getter 将不支持/失败的可选 PPS 查询降级为未知，显式 PPS getter 仍报告错误。

同进程 API 可以在 RX/TX 流运行期间调用，也允许持有 RX 租借缓冲。查询/开关失败不会停流或使 RF 配置失效。原始 `och_command` 的写操作仍要求没有流；正在收发的应用应使用上述类型化接口，不能另开 CLI 抢占设备。

## 板级实现与验证

新比特流包含修订号 2 的 PPS 接收 IP。PPS 中断连接 concat In0 → IRQ_F2P[0] → GIC 61，SDK 显式采用此板级映射；当前 BSP 自动生成的 `XPAR_FABRIC_AXI_AD9361_GPS_PPS_IRQ_INTR=91` 与实线不一致，91 留给现有 CH32 中断。驱动共享已有 GIC，使用上升沿触发，同时解除 RX/TX 两侧 PPS 屏蔽。没有修改 CH32H417 工程。

接入真实外部 10 MHz 和 PPS 后，D5 常亮；30.72 MSPS 下稳定读数为 122880000–122880001 Hz、频偏 0–0.008 ppm。该结果相对于输入 PPS，不是独立仪表的绝对精度验收。计数功能背景可参阅 [ADI AXI AD9361 文档](https://analogdevicesinc.github.io/hdl/2023_R2/library/axi_ad9361/index.html)，具体计数修正和中断映射以本项目 RTL/连线为准。
