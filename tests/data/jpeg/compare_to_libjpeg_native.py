"""Compare our decode against libjpeg at the frame's own sample precision.

Works for any frame this codec reads - baseline, progressive, arithmetic,
lossless - at 8-, 12- or 16-bit, because the oracle side is ljdec.c rather than
djpeg.  Two reasons djpeg is not enough: it does not expose do_block_smoothing,
which libjpeg applies to progressive frames and we deliberately do not
implement, and its output format hides the frame's precision.

Build the oracle first, which is one target and not a hand-typed compiler
invocation any more:

  make oracle-build oracle-tools
  python3 compare_to_libjpeg_native.py *.jpg

ljdec links the libjpeg-turbo 3.0.4 built inside the pinned image and runs
there too, so this script names a version rather than whatever the machine
has. It cannot re-exec itself wholesale the way the verify_* scripts do,
because the other half of every comparison is our own host-built
dump_jpeg_raster, and a host binary is not something the oracle image should
be asked to run. So the reference half is asked through oracle-exec, one call
per file; GIMG_LJDEC still overrides, and GHOTI_ORACLE_MODE=host runs a local
build of it.

Exits quietly with a count; lists the first fifteen files that differ.
"""
import subprocess, struct, sys, os
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
ORACLE_EXEC = os.path.join(_ROOT, "tools", "oracle", "oracle-exec")
# Inside the image this is the path oracle-tools wrote it to, which is under
# the tree and therefore visible there; on a host run it is the same file.
LJ = os.environ.get("GIMG_LJDEC",
    os.path.join(_ROOT, "tests", "tools", "jpeg-oracle", "build", "ljdec"))
DUMP = os.environ.get("GIMG_DUMP",
    "build/linux/release/apps/dump_jpeg_raster")


def oracle_argv(inner, scratch=()):
    """The reference half, at the pin."""
    if os.environ.get("GHOTI_ORACLE_MODE") == "host":
        return list(inner)
    argv = [ORACLE_EXEC]
    for path in scratch:
        argv += ["--scratch", os.path.abspath(path)]
    return argv + ["libjpeg12", "--"] + list(inner)


def require_oracle():
    """Reach the reference once, before any file is judged by it.

    Without this the two failures are the same line. Every comparison below
    that cannot run the oracle is recorded as `oracle-fail` against the file,
    which is the right label for a file libjpeg refuses - 53 of the fixtures
    here are hierarchical or abbreviated and it refuses every one. An engine
    that is not installed produces that label for *all* of them and a count
    that still prints, so "the reference was never reached" and "the reference
    read nothing here" read identically.
    """
    if os.environ.get("GHOTI_ORACLE_MODE") == "host":
        if not os.path.isfile(LJ):
            sys.exit("oracle: %s is not built, and GHOTI_ORACLE_MODE=host "
                     "leaves nothing else to run it with.\n"
                     "  make oracle-build oracle-tools" % LJ)
        return
    env = os.path.join(_ROOT, "tools", "oracle", "oracle_env.py")
    r = subprocess.run([sys.executable or "python3", env, "libjpeg12"],
                       capture_output=True, stdin=subprocess.DEVNULL)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        sys.exit(2)
    if not os.path.isfile(LJ):
        sys.exit("oracle: the image is reachable but %s is not built.\n"
                 "  make oracle-tools" % LJ)

def widen(v, frm, to):
    if frm >= to: return v >> (frm - to)
    r = v; have = frm
    while have < to:
        take = min(to - have, frm)
        r = (r << take) | (v >> (frm - take))
        have += take
    return r

TMP = os.environ.get("GIMG_TMP", "/tmp/gimg_oracle.bin")
require_oracle()
ok = 0; fails = []
for p in sys.argv[1:]:
    if subprocess.run(
            oracle_argv([LJ, os.path.abspath(p), TMP],
                        scratch=[os.path.dirname(os.path.abspath(p)),
                                 os.path.dirname(os.path.abspath(TMP))]),
            capture_output=True, stdin=subprocess.DEVNULL).returncode != 0:
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
# A floor under the count. require_oracle() has already established that the
# reference runs, so a sweep in which it read nothing is a corpus or a mount
# that did not arrive, not a verdict about this codec.
if ok == 0:
    sys.exit("the reference read none of the %d files, so nothing was "
             "compared" % (len(sys.argv) - 1))
