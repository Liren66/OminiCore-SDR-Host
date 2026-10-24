# 文档插图

README 与 `docs/` 中使用的正式插图，保留 SVG 图表与 PNG 终端图片。

| 文件 | 内容 | 数据来源 |
|---|---|---|
| `architecture.svg` | 软件分层、控制面 / 数据面、USB 链路与板卡组成 | `docs/architecture.md` |
| `rx-boundaries.svg` | RX 起始头、溢出恢复、epoch 与 metadata 记录 | `src/core/stream.cpp` 的元数据标志 |
| `reconfigure.svg` | 运行中改参的暂停、应用、恢复流程 | `docs/architecture.md` |
| `wire-format.svg` | packed12 / signed8 字节布局、应用格式、USB 请求对齐 | `docs/iq8.md`、编解码实现 |
| `clock-tree.svg` | 外部 10 MHz、ADF4002、AD9361 时钟链、外部 LO、PPS 监控 | 设备 `clock status` / `adf4002_status` 回读 |
| `throughput.svg` | 实测 USB 吞吐与理论 wire 速率 | 2026-10-25 CLI 实板测试与 8bit 61.44 MSPS 长时间测试（OCH 0.4.2） |
| `test-results.svg` | 三类测试的结果与实板用例分组 | 2026-10-25 测试运行（OCH 0.4.2） |
| `usb2-throughput.svg` | USB2 七种通过配置的实测吞吐与理论 wire 速率 | `docs/data/usb2/selected.json` 及其引用的 300 秒通过记录 |
| `cli-usb2-info.png` | USB2 设备信息，与 USB3 使用相同布局 | 测量记录中的设备信息，经 CLI 显示代码绘制 |
| `cli-usb2-bench.png` | 8bit 双通道全双工 5 MSPS、300 秒测试输出 | `docs/data/usb2/fast300/duplex8-5000000-300s.json`，经 CLI 显示代码绘制 |
| `cli-*.png` | 终端输出截图 | 见下文 |

终端图片按 CLI 显示代码（`src/cli/presentation.hpp`）与 Windows Terminal 默认配色绘制。USB3 图片使用 OmniCoreSDR（固件 `SDK_CMD_1.0_20261024_IQ8`，USB 5 Gbit/s）的实测数据，USB2 图片的数据来源见上表。`cli-hwtest.png` 为 OCH 0.4.2 实板测试输出；较长的输出以 `...` 标明省略部分。
