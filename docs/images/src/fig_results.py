from svgkit import *

# ---------------------------------------------------------------- throughput
groups = [
    # (direction, measured MB/s, theoretical MB/s, ratio % as reported by the test script from exact byte counts)
    ('RX · 2 通道 · 61.44 MSPS', '12bit · 5 秒', [('RX', 367.2, 368.64, 99.6)]),
    ('RX · 1 通道 · 61.44 MSPS', '12bit · 5 秒', [('RX', 183.6, 184.32, 99.6)]),
    ('TX · 2 通道 · 61.44 MSPS', '12bit · 5 秒', [('TX', 362.0, 368.64, 98.2)]),
    ('2RX + 2TX · 30.72 MSPS', '12bit · 5 秒', [('RX', 183.4, 184.32, 99.5), ('TX', 180.9, 184.32, 98.1)]),
    ('1RX + 1TX · 61.44 MSPS', '12bit · 5 秒', [('RX', 183.8, 184.32, 99.7), ('TX', 180.7, 184.32, 98.1)]),
    ('2RX + 2TX · 50 MSPS', '8bit wire · 5 秒', [('RX', 199.2, 200.0, 99.6), ('TX', 196.3, 200.0, 98.1)]),
    ('2RX + 2TX · 61.44 MSPS', '8bit wire · 300 秒连续', [('RX', 245.7, 245.76, 100.0), ('TX', 245.7, 245.76, 100.0)]),
]
BAR, GAP, GROUP_GAP = 16, 2, 22
PX0, PW, MAXV = 290, 570, 400
sx = lambda v: PX0 + v / MAXV * PW
top = 132
height = top + sum(len(g[2]) * BAR + (len(g[2]) - 1) * GAP for g in groups) + GROUP_GAP * (len(groups) - 1) + 96
s = Svg(1000, int(height), 'USB 吞吐实测', 'OCH 0.4.1 在 OmniCoreSDR 上的 bench 结果：实测 USB 吞吐与理论 wire 速率')
s.text(28, 42, 'USB 吞吐实测', 20, INK, 700)
s.text(28, 66, 'OCH 0.4.2 · ThinkPad E14 Gen 7 · Core Ultra 7 255H · Windows 11 25H2 · USB 5 Gbit/s · 平均值含启停 · 2026-10-25', 13, INK2)
# legend
lx = 28; ly = 96
for name, color in [('RX 实测', BLUE), ('TX 实测', ORANGE)]:
    s.rect(lx, ly - 10, 12, 12, color, None, 3); s.text(lx + 18, ly, name, 12.5, INK2, 500); lx += 92
s.line(lx + 6, ly - 12, lx + 6, ly + 4, INK, 2); s.text(lx + 16, ly, '理论 wire 速率', 12.5, INK2, 500)
s.text(972, ly, '标签：实测 MB/s · 实测 / 理论', 12, MUTED, 400, 'end')

bottom = height - 64
for v in range(0, MAXV + 1, 100):
    x = sx(v)
    s.line(x, top - 10, x, bottom, GRID if v else NEUTRAL_S, 1)
    s.text(x, bottom + 18, f'{v}', 11.5, MUTED, 400, 'middle')
s.text(sx(MAXV), bottom + 36, 'MB/s（十进制）', 11.5, MUTED, 400, 'end')

y = top
for label, sub, bars in groups:
    gh = len(bars) * BAR + (len(bars) - 1) * GAP
    s.text(28, y + gh / 2 + 1, label, 13, INK, 600)
    s.text(28, y + gh / 2 + 17, sub, 11, MUTED)
    for d, val, exp, ratio in bars:
        color = BLUE if d == 'RX' else ORANGE
        x1 = sx(val); r = 4
        s.path(f'M{PX0},{y} L{x1 - r},{y} Q{x1},{y} {x1},{y + r} L{x1},{y + BAR - r} Q{x1},{y + BAR} {x1 - r},{y + BAR} L{PX0},{y + BAR} Z',
               color, 0, fill=color)
        xe = sx(exp)
        s.line(xe, y - 3, xe, y + BAR + 3, INK, 2)
        s.text(max(x1, xe) + 8, y + BAR - 3.5, f'{val:.1f} · {ratio:.1f}%', 12, INK, 600)
        s.text(PX0 - 8, y + BAR - 3.5, d, 11, INK2, 700, 'end')
        y += BAR + GAP
    y += GROUP_GAP - GAP
