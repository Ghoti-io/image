#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Compare every WebP fixture's chunk inventory against `webpinfo`.

Phase A gate from notes/image/webp-plan.md: fourcc, payload offset and size
match libwebp's webpinfo on every good corpus file. Corrupt fixtures are
expected to make webpinfo complain and are listed separately.

Outside corpora under third_party/webp-refs/ (from tools/oracle/fetch.sh
webp-refs) are compared the same way when passed as extra directories: files
webpinfo accepts must match; files webpinfo refuses are skipped (those trees
mix conformance vectors with intentional bad inputs).

Arm the gate by dropping one chunk from the walk: webpinfo and this parse
will disagree, and this script must exit non-zero.

Usage:  python3 tests/data/webp/verify_webp_structure.py [DIR ...]
        With no DIR, defaults to tests/data/webp. Extra directories (for
        example third_party/webp-refs) are walked recursively.

Exit: 0 when every good fixture matches webpinfo.
"""

from __future__ import annotations

import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("libwebp")

HERE = os.path.dirname(os.path.abspath(__file__))

# Committed corrupt fixtures: webpinfo must refuse them. They are not compared.
CORRUPT = {
    "corrupt_trunc.webp",
    "corrupt_riff_size.webp",
}

CHUNK_RE = re.compile(
    r"^Chunk (?P<fourcc>.{4}) at offset\s+(?P<offset>\d+), "
    r"length\s+(?P<length>\d+)\s*$"
)

ANMF_HEADER = 16


def fourcc_u32(name: str) -> int:
    b = name.encode("latin-1")
    return struct.unpack("<I", b)[0]


def walk_chunks(data: bytes) -> list[dict]:
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WEBP":
        raise ValueError("not a WebP RIFF")
    riff_size = struct.unpack_from("<I", data, 4)[0]
    riff_end = 8 + riff_size
    if riff_end > len(data):
        raise ValueError("RIFF size past EOF")
    out: list[dict] = []
    off = 12
    while off + 8 <= riff_end:
        fourcc = data[off:off + 4]
        size = struct.unpack_from("<I", data, off + 4)[0]
        payload = off + 8
        if size > riff_end - payload:
            raise ValueError("chunk size past RIFF end at %d" % off)
        out.append({
            "fourcc": fourcc.decode("latin-1"),
            "offset": off,
            # webpinfo's "length" is chunk header + payload + pad byte when
            # the payload size is odd.
            "length": size + 8 + (size & 1),
        })
        if fourcc == b"ANMF":
            nested = payload + ANMF_HEADER
            end = payload + size
            while nested + 8 <= end:
                nf = data[nested:nested + 4]
                ns = struct.unpack_from("<I", data, nested + 4)[0]
                np = nested + 8
                if ns > end - np:
                    break
                if nf not in (b"ALPH", b"VP8 ", b"VP8L"):
                    break
                out.append({
                    "fourcc": nf.decode("latin-1"),
                    "offset": nested,
                    "length": ns + 8 + (ns & 1),
                })
                step = 8 + ns + (ns & 1)
                if step > end - nested:
                    break
                nested += step
        step = 8 + size + (size & 1)
        if step > riff_end - off:
            break
        off += step
    return out


def webpinfo_chunks(path: str) -> list[dict]:
    proc = subprocess.run(
        ["webpinfo", path],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "webpinfo failed (%d): %s"
            % (proc.returncode, proc.stderr.strip() or proc.stdout.strip()))
    entries = []
    for line in proc.stdout.splitlines():
        m = CHUNK_RE.match(line.rstrip())
        if not m:
            continue
        entries.append({
            "fourcc": m.group("fourcc"),
            "offset": int(m.group("offset")),
            "length": int(m.group("length")),
        })
    return entries


def compare(path: str) -> None:
    data = open(path, "rb").read()
    ours = walk_chunks(data)
    theirs = webpinfo_chunks(path)
    # webpinfo lists the chunks it understands (VP8/VP8L/ALPH/VP8X/…) and
    # silently drops others (e.g. ICMT comments in libwebp-test-data). Our
    # walk is the full RIFF inventory; the gate is that every chunk webpinfo
    # reports appears in ours at the same offset with the same length.
    by_key = {(c["offset"], c["fourcc"], c["length"]) for c in ours}
    missing = [
        t for t in theirs
        if (t["offset"], t["fourcc"], t["length"]) not in by_key
    ]
    if missing:
        raise AssertionError(
            "%s: webpinfo chunks not in our walk: %s\n  ours=%s\n  webpinfo=%s"
            % (os.path.basename(path), missing, ours, theirs))


def list_webp(root: str) -> list[str]:
    """Return absolute paths of every .webp under root (recursive)."""
    out: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            if name.endswith(".webp"):
                out.append(os.path.join(dirpath, name))
    return out


def label(path: str, roots: list[str]) -> str:
    """Short path relative to the longest matching root, else basename."""
    abs_path = os.path.abspath(path)
    best = None
    for root in roots:
        root_abs = os.path.abspath(root)
        if abs_path == root_abs or abs_path.startswith(root_abs + os.sep):
            rel = os.path.relpath(abs_path, root_abs)
            if best is None or len(rel) < len(best):
                best = rel
    return best if best is not None else os.path.basename(path)


def main() -> int:
    roots = [os.path.abspath(p) for p in (sys.argv[1:] or [HERE])]
    paths: list[str] = []
    for root in roots:
        if not os.path.isdir(root):
            print("not a directory: %s" % root, file=sys.stderr)
            return 2
        paths.extend(list_webp(root))
    # Prefer committed fixtures first, then outside corpora, stable within.
    paths = sorted(set(paths), key=lambda p: (0 if HERE in p else 1, p))
    if not paths:
        print("no WebP fixtures in %s" % ", ".join(roots), file=sys.stderr)
        return 2

    failed = 0
    ok = 0
    skipped = 0
    for path in paths:
        name = os.path.basename(path)
        shown = label(path, roots)
        if name in CORRUPT:
            # Committed corrupt cases: webpinfo must refuse.
            try:
                webpinfo_chunks(path)
            except RuntimeError:
                print("  skip %s (corrupt, webpinfo refuses)" % shown)
                skipped += 1
                continue
            print("  FAIL %s: expected webpinfo to refuse" % shown, file=sys.stderr)
            failed += 1
            continue
        external = not os.path.abspath(path).startswith(
            os.path.abspath(HERE) + os.sep)
        try:
            compare(path)
            print("  ok  %s" % shown)
            ok += 1
        except RuntimeError as exc:
            # Outside corpora mix good vectors with intentional bad inputs.
            if external and "webpinfo failed" in str(exc):
                print("  skip %s (webpinfo refuses)" % shown)
                skipped += 1
                continue
            print("  FAIL %s: %s" % (shown, exc), file=sys.stderr)
            failed += 1
        except Exception as exc:  # noqa: BLE001
            print("  FAIL %s: %s" % (shown, exc), file=sys.stderr)
            failed += 1
    print(
        "%d ok, %d skipped, %d failed (%d files)"
        % (ok, skipped, failed, len(paths)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
