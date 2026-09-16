#!/usr/bin/env python3
"""Decode every file with our decoder and with Pillow; report any mismatch."""
import sys, os, subprocess, struct
from PIL import Image
TOOL = "build/linux/release/apps/dump_jpeg_raster"
env = dict(os.environ); env["GIMG_JPEG_FANCY_UPSAMPLE"] = "1"
files = sorted(sys.argv[1:])
fail_decode = []; mismatch = []; exact = 0
for p in files:
    r = subprocess.run([TOOL, p], capture_output=True, env=env)
    if r.returncode != 0:
        fail_decode.append((os.path.basename(p), r.stderr.decode().strip()[:60])); continue
    d = r.stdout
    w, h = struct.unpack("<II", d[:8]); px = d[8:]
    im = Image.open(p); im.load()
    if im.size != (w, h):
        mismatch.append((os.path.basename(p), f"size {w}x{h} vs {im.size}", 0)); continue
    ref = im.convert("RGB").tobytes()
    mx = 0; n = 0
    for i in range(w*h):
        for c in range(3):
            dd = abs(px[i*4+c] - ref[i*3+c])
            if dd:
                n += 1
                if dd > mx: mx = dd
    if n == 0: exact += 1
    else: mismatch.append((os.path.basename(p), f"ndiff={n} max={mx}", mx))
print(f"{len(files)} files: {exact} byte-exact, {len(mismatch)} differing, {len(fail_decode)} failed to decode")
for nm, why in fail_decode[:15]: print(f"  DECODE FAIL {nm}: {why}")
for nm, why, mx in sorted(mismatch, key=lambda t: -t[2])[:15]: print(f"  DIFF {nm}: {why}")
