#!/usr/bin/env python3
"""
Check the profiles in this directory against littleCMS, and against the
library's own tabulated colorants.

Why this exists
---------------
`generate.py` computes its colorants with a Bradford adaptation.  Nothing in
that file can tell whether the arithmetic is right: a profile is a bag of
numbers, and a generator grading its own output asserts only that it is
self-consistent.  So the numbers are checked twice, against two sources that
know nothing of each other:

  * **littleCMS**, through the pinned Pillow image, parses every profile and
    transforms pixels with it.  A profile lcms refuses is malformed however
    good it looks in a hex dump.
  * **`src/color/icc_synth.c`'s table**, which holds *published* D50-adapted
    colorants transcribed by hand.  Agreement between a computed value and a
    transcribed one is worth having in both directions: it catches an error
    in this file's matrices, and it would catch a typo in that table.

The discriminating case
-----------------------
`swap_rg.icc` is the profile a colour engine cannot silently ignore.  This
script converts a red pixel through it into sRGB and requires the result to
come out green.  Every other profile here is close enough to sRGB that an
engine doing nothing at all would pass; that is the point of having one that
is not.

Usage:
  python3 tests/data/icc/verify_icc.py
Exit 0 if every profile is well formed and says what generate.py meant.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow")

from PIL import Image, ImageCms  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# Transcribed from src/color/icc_synth.c's gimg_icc_known_gamuts, which took
# them from the published profiles.  Deliberately a *copy*: if these drift
# apart the difference is what the run reports, and either side may be the one
# that is wrong.
LIBRARY_COLORANTS = {
    "srgb_g22.icc": (
        (0.43607, 0.22249, 0.01392),
        (0.38515, 0.71687, 0.09708),
        (0.14307, 0.06061, 0.71410)),
    "adobergb_g22.icc": (
        (0.60974, 0.31111, 0.01947),
        (0.20528, 0.62567, 0.06087),
        (0.14919, 0.06322, 0.74457)),
}

D50 = (0.9642, 1.0000, 0.8249)

failures = []


def fail(msg):
    failures.append(msg)
    print("  FAIL %s" % msg)


def read_tag_xyz(data, sig):
    """Pull an XYZType tag straight out of the bytes, without a CMM."""
    count = struct.unpack(">I", data[128:132])[0]
    for i in range(count):
        off = 132 + i * 12
        tag, at, size = struct.unpack(">4sII", data[off:off + 12])
        if tag == sig:
            body = data[at:at + size]
            if body[:4] != b"XYZ ":
                return None
            return tuple(struct.unpack(">i", body[8 + k * 4:12 + k * 4])[0]
                         / 65536.0 for k in range(3))
    return None


def check_parses(name, data):
    try:
        prof = ImageCms.getOpenProfile(__import__("io").BytesIO(data))
    except Exception as e:  # noqa: BLE001
        fail("%s: littleCMS refused it: %s" % (name, e))
        return None
    desc = ImageCms.getProfileDescription(prof).strip()
    if not desc.startswith("Ghoti test"):
        fail("%s: description came back as %r" % (name, desc))
    return prof


def check_white_sum(name, data):
    """Matrix colorants must sum to the media white point.

    The property that makes a set of them well formed, and the same one
    tests/unit/test_icc_synthesis.cpp asserts of the profiles the library
    writes - so a profile failing here would be one this repository would
    refuse to have written itself.
    """
    triples = [read_tag_xyz(data, s) for s in (b"rXYZ", b"gXYZ", b"bXYZ")]
    if any(t is None for t in triples):
        return
    for axis in range(3):
        total = sum(t[axis] for t in triples)
        if abs(total - D50[axis]) > 0.0006:
            fail("%s: colorants sum to %.5f on axis %d, D50 is %.4f"
                 % (name, total, axis, D50[axis]))


def check_against_library(name, data):
    want = LIBRARY_COLORANTS.get(name)
    if not want:
        return
    got = [read_tag_xyz(data, s) for s in (b"rXYZ", b"gXYZ", b"bXYZ")]
    for i, channel in enumerate("rgb"):
        for axis in range(3):
            # Two decimals short of the transcription: s15Fixed16 rounding is
            # about 1.5e-5, and the published figures are quoted to five.
            if abs(got[i][axis] - want[i][axis]) > 0.0002:
                fail("%s: %sXYZ[%d] computed %.5f, icc_synth.c has %.5f"
                     % (name, channel, axis, got[i][axis], want[i][axis]))


def check_swap_actually_swaps():
    """The one a do-nothing engine cannot pass."""
    with open(os.path.join(HERE, "swap_rg.icc"), "rb") as f:
        swap = ImageCms.getOpenProfile(__import__("io").BytesIO(f.read()))
    srgb = ImageCms.createProfile("sRGB")
    src = Image.new("RGB", (1, 1), (255, 0, 0))
    out = ImageCms.profileToProfile(src, swap, srgb, outputMode="RGB",
                                    renderingIntent=1)
    r, g, b = out.getpixel((0, 0))
    if not (g > 200 and r < 80):
        fail("swap_rg.icc: red converted to (%d, %d, %d); it must come out "
             "green, or the fixture cannot catch an engine that does nothing"
             % (r, g, b))
    else:
        print("  swap_rg.icc: red -> (%d, %d, %d), as it must" % (r, g, b))


def check_wide_gamut_moves():
    """Adobe RGB's gamut differs from sRGB's, so the conversion must move.

    Weaker than the swap and still worth stating: it is the ordinary case an
    engine has to get right, and a transform that skipped the matrix would
    leave the value where it started.

    **The probe colours are mixed, and that is the whole trick.**  The first
    version used saturated green (0, 255, 0) and got (0, 255, 0) back - not
    because nothing happened, but because a saturated primary is exactly the
    colour that cannot tell the two apart: it is out of gamut in the
    destination, so relative colorimetric clips it to sRGB's own maximum
    green, which has the same coordinates.  Backing off to (0, 128, 0) moved
    it by one, for a related reason: a colour on a primary axis stays on that
    axis, and the two spaces differ mostly in how far along it the axis
    reaches.  Only a colour mixed from all three channels separates the two
    readings, and (160, 96, 64) moves by twenty.
    """
    with open(os.path.join(HERE, "adobergb_g22.icc"), "rb") as f:
        adobe = ImageCms.getOpenProfile(__import__("io").BytesIO(f.read()))
    srgb = ImageCms.createProfile("sRGB")
    probes = [(160, 96, 64), (64, 160, 96)]
    for probe in probes:
        src = Image.new("RGB", (1, 1), probe)
        out = ImageCms.profileToProfile(src, adobe, srgb, outputMode="RGB",
                                        renderingIntent=1)
        got = out.getpixel((0, 0))
        moved = max(abs(a - b) for a, b in zip(probe, got))
        if moved < 4:
            fail("adobergb_g22.icc: %s came through as %s, a difference of "
                 "%d - this fixture cannot tell a transform from the identity"
                 % (probe, got, moved))
        else:
            print("  adobergb_g22.icc: %s -> %s, moved by %d"
                  % (probe, got, moved))


def main():
    names = sorted(n for n in os.listdir(HERE) if n.endswith(".icc"))
    if not names:
        print("no profiles; run tests/data/icc/generate.py", file=sys.stderr)
        return 1
    print("littleCMS %s, Pillow %s" % (ImageCms.core.littlecms_version,
                                       __import__("PIL").__version__))
    for name in names:
        with open(os.path.join(HERE, name), "rb") as f:
            data = f.read()
        prof = check_parses(name, data)
        if prof is None:
            continue
        check_white_sum(name, data)
        check_against_library(name, data)
        print("  %-24s %5d bytes  %s" % (
            name, len(data), ImageCms.getProfileDescription(prof).strip()))
    check_swap_actually_swaps()
    check_wide_gamut_moves()
    if failures:
        print("\n%d problem(s)" % len(failures), file=sys.stderr)
        return 1
    print("\nEvery profile parses, is well formed, and does what it says.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
