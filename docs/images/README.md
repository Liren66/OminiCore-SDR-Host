# 文档插图

README 与 `docs/` 中使用的插图。全部由 `src/` 中的脚本生成，修改数据或界面后运行 `src/build.sh` 即可重新生成。

| 文件 | 内容 | 数据来源 |
|---|---|---|
| `architecture.svg` | 软件分层、控制面 / 数据面、USB 链路与板卡组成 | `docs/architecture.md` |
| `rx-boundaries.svg` | RX 起始头、溢出恢复、epoch 与 metadata 记录 | `src/core/stream.cpp` 的元数据标志 |
| `reconfigure.svg` | 运行中改参的暂停、应用、恢复流程 | `docs/architecture.md` |
| `wire-format.svg` | packed12 / signed8 字节布局、应用格式、USB 请求对齐 | `docs/iq8.md`、编解码实现 |
| `clock-tree.svg` | 外部 10 MHz、ADF4002、AD9361 时钟链、外部 LO、PPS 监控 | 设备 `clock status` / `adf4002_status` 回读 |
| `throughput.svg` | 6 种配置的实测 USB 吞吐与理论 wire 速率 | 2026-10-24 CLI 实板测试 |
| `test-results.svg` | 三类测试的结果与实板用例分组 | 2026-10-24 测试运行 |
| `cli-*.png` | 终端输出截图 | 见下文 |

终端截图由 `src/render.cpp` 调用 `och` 自身的显示代码（`src/cli/presentation.hpp`）生成 ANSI 文本，再由 `src/ansi2png.py` 按 Windows Terminal 默认配色绘制，因此版面与颜色和实际终端一致。其中的数值取自 2026-10-24 在 OmniCoreSDR（固件 `SDK_CMD_1.0_20261024_IQ8`，USB 5 Gbit/s）上的实测结果；较长的输出以 `...` 省略了中间部分。

```bash
# 需要 Python 3（playwright + Chromium、Pillow）和 C++17 编译器
docs/images/src/build.sh
```
