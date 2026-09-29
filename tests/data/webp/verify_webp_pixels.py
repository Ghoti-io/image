#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Compare every WebP fixture's decoded pixels against libwebp.

Phase B/C/D/E gate for outside corpora (and a live check of the committed
fixtures): stills must be byte-identical to `dwebp -pam` after the same EXIF
orientation remap this library applies in gimg_item_decode; animations must
be byte-identical frame-for-frame to `anim_dump -pam`.

dwebp refuses animated files (UNSUPPORTED_FEATURE). Those go through
anim_dump. Outside trees mix good vectors with intentional bad inputs: a
file both sides refuse is skipped; a file we refuse and libwebp accepts is
a failure.

Arm the gate by flipping one fixture's red channel in dump_webp_raster, or
by disabling orientation apply in codec.c: dwebp/anim_dump will disagree and
this script must exit non-zero.

Usage:  python3 tests/data/webp/verify_webp_pixels.py [DIR ...]
        With no DIR, defaults to tests/data/webp. Extra directories (for
        example third_party/webp-refs) are walked recursively.
        GIMG_WEBP_DUMP points at dump_webp_raster (or pass --dump PATH).

Exit: 0 when every compared file matched; non-zero on mismatch or missing
dump tool.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))

# Committed corrupt fixtures: neither side is asked for pixels.
CORRUPT = {
    "corrupt_trunc.webp",
    "corrupt_riff_size.webp",
}

STATUS_OK_RE = re.compile(
    r"^(?P<fi>\d+)\t(?P<ii>\d+)\tok\t(?P<w>\d+)\t(?P<h>\d+)\t(?P<orient>\d+)$"
)
STATUS_LOAD_RE = re.compile(r"^(?P<fi>\d+)\t(?P<rest>.+)$")


def list_webp(root: str) -> list[str]:
    out: list[str] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            if name.endswith(".webp") and name not in CORRUPT:
                out.append(os.path.join(dirpath, name))
    return out


def label(path: str, roots: list[str]) -> str:
    abs_path = os.path.abspath(path)
    best = None
    for root in roots:
        root_abs = os.path.abspath(root)
        if abs_path == root_abs or abs_path.startswith(root_abs + os.sep):
            rel = os.path.relpath(abs_path, root_abs)
            if best is None or len(rel) < len(best):
                best = rel
    return best if best is not None else os.path.basename(path)


