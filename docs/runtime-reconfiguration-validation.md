# 0.3.0 运行中改参实机验证

验证日期：2026-10-24。本记录验证同一 OCH device/stream 上的暂停、改参和恢复，以及错误/间断可观察性，不是 OnAir 集成验收或 RF 性能测量。

## 条件与范围

| 项目 | 本次条件 |
|---|---|
| 主机软件 | OmniCoreHost 0.3.0，Windows x64，libusbK 后端 |
| 设备 | OMINICORE00000001 |
| 板上固件 | SDK_CMD_1.0_20261023，使用现有固件协议 |
| USB 链路 | 5 Gbit/s |
| 通道 | 每个活动方向仅物理通道 1，mask=1；未使用方向 mask=0 |
| 流格式 | packed12；RX 第一轮 recv，第二轮 acquire/release |
| 缓冲 | 默认每方向 64 MiB，32 个 USB 请求，每请求 196608 B |
| 初始射频请求 | RX/TX LO 100 MHz，带宽 5 MHz，RX 手动增益 30 dB |
| TX 数据 | 全零 IQ，TX 衰减始终为最大值 89.75 dB |
| 主要采样率 | 30.72 ↔ 15.36 MSPS；额外双向测试为 10 ↔ 12 MSPS |

主要三种模式和 10/12 MSPS 双向模式各执行两轮四项变更：LO 100 ↔ 101 MHz、RX 增益 30 ↔ 31 dB、带宽 5 ↔ 4 MHz、共享采样率往返。RF 控制模式执行一轮四项常规变更后再执行四项控制复写。LO、增益和带宽经 och_configure，采样率经 och_set_sample_rate。TX-only 模式中的 gain 项仍修改 RX 增益配置，不是改变 TX 衰减，也没有 RX 数据可用于核验增益效果。整个过程沿用原 stream 句柄。

测试程序为 [hardware_reconfigure.cpp](../tools/hardware_reconfigure.cpp)，启动脚本为 [run-hardware-reconfigure.ps1](../tools/run-hardware-reconfigure.ps1)。核心不改变固件；既有 [固件依赖](firmware-compatibility.md) 仍适用。双通道、其他 USB 控制器、其他固件版本、RF 频响/增益精度、连续相位和精确定时 TX 不在本次实机验证范围内。额外 RF 控制运行复写并回读已有的完整时钟链、RX/TX FIR 系数和 FIR 启用状态，验证协调路径与配置一致性；它不验证新的滤波器设计或频响。

## 结果与原始记录

主要三种模式共 **24/24 次改参通过**；额外 10/12 MSPS 双向模式 **8/8 次通过**，RF 控制模式 **8/8 次通过**（4 次常规参数变更 + 4 次 typed RF 控制，无跳过），最终合计 **40/40 次**。每次成功改参后，活动方向均恢复数据进展，生成一个 RECONFIGURED 事件；有 RX 的每次改参均观察到新的 epoch 和 flags=51，即 START | DISCONTINUITY | TIME_UNKNOWN | RECONFIGURED。所有这些变更记录的 lease_busy_retries 均为 0。

| 模式 | 变更通过 | 主机 API 调用范围，ms | RX 首个新段在 API 返回后观察到的时间，ms | 原始日志（源码工作区） |
|---|---:|---:|---:|---|
| RX，30.72/15.36 MSPS | 8/8 | 70.781–258.411 | 3.520–14.450 | [rx-30m-final.jsonl](../build/validation-0.3/rx-30m-final.jsonl) |
| 零 IQ TX，30.72/15.36 MSPS | 8/8 | 143.287–313.860 | 不适用 | [tx-zero-30m-final.jsonl](../build/validation-0.3/tx-zero-30m-final.jsonl) |
| RX + 零 IQ TX，30.72/15.36 MSPS | 8/8 | 165.290–352.221 | 3.819–11.720 | [duplex-zero-30m-final.jsonl](../build/validation-0.3/duplex-zero-30m-final.jsonl) |
| RX + 零 IQ TX，10/12 MSPS | 8/8 | 158.399–873.281 | 3.803–12.936 | [duplex-final.jsonl](../build/validation-0.3/duplex-final.jsonl) |
| RX + 零 IQ TX，含 RF 控制 | 8/8 | 158.394–1057.325 | 3.666–11.916 | [rf-controls-final.jsonl](../build/validation-0.3/rf-controls-final.jsonl) |

