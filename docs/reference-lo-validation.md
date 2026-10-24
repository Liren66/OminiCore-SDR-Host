# ADF4002 与外部 LO 验证记录（2026-10-24）

版本：OCH 0.3.2 / ABI 1，SDK `SDK_CMD_1.0_20261024_REF_LO`。实机为 Windows x64、libusbK、`OMINICORE00000001`。

## 功能与硬件范围

- 新增 ADF4002 驱动，CLK/DATA/CE/LE 使用已有 MIO9/10/12/13；没有重新导出硬件或修改 BSP。
- `clock_ref_source=0|1` 控制自由运行/外部 10 MHz 驯服，冷启动默认 0。板载及 AD9361 参考始终 40 MHz，ADF4002 R=1、N=4。
- `adf4002_status?`、`adf4002_self_test!` 提供配置状态、GPIO pad 检查及 CP 高阻下的 D5 三次亮灭指令。
- `rx_lo_source=0|1`、`tx_lo_source=0|1` 独立选择全部 RX / 全部 TX 的 LO，冷启动默认内部。返回内部时恢复该方向最后一次内部 PLL 频率。
- OCH 提供对应命令及 6 个独立 C API / C++ 包装；原有 ABI 结构尺寸保持不变。新参考状态结构为 96 字节，明确报告 `source_known`、`last_error`。

原理图依据：`AD9361_MIX(3).pdf` 第 2、10 页。J3 为 10 MHz 参考输入；J6 虽标有 10M_EXT，实际网络是 PPS_EXT。J11 为 RX 外部 LO，J10 为 TX 外部 LO。外部 LO 物理输入为名义 RF 频率的两倍。

MUXOUT 只接 R111/D5，没有接回处理器。正常外参考模式将其设为数字锁定输出，锁定时由芯片直接点亮 D5。软件不能测到锁定或灯的电平，也无法读取 ADF4002 寄存器。

## 自动与实机结果

| 检查 | 结果 |
|---|---|
| SDK ARM 编译、链接 | 通过；生成 ELF 与 BOOT.bin |
| ADF4002 生产驱动故障注入 | 1175 项通过，含逐边沿失败、MSB-first 24 位写入、安全关闭、自检与状态恢复 |
| 生产 boot/reinit 精确提取回归 | 10 项通过，参考异常禁止 RF 就绪，有效参考选择跨重初始化保留 |
| 生产 snapshot 精确提取回归 | 71 项通过；16 种两路增益模式组合、AGC 不读瞬时增益、MGC 错误仍拒绝、保留内部 PLL 频率 |
| 外部 LO/增益/回滚/重试 | 145 项通过，包括 ALERT 不锁存手动增益模型、AGC 自主增益、部分恢复失败后目标不被覆盖 |
| 原生 PLL/时钟缓存/混合源 | 19 项通过 |
| 原有 DC/IQ 校正与 SPI 错误传播 | 38 + 7 项通过 |
| OCH 运行时回归 | 59/59 通过；含双向停流恢复、无变化请求、失败关闭、未知参考状态的显式恢复 |
| 公共接口 | C11 导出链接、ABI 尺寸、C++ 包装编译通过；6 个新接口可链接 |
| 实机参考与外部 LO | 11 项检查、3 次运行中操作通过；清理成功 |
| 原有校正实机回归 | 18 项检查、6 次运行中切换通过；清理成功 |
| 数字自检 | RX PRBS、TX 数字回环通过 |
| ADF4002 人工 LED 通信复核 | 自检发送三次亮灭，人工观察确认正常闪烁；基本串行控制通信与 MUXOUT/D5 响应通过 |
| CH32 保护核验 | 原先记录的 427 个文件 SHA256 全部一致 |

实机参考检查始终保持内部参考；没有执行 `clock_ref_source=1`。ADF 自检确认 PS pad 电平符合输出，返回 `gpio_verified=1`，结束恢复内部参考。首次检查时没有人工观察 D5；随后在可观察 D5 的条件下重新执行 `clock.ref-test`，发送亮 500 ms / 灭 500 ms、重复三次的序列，观察确认正常闪烁。由此确认 ADF4002 接收控制写入并驱动 MUXOUT/D5 的基本端到端通信正常。自检期间 CP 保持高阻，结束状态为内部参考、`last_error=0`。原始命令返回、最终状态和人工观察记录分别保存在 `led-observation-test.json`、`led-observation-final-status.json`、`led-observation-confirmed.json`。

