"""Compare our decode against libjpeg at the frame's own sample precision.

Works for any frame this codec reads - baseline, progressive, arithmetic,
lossless - at 8-, 12- or 16-bit, because the oracle side is ljdec.c rather than
djpeg.  Two reasons djpeg is not enough: it does not expose do_block_smoothing,
which libjpeg applies to progressive frames and we deliberately do not
implement, and its output format hides the frame's precision.

Build the oracle first (see README.md for the libjpeg-turbo build):

  cc -O2 -o ljdec ljdec.py-adjacent ljdec.c -I <ljt-src> -I <ljt-build> <ljt-build>/libjpeg.a

then:

  python3 compare_to_libjpeg_native.py *.jpg

Exits quietly with a count; lists the first fifteen files that differ.
"""
import subprocess, struct, sys, os
LJ = os.environ.get("GIMG_LJDEC",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "ljdec"))
DUMP = os.environ.get("GIMG_DUMP",
    "build/linux/release/apps/dump_jpeg_raster")

def widen(v, frm, to):
    if frm >= to: return v >> (frm - to)
    r = v; have = frm
    while have < to:
        take = min(to - have, frm)
        r = (r << take) | (v >> (frm - take))
        have += take
    return r

TMP = os.environ.get("GIMG_TMP", "/tmp/gimg_oracle.bin")
ok = 0; fails = []
for p in sys.argv[1:]:
    if subprocess.run([LJ, p, TMP], capture_output=True).returncode != 0:
        fails.append((os.path.basename(p), "oracle-fail")); continue
    # dump_jpeg_raster defaults to the box filter; libjpeg defaults to the
    # triangle one.  Comparing the two defaults reports differences of a few
    # counts on every subsampled file and says nothing about the codec - a trap
    # worth spelling out, because it looks exactly like a real defect.
    env = dict(os.environ)
    env["GIMG_JPEG_FANCY_UPSAMPLE"] = "1"
    m = subprocess.run([DUMP, p], capture_output=True, timeout=120, env=env)
    if m.returncode != 0:
        fails.append((os.path.basename(p), "ours-fail: " + m.stderr.decode().strip()[:40])); continue
    r = open(TMP, "rb").read()
    w,h,n,prec = struct.unpack("<IIII", r[:16])
    ref = struct.unpack("<%dH" % (w*h*n), r[16:16+w*h*n*2])
    d = m.stdout; ow,oh = struct.unpack("<II", d[:8]); px = d[8:]
    if (ow,oh) != (w,h):
        fails.append((os.path.basename(p), "dim %dx%d vs %dx%d" % (ow,oh,w,h))); continue
    wide = len(px) == ow*oh*8
    out_bits = 16 if wide else 8
    c = 0; mx = 0; first = None
    for q in range(w*h):
        for k in range(n):
            a = widen(ref[q*n+k], prec, out_bits)
            b = struct.unpack_from("<H", px, (q*4+k)*2)[0] if wide else px[q*4+k]
            if a != b:
                c += 1; mx = max(mx, abs(a-b))
                if first is None: first = (q%w, q//w, k, a, b)
    if c: fails.append((os.path.basename(p), "ndiff=%d max=%d %s" % (c,mx,first)))
    else: ok += 1
print("%d files: %d byte-exact, %d not" % (len(sys.argv)-1, ok, len(fails)))
for nm, why in fails[:15]: print("  %-34s %s" % (nm, why))
