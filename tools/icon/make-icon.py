"""Generates the TaskManagerPlus application icon (original artwork).

Writes SystemInformer/resources/taskmanagerplus.svg and SystemInformer/SystemInformer.ico.
Run:  python tools/icon/make-icon.py   (needs Pillow)
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEAL = (30, 112, 196, 255)
WHITE = (255, 255, 255, 255)
# 256-unit design grid: (x, y, w, h)
BARS = [(48, 148, 40, 60), (108, 112, 40, 96), (168, 76, 40, 132)]
RADIUS = 56

SVG = f'''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" rx="{RADIUS}" fill="#1E70C4"/>
{chr(10).join(f'  <rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="#FFFFFF"/>' for x, y, w, h in BARS)}
  <rect x="48" y="30" width="96" height="12" rx="6" fill="#FFFFFF" fill-opacity="0.55"/>
</svg>
'''

def render(size, ss=4):
    s = size * ss
    k = s / 256.0
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((0, 0, s - 1, s - 1), radius=RADIUS * k, fill=TEAL)
    for x, y, w, h in BARS:
        d.rounded_rectangle((x * k, y * k, (x + w) * k, (y + h) * k), radius=8 * k, fill=WHITE)
    d.rounded_rectangle((48 * k, 30 * k, 144 * k, 42 * k), radius=6 * k, fill=(255, 255, 255, 140))
    return im.resize((size, size), Image.LANCZOS)

sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
frames = [render(n) for n in sizes]
open(os.path.join(ROOT, 'SystemInformer', 'resources', 'taskmanagerplus.svg'), 'w', newline='\n').write(SVG)
frames[-1].save(os.path.join(ROOT, 'SystemInformer', 'SystemInformer.ico'), format='ICO',
                sizes=[(n, n) for n in sizes], append_images=frames[:-1])
print('ok')