实机 LO 检查覆盖：RX/TX 分别及同时选外部、名义频率 1 GHz→3 GHz、切回内部 1 GHz、频率与手动增益缓存同步、快速 AGC 跨频段切换、快速 AGC 重初始化、RX 3 GHz / TX 2 GHz 外部名义频率跨 `rf_reinit!` 保留。所有 TX 数据输出保持关闭。

运行中 RX 两次 LO 切换以及 ADF 自检后，原流句柄保持有效，新数据均带 `START | DISCONTINUITY | TIME_UNKNOWN | RECONFIGURED`（flags=51），并有新 epoch 和重配置事件。无变化的源设置没有触发重配置。三次 typed 调用约为 301 ms、324 ms、3345 ms；这是本机调用耗时，不是 PLL 锁定时间。

这一轮接收 78,932,646 个单通道样本，USB transfer_errors=0。日志保留 3 次设备溢出和 262,144 个主机丢弃样本，并验证补头恢复；没有宣称连续无丢样。切换停流与清旧队列造成的间断是预期行为。

## 测试发现并修复的问题

- ALERT 状态下立即读手动增益索引仍得到旧值，导致跨频段切回内部失败：现在先装增益表，恢复工作状态，再对 MGC 写入并有界校验索引。
- AGC 会自主改变增益，不能要求其手动索引固定：逐通道检查模式，MGC 精确读回，AGC 检查模式未变和实际指针有效。
- 未锁定的 fast AGC 可使瞬时增益查询返回 EAGAIN：快照只保存 MGC 的固定增益，AGC 模式仍正常保留。
- 部分 RF 恢复失败可能用新实例默认频率覆盖旧外部名义频率：恢复未完成期间禁止采集该实例作为新目标。
- ADF GPIO 故障后的缓存 source=0 不能代表已恢复：OCH 只有完整正常状态才视为无变化，显式切回内部会真实发送恢复命令。
- 冷启动先关闭遗留 RF/PL 状态，再执行可失败的参考驱动初始化；参考异常未解除时，`rf_reinit!` 不能把 RF 重新标为就绪。

## 未验证项目及最终状态

没有外部 10 MHz 输入，未测试实际闭环锁定、VCXO 调谐极性、环路动态、锁定时间或相位噪声。人工观察已确认基本控制通信，但没有芯片读回或 MUXOUT 到处理器的反馈，因此自动状态仍如实返回 `chip_verified=0`、`lock_known=0 locked=-1`、`led_known=0 led_on=-1`；人工反馈单独记录，不改写这些字段。自检闪烁不代表 PLL 已锁定，软件也不会等待不可读的锁定状态。

外部 LO 没有接入信号，验证范围为源选择/电源/分频寄存器、频率语义和恢复逻辑，不包含外部 LO 的实际射频收发性能。

最终参考源、RX LO、TX LO 均为内部，RF/digital 就绪，TX 和双向 USB 关闭。测试通过 JTAG 初始化 PS、重新加载现有 FPGA bitstream 和新应用到易失配置/RAM，未重编 FPGA、未改 CH32 工程或固件、未写启动 Flash。BOOT.bin 使用原 FSBL 和原 bitstream 加新 ELF 生成；尚未进行 Flash 启动验证。

安装包为 `dist/OmniCoreHost-0.3.2-win64-setup.exe`，不含板级 ELF/BOOT，也没有自动替换 Program Files 中的版本。

全部备份、测试源程序及原始日志位于工作区根 `_reference_validation_20261024`；主要记录为 `hardware-reference-lo.jsonl`、`correction-regression/hardware-rx-correction.jsonl`、`host-runtime-tests.log`、`adf4002/`、`sdk/`、`jtag-load-final.txt`。失败复现日志也保留，最终成功结果以无 diagnostic/before 后缀的日志为准。

官方依据：[ADF4002 数据手册](https://www.analog.com/media/en/technical-documentation/data-sheets/adf4002.pdf)、[AD9361 UG-570](https://www.analog.com/media/en/technical-documentation/user-guides/ad9361.pdf)、[ADI 外部 LO 控制](https://analogdevicesinc.github.io/linux/drivers/iio-transceiver/ad9361/)、[板载 VCTCXO](https://taisaw.com/assets/PDF/TX0873AA12AE%20_Rev%201.0_.pdf)。
