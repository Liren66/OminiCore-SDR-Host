import html, re, sys, asyncio
from playwright.async_api import async_playwright
# Windows Terminal "Campbell" scheme
BASE = ['#0C0C0C','#C50F1F','#13A10E','#C19C00','#0037DA','#881798','#3A96DD','#CCCCCC']
BRIGHT = ['#767676','#E74856','#16C60C','#F9F1A5','#3B78FF','#B4009E','#61D6D6','#F2F2F2']
FG, BG = '#CCCCCC', '#0C0C0C'
def convert(text):
    out=[]; fg=None; bg=None; bold=False
    pos=0
    for m in re.finditer(r'\x1b\[([0-9;]*)m', text):
        seg=text[pos:m.start()]
        if seg: out.append(span(seg,fg,bg,bold))
        codes=[int(c) for c in m.group(1).split(';') if c] or [0]
        for c in codes:
            if c==0: fg=bg=None; bold=False
            elif c==1: bold=True
            elif 30<=c<=37: fg=BASE[c-30]
            elif 90<=c<=97: fg=BRIGHT[c-90]
            elif 40<=c<=47: bg=BASE[c-40]
            elif 100<=c<=107: bg=BRIGHT[c-100]
        pos=m.end()
    out.append(span(text[pos:],fg,bg,bold))
    return ''.join(out)
def span(s,fg,bg,bold):
    s=html.escape(s)
    st=[]
    if fg: st.append('color:'+fg)
    if bg: st.append('background:'+bg)
    if bold: st.append('font-weight:700')
    return f'<span style="{";".join(st)}">{s}</span>' if st else s
PROMPT=re.compile(r'^(PS [^\n]*?&gt;)(.*)$', re.M)
def page(body, title, cols):
    body=PROMPT.sub(lambda m: f'<span style="color:#CCCCCC">{m.group(1)}</span><span style="color:#F2F2F2">{m.group(2)}</span>', body)
    return f'''<!doctype html><html><head><meta charset="utf-8"><style>
body{{margin:0;background:transparent;}}
.win{{display:inline-block;margin:24px;border-radius:10px;overflow:hidden;border:1px solid #3a3a3a;background:{BG}}}
.bar{{height:38px;background:#1f1f1f;display:flex;align-items:flex-end;padding-left:10px;}}
.tab{{background:{BG};color:#e8e8e8;font:13px "Segoe UI","Inter",sans-serif;height:30px;display:flex;align-items:center;padding:0 14px;border-radius:8px 8px 0 0;gap:8px}}
.ico{{width:14px;height:14px;border-radius:3px;background:#2b5797;display:inline-block}}
.dots{{margin-left:auto;display:flex;gap:18px;padding:0 16px 10px;color:#9a9a9a;font:13px sans-serif}}
pre{{margin:0;padding:12px 16px 16px;color:{FG};font:15px/1.38 "Cascadia Mono","DejaVu Sans Mono",monospace;width:{cols}ch;white-space:pre}}
</style></head><body><div class="win"><div class="bar"><div class="tab"><span class="ico"></span>{html.escape(title)}</div><div class="dots"><span>&#8212;</span><span>&#9744;</span><span>&#10005;</span></div></div><pre>{body}</pre></div></body></html>'''
async def main(src, dst, title, cols, maxlines):
    text=open(src,encoding='utf-8').read()
    lines=text.split('\n')
    if maxlines and len(lines)>maxlines: text='\n'.join(lines[:maxlines])+'\n\n  ...'
    h=page(convert(text), title, cols)
    async with async_playwright() as p:
        b=await p.chromium.launch(); pg=await b.new_page(device_scale_factor=2, viewport={'width':1400,'height':4000})
        await pg.set_content(h); el=await pg.query_selector('.win')
        box=await el.bounding_box()
        await pg.screenshot(path=dst, clip={'x':box['x'],'y':box['y'],'width':box['width'],'height':box['height']}, omit_background=True)
        await b.close()
    from PIL import Image
    im=Image.open(dst).convert('RGBA'); q=im.quantize(colors=128, method=Image.Quantize.FASTOCTREE); q.save(dst, optimize=True)
src,dst,title=sys.argv[1:4]; cols=int(sys.argv[4]) if len(sys.argv)>4 else 80; maxl=int(sys.argv[5]) if len(sys.argv)>5 else 0
asyncio.run(main(src,dst,title,cols,maxl))
