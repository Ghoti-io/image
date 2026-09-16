#!/usr/bin/env python3
"""Systematic JPEG corpus: the cross-product, not a hand-picked list."""
import os, sys, random
from PIL import Image
out = sys.argv[1]
random.seed(20260916)

def img(w, h, mode, kind):
    im = Image.new(mode, (w, h)); px = im.load()
    for y in range(h):
        for x in range(w):
            if kind == 'noise':
                v = lambda: random.randint(0, 255)
            elif kind == 'ramp':
                v = lambda: (x * 255) // max(1, w - 1)
            elif kind == 'edges':
                v = lambda: 255 if ((x // 3) + (y // 3)) % 2 else 0
            else:
                v = lambda: 128
            if mode == 'L':
                px[x, y] = v()
            else:
                px[x, y] = (v(), v(), v())
    return im

sizes = [(1,1), (1,17), (17,1), (8,8), (9,9), (16,16), (23,41), (64,64),
         (65,65), (127,3), (129,97), (256,64)]
subs = {'444': 0, '422': 1, '420': 2}
n = 0
for (w, h) in sizes:
    for mode in ('L', 'RGB'):
        for kind in ('noise', 'ramp', 'edges', 'flat'):
            for prog in (False, True):
                for sname, sval in (subs.items() if mode == 'RGB' else [('gray', 0)]):
                    for q in (10, 50, 90):
                        name = f"{mode.lower()}_{w}x{h}_{kind}_{'prog' if prog else 'base'}_{sname}_q{q}.jpg"
                        kw = dict(quality=q, progressive=prog)
                        if mode == 'RGB':
                            kw['subsampling'] = sval
                        try:
                            img(w, h, mode, kind).save(os.path.join(out, name), 'JPEG', **kw)
                            n += 1
                        except Exception as e:
                            print('skip', name, e)
# restart intervals, on a size that is not MCU aligned
for dri in (1, 2, 3, 5, 8, 17):
    for (w, h) in [(129, 97), (64, 64), (23, 41)]:
        for mode, sname, sval in (('L','gray',0), ('RGB','420',2)):
            name = f"{mode.lower()}_{w}x{h}_rst{dri}_{sname}.jpg"
            kw = dict(quality=75, restart_marker_blocks=dri)
            if mode == 'RGB':
                kw['subsampling'] = sval
            try:
                img(w, h, mode, 'noise').save(os.path.join(out, name), 'JPEG', **kw)
                n += 1
            except Exception as e:
                print('skip', name, e)
print("generated", n)
