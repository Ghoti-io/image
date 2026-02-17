#!/usr/bin/env python3
"""
Encode oracle using Pillow: same CLI as encode_libjpeg_baseline_scan for the
"write JPEG file" case. Creates a grayscale image pixel (x,y) = (x+y)&0xFF and
saves as baseline JPEG.

Used by C++ tests when the libjpeg-based oracle is not built. Pillow does not
output raw scan bytes; this script only writes the JPEG file.

Usage: encode_oracle_pillow.py <width> <height> <quality> <scan_tmp> <restart_interval> [out.jpg]
  If out.jpg is given, the JPEG is written there. scan_tmp and restart_interval are ignored.

Requires: Pillow (pip install Pillow).
"""
from __future__ import annotations

import argparse
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description="Encode grayscale baseline JPEG with Pillow (same image as C oracle)")
    ap.add_argument("width", type=int, help="Image width")
    ap.add_argument("height", type=int, help="Image height")
    ap.add_argument("quality", type=int, help="JPEG quality 1-100")
    ap.add_argument("scan_tmp", help="Ignored (Pillow does not output scan bytes)")
    ap.add_argument("restart_interval", type=int, help="Ignored")
    ap.add_argument("jpeg_path", nargs="?", default="", help="Output JPEG path (optional)")
    args = ap.parse_args()

    if args.jpeg_path is None:
        args.jpeg_path = ""

    try:
        from PIL import Image
    except ImportError:
        print("encode_oracle_pillow.py requires Pillow: pip install Pillow", file=sys.stderr)
        return 1

    w, h = args.width, args.height
    if w <= 0 or h <= 0:
        print("Invalid dimensions", file=sys.stderr)
        return 1

    # Same image as encode_libjpeg_baseline_scan: (x+y)&0xFF
    data = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            data[y * w + x] = (x + y) & 0xFF
    im = Image.frombytes("L", (w, h), bytes(data))

    if not args.jpeg_path:
        return 0

    try:
        im.save(args.jpeg_path, "JPEG", quality=args.quality, optimize=False)
    except Exception as e:
        print(f"Save failed: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
