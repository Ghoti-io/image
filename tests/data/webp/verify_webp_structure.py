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

Arm the gate by dropping one chunk from the walk: webpinfo and this parse
will disagree, and this script must exit non-zero.

Usage:  python3 tests/data/webp/verify_webp_structure.py [DIR]
        DIR defaults to tests/data/webp.

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
    if len(ours) != len(theirs):
        raise AssertionError(
            "%s: count ours=%d webpinfo=%d\n  ours=%s\n  webpinfo=%s"
            % (os.path.basename(path), len(ours), len(theirs), ours, theirs))
    for i, (a, b) in enumerate(zip(ours, theirs)):
        for key in ("fourcc", "offset", "length"):
            if a[key] != b[key]:
                raise AssertionError(
                    "%s[%d].%s: ours=%r webpinfo=%r"
                    % (os.path.basename(path), i, key, a[key], b[key]))


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else HERE
    names = sorted(
        n for n in os.listdir(root)
        if n.endswith(".webp") and n not in CORRUPT)
    if not names:
        print("no WebP fixtures in %s" % root, file=sys.stderr)
        return 2
    failed = 0
    for name in names:
        path = os.path.join(root, name)
        try:
            compare(path)
            print("  ok  %s" % name)
        except Exception as exc:  # noqa: BLE001
            print("  FAIL %s: %s" % (name, exc), file=sys.stderr)
            failed += 1
    print("%d/%d fixtures match webpinfo" % (len(names) - failed, len(names)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