所有时间均来自主机单调时钟。call_ms 是公开 API 的调用耗时；first_new_epoch_after_return_ms 是消费者观察到带 RECONFIGURED 的新段相对 API 返回的时间，不是 ADC/DAC 的停顿时长。没有硬件时间戳，无法由这些时间推断精确丢样数或 RF 最后/首个样本时刻。原始 JSONL 位于构建目录，不随仅含软件的安装包必然提供。

下表给出每类两次改参的最小值和最大值，避免用所有项目混合的单一平均值掩盖差异：

| 模式 | LO，ms | RX 增益，ms | 带宽，ms | 共享采样率，ms |
|---|---:|---:|---:|---:|
| RX，30.72/15.36 | 85.606–94.271 | 70.781–71.604 | 103.695–117.180 | 256.625–258.411 |
| TX，30.72/15.36 | 158.756–159.683 | 143.287–143.849 | 173.367–174.016 | 313.430–313.860 |
| 双向，30.72/15.36 | 174.455–175.349 | 165.290–165.866 | 190.576–207.012 | 344.246–352.221 |
| 双向，10/12 | 180.905–191.546 | 158.399–160.428 | 205.777–873.281 | 345.527–361.538 |

额外双向测试的 bandwidth_1 调用实际为 **873.281 ms**，从调用开始到消费者观察到新段为 **885.173 ms**。该次结果通过，但明显慢于同组其他非采样率修改；本记录不剔除此值，也不在缺少进一步证据时归因。上述三个有 RX 的常规参数测试，从 API 调用开始到观察到新段的范围分别为 77.222–264.386 ms、169.684–357.296 ms、171.336–885.173 ms。

RF 控制运行先执行一轮常规变更，LO、RX 增益、带宽和 Fs 调用分别为 188.409、158.394、211.050、341.193 ms，采样率由 30.72 改为 15.36 MSPS。随后执行以下操作，四项均成功且 readback.identical=true：

| typed 操作 | 内容 | API 调用，ms | 从调用开始到观察到新 RX 段，ms |
|---|---|---:|---:|
| och_set_clock | 复写当前完整时钟链 | 389.385 | 401.221 |
| och_set_fir，RX | 复写当前 RX 系数 | 630.173 | 640.720 |
| och_set_fir，TX | 复写当前 TX 系数 | 1057.325 | 1062.157 |
| och_set_fir_enabled | 复写当前两个方向启用状态 | 711.986 | 715.652 |

该次 RX/TX FIR 快照均为 enabled=0、factor=1、tap_count=64、crc32=2724806369。系数上传和校验成功不等于滤波器已开启，更不等于已测量频响。typed FIR/clock 写入即使内容相同仍会执行事务，不能与 och_configure 的同参数 no-op 混淆。这组完整运行从 API 调用开始到观察到新段的范围为 170.091–1062.157 ms。

## 错误、丢弃和补零

以下为各日志 final_running 快照；双向 device_overflows 是设备共享计数，在 RX/TX stats 中重复出现，只计一次。它表示收到的相关设备通知，不是精确物理 FIFO 溢出次数或丢样量。

| 模式 | 各活动流 USB transfer_errors | 设备 RX 溢出通知计数 | 设备 TX 欠流 | TX 主机欠供 | RX 已知主机丢样 | TX 补零样本/通道 |
|---|---:|---:|---:|---:|---:|---:|
| RX，30.72/15.36 | 0 | 10 | 0 | 不适用 | 1,966,080 | 不适用 |
| TX，30.72/15.36 | 0 | 0 | 0 | 0 | 不适用 | 2,097,152 |
| 双向，30.72/15.36 | RX=0，TX=0 | 6 | 0 | 0 | 1,966,079 | 2,097,152 |
| 双向，10/12 | RX=0，TX=0 | 9 | 0 | 0 | 589,824 | 2,097,152 |
| 双向，含 RF 控制 | RX=0，TX=0 | 6 | 0 | 0 | 2,097,152 | 2,097,152 |

