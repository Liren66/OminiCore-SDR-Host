from svgkit import *

s = Svg(1000, 866, 'OmniCore Host 架构', '主机软件分层、USB 3.0 链路与 OmniCoreSDR 硬件组成')
X0, W = 140, 836

s.text(28, 42, 'OmniCore Host 架构', 20, INK, 700)
s.text(28, 66, '主机软件分层 · 控制面与数据面 · USB 3.0 · OmniCoreSDR 硬件', 13, INK2)

def layer(y, h, label):
    s.text(28, y + h / 2 + 5, label, 13, INK2, 600)

def down(y1, y2, x=X0 + W / 2, color=INK2):
    s.line(x, y1, x, y2 - 2, color, 1.5, arrow='a-ink')

# 1. applications
y = 92; h = 62; layer(y, h, '应用')
bw = (W - 32) / 3
for i, (t, sub) in enumerate([('och 命令行', 'find · info · get/set · rx · tx · bench'),
                              ('上层软件适配', 'OnAir · GNU Radio · MATLAB · SDRangel'),
                              ('用户程序', 'C · C++ · C# · Python · MATLAB MEX')]):
    s.box(X0 + i * (bw + 16), y, bw, h, t, sub, '#ffffff', NEUTRAL_S)
down(154, 180)

# 2. public interface
y = 180; h = 54; layer(y, h, '公共接口')
bw = (W - 16) / 2
s.box(X0, y, bw, h, 'C ABI  ·  omnicore/och.h', '纯 C 接口，DLL 边界不传递 STL / 异常', BLUE_T, BLUE_S, BLUE_D)
s.box(X0 + bw + 16, y, bw, h, 'C++17 RAII  ·  omnicore/och.hpp', '仅头文件：device · stream · rx_lease', BLUE_T, BLUE_S, BLUE_D)
down(234, 258)

# 3. core
y = 258; h = 252; layer(y, h, '核心')
s.rect(X0, y, W, h, '#f6f9fe', BLUE_S, 10)
s.text(X0 + 14, y + 22, 'omnicore.dll  ·  核心运行库', 13, BLUE_D, 700)
mods = [('Device', '发现 · 打开 · 独占'), ('配置事务', '差分下发 · 运行中改参'), ('射频 · FIR · 时钟', 'LO · 增益 · 采样率'),
        ('校正 · 参考 · PPS', 'DC/IQ · ADF4002'), ('板控', 'RGB · 风扇 · 温度'), ('Event · Metadata', 'epoch · 间断 · 统计')]
mw = (W - 28 - 5 * 8) / 6
for i, (t, sub) in enumerate(mods):
    s.box(X0 + 14 + i * (mw + 8), y + 34, mw, 56, t, sub, '#ffffff', BLUE_S, INK, INK2, 13, 11)
s.text(X0 + 14, y + 112, '控制面', 11, MUTED, 600)

def lane(yy, label, color, tint, stroke, dark, boxes, arrow, leftward):
    s.rect(X0 + 14, yy, W - 28, 58, tint, stroke, 8)
    s.text(X0 + 30, yy + 25, label, 14, dark, 700)
    s.text(X0 + 30, yy + 43, '数据面', 11, MUTED, 600)
    bx0 = X0 + 128; gap = 34; bwid = (X0 + W - 28 - bx0 - 3 * gap) / 4
    for i, (t, sub) in enumerate(boxes):
        bx = bx0 + i * (bwid + gap)
        s.box(bx, yy + 8, bwid, 42, t, sub, '#ffffff', stroke, INK, INK2, 13, 11)
        if i < 3:
            a, b = bx + bwid + 4, bx + bwid + gap - 4
            if leftward: a, b = b, a
            s.line(a, yy + 29, b, yy + 29, color, 2, arrow=arrow)

lane(y + 120, 'RX', BLUE, BLUE_T, BLUE_S, BLUE_D,
     [('USB IN 完成', '32 个并行请求'), ('起始头同步', 'framer · 新 epoch'), ('环形队列', '64 MiB · 有界'), ('交付应用', 'wire 租借 / 格式转换')],
     'a-blue', False)
