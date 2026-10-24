from svgkit import *

s = Svg(1000, 600, 'USB IQ 格式', 'packed12 与 signed8 的字节布局、应用格式与 USB 请求对齐')
s.text(28, 42, 'USB IQ 格式与应用格式', 20, INK, 700)
s.text(28, 66, '双通道按 I1, Q1, I2, Q2 交织；12bit 为默认 wire 格式，8bit 仅用于 2RX + 2TX', 13, INK2)

def byte(x, y, w, label, fill, stroke, color, sub=None):
    s.rect(x, y, w, 40, fill, stroke, 4)
    s.text(x + w / 2, y + (18 if sub else 25), label, 12, color, 700, 'middle', mono=True)
    if sub: s.text(x + w / 2, y + 33, sub, 10, INK2, 400, 'middle')

# --- packed12 -------------------------------------------------------------
y = 96
s.text(28, y + 16, 'packed12', 15, INK, 700); s.text(28, y + 34, '12bit wire · 默认', 11, MUTED)
s.text(28, y + 50, '3 B / 通道 / 样本', 11, MUTED)
bx = 170; bw = 132
s.text(bx, y - 6, '一个复样本（3 字节）', 11, INK2, 600)
byte(bx, y, bw, 'I[11:4]', BLUE_T, BLUE_S, BLUE_D, 'byte 0')
s.rect(bx + bw + 4, y, bw / 2, 40, BLUE_T, BLUE_S, 4); s.text(bx + bw + 4 + bw / 4, y + 18, 'I[3:0]', 12, BLUE_D, 700, 'middle', mono=True)
s.rect(bx + bw + 4 + bw / 2, y, bw / 2, 40, ORANGE_T, ORANGE_S, 4); s.text(bx + bw + 4 + 3 * bw / 4, y + 18, 'Q[11:8]', 12, ORANGE_D, 700, 'middle', mono=True)
s.rect(bx + bw + 4 + bw / 2 - 24, y + 23, 48, 14, '#ffffff', None, 3)
s.text(bx + bw + 4 + bw / 2, y + 33, 'byte 1', 10, INK2, 400, 'middle')
byte(bx + 2 * bw + 8, y, bw, 'Q[7:0]', ORANGE_T, ORANGE_S, ORANGE_D, 'byte 2')
# dual frame
fx = 604
s.text(fx, y - 6, '双通道帧（6 字节）', 11, INK2, 600)
for i, (t, f, st, c) in enumerate([('CH1  I1 Q1', BLUE_T, BLUE_S, BLUE_D), ('CH2  I2 Q2', VIOLET_T, VIOLET_S, VIOLET_D)]):
    s.rect(fx + i * 184, y, 180, 40, f, st, 4)
    s.text(fx + i * 184 + 90, y + 18, t, 12, c, 700, 'middle', mono=True)
    s.text(fx + i * 184 + 90, y + 33, '3 B packed12', 10, INK2, 400, 'middle')

# --- signed8 --------------------------------------------------------------
y = 196
s.text(28, y + 16, 'signed8', 15, INK, 700); s.text(28, y + 34, '8bit wire · 可选', 11, MUTED)
s.text(28, y + 50, '2 B / 通道 / 样本', 11, MUTED)
s.text(bx, y - 6, '双通道帧（4 字节，每字节为 12bit 值的高 8 位，二补码）', 11, INK2, 600)
for i, (t, f, st, c) in enumerate([('I1[11:4]', BLUE_T, BLUE_S, BLUE_D), ('Q1[11:4]', BLUE_T, BLUE_S, BLUE_D),
                                   ('I2[11:4]', VIOLET_T, VIOLET_S, VIOLET_D), ('Q2[11:4]', VIOLET_T, VIOLET_S, VIOLET_D)]):
    byte(bx + i * 112, y, 108, t, f, st, c, f'byte {i}')
s.rect(fx + 20, y, 348, 40, '#ffffff', NEUTRAL_S, 6)
s.text(fx + 36, y + 17, '要求 RX / TX 通道均为 1,2 且 TX 源为 USB', 11, INK2, 500)
s.text(fx + 36, y + 33, 'FPGA 发射时为每个 12bit 值补 4 位零', 11, INK2, 500)

# --- application formats ---------------------------------------------------
y = 290
s.line(28, y - 18, 972, y - 18, GRID, 1)
s.text(28, y + 4, '应用 / 文件格式', 15, INK, 700)
s.text(28, y + 22, 'rx / tx --format，API och_stream_args.format', 11, MUTED)
cols = [('格式', 170), ('每通道每样本', 290), ('数值', 410), ('适用', 760)]
for t, x in cols: s.text(x, y + 4, t, 12, INK2, 700)
rows = [('packed12', '3 B', '与 12bit wire 相同，不转换', 'bench 默认；rx 租借零拷贝'),
        ('sc16', '4 B', 'int16 I / Q，12bit 左对齐（低 4 位为 0）', 'rx / tx 默认'),
        ('fc32', '8 B', 'float32 I / Q，归一化到 ±1', '信号处理、MATLAB'),
        ('sc8', '2 B', 'int8 I / Q', '仅 --iq-bits 8')]
for i, (a, b, c, d) in enumerate(rows):
    yy = y + 32 + i * 30
    s.rect(160, yy - 4, 812, 28, '#ffffff' if i % 2 == 0 else NEUTRAL_T, None, 4)
    s.text(170, yy + 15, a, 13, INK, 700, mono=True); s.text(290, yy + 15, b, 13, INK)
    s.text(410, yy + 15, c, 12, INK2); s.text(760, yy + 15, d, 12, INK2)

# --- transfer alignment ----------------------------------------------------
y = 484
s.line(28, y - 18, 972, y - 18, GRID, 1)
s.text(28, y + 4, 'USB 请求对齐', 15, INK, 700)
s.text(28, y + 22, '--block，默认 196608 B', 11, MUTED)
s.rect(170, y - 10, 802, 96, '#ffffff', NEUTRAL_S, 8)
s.text(186, y + 14, '49152 B = 3 × 16 KiB，同时整除：', 13, INK, 700)
chips = ['16 KiB MCU 缓冲', '6 B 12bit 双通道帧', '3 B 12bit 单通道帧', '4 B 8bit 双通道帧']
cx = 186
for c in chips:
    w = len(c.encode('gbk')) * 6.6 + 24
    s.rect(cx, y + 26, w, 26, BLUE_T, BLUE_S, 13); s.text(cx + w / 2, y + 44, c, 12, BLUE_D, 600, 'middle'); cx += w + 10
s.text(186, y + 76, '默认请求 196608 B = 4 × 49152 B；32 个请求并行，主机环形队列 64 MiB / 方向（双 RX 61.44 MSPS 约可缓冲 182 ms）', 12, INK2)

open('wire-format.svg', 'w').write(s.render())
