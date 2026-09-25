#!/usr/bin/env python3
"""Compare our 12-bit JPEG decode against a libjpeg built with 12-bit support.

Both sides are reduced to the frame's own precision before comparison, so what
is compared is what the codec reconstructed (T.81 A.3.1), and not how this
library later widens those samples for its 16-bit raster.

  GIMG_DJPEG12=/path/to/djpeg python3 compare_12bit_to_libjpeg.py *.jpg

Exits non-zero if any file differs.
"""
import subprocess, sys, struct, os

# Point GIMG_DJPEG12 at a djpeg from a libjpeg-turbo built with 12-bit support.
# See tests/data/jpeg/README.md for how to build one; the libjpeg most systems
# ship is an 8-bit build and cannot open a P=12 frame at all.
ORACLE = os.environ.get(
    "GIMG_DJPEG12", "/opt/libjpeg-turbo/bin/djpeg-static")
# The path inside the image; GIMG_DJPEG12 overrides it for a host run.
#
# This default used to be the bare name `djpeg`, and the one below used to be
# an absolute path into a directory that does not exist - the library moved
# under libs/ and the string did not. Both were wrong in the same quiet way: a
# default that resolves to something is not a default that resolves to the
# right thing.
DUMP = os.environ.get("GIMG_DUMP", os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__))))),
    "build", "linux", "release", "apps", "dump_jpeg_raster"))

# The 12-bit reference runs in the pinned oracle image, not on this machine.
#
# Until it was pinned this was a paragraph of build instructions in
# tests/data/jpeg/README.md and a binary someone had to still have lying
# around: GIMG_DJPEG12 defaulted to `djpeg`, which on any ordinary system is an
# *8-bit* build that cannot open a P=12 frame at all. The figure this script
# produces - "352 of 352 byte-exact" - named a compiler invocation rather than
# a version. It is libjpeg-turbo 3.0.4 now, by commit, in
# tools/oracle/containers/IMAGES under the name `libjpeg12`.
_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
ORACLE_EXEC = os.path.join(_ROOT, "tools", "oracle", "oracle-exec")


def oracle_argv(inner, scratch=()):
    """`inner` run against the pinned libjpeg-turbo, or on this machine.

    GHOTI_ORACLE_MODE=host runs it here instead, which is how a hand-built
    tree is used - and it says so, because the answer is then from a reference
    nothing records.
    """
    if os.environ.get("GHOTI_ORACLE_MODE") == "host":
        return list(inner)
    argv = [ORACLE_EXEC]
    for path in scratch:
        argv += ["--scratch", os.path.abspath(path)]
    return argv + ["libjpeg12", "--"] + list(inner)

def read_pnm(b):
    # P5 (gray) or P6 (rgb), possibly with comments
    fields = []
    i = 0
    while len(fields) < 4:
        while i < len(b) and b[i:i+1].isspace(): i += 1
        if b[i:i+1] == b'#':
            while i < len(b) and b[i] != 0x0a: i += 1
            continue
        j = i
        while j < len(b) and not b[j:j+1].isspace(): j += 1
        fields.append(b[i:j]); i = j
    i += 1
    magic, w, h, maxv = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
    nch = 1 if magic == b'P5' else 3
    n = w * h * nch
    if maxv > 255:
        vals = struct.unpack('>%dH' % n, b[i:i+2*n])
    else:
        vals = tuple(b[i:i+n])
    return w, h, nch, maxv, vals

def narrow(v, maxv):
    # inverse of the library's widening (src/ops/bitdepth.c)
    return (v * maxv + 32767) // 65535

def compare(path, verbose=False):
    # The corpus is usually outside the repository - generate_12bit_matrix.py
    # writes wherever it is told - so the directory holding the file has to be
    # declared or the reference cannot see it. Declared rather than guessed:
    # an undeclared path fails as "can't open", which reads like a corrupt
    # file rather than a missing mount.
    full = os.path.abspath(path)
    o = subprocess.run(
        oracle_argv([ORACLE, "-pnm", full], scratch=[os.path.dirname(full)]),
        capture_output=True, stdin=subprocess.DEVNULL)
    if o.returncode != 0:
        return ("oracle-fail", o.stderr.decode()[:200])
    w, h, nch, maxv, ref = read_pnm(o.stdout)
    env = dict(os.environ); env["GIMG_JPEG_FANCY_UPSAMPLE"] = "1"
    m = subprocess.run([DUMP, path], capture_output=True, env=env)
    if m.returncode != 0:
        return ("ours-fail", m.stderr.decode()[:200])
    ow, oh = struct.unpack('<II', m.stdout[:8])
    body = m.stdout[8:]
    if (ow, oh) != (w, h):
        return ("dim", "%dx%d vs %dx%d" % (ow, oh, w, h))
    npx = ow * oh
    if len(body) == npx * 8:
        ours = struct.unpack('<%dH' % (npx * 4), body)
        ours = [narrow(v, maxv) for v in ours]
    elif len(body) == npx * 4:
        ours = list(body)
    else:
        return ("size", str(len(body)))
    ndiff = 0; mx = 0; first = None
    for p in range(npx):
        for c in range(nch):
            a = ref[p * nch + c]; bb = ours[p * 4 + c]
            if a != bb:
                ndiff += 1
                d = abs(a - bb)
                if d > mx: mx = d
                if first is None:
                    first = (p % ow, p // ow, c, a, bb)
    return ("ok", (ndiff, mx, npx * nch, first))

if __name__ == "__main__":
    worst = 0
    for p in sys.argv[1:]:
        kind, info = compare(p)
        if kind != "ok":
            print("%-50s %s %s" % (os.path.basename(p), kind, info)); worst = max(worst, 999)
        else:
            ndiff, mx, tot, first = info
            flag = "OK " if ndiff == 0 else "DIFF"
            print("%-50s %s ndiff=%d/%d max=%d %s" % (os.path.basename(p), flag, ndiff, tot, mx, first or ""))
            worst = max(worst, mx)
    sys.exit(0 if worst == 0 else 1)