RX 溢出事件及随后重新发头被真实保留，测试通过不代表无溢出、无丢样或无缝切换。重配本身也会清掉旧 RX 队列，已知部分计入 host_dropped_samples；设备内未交付的数据损失仍未知。主要双向测试开始的 initial 快照已有 2 个溢出通知，因此最终 6 不能全部解释为改参单独导致；本记录没有因果隔离测量。

每次活动 TX 恢复补入 4 × 196608 B 的零 IQ，单通道 packed12 对应 **262,144 样本/通道**，8 次合计 2,097,152。重配事件同时记录 tx_queued_discarded_samples、tx_cancelled_bytes 和 tx_delivery=unknown：旧主机排队样本被丢弃，取消请求不重放，不能把已经 send 接受或 USB 完成的样本数当作 DAC 实际发射数。

最终实现先清旧会话并提交新配置及 4 块零 IQ，再提前放行原 TX 生产者，使其在硬件启动交互期间持续补队列；先恢复 RX，最后启动 TX。失败时重新关闭 TX 数据入口并停止双方。这个交接顺序在上述零 IQ 测试中得到 TX 欠流/主机欠供均为 0 的结果；它依赖持续生产者，不能保证任意上层阻塞时仍无欠流。

收尾 [stopped-status.json](../build/validation-0.3/stopped-status.json) 确认 usb_status=0、rx_ready=0/0、tx_ready=0/0、tx_started=0，expected_rx_Bps/expected_tx_Bps 均为 0。该固件快照中的 faults.tx_underflow=8 是修正启动交接前试验留下的累计计数，不能当作最终各运行新增的欠流；以上各日志的本次设备欠流计数均为 0。停止状态不提供精确 RF 最后一个样本的时间。

## 拒绝请求与回归范围

主要三份日志还包含共 14 项通过的边界检查：初始/最终同参数 no-op、非法配置拒绝、有流时 mask 变更拒绝；有 RX 时额外验证未释放租借导致 BUSY 且租借字节不变。额外双向和 RF 控制日志各包含同类 5 项检查。主要测试的同参数 configure 为 0.0023–0.0045 ms，不重启已有流；这是本次主机观测值，不是实时性保证。

实机运行中出现的 recv/send TIMEOUT 被测试程序按可重试状态处理；summary 记录了这些次数，没有把正常协调暂停算作 USB 错误。自动回归 [runtime_tests.cpp](../tests/runtime_tests.cpp) 最终 **21 项连续 5 轮，共 105 次用例全部通过**，覆盖句柄/epoch 保持、旧 TX IQ 丢弃、未启动/显式停止状态、60 秒等待被及时打断、生产者提前补队列、最小环形队列上的失败交接、参数/重启/FIR 失败及 typed 时钟/FIR 恢复。它们使用受控模拟传输，与本记录的硬件测量范围分开。

可在源码工程目录按以下方式复现主要测试；TX 模式需显式选择，默认脚本模式为 RX：

~~~powershell
powershell -File tools/build.ps1 -Configuration Release
powershell -File tools/run-hardware-reconfigure.ps1 -Mode rx -Serial OMINICORE00000001 -SampleRate 30720000 -Rounds 2
powershell -File tools/run-hardware-reconfigure.ps1 -Mode tx-zero -Serial OMINICORE00000001 -SampleRate 30720000 -Rounds 2
powershell -File tools/run-hardware-reconfigure.ps1 -Mode duplex-zero -Serial OMINICORE00000001 -SampleRate 30720000 -Rounds 2
~~~

将最后一条的 SampleRate 改为 10000000 可执行 10/12 MSPS 双向测试。RF 控制复写验证使用：

~~~powershell
powershell -File tools/run-hardware-reconfigure.ps1 -Mode duplex-zero -Serial OMINICORE00000001 -SampleRate 30720000 -Rounds 1 -RfControls
~~~

设备仍由测试进程独占；本能力不提供另一个 och.exe 修改现有软件流的 IPC，也不表示 OnAir 已有原生数据源。
