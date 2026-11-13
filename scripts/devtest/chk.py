#!/usr/bin/env python3
"""stdin'den [ui] dump okur; ekran dışına taşan nesneleri raporlar.
LCD128: 240x240 YUVARLAK ekran — dikdörtgen sınırın yanı sıra (--round ile)
nesne köşelerinin r=120 daire içinde kalıp kalmadığı da denetlenir."""
import sys, re
W, H = 240, 240
ycheck = '--y' in sys.argv
rcheck = '--round' in sys.argv
CX, CY, R = W / 2.0, H / 2.0, W / 2.0
n = 0; bad = 0
for l in sys.stdin:
    m = re.search(r'\[ui\]\s+(.*?)\[(-?\d+),(-?\d+) (\d+)x(\d+)\](.*)', l)
    if not m: continue
    n += 1
    x, y, w, h = map(int, m.groups()[1:5])
    if x + w > W or x < 0 or (ycheck and (y + h > H or y < 0)):
        bad += 1; print("OVERFLOW:", l.rstrip()); continue
    if rcheck and w > 0 and h > 0:
        # Dört köşeden en uzağı daire dışındaysa bezel arkasında kalır.
        far = max(((cx - CX) ** 2 + (cy - CY) ** 2) ** 0.5
                  for cx in (x, x + w - 1) for cy in (y, y + h - 1))
        if far > R + 0.5:
            bad += 1; print("ROUND-CLIP: r=%.1f" % far, l.rstrip())
print(f"checked {n} objs, {bad} overflow")
