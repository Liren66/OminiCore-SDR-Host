# 固件与协议依赖

主机安装器只安装 Windows 程序、主机 C/C++ 开发接口和目标数据口的 libusbK 驱动，不含板级 SDK 源码副本、ELF、BOOT.bin 或 FPGA bitstream，不自动烧写 CH32、SDK、FPGA 或启动 Flash。把软件装到新电脑后仍须连接具有匹配固件的 OmniCoreSDR。

本版实际检查/使用以下能力：

| 组件 | 必要能力 | 检查方式 |
|---|---|---|
| ZYNQ SDK | 命令封装协议 v1 | host_protocol? 包含 host_protocol=1 |
| ZYNQ SDK | RX 溢出清错后仅重启 RX 并重新发完整头 | system_info? 包含 header_on=rx_start_and_overflow_recovery |
| ZYNQ SDK FIR 扩展 | 安全分块上传、快照读取、双通道 SPI 校验 | 使用 FIR 功能时要求 system_info? 包含 fir_upload=1 |
| ZYNQ SDK 时钟扩展 | 无降速回退的共享 Fs 和完整链事务 | 使用安全时钟设置时要求 system_info? 包含 clock_config=1 |
| ZYNQ SDK RX 校正扩展 | RF/BBDC 直流及 RX 正交跟踪开关、状态回读 | 使用 RX correction 时要求 system_info? 包含 rx_correction=1 |
| ZYNQ SDK ADF4002 扩展 | 外部 10 MHz 驯服、GPIO 检查和 D5 自检 | 状态/自检要求 adf4002=1；选源还要求 ref_source=1 |
| ZYNQ SDK 外部 LO 扩展 | 独立 RX/TX 源、验证和名义频率状态 | 使用 LO source 时要求 external_lo=1 |
| 可选 2TX2RX 8bit 扩展 | SDK 格式命令与 FPGA signed8 路径 | 仅选择 8bit 时要求 iq_format=1 iq8_2t2r=1；默认 12bit 保持旧 SDK 兼容 |
| CH32 | 已有诊断及流清理协议 v4 | USB 厂商请求 0xD0 的诊断版本 |
| FPGA/IP | RX 启动时 16384 B 的 00..FF×64 头；现有 packed12 及通道布局 | 匹配本工程使用的 FPGA 构建；由数据流检测头 |
| USB | SuperSpeed 或 USB 2.0 High-Speed（480 Mbit/s）；数据端点 01/81，控制端点 02/82 | 枚举及打开时确认；Full-Speed（12 Mbit/s）不支持 |
| Windows 驱动 | 发行包 libusbK 3.1.0.0 | och-driver check；实际安装仍受 Windows 策略约束 |

旧 SDK 即使支持原始串口命令和 16 KiB 起始头，也可能缺少 OCH 所需的命令封装协议，不能直接假定兼容。版本不满足时基座明确失败，避免把超时/残留回复误认为新命令已成功。

FIR/clock 扩展继续沿用固件名字 SDK_CMD_1.0_20261023，因此不能只比较名字来判断功能。以能力字段为准；规范的 sample-rate setter 使用安全时钟入口，也需要新扩展。协议详细格式见 [设备协议参考](protocol.md)。自定义 FIR/时钟状态保存在运行中 SDK 的 RAM，重新下载 ELF 或断电不保证保留。

RX 校正同样按 `rx_correction=1` 检查，不要求更换 CH32 固件。旧 SDK 缺少该字段时只拒绝新增校正 API/规范命令，原有功能继续按各自能力工作。typed 校正 setter 在暂停流之前完成能力检查；相同设置不造成重启，真正变更使用 OCH 0.3 的协调暂停/恢复机制。

支持 RX 校正的 SDK 冷启动默认启用两项 RX 跟踪；FIR/时钟操作、RF 重建和 rf_reinit 恢复最近成功保存的开关目标。该目标仅保存在 RAM，断电或重新下载程序后恢复默认值。

