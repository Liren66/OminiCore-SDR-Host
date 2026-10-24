"""Tiny SVG helpers shared by the README figures."""
from xml.sax.saxutils import escape

SANS = "'Segoe UI','Microsoft YaHei','PingFang SC','Noto Sans CJK SC','Noto Sans SC',sans-serif"
MONO = "'Cascadia Mono',Consolas,'DejaVu Sans Mono',monospace"

# Light card palette (fixed so figures read the same on light and dark pages).
SURFACE = '#fcfcfb'; BORDER = '#e4e3df'; INK = '#0b0b0b'; INK2 = '#52514e'; MUTED = '#8a8984'; GRID = '#e9e8e4'
BLUE = '#2a78d6'; BLUE_T = '#eaf2fd'; BLUE_S = '#9ec5f4'; BLUE_D = '#184f95'
ORANGE = '#eb6834'; ORANGE_T = '#fdeee7'; ORANGE_S = '#f3b49a'; ORANGE_D = '#a8431b'
AQUA = '#1baf7a'; AQUA_T = '#e6f6f0'; AQUA_S = '#8fd6bb'; AQUA_D = '#0d6b49'
VIOLET = '#6250d6'; VIOLET_T = '#efedfb'; VIOLET_S = '#b9b1ef'; VIOLET_D = '#3f2fa3'
NEUTRAL_T = '#f3f2ef'; NEUTRAL_S = '#cfcdc6'
GOOD = '#0ca30c'; GOOD_T = '#e7f6e7'


class Svg:
    def __init__(self, w, h, title, desc=''):
        self.w, self.h = w, h
        self.parts = []
        self.title, self.desc = title, desc

    def add(self, s):
        self.parts.append(s)

    def rect(self, x, y, w, h, fill, stroke=None, r=8, sw=1, dash=None, opacity=None):
        a = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"'
        if stroke: a += f' stroke="{stroke}" stroke-width="{sw}"'
        if dash: a += f' stroke-dasharray="{dash}"'
        if opacity is not None: a += f' opacity="{opacity}"'
        self.add(a + '/>')

    def text(self, x, y, s, size=14, color=INK, weight=400, anchor='start', mono=False, italic=False):
        fam = MONO if mono else SANS
        st = ' font-style="italic"' if italic else ''
        self.add(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" font-weight="{weight}" '
                 f'fill="{color}" text-anchor="{anchor}"{st}>{escape(s)}</text>')

    def line(self, x1, y1, x2, y2, color=INK2, sw=1.5, dash=None, arrow=None):
        a = f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}" stroke-linecap="round"'
        if dash: a += f' stroke-dasharray="{dash}"'
        if arrow: a += f' marker-end="url(#{arrow})"'
        self.add(a + '/>')

    def path(self, d, color=INK2, sw=1.5, fill='none', dash=None, arrow=None):
        a = f'<path d="{d}" stroke="{color}" stroke-width="{sw}" fill="{fill}" stroke-linecap="round" stroke-linejoin="round"'
        if dash: a += f' stroke-dasharray="{dash}"'
        if arrow: a += f' marker-end="url(#{arrow})"'
        self.add(a + '/>')

    def box(self, x, y, w, h, title, sub=None, fill=NEUTRAL_T, stroke=NEUTRAL_S, tcolor=INK, scolor=INK2,
            tsize=14, ssize=12, r=8, mono_sub=False, align='middle'):
        self.rect(x, y, w, h, fill, stroke, r)
        lines = sub if isinstance(sub, list) else ([sub] if sub else [])
        total = tsize + sum(ssize + 5 for _ in lines)
        ty = y + (h - total) / 2 + tsize - 2
        tx = x + w / 2 if align == 'middle' else x + 14
        self.text(tx, ty, title, tsize, tcolor, 600, align)
        for i, l in enumerate(lines):
            self.text(tx, ty + (ssize + 5) * (i + 1), l, ssize, scolor, 400, align, mono=mono_sub)

    def badge(self, x, y, label, fill, color='#ffffff', size=11, w=None):
        w = w or (len(label) * size * 0.62 + 14)
        self.rect(x, y, w, size + 9, fill, None, 4)
        self.text(x + w / 2, y + size + 2.5, label, size, color, 600, 'middle')
        return w

    def render(self):
        markers = []
        for name, color in [('a-ink', INK2), ('a-blue', BLUE), ('a-orange', ORANGE), ('a-muted', MUTED),
                            ('a-aqua', AQUA), ('a-violet', VIOLET)]:
            markers.append(f'<marker id="{name}" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="7" markerHeight="7" '
                           f'orient="auto-start-reverse"><path d="M0,0.8 L9,5 L0,9.2 Z" fill="{color}"/></marker>')
        head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}" '
                f'role="img" aria-labelledby="t d">'
                f'<title id="t">{escape(self.title)}</title><desc id="d">{escape(self.desc)}</desc>'
                f'<defs>{"".join(markers)}</defs>'
                f'<rect x="0.5" y="0.5" width="{self.w-1}" height="{self.h-1}" rx="14" fill="{SURFACE}" stroke="{BORDER}"/>')
        return head + '\n'.join(self.parts) + '</svg>\n'
