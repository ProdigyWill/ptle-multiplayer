"""Starts %LOCALAPPDATA%\\PTLE Multiplayer\\anim-notes.txt (the in-game animation viewer's
list, F10) from the animation survey's log, so its numbers match the survey's. A
few clips that looked like emotes in the survey are starred, with a guess ending
in "?". Won't overwrite an existing file unless given --force.

    python make-anim-notes.py [--force]
"""
import os
import re
import sys

DOCS = os.path.join(os.environ['LOCALAPPDATA'], 'PTLE Multiplayer')   # Where the mod keeps its files.
LOG = os.path.join(DOCS, 'logs', 'multiplayer.log')
OUT = os.path.join(DOCS, 'anim-notes.txt')

GUESSES = {
    32: 'arms raised overhead?', 33: 'arms raised overhead?', 34: 'arms raised overhead?',
    36: 'tumble?', 37: 'tumble?',
    64: 'hands up, waving?', 73: 'waving arms?', 82: 'flip or handstand?', 83: 'flip or handstand?',
    89: 'wave?', 90: 'shrug, arms out?', 96: 'arms up?', 99: 'arms out wide?', 100: 'arms out wide?',
    123: 'arms up?', 134: 'T-pose?', 153: 'kick?', 167: 'crouched, sneaking?', 172: 'crouched, sneaking?',
    188: 'cheer jump?', 196: 'lifts something overhead?', 198: 'T-pose?', 206: 'holds something overhead?',
    207: 'holds something overhead?', 220: 'arms up?', 221: 'T-pose?', 226: 'cheer jump?', 237: 'hands up?',
}

if os.path.exists(OUT) and '--force' not in sys.argv:
    sys.exit('%s exists (use --force to replace it).' % OUT)

clips = {}
for line in open(LOG, encoding='utf-8', errors='replace'):
    m = re.search(r'ANIM (\d{3}) : clip (0x[0-9A-F]{8}) \((.*), layer (\d+)\)', line)
    if m:
        clips[int(m.group(1))] = (m.group(2), m.group(3).replace(' ', ''), int(m.group(4)))
if not clips:
    sys.exit('No survey in %s (run-autotest.ps1 with PTLE_MP_ANIMSURVEY=1 first).' % LOG)

with open(OUT, 'w', encoding='utf-8', newline='\r\n') as f:
    f.write("# Harry's animations : number, clip id, where it was found (layer), * if starred, then notes.\n")
    f.write("# Edit freely, even while the game runs (it reloads). In the game : F10 opens the viewer,\n")
    f.write("# [ ] previous / next (Shift : by 10), \\ replays, F11 stars, ' shows only starred ones.\n")
    f.write("# Other lines starting with # are not kept.\n")
    for n in sorted(clips):
        crc, owner, layer = clips[n]
        guess = GUESSES.get(n)
        f.write('%03d %s %-14s %s\n' % (n, crc, '%s/%d' % (owner, layer), ('* ' + guess) if guess else ''))
print('%d clips, %d starred -> %s' % (len(clips), len(GUESSES), OUT))
