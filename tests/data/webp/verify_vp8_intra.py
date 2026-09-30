#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Check VP8 intra prediction, the loop filter, and segmentation.

cwebp encodes flat blocks (16x16 modes) and 4x4 noise (subblock modes),
with the filter off, with a nonzero filter, and with four segments.
Our decoder must match `dwebp`, including the 9-3-3-1 chroma upsample.

A frame that comes back unsupported fails the run. This script
builds keyframes only.

Usage:  python3 tests/data/webp/verify_vp8_intra.py
        GIMG_WEBP_DUMP points at dump_webp_raster (or pass --dump PATH).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))


def find_dump(explicit: str | None) -> str:
    if explicit and os.path.isfile(explicit):
        return os.path.abspath(explicit)
    env = os.environ.get("GIMG_WEBP_DUMP")
    if env and os.path.isfile(env):
        return os.path.abspath(env)
    for cand in (
            os.path.join(ROOT, "build", "linux", "release", "apps",
                         "dump_webp_raster"),
            os.path.join(ROOT, "build", "linux", "debug", "apps",
                         "dump_webp_raster"),
    ):
        if os.path.isfile(cand):
            return cand
    raise FileNotFoundError("dump_webp_raster not found")


def blocks_ppm(width: int, height: int) -> bytes:
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            if x < 16:
                row += bytes((40, 40, 200))
            elif x < 32:
                row += bytes((220, 30, 30))
            elif y < 16:
                row += bytes((30, 200, 40))
            else:
                row += bytes((x * 3, y * 4, 128))
        rows.append(bytes(row))
    return b"P6\n%d %d\n255\n" % (width, height) + b"".join(rows)


def detail_ppm(width: int, height: int) -> bytes:
    rows = []
    seed = 1
    for y in range(height):
        row = bytearray()
        for x in range(width):
            if ((x // 4) + (y // 4)) & 1:
                seed = (seed * 1103515245 + 12345) & 0x7fffffff
                row += bytes((
                    seed & 255,
                    (seed >> 8) & 255,
                    (seed >> 16) & 255,
                ))
            else:
                row += bytes(((x * 13) & 255, (y * 17) & 255, (x * y) & 255))
        rows.append(bytes(row))
    return b"P6\n%d %d\n255\n" % (width, height) + b"".join(rows)


def run(argv: list[str]) -> None:
    proc = subprocess.run(argv, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "%s failed (%d): %s"
            % (os.path.basename(argv[0]), proc.returncode,
               (proc.stderr or proc.stdout).strip()))


def pam_rgba(path: str) -> tuple[int, bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    marker = b"ENDHDR\n"
    idx = data.find(marker)
    if idx < 0:
        raise RuntimeError("no ENDHDR in %s" % path)
    width = None
    for line in data[:idx].decode("ascii", errors="replace").splitlines():
        if line.startswith("WIDTH "):
            width = int(line.split()[1])
    if width is None:
        raise RuntimeError("PAM missing WIDTH in %s" % path)
    body = data[idx + len(marker):]
    return width, body


def first_diff(got: bytes, want: bytes, width: int) -> str | None:
    n = min(len(got), len(want))
    if len(got) != len(want):
        return "decoded %d bytes, dwebp %d" % (len(got), len(want))
    for off in range(0, n, 4):
        if got[off:off + 4] == want[off:off + 4]:
            continue
        pixel = off // 4
        a = got[off:off + 4]
        b = want[off:off + 4]
        return (
            "pixel (%d,%d) is rgba(%d,%d,%d,%d), dwebp rgba(%d,%d,%d,%d)"
            % (pixel % width, pixel // width, a[0], a[1], a[2], a[3],
               b[0], b[1], b[2], b[3]))
    return None


def check_one(dump: str, oracle: str, work: str, name: str, ppm: bytes,
        quality: str, method: str, filt: str, sharp: str, segments: str,
        extra: list[str]) -> str | None:
    ppm_path = os.path.join(work, name + ".ppm")
    webp_path = os.path.join(work, name + ".webp")
    pam_path = os.path.join(work, name + ".pam")
    with open(ppm_path, "wb") as handle:
        handle.write(ppm)
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "cwebp", "-q", quality, "-m", method, "-f", filt, "-sharpness", sharp,
        "-segments", segments, "-sns", "0" if segments == "1" else "80",
    ] + extra + ["-o", webp_path, ppm_path])
    proc = subprocess.run(
        [dump, work, webp_path], check=False, capture_output=True, text=True)
    if proc.returncode != 0 or "\tok\t" not in proc.stdout:
        return "%s: our decoder refused it: %s" % (
            name, (proc.stderr or proc.stdout).strip())
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "dwebp", "-pam", "-o", pam_path, webp_path,
    ])
    width, ref = pam_rgba(pam_path)
    rgba_path = os.path.join(work, "0.0.rgba")
    with open(rgba_path, "rb") as handle:
        got = handle.read()
    return first_diff(got, ref, width)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", default=None)
    args = parser.parse_args(argv)
    try:
        dump = find_dump(args.dump)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    oracle = os.path.join(ROOT, "tools", "oracle", "oracle-exec")
    work = os.path.join(ROOT, "tests", "out", "webp-intra")
    os.makedirs(work, exist_ok=True)
    cases = (
        ("blocks", blocks_ppm(64, 48), "40", "3", "0", "0", "1", []),
        ("detail", detail_ppm(48, 32), "50", "4", "0", "0", "1", []),
        ("blocks_f", blocks_ppm(64, 48), "40", "3", "30", "0", "1", []),
        ("detail_f", detail_ppm(48, 32), "50", "4", "20", "2", "1", []),
        ("blocks_simple", blocks_ppm(64, 48), "40", "3", "30", "0", "1",
         ["-nostrong"]),
        ("blocks_sharp", blocks_ppm(64, 48), "40", "3", "40", "7", "1", []),
        ("detail_sharp", detail_ppm(48, 32), "40", "4", "60", "5", "1", []),
        ("detail_hi", detail_ppm(48, 32), "30", "4", "80", "0", "1", []),
        ("blocks_seg", blocks_ppm(64, 48), "40", "3", "20", "0", "4", []),
        ("detail_seg", detail_ppm(48, 32), "40", "4", "30", "2", "4", []),
    )
    failed = 0
    for name, ppm, quality, method, filt, sharp, segments, extra in cases:
        try:
            err = check_one(
                dump, oracle, work, name, ppm, quality, method, filt, sharp,
                segments, extra)
        except (RuntimeError, OSError) as exc:
            print("  FAIL  %s  %s" % (name, exc), file=sys.stderr)
            failed += 1
            continue
        if err:
            print("  FAIL  %s  %s" % (name, err), file=sys.stderr)
            failed += 1
        else:
            print("  ok  %s  decode parity dwebp" % name)
    print("%d ok, %d failed (%d files)" % (
        len(cases) - failed, failed, len(cases)))
    if failed:
        print("VP8 intra verification FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
