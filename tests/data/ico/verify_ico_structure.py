#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Compare every ICO/CUR fixture's directory against `icotool -l`.

Phase A's gate: entry count, dimensions and (for cursors) hotspots match
icoutils. Payload kind is ours to decide from the bytes; icotool does not
report it. Corrupt fixtures are expected to make icotool fail or disagree and
are listed separately so a green run does not depend on them.

Arm the gate by flipping one entry's declared width in a fixture: the parse
below and icotool will then disagree, and this script must exit non-zero.

Usage:  python3 tests/data/ico/verify_ico_structure.py [DIR ...]
        With no DIR, defaults to tests/data/ico. Extra directories (for example
        third_party/ico-refs from tools/oracle/fetch.sh) are compared too.

Exit: 0 when every good fixture matches icotool.
"""

from __future__ import annotations

import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("icoutils")

HERE = os.path.dirname(os.path.abspath(__file__))

# Fixtures that must refuse rather than parse. They are not compared.
CORRUPT = {
    "ico_corrupt_count0.ico",
    "ico_corrupt_overflow.ico",
    "ico_corrupt_truncated.ico",
}

# Directory dimensions disagree with the payload. icotool -l reports the
# payload; our ICONDIR parse reports the directory. The codec trusts the
# payload and warns - that is Test(Ico, DirMismatchStillLoads), not this gate.
DIR_MISMATCH = {
    "ico_dir_mismatch.ico",
}

# icotool -l stops when dwBytesInRes does not match the size it derives from
# the BITMAPINFOHEADER. Pillow's pillow.ico declares 9832 for the 48x48 32-bpp
# entry where the DIB+AND is 9640 bytes; icotool then lists only the preceding
# PNG entry. This library and the pixel gate still read all five entries.
# Structure evidence for that file is the pixel gate + ImageMagick identify.
STRUCTURE_SKIP = {
    "pillow.ico",
}

SKIP = CORRUPT | DIR_MISMATCH | STRUCTURE_SKIP

LINE_RE = re.compile(
    r"--(?P<kind>icon|cursor)\s+"
    r"--index=(?P<index>\d+)\s+"
    r"--width=(?P<width>\d+)\s+"
    r"--height=(?P<height>\d+)\s+"
    r"--bit-depth=(?P<bpp>\d+)\s+"
    r"--palette-size=(?P<palette>\d+)"
    r"(?:\s+--hotspot-x=(?P<hx>\d+)\s+--hotspot-y=(?P<hy>\d+))?"
)


def read_dir(path: str) -> list[dict]:
    data = open(path, "rb").read()
    if len(data) < 6:
        raise ValueError("shorter than ICONDIR")
    reserved, typ, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or typ not in (1, 2) or count < 1:
        raise ValueError(f"bad ICONDIR reserved={reserved} type={typ} count={count}")
    entries = []
    for i in range(count):
        off = 6 + i * 16
        if off + 16 > len(data):
            raise ValueError(f"entry {i} past EOF")
        w, h, colors, reserved_b, planes_or_hx, bpp_or_hy, size, offset = (
            struct.unpack_from("<BBBBHHII", data, off))
        width = 256 if w == 0 else w
        height = 256 if h == 0 else h
        ent = {
            "width": width,
            "height": height,
            "kind": "cursor" if typ == 2 else "icon",
        }
        if typ == 2:
            ent["hotspot_x"] = planes_or_hx
            ent["hotspot_y"] = bpp_or_hy
        entries.append(ent)
    return entries


def icotool_list(path: str) -> list[dict]:
    proc = subprocess.run(
        ["icotool", "-l", path],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"icotool -l failed ({proc.returncode}): {proc.stderr.strip()}")
    entries = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        m = LINE_RE.match(line)
        if not m:
            raise RuntimeError(f"unparsed icotool line: {line!r}")
        ent = {
            "width": int(m.group("width")),
            "height": int(m.group("height")),
            "kind": m.group("kind"),
        }
        if m.group("hx") is not None:
            ent["hotspot_x"] = int(m.group("hx"))
            ent["hotspot_y"] = int(m.group("hy"))
        entries.append(ent)
    return entries


def compare(path: str) -> None:
    ours = read_dir(path)
    theirs = icotool_list(path)
    if len(ours) != len(theirs):
        raise AssertionError(
            f"{os.path.basename(path)}: count ours={len(ours)} icotool={len(theirs)}")
    for i, (a, b) in enumerate(zip(ours, theirs)):
        for key in ("width", "height", "kind"):
            if a[key] != b[key]:
                raise AssertionError(
                    f"{os.path.basename(path)}[{i}].{key}: ours={a[key]} icotool={b[key]}")
        if a["kind"] == "cursor":
            for key in ("hotspot_x", "hotspot_y"):
                if a[key] != b[key]:
                    raise AssertionError(
                        f"{os.path.basename(path)}[{i}].{key}: "
                        f"ours={a[key]} icotool={b[key]}")


def collect(dirs: list[str]) -> list[tuple[str, str]]:
    """(basename, path) for every good fixture under dirs, first wins on clash."""
    seen: set[str] = set()
    out: list[tuple[str, str]] = []
    for root in dirs:
        for name in sorted(os.listdir(root)):
            if not name.endswith((".ico", ".cur")) or name in SKIP:
                continue
            if name in seen:
                raise SystemExit(
                    "duplicate fixture name %r under %s; rename one copy"
                    % (name, root))
            seen.add(name)
            out.append((name, os.path.join(root, name)))
    return out


def main() -> int:
    dirs = [os.path.abspath(d) for d in sys.argv[1:]] or [HERE]
    for d in dirs:
        if not os.path.isdir(d):
            print("not a directory: %s" % d, file=sys.stderr)
            return 2
    fixtures = collect(dirs)
    if not fixtures:
        print("no ICO/CUR fixtures in %s" % ", ".join(dirs), file=sys.stderr)
        return 2
    failed = 0
    for name, path in fixtures:
        try:
            compare(path)
            print(f"  ok  {name}")
        except Exception as exc:  # noqa: BLE001 - report every fixture
            print(f"  FAIL {name}: {exc}", file=sys.stderr)
            failed += 1
    print(f"{len(fixtures) - failed}/{len(fixtures)} fixtures match icotool -l")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
