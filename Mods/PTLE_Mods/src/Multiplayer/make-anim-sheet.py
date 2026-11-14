"""Turns the animation survey's frames into a page to browse : one looping GIF per
clip, numbered, with its clip id (run-autotest.ps1 with PTLE_MP_ANIMSURVEY=1 first).

    python make-anim-sheet.py

Reads  %LOCALAPPDATA%\\PTLE Multiplayer\\logs\\shots\\anim_<n>_f<k>.png (or .bmp) and the log.
Writes Documents\\PTLE Multiplayer\\anim-survey\\index.html and its GIFs.
"""
import html
import os
import re

from PIL import Image

DOCS = os.path.join(os.path.expanduser('~'), 'Documents', 'PTLE Multiplayer')
DATA = os.path.join(os.environ['LOCALAPPDATA'], 'PTLE Multiplayer')   # Where the mod keeps its files.
SHOTS = os.path.join(DATA, 'logs', 'shots')
LOG = os.path.join(DATA, 'logs', 'multiplayer.log')
OUT = os.path.join(DOCS, 'anim-survey')
FRAME_MS = 500   # The survey films a frame every 500 ms.
SCALE = 0.6

clips = {}
for line in open(LOG, encoding='utf-8', errors='replace'):
    m = re.search(r'ANIM (\d{3}) : clip (0x[0-9A-F]{8}) \((.*), layer (\d+)\)', line)
    if m:
        clips[int(m.group(1))] = (m.group(2), m.group(3), int(m.group(4)))

os.makedirs(OUT, exist_ok=True)
cards = []
for n in sorted(clips):
    frames = []
    for k in range(16):
        for ext in ('png', 'bmp'):
            path = os.path.join(SHOTS, 'anim_%03d_f%d.%s' % (n, k, ext))
            if os.path.exists(path):
                im = Image.open(path).convert('RGB')
                frames.append(im.resize((int(im.width * SCALE), int(im.height * SCALE))))
                break
    if not frames:
        continue
    name = 'anim_%03d.gif' % n
    frames[0].save(os.path.join(OUT, name), save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0)
    crc, owner, layer = clips[n]
    cards.append('<figure><img src="%s" loading="lazy"><figcaption><b>#%d</b> %s<br><small>%s, layer %d</small></figcaption></figure>'
                 % (name, n, crc, html.escape(owner), layer))

page = """<!doctype html><meta charset="utf-8"><title>Harry's animations</title>
<style>
body { font: 14px system-ui, sans-serif; margin: 16px; background: #1b1b1b; color: #eee; }
main { display: grid; grid-template-columns: repeat(auto-fill, minmax(330px, 1fr)); gap: 12px; }
figure { margin: 0; background: #2a2a2a; border-radius: 6px; overflow: hidden; }
img { display: block; width: 100%%; }
figcaption { padding: 6px 8px; }
small { color: #aaa; }
</style>
<h1>Harry's animations (%d)</h1>
<p>Every clip loaded on Harry and on Evil Harry, played on Evil Harry : 6 frames, half a second apart.
Pick emotes by their number.</p>
<main>%s</main>
""" % (len(cards), '\n'.join(cards))
open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(page)
print('%d clips -> %s' % (len(cards), os.path.join(OUT, 'index.html')))