lane(y + 186, 'TX', ORANGE, ORANGE_T, ORANGE_S, ORANGE_D,
     [('USB OUT 提交', '32 个并行请求'), ('预填 · flush', '≥ 4 块 · 补零计数'), ('环形队列', '64 MiB · 背压'), ('接收应用数据', '格式转换 codec')],
     'a-orange', True)
down(510, 534)

# 4. transport
y = 534; h = 56; layer(y, h, '传输')
bw = (W - 16) / 2
s.box(X0, y, bw, h, 'libusbK  ·  控制面', 'SDK 命令封装 v1 · 端点 02 / 82', VIOLET_T, VIOLET_S, VIOLET_D)
s.box(X0 + bw + 16, y, bw, h, 'libusbK  ·  数据面', '批量 IQ 传输 · 端点 01 / 81', VIOLET_T, VIOLET_S, VIOLET_D)

# 5. link
y = 606; layer(y - 2, 30, '链路')
s.rect(X0, y, W, 28, '#2f2e2b', None, 14)
s.text(X0 + W / 2, y + 19, 'USB 3.0 SuperSpeed  ·  5 Gbit/s  ·  VID 1A86 / PID 5537', 13, '#ffffff', 600, 'middle')

# 6. hardware
y = 650; h = 196; layer(y, h, '硬件')
s.rect(X0, y, W, h, '#f7f6f3', NEUTRAL_S, 10)
s.text(X0 + 14, y + 22, 'OmniCoreSDR 板卡', 13, INK, 700)
by = y + 38
s.box(X0 + 14, by, 140, 92, 'CH32H417', ['USB 3.0 桥', '诊断 / 清理协议 v4'], '#ffffff', NEUTRAL_S)
zx = X0 + 198; zw = 350
s.rect(zx, by, zw, 92, '#ffffff', NEUTRAL_S, 8)
s.text(zx + 12, by + 20, 'Zynq', 13, INK, 700)
s.box(zx + 12, by + 30, (zw - 36) / 2, 54, 'PL 数据通路', ['IQ 打包 · 16 KiB 起始头', 'PPS 测频 · 板控 AXI'], NEUTRAL_T, NEUTRAL_S, INK, INK2, 12, 10.5)
s.box(zx + 24 + (zw - 36) / 2, by + 30, (zw - 36) / 2, 54, 'PS · SDK', ['命令解析 · 射频驱动', 'FIR / 时钟事务'], NEUTRAL_T, NEUTRAL_S, INK, INK2, 12, 10.5)
ax = zx + zw + 44
s.box(ax, by, X0 + W - 14 - ax, 92, 'AD9361', ['2R2T 射频收发器', '70 MHz – 6 GHz', '≤ 61.44 MSPS / 通道'], '#ffffff', NEUTRAL_S)
def both(x1, x2, yy, label):
    s.line(x1 + 6, yy, x2 - 6, yy, INK2, 1.5, arrow='a-ink'); s.line(x2 - 6, yy, x1 + 6, yy, INK2, 1.5, arrow='a-ink')
    s.text((x1 + x2) / 2, yy - 8, label, 10, MUTED, 600, 'middle')
both(X0 + 154, zx, by + 52, '')
both(zx + zw, ax, by + 52, 'LVDS')
cy = by + 104; chips = ['ADF4002 + 40 MHz VCXO', 'HD3SS3220 USB-C', 'RGB 指示灯 · 风扇 PWM', '外部 10 MHz · PPS · LO 输入']
cw = (W - 28 - 3 * 8) / 4
for i, c in enumerate(chips):
    s.rect(X0 + 14 + i * (cw + 8), cy, cw, 30, NEUTRAL_T, NEUTRAL_S, 15)
    s.text(X0 + 14 + i * (cw + 8) + cw / 2, cy + 20, c, 12, INK2, 500, 'middle')

open('architecture.svg', 'w').write(s.render())
