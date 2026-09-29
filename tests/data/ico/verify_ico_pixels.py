#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Compare each ICO/CUR fixture entry against Pillow, ImageMagick and GdkPixbuf.

Phase B/C gate from notes/image/ico-plan.md: per-entry pixels, never "the"
icon. This library dumps every entry through dump_ico_raster on the host; each
outside reader is asked inside the pinned oracle image only about entries it
can see:

  - **Pillow** selects by size (`Image.size = (w, h)` on ICO; CUR is one size).
  - **ImageMagick** enumerates scenes in directory order (`magick identify`
    then a concatenated RGBA dump).
  - **GdkPixbuf** returns one size and refuses PNG-compressed icons
    ("Compressed icons are not supported"). It is evidence only when its
    chosen size matches an entry we dumped.

The dump runs on the host because it links this library's shared objects; the
oracle comparison runs in the image because Pillow is the reference. The dump
directory is passed through as a scratch mount so the container sees it.

WHAT IS NOT COMPARED
====================

- Corrupt fixtures and `ico_dir_mismatch.ico` (directory dimensions disagree
  with the payload; outside readers do not share one rule for which wins).
- `ico_zero_alpha_and.ico`. This codec's §5.3 rule restores transparency from
  the AND mask when every XOR alpha byte is zero. Pillow, ImageMagick and
  GdkPixbuf all leave those pixels fully transparent instead. That disagreement
  is documented in documentation/formats/ico.md and asserted in
  Test(Ico, ZeroAlphaFallsBackToAndMask); asking the oracles to match it would
  fail the intentional deviation.
- Pillow on `wine_blank.ico` entry 0. That entry is a 1-bit greyscale PNG with
  a tRNS chunk; this library and ImageMagick decode it as fully transparent,
  Pillow's ICO path yields opaque black. ImageMagick still covers the entry.
- The colour under a fully transparent pixel (both sides alpha 0).

Arm the gate by flipping a fixture's red channel: every reader that can see
that entry will disagree with the dump, and this script must exit non-zero.

Usage:  python3 tests/data/ico/verify_ico_pixels.py [DIR ...]
        With no DIR, defaults to tests/data/ico. Extra directories (for example
        third_party/ico-refs) are compared in the same dump/oracle pass.
        GIMG_ICO_DUMP points at dump_ico_raster (or pass --dump PATH).

Exit: 0 when every compared entry matched every reader that could see it, and
each of the three readers answered for at least one entry.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))

SKIP = {
    "ico_corrupt_count0.ico",
    "ico_corrupt_overflow.ico",
    "ico_corrupt_truncated.ico",
    "ico_dir_mismatch.ico",
    "ico_zero_alpha_and.ico",
}

# GdkPixbuf refuses PNG-compressed icons entirely, and on a mixed DIB+PNG
# file it returns a transparent-black 16x16 where Pillow, ImageMagick and this
# library all agree the first entry is opaque yellow. It is not asked about
# those files; Pillow and ImageMagick still cover every entry.
GDK_SKIP = {
    "ico_mixed.ico",
    "ico_png_32.ico",
    "ico_256_png.ico",
}

# Entry indices where Pillow is known to disagree with this library and with
# ImageMagick. Keyed by basename; those entries are still checked by the
# other readers.
PILLOW_SKIP_ENTRIES = {
    # 1-bit greyscale PNG + tRNS: Pillow's ICO path ignores transparency.
    "wine_blank.ico": {0},
}

STATUS_RE = re.compile(
    r"^(?P<stem>\S+)\t(?P<index>\d+)\tok\t(?P<w>\d+)\t(?P<h>\d+)$")


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
    if not dims:
        raise RuntimeError("identify reported no scenes")

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
            raise RuntimeError(
                "RGBA dump shorter than identify (%d scenes, %d bytes)"
                % (len(dims), len(data)))
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


