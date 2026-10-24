# 2TX2RX 可选 signed8 USB IQ

本功能随 OCH 0.4.0 发布。默认 12bit、原单/双通道模式、板控、RGB 15%/25% 兼容及 ABI 1 均保留。已完成下列 USB 持续传输、应用转换和控制恢复实测；存在启动恢复的项目与全程无错项目分别记录。8bit 需要匹配的 FPGA IP 1.1 / revision 10 与支持 iq_format 的 SDK。

## Wire 与数值

![USB IQ 格式](images/wire-format.svg)

12bit 每通道每复样本为 3 字节 packed12。8bit 仅在 RX/TX mask 都为 3、TX source 为 USB 时选择；每时间帧为 `I1[11:4],Q1[11:4],I2[11:4],Q2[11:4]` 四字节，均为二补码 signed8。FPGA TX 恢复每个 12bit 值时补低 4 位零。通道顺序和 16384 字节递增起始头完全不变；USB 请求完成边界仍不是样本或起始头边界。

| 应用格式 | 每通道每复样本字节 | 8bit RX 输出 | 8bit TX 输入处理 |
|---|---:|---|---|
| packed12 | 3 | signed8 高位扩展为 12bit，低 4 位零 | 丢弃每个 12bit 值的低 4 位 |
| sc16 | 4 | signed8 × 256，左对齐 | 取 signed16 的高 8 位 |
| fc32 | 8 | signed8 ÷ 128 | 沿用旧 12bit 量化，再丢低 4 位 |
| sc8 | 2 | signed8 I/Q 原样 | 原样 |

FC32 沿用半步远离零、饱和到 -2048..2047、NaN 为零、无穷饱和的原规则。8bit 降位是删除二补码低 4 位，不改成另一个 8bit 四舍五入规则。例如 12bit 的 -1 变成 signed8 的 -1，并在 FPGA 恢复为 12bit 的 -16。

## 配置与兼容

`och_config.usb_iq_bits` 占原 `reserved[0]`，偏移 76、结构大小 104 字节，其他既有成员偏移不变。0 或 12 为默认旧格式；8 需要两个 channel mask 都为 3。`och_default_config` 置 0，旧应用的零保留槽保持原行为。`OCH_SC8=3` 新增且不改变旧枚举值；SC8 仅在 8bit wire 下创建流。

SDK `system_info?` 必须有 `iq_format=1`，8bit 还要求 PL 能力 `iq8_2t2r=1`。缺少扩展的旧 SDK 仍可配置 12bit，新主机不会向其发送新命令。每个新主机配置事务先全停并清理，在支持扩展时先恢复 12bit，再应用通道/源，最后按需选择 8bit，避免继承上一句柄遗留的 8bit。格式写后查询严格核对 requested/applied、busy/error、verified 和 scope。

存在任意流对象时改变 wire 位数返回 BUSY，需先 stop/destroy 双向流，再 configure/create。普通频率、增益、时钟等重配置保留线格式，仍使用既有暂停、清理、零预填及重启机制。RX/TX 允许按原顺序分别启动、停止；约束的是两个通道 mask，不要求两个 USB enable 在每个瞬间同时为 1。

IQ8 的 TX 启动使用 1536 raw FIFO 组门槛（与 SDK 相同），12bit 仍保留原双通道 2700/单通道 1350。第一 16KiB 包在 IQ8 下约填入 2046 主 FIFO 组，足够启动；等待旧 2700 会因 FIFO 无空间接受第二包而卡住。主机异步提交 USB 大块，轮询硬件水位，不等整个 USB 块完成才打开 TX_RUN。

`och_rx_acquire` 总是返回当前 wire，独立于应用 `format`。8bit 双通道样本数为 `bytes / 4`；12bit 双通道为 `bytes / 6`。`och_recv` 进行所选应用格式转换。所有样本索引、丢样、TX padding、预填均按每通道时间样本计；USB/ring 字节按当前 wire 计。

## CLI 与示例