open('throughput.svg', 'w').write(s.render())

# ---------------------------------------------------------------- test summary
s = Svg(1000, 448, '测试结果', 'CLI 离线测试、运行时回归与 CLI 实板测试的结果和覆盖范围')
s.text(28, 42, '测试结果', 20, INK, 700)
s.text(28, 66, 'OCH 0.4.2 · 2026-10-25 · 离线测试不需要硬件；实板测试在 OmniCoreSDR 上运行 och.exe', 13, INK2)

def check_badge(x, y):
    s.rect(x, y, 86, 24, GOOD_T, '#9fd89f', 12)
    s.path(f'M{x + 12},{y + 12} l4,4 l8,-8', GOOD, 2.2)
    s.text(x + 30, y + 16.5, '无失败', 11.5, '#0a6b0a', 700)

suites = [('CLI 离线测试', '20 / 20', 'tests/cli_tests.cpp', '参数与数值解析 · FTR 校验 · 输出布局'),
          ('运行时回归', '91 / 91', 'tests/runtime_tests.cpp', '模拟 USB 设备 · 改参 · 校正 · PPS · 8bit'),
          ('CLI 实板测试', '42 / 42', 'tests/hardware/cli_hardware_tests.ps1', '0 失败 · 文件采集 · 播放 · 测速')]
tw = (944 - 2 * 16) / 3
for i, (name, num, src, desc) in enumerate(suites):
    x = 28 + i * (tw + 16); y = 90
    s.rect(x, y, tw, 132, '#ffffff', NEUTRAL_S, 10)
    s.text(x + 18, y + 28, name, 14, INK, 700)
    check_badge(x + tw - 104, y + 12)
    s.text(x + 18, y + 76, num, 34, INK, 700)
    s.text(x + 18, y + 100, desc, 12, INK2)
    s.text(x + 18, y + 120, src, 11, MUTED, 400, mono=True)

s.text(28, 258, 'CLI 实板测试的 7 组用例', 14, INK, 700)
s.text(972, 258, 'TX 用例默认 60 dB 衰减', 12, MUTED, 400, 'end')
hw = [('basic', 9, ['版本 · 发现 · info', '状态 · 时钟 · FIR', '温度 · 数字自检']),
      ('control', 3, ['LO / 衰减', '设置与回读', 'config 不启动 TX']),
      ('args', 13, ['12 种错误参数', '退出码 2', 'JSON / 文本错误']),
      ('capture', 4, ['sc16 · fc32 · p12', '大小与 metadata', '覆盖保护']),
      ('playback', 4, ['文件到 EOF', '循环 · 1 MHz 正弦', '过短文件拒绝']),
      ('bench', 6, ['RX / TX / 双工', '12bit · 8bit', '吞吐 ≥ 95%']),
      ('output', 3, ['重定向纯 ASCII', '78 列以内', 'JSON 无装饰'])]
total = sum(n for _, n, _ in hw)
W = 944
cw = (W - 6 * 8) / 7
for i, (name, n, lines) in enumerate(hw):
    x = 28 + i * (cw + 8); y = 276
    s.rect(x, y, cw, 148, '#ffffff', NEUTRAL_S, 8)
    s.text(x + 12, y + 24, name, 13, INK, 700, mono=True)
    s.text(x + cw - 12, y + 26, str(n), 22, BLUE_D, 700, 'end')
    for j, l in enumerate(lines):
        s.text(x + 12, y + 62 + j * 22, l, 11.5, INK2)
    s.text(x + 12, y + 136, f'{n} / {n} 无失败', 11, '#0a6b0a', 600)
open('test-results.svg', 'w').write(s.render())
