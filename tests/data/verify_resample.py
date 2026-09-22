#!/usr/bin/env python3
"""Compare this library's resampler against Pillow's, byte for byte.

The invariants in tests/unit/test_resample.cpp check that the resampler is
structurally sound - that it normalizes its kernel, low-passes a reduction,
keeps a constant constant, and undoes its own premultiplication.  None of
them can tell one correct kernel from another: a Catmull-Rom written with the
wrong `a`, or a Lanczos windowed at 2 instead of 3, passes every one of them
while being a different filter from the one it claims to be.

That is what this checks.  Pillow implements the same five named kernels, and
the arrangement of the coefficient computation here follows its Resample.c
deliberately so that the comparison can be exact rather than approximate.

Two of the five are expected to differ, and in a way this pins rather than
tolerates:

  * NEAREST, only where a destination pixel's centre falls exactly on a source
    boundary.  This library takes the pixel to the right, as the half-open
    interval [i, i+1) says and as ImageMagick's Point filter does; Pillow
    advances its source coordinate by repeated addition and so arrives a
    fraction below the boundary and takes the one to the left.  Any NEAREST
    disagreement at a non-integer centre is a failure.

  * Nothing else.  BOX, TRIANGLE, CATMULL_ROM and LANCZOS3 must agree with
    Pillow on every byte.

Pillow is required.  An absent oracle fails the run rather than skipping it:
a check that quietly does nothing reports success on a question it never
asked.
"""

import os
import subprocess
import sys
import tempfile
from fractions import Fraction

try:
    from PIL import Image
except ImportError:
    Image = None

# Our GIMG_Resample_Filter value -> (name, Pillow resampling, exact?)
FILTERS = [
    (1, "NEAREST", "NEAREST", False),
    (2, "BOX", "BOX", True),
    (3, "TRIANGLE", "BILINEAR", True),
    (4, "CATMULL_ROM", "BICUBIC", True),
    (5, "LANCZOS3", "LANCZOS", True),
]

SIZES = [(64, 64), (37, 23), (100, 40), (8, 8), (1, 17), (17, 1), (129, 7)]
TARGETS = [(32, 32), (16, 7), (128, 128), (1, 1), (97, 13), (3, 61), (64, 64)]


class Rng:
    """xorshift32, so the corpus is identical on every machine and run."""

    def __init__(self, seed):
        self.state = seed or 1

    def byte(self):
        x = self.state
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.state = x
        return x & 0xFF


def make_input(w, h, mode, seed):
    rng = Rng(seed)
    n = w * h * (1 if mode == "L" else 4)
    data = bytearray(rng.byte() for _ in range(n))
    if mode == "RGBA":
        # Opaque, because Pillow filters straight alpha and this library
        # filters premultiplied.  Where alpha is 255 the two are identical,
        # so the comparison stays exact and the premultiplication is checked
        # by the unit tests instead.
        data[3::4] = b"\xff" * (w * h)
    return bytes(data)


def nearest_expected(src, n, m, channels):
    """floor((i + 0.5) * n / m) per destination pixel, in exact arithmetic."""
    out = bytearray()
    for i in range(m):
        s = ((2 * i + 1) * n) // (2 * m)
        if s >= n:
            s = n - 1
        out += src[s * channels:(s + 1) * channels]
    return bytes(out)


def run_tool(tool, raw, w, h, channels, dst_w, dst_h, filt, workdir):
    in_path = os.path.join(workdir, "in.raw")
    out_path = os.path.join(workdir, "out.raw")
    with open(in_path, "wb") as f:
        f.write(raw)
    result = subprocess.run(
        [tool, in_path, str(w), str(h), str(channels), "8", str(dst_w),
         str(dst_h), str(filt), out_path],
        capture_output=True)
    if result.returncode != 0:
        return None, result.stderr.decode(errors="replace").strip()
    with open(out_path, "rb") as f:
        return f.read(), None


def tie_positions(n, m):
    """Destination indices whose centre lands exactly on a source boundary."""
    return {i for i in range(m)
            if (Fraction(2 * i + 1, 2) * Fraction(n, m)).denominator == 1}


def check_nearest(ours, theirs, src_w, src_h, dst_w, dst_h, channels):
    """Every disagreement with Pillow must sit on an exact tie."""
    if ours == theirs:
        return []
    x_ties = tie_positions(src_w, dst_w)
    y_ties = tie_positions(src_h, dst_h)
    problems = []
    for y in range(dst_h):
        for x in range(dst_w):
            off = (y * dst_w + x) * channels
            if ours[off:off + channels] == theirs[off:off + channels]:
                continue
            if x not in x_ties and y not in y_ties:
                problems.append(
                    f"NEAREST differs from Pillow at {x},{y} where the centre "
                    f"is not on a source boundary")
                if len(problems) > 4:
                    return problems
    return problems


