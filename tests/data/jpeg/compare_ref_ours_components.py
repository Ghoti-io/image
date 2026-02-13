#!/usr/bin/env python3
"""
Compare libjpeg ref Y/Cb/Cr component dumps to our decoder's dumps.

Ref: run dump_jpeg_pixels_ref_debug (instrumented libjpeg) with
  DUMP_JPEG_COMPONENTS_REF=<ref_dir>
  → writes ref_Y.raw, ref_Cb.raw, ref_Cr.raw (same format: 4b w LE, 4b h LE, raw).

Ours: run dump_jpeg_raster with DUMP_JPEG_COMPONENTS=<ours_dir>
  → writes Y.raw, Cb.raw, Cr.raw (same format).

This script compares the two directories and reports per-component:
  - dimensions match / mismatch
  - number of differing samples, max absolute difference
  - first differing (x, y) and ref vs ours values

Usage:
  python3 compare_ref_ours_components.py <ref_dir> <ours_dir>

Example (after generating both dumps):
  DUMP_JPEG_COMPONENTS_REF=/tmp/ref tests/data/jpeg/dump_jpeg_pixels_ref_debug progressive_sample.jpg -o /dev/null
  DUMP_JPEG_COMPONENTS=/tmp/ours dump_jpeg_raster progressive_sample.jpg
  python3 compare_ref_ours_components.py /tmp/ref /tmp/ours
"""
from __future__ import annotations

import argparse
import struct
import sys


def read_component_raw(path: str) -> tuple[bytes, int, int]:
    """Read .raw: 4 bytes LE width, 4 bytes LE height, then w*h bytes. Return (data, w, h)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ValueError(f"{path}: too short")
    w, h = struct.unpack("<II", data[:8])
    expected = 8 + w * h
    if len(data) != expected:
        raise ValueError(f"{path}: expected {expected} bytes, got {len(data)}")
    return data[8:], w, h


def compare_component(name: str, ref_data: bytes, ref_w: int, ref_h: int,
                      ours_data: bytes, ours_w: int, ours_h: int,
                      verbose: bool) -> bool:
    """Return True if match. Print stats and first diff."""
    if ref_w != ours_w or ref_h != ours_h:
        print(f"  {name}: dimension mismatch ref {ref_w}x{ref_h} vs ours {ours_w}x{ours_h}")
        return False
    n = ref_w * ref_h
    if len(ref_data) != n or len(ours_data) != n:
        print(f"  {name}: length mismatch")
        return False
    diffs = []
    max_abs = 0
    for i in range(n):
        a, b = ref_data[i], ours_data[i]
        if a != b:
            d = abs(int(a) - int(b))
            if d > max_abs:
                max_abs = d
            y, x = divmod(i, ref_w)
            diffs.append((x, y, a, b, d))
    if not diffs:
        print(f"  {name}: match ({ref_w}x{ref_h})")
        return True
    print(f"  {name}: {len(diffs)}/{n} differ, max_abs_diff={max_abs}")
    first = diffs[0]
    print(f"    first diff at ({first[0]},{first[1]}): ref={first[2]} ours={first[3]} |diff|={first[4]}")
    if verbose and diffs:
        print(f"    first row (x=0..{min(15, ref_w-1)}): ref then ours")
        row0_diffs = [d for d in diffs if d[1] == 0][:16]
        ref_vals = [ref_data[i] for i in range(min(16, ref_w))]
        ours_vals = [ours_data[i] for i in range(min(16, ref_w))]
        print(f"      ref:  {ref_vals}")
        print(f"      ours: {ours_vals}")
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare ref vs ours Y/Cb/Cr component dumps")
    ap.add_argument("ref_dir", help="Directory with ref_Y.raw, ref_Cb.raw, ref_Cr.raw")
    ap.add_argument("ours_dir", help="Directory with Y.raw, Cb.raw, Cr.raw")
    ap.add_argument("-v", "--verbose", action="store_true", help="Print first row of first component that differs")
    args = ap.parse_args()

    components = [
        ("Y", "ref_Y.raw", "Y.raw"),
        ("Cb", "ref_Cb.raw", "Cb.raw"),
        ("Cr", "ref_Cr.raw", "Cr.raw"),
    ]
    all_ok = True
    for name, ref_name, ours_name in components:
        ref_path = f"{args.ref_dir.rstrip('/')}/{ref_name}"
        ours_path = f"{args.ours_dir.rstrip('/')}/{ours_name}"
        try:
            ref_data, ref_w, ref_h = read_component_raw(ref_path)
            ours_data, ours_w, ours_h = read_component_raw(ours_path)
        except FileNotFoundError as e:
            print(f"  {name}: missing file {e.filename}")
            all_ok = False
            continue
        except ValueError as e:
            print(f"  {name}: {e}")
            all_ok = False
            continue
        if not compare_component(name, ref_data, ref_w, ref_h, ours_data, ours_w, ours_h, args.verbose):
            all_ok = False
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
