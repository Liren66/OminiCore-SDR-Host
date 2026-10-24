from svgkit import *

s = Svg(1000, 520, 'RX 起始头、溢出恢复与 epoch', 'USB 字节流中的 16 KiB 起始头、溢出后的重同步，以及采集文件与 metadata 记录的对应关系')
s.text(28, 42, 'RX 数据边界：起始头、溢出恢复与 epoch', 20, INK, 700)
s.text(28, 66, 'FPGA 在每次 RX 启动或溢出恢复时发送 16 KiB 递增起始头；OCH 剥离起始头，并以新 epoch 标记数据间断', 13, INK2)

X0, X1 = 150, 972
def label(y, t, sub=None):
    s.text(28, y, t, 13, INK, 700)
    if sub: s.text(28, y + 17, sub, 11, MUTED)

# Row 1: USB byte stream
y = 108; h = 46
label(y + 20, 'USB 字节流', '设备 → 主机')
segs = [('hdr', 140, 210), ('iq1', 210, 560), ('gap', 560, 610), ('hdr', 610, 680), ('iq2', 680, X1)]
for kind, a, b in segs:
    if kind == 'hdr':
        s.rect(a, y, b - a, h, '#2f2e2b', None, 6)
        s.text((a + b) / 2, y + 19, '起始头', 12, '#ffffff', 600, 'middle')
        s.text((a + b) / 2, y + 35, '16 KiB', 11, '#cfcdc6', 400, 'middle')
    elif kind == 'gap':
        s.add(f'<pattern id="hatch" width="8" height="8" patternTransform="rotate(45)" patternUnits="userSpaceOnUse">'
              f'<rect width="8" height="8" fill="#fdeee7"/><line x1="0" y1="0" x2="0" y2="8" stroke="{ORANGE}" stroke-width="2"/></pattern>')
        s.rect(a, y, b - a, h, 'url(#hatch)', ORANGE_S, 6)
    else:
        s.rect(a, y, b - a, h, BLUE_T, BLUE_S, 6)
        s.text((a + b) / 2, y + 28, 'IQ 样本  I1 Q1 I2 Q2 …', 13, BLUE_D, 600, 'middle')
s.text(147, y - 10, '00 01 02 … FF  × 64', 11, INK2, 400, 'start', mono=True)
s.text(585, y - 10, '溢出 · 丢样数未知', 11, ORANGE_D, 600, 'middle')

# Row 2: events
y = 210
label(y + 16, '事件', 'stderr / och_poll_event')
evs = [(175, 'RX SYNC', 'epoch=1', BLUE), (585, 'DEVICE', 'rx_overflow · 等待重同步', ORANGE), (645, 'RX SYNC', 'epoch=2', BLUE)]
for x, t, sub, c in evs:
    s.line(x, 156, x, y - 4, c, 1.2, '3 3')
for x, t, sub, c in [(175, 'RX SYNC', 'epoch=1', BLUE)]:
    s.add(f'<circle cx="{x}" cy="{y + 8}" r="5" fill="{c}" stroke="{SURFACE}" stroke-width="2"/>')
    s.text(x + 12, y + 12, f'{t}  {sub}', 12, INK2, 600)
s.add(f'<circle cx="585" cy="{y + 8}" r="5" fill="{ORANGE}" stroke="{SURFACE}" stroke-width="2"/>')
s.text(573, y + 12, 'DEVICE  rx_overflow', 12, INK2, 600, 'end')
s.add(f'<circle cx="645" cy="{y + 8}" r="5" fill="{BLUE}" stroke="{SURFACE}" stroke-width="2"/>')
s.text(657, y + 12, 'RX SYNC  epoch=2  ·  DEVICE  rx_restarted', 12, INK2, 600)

# Row 3: application / file view
y = 262; h = 46
label(y + 20, '应用 / 采集文件', '起始头已剥离')
s.rect(X0, y, 400, h, BLUE_T, BLUE_S, 6)
s.text(X0 + 200, y + 20, 'epoch 1', 13, BLUE_D, 700, 'middle')
s.text(X0 + 200, y + 36, 'flags: START | TIME_UNKNOWN', 11, INK2, 400, 'middle', mono=True)
s.rect(X0 + 404, y, X1 - X0 - 404, h, BLUE_T, BLUE_S, 6)
s.text(X0 + 404 + (X1 - X0 - 404) / 2, y + 20, 'epoch 2', 13, BLUE_D, 700, 'middle')
s.text(X0 + 404 + (X1 - X0 - 404) / 2, y + 36, 'START | DISCONTINUITY | DEVICE_OVERFLOW | TIME_UNKNOWN', 11, INK2, 400, 'middle', mono=True)
s.path(f'M{X0 + 402},{y - 8} L{X0 + 402},{y + h + 8}', ORANGE, 2.5)
s.text(X0 + 402, y + h + 22, '间断：两侧不可拼接为连续采样', 11, ORANGE_D, 600, 'middle')
# Row 4: metadata sidecar
y = 360
label(y + 20, 'metadata.jsonl', '与 IQ 文件同名')
s.rect(X0, y, X1 - X0, 134, '#ffffff', NEUTRAL_S, 8)
rows = [('{"type":"capture", "format":"sc16", "config":{…}, "hardware_timestamps":false}', INK2),
        ('{"type":"boundary", "file_byte":0, "epoch":1, "flags":17, "device_dropped_samples":0, …}', BLUE_D),
        ('{"type":"boundary", "file_byte":…, "epoch":2, "flags":27, "device_dropped_samples":-1, …}', BLUE_D),
        ('{"type":"end", "stats":{"samples_per_channel":…, "resyncs":1, "host_dropped_samples":0, …}}', INK2)]
for i, (t, c) in enumerate(rows):
    s.text(X0 + 16, y + 28 + i * 28, t, 12, c, 500, mono=True)
s.text(X1 - 14, y + 28 + 2 * 28, '← 第二段在文件中的起点', 11, ORANGE_D, 600, 'end')

open('rx-boundaries.svg', 'w').write(s.render())