def self_test():
    """The comparison must reject a wrong answer.

    Without this the whole file could be comparing something to itself - or
    nothing to nothing - and still print a row of passes.
    """
    failures = []
    src = bytes(range(8))
    good = nearest_expected(src, 8, 4, 1)
    if nearest_expected(src, 8, 4, 1) != good:
        failures.append("nearest_expected is not deterministic")
    if good == nearest_expected(src, 8, 5, 1):
        failures.append("nearest_expected ignores the destination size")
    # 2 -> 7 has its fourth destination pixel exactly on the boundary.
    if 3 not in tie_positions(2, 7):
        failures.append("tie_positions misses the 2 -> 7 boundary case")
    if tie_positions(3, 7):
        failures.append("tie_positions invents ties where there are none")
    # A single altered byte away from a tie must be reported.
    ours = bytearray(nearest_expected(bytes(range(3)), 3, 7, 1))
    theirs = bytes(ours)
    ours[1] ^= 0xFF
    if not check_nearest(bytes(ours), theirs, 3, 1, 7, 1, 1):
        failures.append("check_nearest accepts a difference away from a tie")
    return failures


def main():
    if len(sys.argv) != 2:
        print("usage: verify_resample.py <path to resample_tool>",
              file=sys.stderr)
        return 2
    tool = sys.argv[1]
    if not os.path.isfile(tool) or not os.access(tool, os.X_OK):
        print(f"resample_tool not found or not executable: {tool}\n"
              f"Build it with: make resample-tool", file=sys.stderr)
        return 1
    if Image is None:
        print("Pillow (PIL) is required to verify the resampler. "
              "Install with: pip install Pillow", file=sys.stderr)
        return 1

    problems = self_test()
    if problems:
        for p in problems:
            print(f"  self-test: {p}", file=sys.stderr)
        print("verify_resample.py is not checking what it claims to; "
              "refusing to report on the resampler.", file=sys.stderr)
        return 2

    errors = []
    compared = 0
    exact = 0
    tie_diffs = 0
    with tempfile.TemporaryDirectory() as workdir:
        for mode, channels in (("L", 1), ("RGBA", 4)):
            for filt, name, pil_name, must_match in FILTERS:
                resampling = getattr(Image.Resampling, pil_name)
                for (sw, sh) in SIZES:
                    raw = make_input(sw, sh, mode, 0x9E3779B9 ^ (sw * 131 + sh))
                    img = Image.frombytes(mode, (sw, sh), raw)
                    for (dw, dh) in TARGETS:
                        ours, err = run_tool(tool, raw, sw, sh, channels, dw,
                                             dh, filt, workdir)
                        if ours is None:
                            errors.append(
                                f"{name} {mode} {sw}x{sh} -> {dw}x{dh}: "
                                f"tool failed: {err}")
                            continue
                        theirs = img.resize((dw, dh), resampling).tobytes()
                        compared += 1
                        if ours == theirs:
                            exact += 1
                            continue
                        if must_match:
                            differing = sum(1 for a, b in zip(ours, theirs)
                                            if a != b)
                            worst = max(abs(a - b)
                                        for a, b in zip(ours, theirs))
                            errors.append(
                                f"{name} {mode} {sw}x{sh} -> {dw}x{dh}: "
                                f"{differing} of {len(ours)} bytes differ from "
                                f"Pillow, worst by {worst}")
                        else:
                            found = check_nearest(ours, theirs, sw, sh, dw, dh,
                                                  channels)
                            if found:
                                errors.append(
                                    f"{name} {mode} {sw}x{sh} -> {dw}x{dh}: "
                                    + found[0])
                            else:
                                tie_diffs += 1
                        # NEAREST is also held to the exact integer mapping,
                        # independently of what Pillow thinks.
                        if name == "NEAREST" and sh == 1 and dh == 1:
                            want = nearest_expected(raw, sw, dw, channels)
                            if ours != want:
                                errors.append(
                                    f"NEAREST {mode} {sw} -> {dw}: does not "
                                    f"match floor((i + 0.5) * n / m)")

    if errors:
        print("\033[0;31m\n### Resampler disagrees with Pillow ###\033[0m",
              file=sys.stderr)
        for e in errors[:25]:
            print(f"  {e}", file=sys.stderr)
        if len(errors) > 25:
            print(f"  ... and {len(errors) - 25} more", file=sys.stderr)
        return 1

    print(f"  {exact} of {compared} resamplings byte-identical to Pillow "
          f"({tie_diffs} NEAREST cases differ only at an exact tie)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
