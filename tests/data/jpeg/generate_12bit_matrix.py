#!/usr/bin/env python3
"""Generate a 12-bit JPEG corpus with a libjpeg built for 12-bit support.

  GIMG_CJPEG12=/path/to/cjpeg python3 generate_12bit_matrix.py <outdir>

Writes PNM sources and then encodes each across the cross-product of quality,
baseline/progressive and chroma subsampling.  Sizes deliberately include 1xN,
Nx1 and non-MCU-aligned shapes: the single-row cases are where chroma
upsampling has to pick its filter from the sampling factors rather than from the
shape of the decoded plane, and that distinction is invisible at any other size.

Compare the result with compare_12bit_to_libjpeg.py.
"""
import math, os, random, struct, subprocess, sys

CJPEG = os.environ.get("GIMG_CJPEG12", "cjpeg")


def write_pnm(path, w, h, vals, colour):
    magic = b"P6" if colour else b"P5"
    with open(path, "wb") as f:
        f.write(b"%s\n%d %d\n4095\n" % (magic, w, h))
        f.write(struct.pack(">%dH" % len(vals), *vals))


def sources(outdir):
    random.seed(7)
    made = []
    sizes = [(8, 8), (16, 16), (17, 9), (9, 17), (1, 1), (1, 16), (16, 1),
             (33, 33), (64, 64), (65, 31)]
    for (w, h) in sizes:
        g, c = [], []
        for y in range(h):
            for x in range(w):
                g.append((x * 4095) // (w - 1) if w > 1 else 2048)
                c.append(random.randrange(4096))
                c.append((y * 4095) // (h - 1) if h > 1 else 1000)
                c.append(((x + y) * 4095) // (w + h - 2) if w + h > 2 else 3000)
        p = os.path.join(outdir, "g_%dx%d.pgm" % (w, h))
        write_pnm(p, w, h, g, False); made.append(p)
        p = os.path.join(outdir, "c_%dx%d.ppm" % (w, h))
        write_pnm(p, w, h, c, True); made.append(p)
    w, h = 128, 96
    g = [int(2047 + 2000 * math.sin(x / 9.0) * math.cos(y / 7.0))
         for y in range(h) for x in range(w)]
    p = os.path.join(outdir, "g_smooth.pgm")
    write_pnm(p, w, h, g, False); made.append(p)
    c = []
    for y in range(h):
        for x in range(w):
            c += [int(2047 + 2000 * math.sin(x / 5.0)),
                  int(2047 + 2000 * math.cos(y / 6.0)),
                  random.randrange(4096)]
    p = os.path.join(outdir, "c_mixed.ppm")
    write_pnm(p, w, h, c, True); made.append(p)
    return made


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for src in sources(outdir):
        base = os.path.splitext(os.path.basename(src))[0]
        colour = src.endswith(".ppm")
        for q in (25, 75, 95, 100):
            for mode, tag in (([], "base"), (["-progressive"], "prog")):
                for samp in (["1x1", "2x1", "2x2"] if colour else [None]):
                    name = "%s_q%d_%s%s.jpg" % (
                        base, q, tag, "_" + samp if samp else "")
                    cmd = [CJPEG, "-precision", "12", "-quality", str(q)] + mode
                    if samp:
                        cmd += ["-sample", samp]
                    cmd += ["-outfile", os.path.join(outdir, name), src]
                    if subprocess.run(cmd, capture_output=True).returncode == 0:
                        n += 1
                    else:
                        print("encode failed:", name, file=sys.stderr)
    print("%d files in %s" % (n, outdir))


if __name__ == "__main__":
    main()
