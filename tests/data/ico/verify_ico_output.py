#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Check what the ICO encoder wrote, with decoders that are not ours.

The encode tests write each file into tests/out/ico/ together with one sidecar
per entry, `<name>.expected.<i>.rgba`, holding the pixels that entry was meant
to show - top-down, four bytes per pixel. This script decodes each `.ico` /
`.cur` with Pillow, ImageMagick and GdkPixbuf and compares.

Our decoder agreeing with our encoder proves nothing about either. Phase E of
notes/image/ico-plan.md asks three outside readers to accept the output.

  - **Pillow** selects by size.
  - **ImageMagick** reads every scene in order.
  - **GdkPixbuf** returns one size and refuses PNG-compressed icons; it is
    evidence only when its chosen size matches a sidecar and it can open the
    file.

Fully transparent pixels compare alpha only (RGB under alpha 0 is free).

Usage:  python3 tests/data/ico/verify_ico_output.py [DIR]
        DIR defaults to tests/out/ico.

Exit: 0 when every entry was read by at least one decoder and matched, and
each of the three readers answered for at least one entry.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

SIDE_RE = re.compile(r"^(?P<name>.+)\.expected\.(?P<index>\d+)\.rgba$")


def compare(got: bytes, want: bytes) -> str | None:
    if len(got) != len(want):
        return "decoded %d bytes, expected %d" % (len(got), len(want))
    for off in range(0, len(got), 4):
        a = got[off:off + 4]
        b = want[off:off + 4]
        if a == b:
            continue
        if a[3] == 0 and b[3] == 0:
            continue
        pixel = off // 4
        return ("pixel %d is rgba(%d,%d,%d,%d), expected rgba(%d,%d,%d,%d)"
                % (pixel, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]))
    return None


def load_pillow(path: str, width: int, height: int) -> bytes:
    from PIL import Image

    with Image.open(path) as im:
        sizes = im.info.get("sizes")
        if sizes and (width, height) in sizes:
            im.size = (width, height)
            im.load()
        elif im.size != (width, height):
            raise RuntimeError(
                "Pillow default size %s does not match entry %dx%d"
                % (im.size, width, height))
        return im.convert("RGBA").tobytes()


def load_imagemagick_scenes(path: str) -> list[tuple[int, int, bytes]]:
    id_proc = subprocess.run(
        ["magick", "identify", "-format", "%w %h\n", path],
        capture_output=True,
        text=True,
    )
    if id_proc.returncode != 0:
        raise RuntimeError(id_proc.stderr.strip())
    dims = []
    for line in id_proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        w_s, h_s = line.split()
        dims.append((int(w_s), int(h_s)))
    rgba_proc = subprocess.run(
        ["magick", path, "-depth", "8", "RGBA:-"],
        capture_output=True,
    )
    if rgba_proc.returncode != 0:
        raise RuntimeError(rgba_proc.stderr.decode(errors="replace").strip())
    data = rgba_proc.stdout
    out = []
    offset = 0
    for w, h in dims:
        need = w * h * 4
        if offset + need > len(data):
            raise RuntimeError("RGBA dump shorter than identify")
        out.append((w, h, bytes(data[offset:offset + need])))
        offset += need
    return out


def load_pixbuf(path: str) -> tuple[int, int, bytes]:
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    pb = GdkPixbuf.Pixbuf.new_from_file(path)
    w, h, rowstride = pb.get_width(), pb.get_height(), pb.get_rowstride()
    channels, data = pb.get_n_channels(), pb.get_pixels()
    out = bytearray()
    for y in range(h):
        row = data[y * rowstride:y * rowstride + w * channels]
        if channels == 4:
            out += row
        else:
            for x in range(w):
                out += row[x * 3:x * 3 + 3] + b"\xff"
    return w, h, bytes(out)


def sidecars_for(dirpath: str, name: str) -> list[tuple[int, str]]:
    out = []
    for entry in os.listdir(dirpath):
        m = SIDE_RE.match(entry)
        if m and m.group("name") == name:
            out.append((int(m.group("index")), os.path.join(dirpath, entry)))
    return sorted(out)


def read_size(side_path: str) -> tuple[int, int]:
    size_path = side_path[: -len(".rgba")] + ".size"
    with open(size_path, "r", encoding="utf-8") as f:
        w_s, h_s = f.read().split()
    return int(w_s), int(h_s)


def verify_directory(dirpath: str):
    errors: list[str] = []
    checked = 0
    answered: set[str] = set()

    if not os.path.isdir(dirpath):
        return ["not a directory: %s" % dirpath], 0, set()

    names = sorted(
        n for n in os.listdir(dirpath)
        if n.lower().endswith((".ico", ".cur")))
    if not names:
        return ["no .ico/.cur files in %s; run the encode tests first"
                % dirpath], 0, set()

    for name in names:
        path = os.path.join(dirpath, name)
        sides = sidecars_for(dirpath, name)
        if not sides:
            errors.append(
                "%s: no %s.expected.<i>.rgba beside it" % (name, name))
            continue

        try:
            im_scenes = load_imagemagick_scenes(path)
        except Exception:
            im_scenes = None

        try:
            gdk = load_pixbuf(path)
        except Exception:
            gdk = None

        for index, side in sides:
            with open(side, "rb") as f:
                want = f.read()
            try:
                w, h = read_size(side)
            except Exception as exc:  # noqa: BLE001
                errors.append("%s expected %d: missing .size (%s)"
                              % (name, index, exc))
                continue

            entry_agreed = False
            entry_read = False

            try:
                got = load_pillow(path, w, h)
            except Exception:
                got = None
            if got is not None:
                entry_read = True
                answered.add("Pillow")
                problem = compare(got, want)
                if problem:
                    errors.append("%s[%d]: Pillow reads it as %s"
                                  % (name, index, problem))
                else:
                    entry_agreed = True

            if im_scenes is not None and index < len(im_scenes):
                iw, ih, got = im_scenes[index]
                if (iw, ih) == (w, h):
                    entry_read = True
                    answered.add("ImageMagick")
                    problem = compare(got, want)
                    if problem:
                        errors.append("%s[%d]: ImageMagick reads it as %s"
                                      % (name, index, problem))
                    else:
                        entry_agreed = True

            if gdk is not None and gdk[0] == w and gdk[1] == h:
                entry_read = True
                answered.add("GdkPixbuf")
                problem = compare(gdk[2], want)
                if problem:
                    errors.append("%s[%d]: GdkPixbuf reads it as %s"
                                  % (name, index, problem))
                else:
                    entry_agreed = True

            if not entry_read:
                errors.append(
                    "%s[%d]: no outside decoder read it" % (name, index))
            elif entry_agreed:
                checked += 1

    return errors, checked, answered


def main() -> int:
    dirpath = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "tests", "out", "ico")
    errors, checked, answered = verify_directory(dirpath)
    for e in errors:
        print("  %s" % e, file=sys.stderr)
    missing = [n for n in ("Pillow", "ImageMagick", "GdkPixbuf")
               if n not in answered]
    print("  %d encoder entries read back and matched, by %s"
          % (checked, ", ".join(sorted(answered)) or "nothing"))
    if missing:
        print(
            "  %s did not answer for any file; every decoder named here is "
            "pinned in tools/oracle/containers/IMAGES."
            % ", ".join(missing),
            file=sys.stderr)
        return 1
    if errors:
        print("ICO output verification FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