def run_dump(dump: str, outdir: str, paths: list[str]) -> list[dict]:
    os.makedirs(outdir, exist_ok=True)
    env = os.environ.copy()
    # The dump binary's rpath names the install prefix; when that is not
    # enough (a just-built tree), TEST_LD_PATH from the Makefile helps.
    proc = subprocess.run(
        [dump, outdir] + paths, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise RuntimeError(
            "dump_ico_raster failed (%d): %s"
            % (proc.returncode, proc.stderr.strip() or proc.stdout.strip()))
    entries = []
    for line in proc.stdout.splitlines():
        m = STATUS_RE.match(line.strip())
        if not m:
            continue
        entries.append({
            "stem": m.group("stem"),
            "index": int(m.group("index")),
            "width": int(m.group("w")),
            "height": int(m.group("h")),
            "rgba_path": os.path.join(
                outdir, "%s.%d.rgba" % (m.group("stem"), int(m.group("index")))),
        })
    return entries


def find_dump(explicit: str | None) -> str:
    if explicit and os.path.isfile(explicit):
        return os.path.abspath(explicit)
    env = os.environ.get("GIMG_ICO_DUMP")
    if env and os.path.isfile(env):
        return os.path.abspath(env)
    for cand in (
            os.path.join(ROOT, "build", "linux", "release", "apps",
                         "dump_ico_raster"),
            os.path.join(ROOT, "build", "linux", "debug", "apps",
                         "dump_ico_raster"),
    ):
        if os.path.isfile(cand):
            return cand
    raise FileNotFoundError(
        "dump_ico_raster not found; build with `make ico-dump-raster` "
        "or pass --dump PATH")


def compare_in_image(fixtures: list[tuple[str, str]], dump_dir: str) -> int:
    errors: list[str] = []
    checked = 0
    answered: set[str] = set()

    by_stem: dict[str, list[dict]] = {}
    for name, _path in fixtures:
        stem = os.path.splitext(name)[0]
        # Discover dumps written as <stem>.<i>.rgba beside a status we re-derive
        # from the file size of each dump via a small meta file the host wrote.
        meta = os.path.join(dump_dir, stem + ".entries")
        if not os.path.isfile(meta):
            errors.append("%s: no dump meta at %s" % (name, meta))
            continue
        with open(meta, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) != 3:
                    continue
                idx, w, h = int(parts[0]), int(parts[1]), int(parts[2])
                by_stem.setdefault(stem, []).append({
                    "stem": stem,
                    "index": idx,
                    "width": w,
                    "height": h,
                    "rgba_path": os.path.join(
                        dump_dir, "%s.%d.rgba" % (stem, idx)),
                })

    for name, path in fixtures:
        stem = os.path.splitext(name)[0]
        ours = sorted(by_stem.get(stem, []), key=lambda e: e["index"])
        if not ours:
            errors.append("%s: dump_ico_raster produced no entries" % name)
            continue

        try:
            im_scenes = load_imagemagick_scenes(path)
        except Exception as exc:  # noqa: BLE001
            im_scenes = None
            im_err = str(exc)
        else:
            im_err = None

        try:
            gdk_w, gdk_h, gdk_rgba = load_pixbuf(path)
            gdk = (gdk_w, gdk_h, gdk_rgba)
        except Exception:
            gdk = None
        if name in GDK_SKIP:
            gdk = None

        if im_scenes is not None and len(im_scenes) != len(ours):
            errors.append(
                "%s: ImageMagick reports %d scenes, we dumped %d"
                % (name, len(im_scenes), len(ours)))

        pillow_skip = PILLOW_SKIP_ENTRIES.get(name, set())

        for ent in ours:
            with open(ent["rgba_path"], "rb") as f:
                want = f.read()
            w, h, idx = ent["width"], ent["height"], ent["index"]
            entry_agreed = False
            entry_read = False

            if idx not in pillow_skip:
                try:
                    got = load_pillow(path, w, h)
                except Exception:
                    got = None
                if got is not None:
                    entry_read = True
                    answered.add("Pillow")
                    problem = compare(got, want)
                    if problem:
                        errors.append(
                            "%s[%d] %dx%d: Pillow reads it as %s"
                            % (name, idx, w, h, problem))
                    else:
                        entry_agreed = True

            if im_scenes is not None and idx < len(im_scenes):
                iw, ih, got = im_scenes[idx]
                if (iw, ih) != (w, h):
                    errors.append(
                        "%s[%d]: ImageMagick scene is %dx%d, dump is %dx%d"
                        % (name, idx, iw, ih, w, h))
                else:
                    entry_read = True
                    answered.add("ImageMagick")
                    problem = compare(got, want)
                    if problem:
                        errors.append(
                            "%s[%d] %dx%d: ImageMagick reads it as %s"
                            % (name, idx, w, h, problem))
                    else:
                        entry_agreed = True
            elif im_err is not None and idx == 0:
                errors.append("%s: ImageMagick failed: %s" % (name, im_err))

            if gdk is not None and gdk[0] == w and gdk[1] == h:
                entry_read = True
                answered.add("GdkPixbuf")
                problem = compare(gdk[2], want)
                if problem:
                    errors.append(
                        "%s[%d] %dx%d: GdkPixbuf reads it as %s"
                        % (name, idx, w, h, problem))
                else:
                    entry_agreed = True

            if not entry_read:
                errors.append(
                    "%s[%d] %dx%d: no outside decoder read this entry"
                    % (name, idx, w, h))
            elif entry_agreed:
                checked += 1
                print("  ok  %s[%d] %dx%d" % (name, idx, w, h))

    for e in errors:
        print("  %s" % e, file=sys.stderr)
    missing = [n for n in ("Pillow", "ImageMagick", "GdkPixbuf")
               if n not in answered]
    print("  %d entries matched, by %s"
          % (checked, ", ".join(sorted(answered)) or "nothing"))
    if missing:
        print(
            "  %s did not answer for any entry; every decoder named here is "
            "pinned in tools/oracle/containers/IMAGES."
            % ", ".join(missing),
            file=sys.stderr)
        return 1
    if errors:
        print("ICO pixel verification FAILED", file=sys.stderr)
        return 1
    return 0


def collect_fixtures(dirs: list[str]) -> list[tuple[str, str]]:
    seen: set[str] = set()
    out: list[tuple[str, str]] = []
    for dirpath in dirs:
        for name in sorted(os.listdir(dirpath)):
            if not name.endswith((".ico", ".cur")) or name in SKIP:
                continue
            if name.startswith("_"):
                continue
            if name in seen:
                raise SystemExit(
                    "duplicate fixture name %r under %s; rename one copy"
                    % (name, dirpath))
            seen.add(name)
            out.append((name, os.path.join(dirpath, name)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "dirs", nargs="*", default=[HERE],
        help="fixture directories (default: tests/data/ico)")
    ap.add_argument("--dump", default=None, help="path to dump_ico_raster")
    ap.add_argument(
        "--dump-dir", default=None,
        help="host-prepared dump directory (set across oracle re-exec)")
    args = ap.parse_args()
    dirs = [os.path.abspath(d) for d in (args.dirs or [HERE])]
    for d in dirs:
        if not os.path.isdir(d):
            print("not a directory: %s" % d, file=sys.stderr)
            return 2

    fixtures = collect_fixtures(dirs)
    if not fixtures:
        print("no ICO/CUR fixtures to compare in %s" % ", ".join(dirs),
              file=sys.stderr)
        return 2

    # Already prepared (re-exec argv, or a caller that dumped first).
    if args.dump_dir and os.path.isdir(args.dump_dir):
        from oracle_reexec import inside_or_reexec  # noqa: WPS433
        inside_or_reexec("pillow", scratch=[args.dump_dir])
        return compare_in_image(fixtures, args.dump_dir)

    # Host side: dump, then re-exec into the image with the dump dir mounted.
    try:
        dump = find_dump(args.dump)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="ico-pixels-")
    paths = [path for _name, path in fixtures]
    try:
        dumped = run_dump(dump, tmp, paths)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    by_stem: dict[str, list[dict]] = {}
    for ent in dumped:
        by_stem.setdefault(ent["stem"], []).append(ent)
    for stem, ents in by_stem.items():
        with open(os.path.join(tmp, stem + ".entries"), "w",
                  encoding="utf-8") as f:
            for ent in sorted(ents, key=lambda e: e["index"]):
                f.write("%d %d %d\n" % (
                    ent["index"], ent["width"], ent["height"]))

    # Re-invoke ourselves with --dump-dir so the path survives docker/podman
    # (which does not forward the host environment).
    from oracle_reexec import inside_or_reexec  # noqa: WPS433
    sys.argv = [sys.argv[0]] + dirs + ["--dump-dir", tmp]
    inside_or_reexec("pillow", scratch=[tmp])
    return compare_in_image(fixtures, tmp)


if __name__ == "__main__":
    sys.exit(main())
