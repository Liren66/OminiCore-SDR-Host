from svgkit import *

s = Svg(1000, 500, '运行中改参', '设备协调器在不销毁流句柄的情况下暂停、改参并恢复 RX/TX')
s.text(28, 42, '运行中改参：暂停 → 应用 → 恢复', 20, INK, 700)
s.text(28, 66, '管理线程调用 configure / set_fir / set_sample_rate 等接口；流句柄和已分配缓冲保持不变', 13, INK2)

X0, X1 = 150, 972
steps = [('1', '校验请求', '同参数直接返回'), ('2', '暂停', '唤醒阻塞的读写'), ('3', '清理会话', '取消旧 USB 请求'),
         ('4', '写入参数', '只下发变化项'), ('5', '恢复', 'RX 先启动，TX 后启动')]
cw = (X1 - X0) / 5
for i, (n, t, sub) in enumerate(steps):
    x = X0 + i * cw
    pts = f'{x},{96} {x + cw - 12},{96} {x + cw},{122} {x + cw - 12},{148} {x},{148} {x + 12},{122}' if i else \
          f'{x},{96} {x + cw - 12},{96} {x + cw},{122} {x + cw - 12},{148} {x},{148}'
    s.add(f'<polygon points="{pts}" fill="{BLUE_T if i != 3 else VIOLET_T}" stroke="{BLUE_S if i != 3 else VIOLET_S}"/>')
    s.text(x + cw / 2 + 4, 118, f'{n}  {t}', 13, BLUE_D if i != 3 else VIOLET_D, 700, 'middle')
    s.text(x + cw / 2 + 4, 136, sub, 11, INK2, 400, 'middle')
s.text(28, 126, '管理线程', 13, INK, 700)

def lane_label(y, t, sub):
    s.text(28, y + 20, t, 13, INK, 700); s.text(28, y + 37, sub, 11, MUTED)

hatch = (f'<pattern id="h2" width="8" height="8" patternTransform="rotate(45)" patternUnits="userSpaceOnUse">'
         f'<rect width="8" height="8" fill="#f3f2ef"/><line x1="0" y1="0" x2="0" y2="8" stroke="{MUTED}" stroke-width="1.5"/></pattern>')
s.add(hatch)
pa, pb = X0 + cw * 1 + 6, X0 + cw * 4 - 10   # pause window

# RX lane
y = 184
lane_label(y, 'RX 流', '同一句柄')
s.rect(X0, y, pa - X0 - 4, 46, BLUE_T, BLUE_S, 6)
s.text((X0 + pa) / 2, y + 20, 'epoch N', 13, BLUE_D, 700, 'middle'); s.text((X0 + pa) / 2, y + 36, '正常收样', 11, INK2, 400, 'middle')
s.rect(pa, y, pb - pa, 46, 'url(#h2)', NEUTRAL_S, 6)
s.rect(pa + 75, y + 12, 250, 22, SURFACE, None, 4)
s.text(pa + 200, y + 28, '暂停窗口：旧队列丢弃，丢样属预期', 12, INK2, 600, 'middle')
s.rect(pb + 4, y, X1 - pb - 4, 46, BLUE_T, BLUE_S, 6)
s.text((pb + X1) / 2 + 2, y + 20, 'epoch N+1', 13, BLUE_D, 700, 'middle')
s.text((pb + X1) / 2 + 2, y + 36, 'RECONFIGURED', 11, INK2, 400, 'middle', mono=True)

# TX lane
y = 252
lane_label(y, 'TX 流', '同一句柄')
s.rect(X0, y, pa - X0 - 4, 46, ORANGE_T, ORANGE_S, 6)
s.text((X0 + pa) / 2, y + 20, '应用数据', 13, ORANGE_D, 700, 'middle'); s.text((X0 + pa) / 2, y + 36, '正常发射', 11, INK2, 400, 'middle')
s.rect(pa, y, pb - pa, 46, 'url(#h2)', NEUTRAL_S, 6)
s.rect(pa + 75, y + 12, 250, 22, SURFACE, None, 4)
s.text(pa + 200, y + 28, '暂停窗口：旧排队数据丢弃', 12, INK2, 600, 'middle')
zw = 86
s.rect(pb + 4, y, zw, 46, '#ffffff', ORANGE_S, 6)
s.text(pb + 4 + zw / 2, y + 20, '零 IQ × 4 块', 12, ORANGE_D, 700, 'middle'); s.text(pb + 4 + zw / 2, y + 36, 'tx_padding', 11, INK2, 400, 'middle', mono=True)
s.rect(pb + 8 + zw, y, X1 - pb - 8 - zw, 46, ORANGE_T, ORANGE_S, 6)
s.text((pb + 8 + zw + X1) / 2, y + 28, '新数据', 13, ORANGE_D, 700, 'middle')

# application threads
y = 320
lane_label(y, '应用 IO 线程', 'recv / send')
s.rect(X0, y, X1 - X0, 46, '#ffffff', NEUTRAL_S, 6)
s.text(X0 + 16, y + 20, '暂停期间 recv / acquire / send 以 OCH_TIMEOUT 返回，重试即可；已部分接受的 send 返回 OK', 12, INK, 500)
s.text(X0 + 16, y + 37, '持有 RX 租借时改参返回 OCH_BUSY，硬件保持不变；有流时不能改变通道 mask 或 USB IQ 位数', 12, INK2, 400)

# failure path
y = 392
s.rect(X0, y, X1 - X0, 82, '#fff7f4', ORANGE_S, 8)
s.text(X0 + 16, y + 24, '失败处理', 13, ORANGE_D, 700)
s.text(X0 + 16, y + 46, '任一步骤失败：停止 RX 与 TX，配置缓存失效。应用保存错误、结束 IO 线程并销毁旧流，', 12, INK, 400)
s.text(X0 + 16, y + 66, '确认设备状态后重新 configure、创建并启动流。核心不会自动回滚，也不会自行启动发射。', 12, INK, 400)
s.text(28, y + 46, '异常', 13, INK, 700)
for x in (pa, pb):
    s.line(x, 150, x, 302, MUTED, 1, '3 3')

open('reconfigure.svg', 'w').write(s.render())
