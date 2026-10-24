from svgkit import *

s = Svg(1000, 600, '参考时钟、LO 与 PPS', '外部 10 MHz 参考、ADF4002 驯服 40 MHz VCXO、AD9361 时钟链、外部 LO 与 PPS 频率监控')
s.text(28, 42, '参考时钟、外部 LO 与 PPS 监控', 20, INK, 700)
s.text(28, 66, '示例采样率 30.72 MSPS；括号内为对应的 och 命令', 13, INK2)

def conn(x, y, name, sub):
    s.rect(x, y, 112, 46, '#2f2e2b', None, 23)
    s.text(x + 56, y + 20, name, 13, '#ffffff', 700, 'middle')
    s.text(x + 56, y + 36, sub, 10.5, '#d8d6cf', 400, 'middle')

def arrow(x1, y1, x2, y2, color=INK2, dash=None, mk='a-ink'):
    s.line(x1, y1, x2, y2, color, 1.6, dash, arrow=mk)

# Column: board connectors
s.text(28, 100, '板上接口', 12, MUTED, 700)
conn(28, 112, 'J3', '外部 10 MHz')
conn(28, 300, 'J11', 'RX 外部 LO')
conn(28, 360, 'J10', 'TX 外部 LO')
conn(28, 500, 'J6', 'PPS 输入')

# Reference chain (top)
s.box(200, 106, 190, 58, 'ADF4002 PLL', 'R = 1 · N = 4 · PFD 10 MHz', VIOLET_T, VIOLET_S, VIOLET_D)
arrow(140, 135, 198, 135, INK2)
s.text(169, 128, '外部', 10, MUTED, 600, 'middle')
s.box(450, 106, 170, 58, '40 MHz VCXO', '内部模式：自由运行', '#ffffff', NEUTRAL_S)
arrow(390, 135, 448, 135, VIOLET, mk='a-violet'); s.text(419, 128, '调谐', 10, VIOLET_D, 600, 'middle')
s.path('M535,106 L535,90 L295,90 L295,104', MUTED, 1.2, dash='3 3', arrow='a-muted')
s.text(415, 84, 'N 分频反馈', 10, MUTED, 600, 'middle')
s.box(200, 196, 190, 44, 'D5 指示灯', 'MUXOUT · 锁定时常亮', '#fffbe9', '#e9cf7a', INK, INK2, 13, 11)
arrow(295, 164, 295, 194, INK2)
s.text(405, 222, '(clock.ref-source / clock.ref-test)', 11, MUTED, 400, 'start', mono=True)

# AD9361 block
ax, ay, aw, ah = 680, 96, 292, 330
s.rect(ax, ay, aw, ah, '#f6f9fe', BLUE_S, 10)
s.text(ax + 14, ay + 22, 'AD9361', 14, BLUE_D, 700)
s.box(ax + 14, ay + 34, aw - 28, 40, 'REF_CLK  40 MHz', None, '#ffffff', BLUE_S, INK, INK2, 13)
arrow(620, 135, ax + 12, ay + 54, INK2)
s.box(ax + 14, ay + 90, aw - 28, 40, 'BBPLL  983.04 MHz', None, '#ffffff', BLUE_S, INK, INK2, 13)
arrow(ax + aw / 2, ay + 74, ax + aw / 2, ay + 88, BLUE, mk='a-blue')
s.box(ax + 14, ay + 146, aw - 28, 40, 'ADC 245.76 → … → Fs 30.72 MSPS', None, '#ffffff', BLUE_S, INK, INK2, 12.5)
arrow(ax + aw / 2, ay + 130, ax + aw / 2, ay + 144, BLUE, mk='a-blue')
s.text(ax + 14, ay + 204, '(clock status / clock sample-rate)', 10.5, MUTED, 400, mono=True)
s.box(ax + 14, ay + 218, aw - 28, 54, 'RX / TX LO 合成器', ['内部 PLL 70 MHz – 6 GHz'], '#ffffff', BLUE_S, INK, INK2, 13, 11)
s.text(ax + 14, ay + 292, '(rx.lo-source / tx.lo-source)', 10.5, MUTED, 400, mono=True)
s.text(ax + 14, ay + 312, 'LVDS 2R2T · DATA_CLK = 4 × Fs', 11.5, INK2, 600)

# External LO
s.rect(200, 300, 420, 106, '#ffffff', NEUTRAL_S, 8)
s.text(216, 322, '外部 LO（RX / TX 可分别选择）', 13, INK, 700)
s.text(216, 344, '物理输入频率 = 2 × 名义 RF 频率', 12, INK2)
s.text(216, 364, '名义 RF 70 MHz – 4 GHz；lo-frequency 只声明，不调节信号源', 12, INK2)
s.text(216, 384, '切回 internal 时恢复该方向最后一次内部 PLL 频率', 12, INK2)
arrow(140, 323, 198, 323, INK2); arrow(140, 383, 198, 383, INK2)
arrow(620, 353, ax + 12, ay + 245, INK2)

# FPGA PPS monitoring
fy = 470
s.rect(200, fy, 772, 108, '#f7f6f3', NEUTRAL_S, 10)
s.text(956, fy + 22, 'FPGA · PPS 频率监控（只测量，不调节时钟）', 13, INK, 700, 'end')
s.box(216, fy + 36, 230, 56, 'DATA_CLK 计数器', '30.72 MSPS → 122.88 MHz', '#ffffff', NEUTRAL_S, INK, INK2, 13, 11)
arrow(140, 523, 214, 523, INK2)
s.path(f'M{ax + aw / 2},{ay + ah} L{ax + aw / 2},{fy - 14} L331,{fy - 14} L331,{fy + 34}', BLUE, 1.6, arrow='a-blue')
s.text(560, fy - 20, 'DATA_CLK', 10.5, BLUE_D, 600, 'middle')
s.box(500, fy + 36, 456, 56, 'pps.status', ['measured_hz · error_ppm · |误差| ≤ 1 ppm 时 locked = 1', '无输入：waiting → no_signal'], '#ffffff', NEUTRAL_S, INK, INK2, 13, 11)
arrow(446, fy + 64, 498, fy + 64, INK2)

open('clock-tree.svg', 'w').write(s.render())
