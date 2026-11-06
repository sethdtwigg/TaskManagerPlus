"""Generates the TaskManagerPlus ToolStatus toolbar icon set (original artwork).

Flat monochrome stroke icons on a 24-unit grid, one SVG + one 512px PNG per
icon per UI theme. Writes plugins/ToolStatus/resources/tms_<name>_{onlight,ondark}.{svg,png}.
"onlight" = dark ink for a light UI, "ondark" = light ink for a dark UI.
Hover/pressed backgrounds are not baked in: the toolbar custom draw takes them
from the theme palette (phlib/theme.c, PhThemeWindowDrawToolbar).
Run:  python tools/icon/make-toolbar-icons.py   (needs Pillow)
"""
import math
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'plugins', 'ToolStatus', 'resources')
INKS = {'onlight': '#2F3E46', 'ondark': '#E4ECEF'}
ACCENT = {'onlight': '#005FB8', 'ondark': '#3A96DD'}   # blue accent, matches theme palettes
SW = 1.75          # stroke width in grid units
PX = 512           # PNG size
SS = 4             # supersampling

# Primitives: ('path', [(x,y),...], closed)  ('circle', cx, cy, r)  ('arc', cx, cy, r, a0, a1)
# ('rect', x, y, w, h, r)  ('dot', cx, cy, r)  -- add ',accent' as last item to use accent ink
def arc_pts(cx, cy, r, a0, a1, n=48):
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]

ICONS = {
    # circular refresh arrow
    'refresh': [('arc', 12, 12, 7.5, 20, 300), ('head', 12, 12, 7.5, 300)],
    # gear: ring of teeth + hub
    'options': [('gear', 12, 12), ('circle', 12, 12, 3)],
    # magnifier
    'find': [('circle', 10.5, 10.5, 6), ('path', [(15, 15), (20, 20)], False)],
    # activity chart in a frame
    'sysinfo': [('rect', 3.5, 4, 17, 16, 2.5), ('path', [(6.5, 15), (10, 11), (13, 13.5), (17.5, 8)], False)],
    # window with crosshair target
    'findwindow': [('rect', 3.5, 4, 17, 16, 2.5), ('circle', 12, 13, 3.5), ('path', [(12, 7.5), (12, 9.5)], False),
                   ('path', [(12, 16.5), (12, 18.5)], False), ('path', [(6.5, 13), (8.5, 13)], False),
                   ('path', [(15.5, 13), (17.5, 13)], False)],
    # window with forward arrow
    'findwindowthread': [('rect', 3.5, 4, 17, 16, 2.5), ('path', [(3.5, 8.5), (20.5, 8.5)], False),
                         ('path', [(8, 14.5), (16, 14.5)], False), ('path', [(13, 11.5), (16, 14.5), (13, 17.5)], False)],
    # close cross
    'kill': [('path', [(6, 6), (18, 18)], False), ('path', [(18, 6), (6, 18)], False)],
    # pin / always on top: window with bar on top
    'alwaysontop': [('rect', 3.5, 4, 17, 16, 2.5), ('path', [(3.5, 8), (20.5, 8)], False),
                    ('path', [(12, 17), (12, 11.5)], False), ('path', [(9.2, 14), (12, 11.2), (14.8, 14)], False)],
    # power symbol
    'power': [('arc', 12, 12.5, 7.5, -50, 230), ('path', [(12, 3.5), (12, 11)], False)],
}

def gear_paths(cx, cy):
    # 8 teeth, outer radius 9, inner 6.6, outline polygon
    pts = []
    n = 8
    for i in range(n):
        base = i * 360 / n
        for a, r in ((base - 11, 6.8), (base - 8, 9), (base + 8, 9), (base + 11, 6.8)):
            pts.append((cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))))
    return pts

def expand(elems):
    """Reduce every element to ('path', pts, closed) | ('circle', cx, cy, r) | ('rect', ...)."""
    out = []
    for e in elems:
        if e[0] == 'arc':
            out.append(('path', arc_pts(*e[1:]), False))
        elif e[0] == 'head':
            # chevron at the end of a clockwise arc, pointing along its tangent
            cx, cy, r, a = e[1:]
            ex, ey = cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))
            tx, ty = -math.sin(math.radians(a)), math.cos(math.radians(a))
            pts = []
            for da in (-40, 40):
                c, sn = math.cos(math.radians(da)), math.sin(math.radians(da))
                pts.append((ex - 4.2 * (tx * c - ty * sn), ey - 4.2 * (tx * sn + ty * c)))
            out.append(('path', [pts[0], (ex, ey), pts[1]], False))
        elif e[0] == 'gear':
            out.append(('path', gear_paths(e[1], e[2]), True))
        else:
            out.append(e)
    return out

def fmt(v):
    return ('%.2f' % v).rstrip('0').rstrip('.')

def svg_of(elems, ink):
    body = []
    for e in expand(elems):
        if e[0] == 'path':
            d = 'M' + ' L'.join(f'{fmt(x)} {fmt(y)}' for x, y in e[1]) + (' Z' if e[2] else '')
            body.append(f'  <path d="{d}"/>')
        elif e[0] == 'circle':
            body.append(f'  <circle cx="{fmt(e[1])}" cy="{fmt(e[2])}" r="{fmt(e[3])}"/>')
        elif e[0] == 'rect':
            body.append(f'  <rect x="{fmt(e[1])}" y="{fmt(e[2])}" width="{fmt(e[3])}" height="{fmt(e[4])}" rx="{fmt(e[5])}"/>')
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" '
            f'stroke="{ink}" stroke-width="{SW}" stroke-linecap="round" stroke-linejoin="round">\n'
            + '\n'.join(body) + '\n</svg>\n')

def hex2rgba(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) + (255,)

def render(elems, ink):
    s = PX * SS
    k = s / 24.0
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    col = hex2rgba(ink)
    w = SW * k
    r = w / 2

    def dot(x, y):
        d.ellipse((x * k - r, y * k - r, x * k + r, y * k + r), fill=col)

    def poly(pts, closed):
        P = [(x * k, y * k) for x, y in pts]
        if closed:
            P = P + [P[0]]
        d.line(P, fill=col, width=int(round(w)), joint='curve')
        for x, y in (pts if not closed else pts):
            dot(x, y)

    for e in expand(elems):
        if e[0] == 'path':
            poly(e[1], e[2])
        elif e[0] == 'circle':
            poly(arc_pts(e[1], e[2], e[3], 0, 360, 96), False)
        elif e[0] == 'rect':
            x, y, ww, hh, rr = e[1:]
            pts = []
            for cx, cy, a0 in ((x + ww - rr, y + rr, -90), (x + ww - rr, y + hh - rr, 0),
                               (x + rr, y + hh - rr, 90), (x + rr, y + rr, 180)):
                pts += arc_pts(cx, cy, rr, a0, a0 + 90, 12)
            poly(pts + [pts[0]], False)
    return im.resize((PX, PX), Image.LANCZOS)

def main():
    for name, elems in ICONS.items():
        for theme, ink in INKS.items():
            base = os.path.join(OUT, f'tms_{name}_{theme}')
            with open(base + '.svg', 'w', newline='\n') as f:
                f.write(svg_of(elems, ink))
            render(elems, ink).save(base + '.png')
    print('ok', len(ICONS) * len(INKS) * 2, 'files')

main()
