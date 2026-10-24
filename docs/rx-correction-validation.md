# RX 直流 / IQ 跟踪校正验证（2026-10-24）

OCH 0.3.1 与 `SDK_CMD_1.0_20261024` 已在 Windows x64 / libusbK、设备 `OMINICORE00000001` 上验证。功能是 AD9361 原生 RX 连续跟踪：DC 同时控制 RFDC 和 BBDC，IQ 控制接收正交误差跟踪。关闭后已有校正系数仍应用，不是 TX IQ 校准或软件去直流。能力按 `system_info?` 的 `rx_correction=1` 判断。

```powershell
.\build\och.exe get rx.dc-correction
.\build\och.exe set rx.dc-correction on
.\build\och.exe get rx.iq-correction
.\build\och.exe set rx.iq-correction on
```

用 `off` 关闭，也接受 `0/1`。SDK 原始指令为 `rx_dc_correction?/=0|1`、`rx_iq_correction?/=0|1`，设置时须停 TX 和 USB 数据通路。应用在运行中通过新增 C/C++ typed setter 调用，OCH 自动协调暂停、验证、恢复原流。

## 构建与测试结果

| 项目 | 结果及范围 |
|---|---|
| Zynq SDK | Xilinx SDK 2018.3 ARM GCC 编译和链接通过；保留旧驱动既有 warning，新增校正代码没有 warning |
| OCH | Release 构建、安装暂存、0.3.1 安装包生成通过；C11 公共头文件/导入库/三项新 API 链接及参数检查通过 |
| 主机自动回归 | **34/34**，包括既有流协调，以及新校正查询、独立状态、双向恢复、epoch、no-op、非法值、旧固件、RX lease、停用通道、写失败、回读不符与能力预检 |
| SDK 故障注入 | **45/45**：38 项校正/原生驱动测试，7 项 SPI 包装测试；测试直接编译生产函数/校正模块，注入底层读写失败和静默丢写 |
| 实机 RX 切换 | 最终固件上 **6 次切换 + 18 项检查** 全部通过；RX1 30.72 MSPS，LO 1 GHz，保留原流句柄，产生新 epoch 与 RECONFIGURED |
| 实机状态保持 | DC/IQ 均关闭后，复写当前 Fs、完整时钟链、RX/TX FIR、FIR 启停和 `rf_reinit!`，均保持关闭且读回一致 |
| 实机参数拒绝 | `2`、`-1`、`0.5`、多余参数被 SDK 拒绝，校正状态不变 |
| 数字接口 | 最终固件 30.72 MSPS 下 RX PRBS、TX 数字回环均 pass；未创建 TX 发射流 |
| CH32H417 | 两个工程目录共 **427 个文件**，修改前后 SHA256 全部一致，无新增/删除 |

最终实机运行接收 143,725,906 个复采样点，USB transfer error 为 0；6 次 setter 完整调用耗时约 168–180 ms（包括串口能力查询和回读，不等于纯硬件校正时间）。测试保留了 **5 次 RX 溢出**，其中 4 次完成自动补头恢复，最后 1 次在测试结束停流前出现；最终 pending=0、failures=0。重配主动丢弃的已知主机样本为 655,360，设备侧丢样数未知。因此通过表示控制与恢复行为正确，不表示无缝、无丢样收发。最终恢复 DC=1、IQ=1，RX/TX 停止。

没有使用外部 RF 信号源/频谱仪测量 DC 尖峰、镜像抑制度、EVM 或温漂；双向切换的故障与队列行为由主机模拟回归覆盖，本次真实设备流测试为 RX only。

## 本次完成的适配优化

- SPI 传输、tracking 写入错误向上传递；五个寄存器读回后才提交成功状态；回滚失败标记 RF 未就绪，要求 `rf_reinit!`。
- 原生 tracking setter 成功后再更新缓存；带宽/校准失败走恢复清理路径，保留原始校准错误。
- 最近成功的校正目标保留到 RF 重初始化，FIR/时钟事务检查校正状态，避免重配悄悄重新开启。
- FIR/clock 功能能力与固定参考钟检查前移，旧固件/不支持请求不会先破坏已有流。
- 运行中改参示例在立即 TIMEOUT 后短暂休眠，避免忙重试；实机验证程序采用相同策略。首轮无休眠时累计约 624 万次立即返回，加入 1 ms 退避后最终运行仅 36 次；这不是 RF 性能改进的证明。
- 硬件重配脚本显式链接 MinGW `libomnicore.dll.a`，避免误选旧的 MSVC `omnicore.lib`；本次两种导入库均已刷新。

## 后续值得完善的项目

1. 原 no-OS `util.c` 的 `clk_set_rate` 和 ENSM force/restore 仍有未完整传播的底层失败路径。建议单独补齐错误传播与故障注入，避免扩大本次已验证补丁。
2. 单项 SDK 校准轮询上限约 24 秒，部分 OCH 命令预算为 5/15 秒。需结合多阶段校准及失败回滚统一超时预算，验证慢校准时控制通道恢复；本次没有人为制造硬件失锁。
3. 用已知 RF 输入测试两 RX 通道的 DC 和镜像抑制度，并覆盖 LO/Fs/FIR、温度变化。不要用数字自检替代模拟 RF 指标。

## 产物与复现记录

板级 ELF 为同级工程 `AD9361_Template_1/AD9361_Template.sdk/AD9361_TEST/Debug/AD9361_TEST.elf`，SHA256：

`4674a649736643d094a1bec5ce10671778f395e543bbb0e673f986abbc3f3b0f`

对应 `bootimage/BOOT.bin` 已用新 ELF、原有 FSBL 和原有 bitstream 重新生成。实机仅通过 JTAG 将 ELF 加载到 RAM，未烧写启动 Flash；断电后的固件仍取决于原启动介质。CH32、BSP 和 FPGA 工程未改。

安装包位于 `dist/OmniCoreHost-0.3.1-win64-setup.exe`，附 SHA256 文件；没有替换系统 Program Files 中的已安装版本。主机安装包不包含板级 BOOT/ELF。

全部临时脚本、旧 ELF/BOOT 备份和测试日志在工作区根目录 `_correction_validation_20261024`：`runtime-tests-result.txt`、`sdk/fault_injection.log`、`hardware-rx-correction.jsonl`、`new-firmware-selftest.json`、`ch32-unchanged.json`。`sdk/test_correction.ps1` 可重跑 SDK 故障注入；`build-run-hardware-rx-correction.ps1` 默认仅编译，显式 `-Run` 才运行实机。