def is_animated(path: str) -> bool:
    """True when the file carries an ANIM/ANMF bitstream (dwebp cannot decode)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20 or data[0:4] != b"RIFF" or data[8:12] != b"WEBP":
        return False
    pos = 12
    end = min(len(data), 8 + struct.unpack_from("<I", data, 4)[0])
    while pos + 8 <= end:
        tag = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        payload = pos + 8
        if tag == b"VP8X" and size >= 1 and payload < len(data):
            # Bit 1 of the flags byte is Animation.
            if data[payload] & 0x02:
                return True
        if tag in (b"ANIM", b"ANMF"):
            return True
        step = 8 + size + (size & 1)
        if step <= 0 or step > end - pos:
            break
        pos += step
    return False


def read_pam_rgba(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as f:
        data = f.read()
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


def apply_orientation(rgba: bytes, width: int, height: int, orient: int) -> tuple[int, int, bytes]:
    """Remap bitstream pixels to display pixels (match gimg_ops_apply_orientation)."""
    if orient in (0, 1):
        return width, height, rgba
    bpp = 4
    src_w, src_h = width, height
    swaps = orient in (5, 6, 7, 8)
    dst_w = src_h if swaps else src_w
    dst_h = src_w if swaps else src_h
    out = bytearray(dst_w * dst_h * bpp)

    def get(sx: int, sy: int) -> bytes:
        i = (sy * src_w + sx) * bpp
        return rgba[i:i + bpp]

    for dy in range(dst_h):
        for dx in range(dst_w):
            if orient == 2:  # flip H
                sx, sy = src_w - 1 - dx, dy
            elif orient == 3:  # rotate 180
                sx, sy = src_w - 1 - dx, src_h - 1 - dy
            elif orient == 4:  # flip V
                sx, sy = dx, src_h - 1 - dy
            elif orient == 5:  # transpose
                sx, sy = dy, dx
            elif orient == 6:  # 90 CW
                sx, sy = dy, src_h - 1 - dx
            elif orient == 7:  # transverse
                sx, sy = src_w - 1 - dy, src_h - 1 - dx
            elif orient == 8:  # 90 CCW
                sx, sy = src_w - 1 - dy, dx
            else:
                sx, sy = dx, dy
            i = (dy * dst_w + dx) * bpp
            out[i:i + bpp] = get(sx, sy)
    return dst_w, dst_h, bytes(out)


def compare_rgba(got: bytes, want: bytes, width: int, height: int) -> str | None:
    if len(got) != len(want):
        return "decoded %d bytes, expected %d" % (len(got), len(want))
    if len(want) != width * height * 4:
        return "size %d does not match %dx%d" % (len(want), width, height)
    for off in range(0, len(got), 4):
        a = got[off:off + 4]
        b = want[off:off + 4]
        if a == b:
            continue
        # Colour under a fully transparent pixel is not part of the display
        # image. dwebp keeps the YUV residue; anim_dump clears it to zero.
        # Agreeing on alpha 0 is enough (same rule as verify_ico_pixels.py).
        if a[3] == 0 and b[3] == 0:
            continue
        pixel = off // 4
        x = pixel % width
        y = pixel // width
        return (
            "pixel (%d,%d) is rgba(%d,%d,%d,%d), expected rgba(%d,%d,%d,%d)"
            % (x, y, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]))
    return None


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


def run_dump(dump: str, outdir: str, paths: list[str]):
    """Run dump_webp_raster; return (by_index, load_fail)."""
    by_index: dict[int, list[dict]] = {}
    load_fail: dict[int, str] = {}
    proc = subprocess.run(
        [dump, outdir] + paths,
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "dump_webp_raster failed (%d): %s"
            % (proc.returncode, proc.stderr.strip() or proc.stdout.strip()))
    for line in proc.stdout.splitlines():
        m = STATUS_OK_RE.match(line)
        if m:
            fi = int(m.group("fi"))
            ii = int(m.group("ii"))
            by_index.setdefault(fi, []).append({
                "index": ii,
                "width": int(m.group("w")),
                "height": int(m.group("h")),
                "orient": int(m.group("orient")),
                "rgba_path": os.path.join(
                    outdir, "%d.%d.rgba" % (fi, ii)),
            })
            continue
        m = STATUS_LOAD_RE.match(line)
        if m and "\tok\t" not in line:
            fi = int(m.group("fi"))
            rest = m.group("rest")
            # "fi\tii\tdecode:..." also matches STATUS_LOAD_RE with rest
            # containing a tab; keep the whole rest as the reason.
            if rest.startswith("load:") or rest == "unreadable" or \
                    rest == "stream-failed" or "decode:" in rest or \
                    rest.endswith("cannot-write-dump"):
                load_fail[fi] = rest
    return by_index, load_fail


def dwebp_pam(path: str, out_pam: str) -> None:
    proc = subprocess.run(
        ["dwebp", path, "-pam", "-o", out_pam],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "dwebp failed (%d): %s"
            % (proc.returncode, (proc.stderr or proc.stdout).strip()))


def anim_dump_pams(path: str, folder: str, prefix: str) -> list[str]:
    # Clear prior frames with this prefix.
    for name in os.listdir(folder):
        if name.startswith(prefix) and name.endswith(".pam"):
            os.remove(os.path.join(folder, name))
    proc = subprocess.run(
        ["anim_dump", "-pam", "-folder", folder, "-prefix", prefix, path],
        check=False,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "anim_dump failed (%d): %s"
            % (proc.returncode, (proc.stderr or proc.stdout).strip()))
    frames = sorted(
        n for n in os.listdir(folder)
        if n.startswith(prefix) and n.endswith(".pam"))
    if not frames:
        raise RuntimeError("anim_dump wrote no PAM frames for %s" % path)
    return [os.path.join(folder, n) for n in frames]


def compare_in_image(
        paths: list[str], roots: list[str], dump_dir: str,
        by_index: dict[int, list[dict]], load_fail: dict[int, str]) -> int:
    errors: list[str] = []
    ok = 0
    skipped = 0
    scratch = os.path.join(dump_dir, "oracle")
    os.makedirs(scratch, exist_ok=True)

    for fi, path in enumerate(paths):
        shown = label(path, roots)
        external = not os.path.abspath(path).startswith(
            os.path.abspath(HERE) + os.sep)
        ours = sorted(by_index.get(fi, []), key=lambda e: e["index"])
        fail_reason = load_fail.get(fi)

        if is_animated(path):
            try:
                ref_paths = anim_dump_pams(
                    path, scratch, "f%d_" % fi)
            except RuntimeError as exc:
                if fail_reason:
                    print("  skip %s (both refuse: %s / %s)"
                          % (shown, fail_reason, exc))
                    skipped += 1
                    continue
                if external:
                    print("  skip %s (anim_dump refuses: %s)" % (shown, exc))
                    skipped += 1
                    continue
                errors.append("%s: anim_dump failed: %s" % (shown, exc))
                continue
            if fail_reason:
                errors.append(
                    "%s: we %s but anim_dump decoded %d frames"
                    % (shown, fail_reason, len(ref_paths)))
                continue
            if len(ours) != len(ref_paths):
                errors.append(
                    "%s: we dumped %d frames, anim_dump wrote %d"
                    % (shown, len(ours), len(ref_paths)))
                continue
            frame_ok = True
            for ent, ref_pam in zip(ours, ref_paths):
                try:
                    rw, rh, ref = read_pam_rgba(ref_pam)
                except ValueError as exc:
                    errors.append("%s[%d]: %s" % (shown, ent["index"], exc))
                    frame_ok = False
                    break
                with open(ent["rgba_path"], "rb") as f:
                    want = f.read()
                if (rw, rh) != (ent["width"], ent["height"]):
                    errors.append(
                        "%s[%d]: anim_dump is %dx%d, dump is %dx%d"
                        % (shown, ent["index"], rw, rh, ent["width"],
                           ent["height"]))
                    frame_ok = False
                    break
                problem = compare_rgba(ref, want, rw, rh)
                if problem:
                    errors.append(
                        "%s[%d] %dx%d: %s" % (
                            shown, ent["index"], rw, rh, problem))
                    frame_ok = False
                    break
            if frame_ok:
                ok += 1
                print("  ok  %s (%d frames)" % (shown, len(ours)))
            continue

        # Still image: dwebp.
        ref_pam = os.path.join(scratch, "still_%d.pam" % fi)
        try:
            dwebp_pam(path, ref_pam)
        except RuntimeError as exc:
            if fail_reason:
                print("  skip %s (both refuse: %s / %s)"
                      % (shown, fail_reason, exc))
                skipped += 1
                continue
            if external:
                print("  skip %s (dwebp refuses: %s)" % (shown, exc))
                skipped += 1
                continue
            errors.append("%s: dwebp failed: %s" % (shown, exc))
            continue
        if fail_reason:
            errors.append(
                "%s: we %s but dwebp decoded it" % (shown, fail_reason))
            continue
        if len(ours) != 1:
            errors.append(
                "%s: expected one still dump, got %d" % (shown, len(ours)))
            continue
        ent = ours[0]
        try:
            rw, rh, ref = read_pam_rgba(ref_pam)
        except ValueError as exc:
            errors.append("%s: %s" % (shown, exc))
            continue
        rw, rh, ref = apply_orientation(ref, rw, rh, ent["orient"])
        with open(ent["rgba_path"], "rb") as f:
            want = f.read()
        if (rw, rh) != (ent["width"], ent["height"]):
            errors.append(
                "%s: oriented dwebp is %dx%d, dump is %dx%d"
                % (shown, rw, rh, ent["width"], ent["height"]))
            continue
        problem = compare_rgba(ref, want, rw, rh)
        if problem:
            errors.append("%s %dx%d: %s" % (shown, rw, rh, problem))
            continue
        ok += 1
        print("  ok  %s" % shown)

    for e in errors:
        print("  %s" % e, file=sys.stderr)
    print(
        "%d ok, %d skipped, %d failed (%d files)"
        % (ok, skipped, len(errors), len(paths)))
    if errors:
        print("WebP pixel verification FAILED", file=sys.stderr)
        return 1
    if ok == 0:
        print("WebP pixel verification matched nothing", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "dirs", nargs="*", default=[HERE],
        help="fixture directories (default: tests/data/webp)")
    ap.add_argument("--dump", default=None, help="path to dump_webp_raster")
    ap.add_argument(
        "--dump-dir", default=None,
        help="host-prepared dump directory (set across oracle re-exec)")
    ap.add_argument(
        "--manifest", default=None,
        help="path list written on the host (survives re-exec)")
    args = ap.parse_args()
    roots = [os.path.abspath(d) for d in (args.dirs or [HERE])]
    for d in roots:
        if not os.path.isdir(d):
            print("not a directory: %s" % d, file=sys.stderr)
            return 2

    if args.manifest and os.path.isfile(args.manifest):
        with open(args.manifest, "r", encoding="utf-8") as f:
            paths = [line.rstrip("\n") for line in f if line.strip()]
    else:
        paths = []
        for root in roots:
            paths.extend(list_webp(root))
        # Prefer committed fixtures first.
        paths = sorted(set(paths), key=lambda p: (0 if HERE in p else 1, p))

    if not paths:
        print("no WebP fixtures in %s" % ", ".join(roots), file=sys.stderr)
        return 2

    # Already prepared (re-exec argv).
    if args.dump_dir and os.path.isdir(args.dump_dir):
        from oracle_reexec import inside_or_reexec  # noqa: WPS433
        inside_or_reexec("libwebp", scratch=[args.dump_dir])
        meta = os.path.join(args.dump_dir, "dump.meta")
        by_index: dict[int, list[dict]] = {}
        load_fail: dict[int, str] = {}
        with open(meta, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2 and parts[1] == "ok":
                    fi, _, ii, w, h, orient = (
                        int(parts[0]), parts[1], int(parts[2]), int(parts[3]),
                        int(parts[4]), int(parts[5]))
                    by_index.setdefault(fi, []).append({
                        "index": ii,
                        "width": w,
                        "height": h,
                        "orient": orient,
                        "rgba_path": os.path.join(
                            args.dump_dir, "%d.%d.rgba" % (fi, ii)),
                    })
                elif len(parts) >= 2:
                    load_fail[int(parts[0])] = parts[1]
        return compare_in_image(
            paths, roots, args.dump_dir, by_index, load_fail)

    try:
        dump = find_dump(args.dump)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="webp-pixels-")
    try:
        by_index, load_fail = run_dump(dump, tmp, paths)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    manifest = os.path.join(tmp, "paths.txt")
    with open(manifest, "w", encoding="utf-8") as f:
        for p in paths:
            f.write(p + "\n")
    meta = os.path.join(tmp, "dump.meta")
    with open(meta, "w", encoding="utf-8") as f:
        for fi, ents in sorted(by_index.items()):
            for ent in sorted(ents, key=lambda e: e["index"]):
                f.write("%d\tok\t%d\t%d\t%d\t%d\n" % (
                    fi, ent["index"], ent["width"], ent["height"],
                    ent["orient"]))
        for fi, reason in sorted(load_fail.items()):
            f.write("%d\t%s\n" % (fi, reason))

    from oracle_reexec import inside_or_reexec  # noqa: WPS433
    sys.argv = (
        [sys.argv[0]] + roots +
        ["--dump-dir", tmp, "--manifest", manifest])
    inside_or_reexec("libwebp", scratch=[tmp])
    # If re-exec did not replace us (host mode), compare here.
    return compare_in_image(paths, roots, tmp, by_index, load_fail)


if __name__ == "__main__":
    sys.exit(main())
