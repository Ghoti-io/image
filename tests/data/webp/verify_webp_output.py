#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Check lossless WebP files this library encoded, with libwebp.

The encode tests leave each file in tests/out/webp/ with two sidecars:

  <name>.webp.expected.rgba   source pixels, top to bottom, four bytes each
  <name>.webp.time            microseconds spent in gimg_doc_save

Four things are measured for every file. Two of them fail the run:

  - fidelity: `dwebp -pam` must match the source pixels
  - parity: our decoder (dump_webp_raster) must match `dwebp`

The other two are printed, because a larger or slower file is still a
valid stream:

  - size, beside `cwebp -lossless -exact` of the same pixels
  - encode time, beside the "Time to encode picture" line cwebp prints

A round trip through our own decoder is not this check. That is how a
tree libwebp rejects used to pass.

Usage:  python3 tests/data/webp/verify_webp_output.py [DIR]
        DIR defaults to tests/out/webp.
        GIMG_WEBP_DUMP points at dump_webp_raster (or pass --dump PATH).

Exit: 0 when every file matched. Non-zero when dwebp refuses a file,
pixels differ, or cwebp could not be measured.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

ENCODE_TIME_RE = re.compile(r"Time to encode picture:\s*([0-9.]+)s")


def default_dir() -> str:
    return os.path.join(ROOT, "tests", "out", "webp")


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
    raise FileNotFoundError(
        "dump_webp_raster not found; build with `make webp-dump-raster` "
        "or pass --dump PATH")


def list_webp(directory: str) -> list[str]:
    names = []
    for name in sorted(os.listdir(directory)):
        if name.endswith(".webp") and os.path.isfile(
                os.path.join(directory, name)):
            names.append(name)
    return names