```powershell
och get system.info
och get usb.iq-bits
och bench --rx --tx --channels 1,2 --iq-bits 8 --sample-rate 30.72M --seconds 10 --attenuation 89.75 --amplitude 0 --json
och bench --rx --tx --channels 1,2 --iq-bits 12 --sample-rate 30.72M --seconds 10 --attenuation 89.75 --amplitude 0 --json
```

bench 自动为 8bit 选择 sc8，为 12bit 选择 packed12，并只支持这两种原生应用格式。SC16/FC32 转换须通过 API 的 `och_send` / `och_recv` 测试；不能用 bench 或零拷贝 RX 冒充转换测试。以上命令输出零 IQ，TX 衰减为 89.75 dB，不读写 IQ 文件。

31.25 MSPS 时 8bit 双通道每方向为 125 MB/s；30.72 MSPS 时为 122.88 MB/s（双向 245.76 MB/s）；61.44 MSPS 时每方向 245.76 MB/s（双向 491.52 MB/s）。起始头及 USB 协议开销另计。CLI JSON `usb_MBps` 为该流累计 USB 字节除以 `elapsed_seconds`，含启动/停止开销；不是独立稳态窗口的速率。

`examples/duplex_iq8.cpp` 展示双通道 SC8 接收和双通道零 IQ 发送，使用独立 TX producer。改变应用 args.format 可用 SC16/FC32/packed12，缓冲按应用格式尺寸分配。

SDK 控制命令为 `iq_format?`、`iq_format=8` 和 `iq_format=12`；规范 CLI 名为 `usb.iq-bits`。CH32 数据/控制固件保持透明传输。新 SDK 查询包括 `iq_format=8 requested=8 applied=8 supported=12,8 cfg_busy=0 cfg_error=0 verified=1 scope=2tx2rx`；后续诊断字段允许附加。

## 自动验证

生产 codec、framer、API、配置和真实 Windows worker 在模拟 USB 下运行。覆盖全部 4096 个 12bit 数值、高位/符号/缩放、浮点边界/NaN/Inf、分片及相邻起始头、4 字节帧、四种应用格式、零拷贝、默认/旧 SDK、能力/通道限制、读回故障、格式切换限制、双向重配置、统计和 CLI。原板控/PPS/LO/FIR/clock 回归继续运行。模拟测试不替代 FPGA/CH32 FIFO、ILA、RF 波形及持续 USB 性能实测。

## 2026-10-24 隔离工程实板记录

以下日志均位于隔离验证根目录。启动错误与稳态差值分开，不据短测推断所有环境或所有持续时长。

| 项目 | 实际观察窗口 | 结果 |
|---|---|---|
| IQ8 / SC8 API，61.44 MSPS，ramp | 稳态 300.133 秒 | RX 245.762、TX 245.763 MB/s；启动 overflow/underflow/resync 为 0，稳态新增错误为 0 |
| IQ8 / SC8 真实 CLI，61.44 MSPS | 请求 30 秒 | 全程主机错误/resync 为 0；RX/TX 累计平均 245.562/245.042 MB/s，含启停开销 |
| IQ8 / SC16 API，30.72 MSPS | 稳态 10.0948 秒 | 启动 RX overflow=1、resync=1；稳态错误差值 0，数值验证错误 0 |
| IQ8 / FC32 API，30.72 MSPS | 稳态 10.109 秒 | 启动 RX overflow=2、resync=2；稳态错误差值 0，数值验证错误 0 |
| 原 12bit 单 RX/TX mask1、mask2，交叉 RX1/TX2、RX2/TX1 | 61.44 MSPS，6 项各请求 10 秒 | 真实 CLI 六项全程严格无错 |
| IQ8 控制重配置 | 7 操作、8 个 3 秒稳态窗口；总计 56.4793 秒 | 3 秒启动/重配置恢复窗口后全部稳态通过，IQ8 保持，最终停止并恢复已保存控制 |