0.3.2 的 ADF4002 与 LO source API 通过各自能力字段预检，不改变旧 ABI 结构。冷启动默认内部参考和 RX/TX 内部 LO。10 MHz 驯服后 AD9361 的参考频率仍为 40 MHz；MUXOUT 只接 D5，没有处理器反馈，不能提供软件锁定/LED 读回。D5 自检自动确认的范围是 GPIO pad，仍需人工观察闪烁确认芯片响应。这些 0.3.2 扩展不修改 CH32，也不提供 PPS 或跨设备相干同步，详见 [参考与外部 LO](reference-lo.md)。

封装请求在原 SDK 命令前加入 @请求号；响应以相同请求号的 RSP begin/end 包围，结束行给出结果。独立 EVT 可穿插其间，由控制线程分离。这样多行回复、异步错误和连续命令不会靠不可靠的“静默一段时间”猜测完成。本版同时只在控制通道发出一条待完成请求，不是命令并行执行。

CH32 的协议 v4 是使用原有诊断能力，不代表主机安装器为您升级了 CH32 固件。实际板级 SDK 工程位于同级 AD9361_Template_1/AD9361_Template.sdk，FPGA 工程也在 AD9361_Template_1 内，独立于 OmniCoreHost。构建、下载及持久化状态由这些板级工程管理；RAM 中已运行的新 ELF 不等于断电后仍会启动该版本。需要长期使用时，请按板级工程流程部署已确认兼容的启动镜像。

打开设备会停止旧 USB 会话并清理缓存，因此不要同时让原上位机、调试脚本和本基座操作同一个数据口。只需检查 Windows 驱动时用 och-driver check，只需枚举时用 och find。

0.3.3 新增 [PPS 频率监控](pps.md)，要求修订号 2 的 PPS IP 比特流和 `pps_monitor=1` SDK。冷启动默认关闭，缺少硬件或固件支持时返回 OCH_UNSUPPORTED。新增 API 保持 ABI 1，仍不修改 CH32 固件。

## 0.3.5 板级 RGB 与风扇温控

新增 RGB 开关/状态、风扇温控开关/状态和 Zynq 核心温度查询。使用新 FPGA BOARD_CTRL_AXI 及新版 SDK，能力标志为 `board_rgb=1 fan_control=1 core_temp=1`。当前 SDK 默认 RGB 开启、亮度 15%，风扇温控关闭；OCH 0.3.6 同时兼容旧版 25% 固件，亮度以设备回报为准；不改变已有 C ABI 结构或停止 IQ 流。详见 [板级控制及接口](board-control.md)。

## USB 2.0 高速连接

USB 2.0 需要同时使用更新后的 UHSIF 固件与重新构建的 OmniCoreHost。旧固件缺少 USBHS 控制端点和 0xD0 诊断/流清理实现，旧主机程序则会拒绝非 SuperSpeed 连接。

HS/FS 配置描述符与 USB3 使用相同的端点分工：01/81 为 IQ 数据，02/82 为 USART1 控制，83 为调试文本。USBHS 的控制与调试 IN 使用小于最大包长的短包；复位、取消配置和清除端点 HALT 会同步处理桥接状态。OCH 支持 512 B 最大包长的 High-Speed，仍拒绝 64 B 最大包长的 Full-Speed。

编译下载 CH32 固件并重新构建主机程序后，重新插拔设备，依次运行 `och-driver check`、`och find`、`och info`。OCH 0.4.3 在 USB2 下精确显示 **480 Mbit/s（High-Speed, USB 2.0）**，且不再出现控制端点缺失、SuperSpeed 限制或 CH32 diagnostics unavailable。再通过 USB3 连接重复检查，并分别验证低采样率 RX/TX、停止重启与重新插拔。

USB2 连续传输已单独上板测量，配置、持续时间和通过记录见 [USB2 测试结果](usb2.md)。`och info` 的 61.44 MSPS 是射频采样率上限，USB2 吞吐参考见 README 和实测报告，且只与本次固件和通道 / 精度配置匹配。此修改未降低 SDK 的射频采样率范围，也未自动改变用户配置；换主机、USB 拓扑或固件后应重新测试。