def read_pam_rgba(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    marker = b"ENDHDR\n"
    idx = data.find(marker)
    if idx < 0:
        raise ValueError("no ENDHDR in %s" % path)
    hdr = data[:idx].decode("ascii", errors="replace")
    body = data[idx + len(marker):]
    width = height = None
    for line in hdr.splitlines():
        if line.startswith("WIDTH "):
            width = int(line.split()[1])
        elif line.startswith("HEIGHT "):
            height = int(line.split()[1])
    if width is None or height is None:
        raise ValueError("PAM missing WIDTH/HEIGHT in %s" % path)
    expect = width * height * 4
    if len(body) < expect:
        raise ValueError(
            "PAM short: %d bytes, need %d in %s" % (len(body), expect, path))
    return width, height, body[:expect]


def first_diff(got: bytes, want: bytes, width: int, want_name: str) -> str | None:
    if len(got) != len(want):
        return "decoded %d bytes, %s is %d" % (len(got), want_name, len(want))
    for off in range(0, len(got), 4):
        if got[off:off + 4] == want[off:off + 4]:
            continue
        pixel = off // 4
        x = pixel % width if width else pixel
        y = pixel // width if width else 0
        a = got[off:off + 4]
        b = want[off:off + 4]
        return (
            "pixel (%d,%d) is rgba(%d,%d,%d,%d), %s rgba(%d,%d,%d,%d)"
            % (x, y, a[0], a[1], a[2], a[3], want_name,
               b[0], b[1], b[2], b[3]))
    return None


def dwebp_pam(src: str, dest: str) -> None:
    proc = subprocess.run(
        ["dwebp", src, "-pam", "-o", dest],
        check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "dwebp failed (%d): %s"
            % (proc.returncode, (proc.stderr or proc.stdout).strip()))


def cwebp_lossless(src: str, dest: str) -> tuple[int, float]:
    proc = subprocess.run(
        ["cwebp", "-lossless", "-exact", "-v", "-o", dest, src],
        check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "cwebp failed (%d): %s"
            % (proc.returncode, (proc.stderr or proc.stdout).strip()))
    match = ENCODE_TIME_RE.search(proc.stderr + proc.stdout)
    if not match:
        raise RuntimeError("cwebp printed no encode time")
    return os.path.getsize(dest), float(match.group(1))


def dump_ours(dump: str, outdir: str, paths: list[str]) -> None:
    proc = subprocess.run(
        [dump, outdir] + paths,
        check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "dump_webp_raster failed (%d): %s"
            % (proc.returncode, proc.stderr.strip() or proc.stdout.strip()))
    ok = 0
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3 and parts[2] == "ok":
            ok += 1
            continue
        raise RuntimeError("our decoder refused %s" % line)
    if ok != len(paths):
        raise RuntimeError(
            "dump_webp_raster wrote %d rasters for %d files" % (ok, len(paths)))


def compare(directory: str, names: list[str]) -> int:
    errors: list[str] = []
    failed = 0
    oracle = os.path.join(directory, "oracle")
    os.makedirs(oracle, exist_ok=True)
    for index, name in enumerate(names):
        path = os.path.join(directory, name)
        expected_path = path + ".expected.rgba"
        time_path = path + ".time"
        bad = False
        if not os.path.isfile(expected_path):
            errors.append("%s: no source sidecar" % name)
            failed += 1
            continue
        with open(expected_path, "rb") as handle:
            expected = handle.read()
        ours_path = os.path.join(oracle, "%d.0.rgba" % index)
        if not os.path.isfile(ours_path):
            errors.append("%s: our decoder wrote no raster" % name)
            failed += 1
            continue
        with open(ours_path, "rb") as handle:
            ours = handle.read()
        pam_path = os.path.join(oracle, "%d.pam" % index)
        try:
            dwebp_pam(path, pam_path)
            width, _height, ref = read_pam_rgba(pam_path)
        except (RuntimeError, ValueError) as exc:
            errors.append("%s: %s" % (name, exc))
            failed += 1
            continue
        if len(expected) != len(ref):
            errors.append(
                "%s: source is %d bytes, dwebp decoded %d"
                % (name, len(expected), len(ref)))
            failed += 1
            continue
        fidelity = first_diff(ref, expected, width, "source")
        if fidelity:
            errors.append("%s: dwebp %s" % (name, fidelity))
            bad = True
        parity = first_diff(ours, ref, width, "dwebp")
        if parity:
            errors.append("%s: our decoder %s" % (name, parity))
            bad = True
        try:
            cwebp_path = os.path.join(oracle, "%d.cwebp" % index)
            cwebp_bytes, cwebp_s = cwebp_lossless(path, cwebp_path)
        except (RuntimeError, OSError) as exc:
            errors.append("%s: %s" % (name, exc))
            failed += 1
            continue
        if bad:
            failed += 1
        ours_us = None
        if os.path.isfile(time_path):
            with open(time_path, "r", encoding="ascii") as handle:
                text = handle.read().strip()
            if text.isdigit():
                ours_us = int(text)
        ours_ms = ("%.3f ms" % (ours_us / 1000.0)) if ours_us is not None \
            else "time not recorded"
        print(
            "  %s  %s  %d bytes (cwebp %d)  encode %s (cwebp %.3f ms)"
            % ("FAIL" if bad else "ok", name, os.path.getsize(path),
               cwebp_bytes, ours_ms, cwebp_s * 1000.0))
    for err in errors:
        print("  %s" % err, file=sys.stderr)
    print("%d ok, %d failed (%d files)" % (
        len(names) - failed, failed, len(names)))
    if failed:
        print("WebP encode output verification FAILED", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", default=default_dir())
    parser.add_argument("--dump", default=None)
    parser.add_argument(
        "--compare", action="store_true",
        help="dwebp/cwebp phase; dumps were already written")
    args = parser.parse_args(argv)
    if not os.path.isdir(args.directory):
        print("not a directory: %s" % args.directory, file=sys.stderr)
        return 2
    names = list_webp(args.directory)
    if not names:
        print(
            "no encoded WebP files in %s (run the WebP tests first)"
            % args.directory, file=sys.stderr)
        return 1
    oracle = os.path.join(args.directory, "oracle")
    os.makedirs(oracle, exist_ok=True)
    if not args.compare:
        try:
            dump = find_dump(args.dump)
            dump_ours(dump, oracle, [os.path.join(args.directory, n) for n in names])
        except (FileNotFoundError, RuntimeError) as exc:
            print(str(exc), file=sys.stderr)
            return 2
        sys.path.insert(0, os.path.dirname(HERE))
        from oracle_reexec import inside_or_reexec  # noqa: E402
        sys.argv = [sys.argv[0], args.directory, "--compare"]
        inside_or_reexec("libwebp", scratch=[args.directory])
    return compare(args.directory, names)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