持续测试与 CLI 日志：`logs/iq8-61440-300s.jsonl`、`logs/iq8-cli-61440.json`。转换与通道矩阵：`logs/cli-matrix-hardware/summary.json`；SC16/FC32 虽稳态通过，但启动有错，原始 `strict_clean=false` 保留，不称全程无错。另一个 `logs/iq8-30720-smoke.jsonl` 的 10.1029 秒稳态通过，但启动 RX overflow/resync 各 1。

控制日志为 `logs/iq8-control-hardware.jsonl`。首次 fixture 把 `usb_stream rx/tx` 的 epoch 计数误判成 enable，导致 cleanup 误报；第二次使用 1 秒恢复窗口，恢复 header 跨入稳态而失败。两次原始日志分别保留为 `iq8-control-first-fixture-error.jsonl`、`iq8-control-second-recovery-window.jsonl`。修正停止判据并将恢复窗口设为 3 秒后通过，未放宽稳态无错断言。控制切换阶段允许预期丢样和新 epoch，不等同于无间断切换。

## 实板验证（2026-10-24）

验证使用重新生成并导出的正式 bitstream（IP 1.1 / revision 10），以及重新编译的 SDK 和 OCH 0.4.0。日志位于同级目录 `../_iq8_test_20261024/promotion_software_20261024_212721/logs/`。早期带 ILA 的隔离工程结果不作为本节依据。

| 项目 | 结果 |
|---|---|
| SDK / OCH 离线回归 | SDK 401 项、OCH 91/91 项、CLI 参数 19 项通过；旧 0.3.6 头文件的 ABI/default 探针通过 |
| 格式配置限制 | 实板 31 项检查通过；默认 12bit、仅 2TX2RX USB 配置可用 8bit、忙时拒绝切换 |
| 12bit 2TX2RX / 44.25 MSPS | 60.128 秒稳态，RX/TX 各 265.503 MB/s，启动与稳态新增错误均为 0 |
| 通道与应用格式矩阵 | 9/9 项全程无新增错误：12bit 六项单通道/交叉组合、61.44 MSPS SC8、30.72 MSPS SC16/FC32；后两项转换数值检查通过 |
| IQ8 运行中控制 | 7 次操作通过；恢复窗口外各稳态无新增错误，最终停止并恢复控制设置 |
| 原功能 | DC/IQ、参考选源/LO 命令、真实 PPS 测频、RGB/风扇/温度、数字接口自检通过；不代表外部 LO 射频性能或 ADF4002 锁定已由软件测得 |
| 61.44 MSPS，第 1 轮 | 263.515 秒稳态时出现 RX overflow=1、TX underflow=1、RX resync=1，未完成要求的 300 秒无错窗口 |
| 61.44 MSPS，第 2 轮 | 88.7884 秒稳态时再次出现相同计数，未通过；两轮启动无错，主机丢样/断供和 USB transfer_errors 均为 0 |
| 50 MSPS 对照长测 | 300.119 秒稳态，RX/TX 各 200.002 MB/s；启动与稳态新增错误、重同步、主机丢样、TX 断供均为 0 |

61.44 MSPS 的单向平均吞吐仍约 245.76 MB/s，但**这版系统当前不能宣称该速率下持续零丢样**。电脑调度或 USB 总线瞬时停顿是可能原因，尚无足够证据归因；平均吞吐正常、FPGA 已约束路径时序通过，也不能排除短暂停顿。原始失败日志 `iq8-61440-300s.jsonl` 与 `iq8-61440-300s-repeat.jsonl` 均保留。

50 MSPS 对照日志为 `iq8-50000-300s.jsonl`，参数同前两轮：双向 2TX2RX、SC8、ramp、每块 196608 字节、32 个 USB 请求、64 MiB 主机缓冲、5 秒预热，随后独立统计 300 秒稳态。该次无错结果提供了留出吞吐余量后的观察点，不代表已确定最高稳定速率或保证任意电脑、任意持续时长均无丢样。
