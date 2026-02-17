#!/usr/bin/env python3
"""
Decode oracle using Pillow: same CLI and output as dump_jpeg_pixels_ref.

Used by C++ tests when the libjpeg-based oracle is not built. Decodes the JPEG
with Pillow, computes FNV-1a hash over pixels (L, RGBA with A=255, or CMYK),
and optionally writes a .raw file (mode byte, 4b width LE, 4b height LE, pixels).

Usage: decode_oracle_pillow.py [ -o out.raw ] <file.jpg>
Output: HASH <hex16> WIDTH <w> HEIGHT <h> MODE <L|RGBA|CMYK>

Requires: Pillow (pip install Pillow).

CMYK: Pillow uses 0=no ink; JPEG/libjpeg use Adobe polarity (255=no ink in file).
We invert CMYK when writing .raw so the oracle matches raw file bytes (libjpeg/our decoder).
"""
from __future__ import annotations

import argparse
import struct
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3

RAW_MODE_L = 0
RAW_MODE_RGB = 1
RAW_MODE_CMYK = 2


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFF_FFFFFFFF
    return h


def main() -> int:
    ap = argparse.ArgumentParser(description="Decode JPEG with Pillow, output hash and optional .raw")
    ap.add_argument("jpeg_path", help="Path to JPEG file")
    ap.add_argument("-o", "--raw", dest="raw_path", default="", help="Write .raw to this path")
    args = ap.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("decode_oracle_pillow.py requires Pillow: pip install Pillow", file=sys.stderr)
        return 1

    try:
        im = Image.open(args.jpeg_path)
        im.load()
    except Exception as e:
        print(f"{args.jpeg_path}: {e}", file=sys.stderr)
        return 1

    w, h = im.size
    if w <= 0 or h <= 0 or w > 65535 or h > 65535:
        print(f"{args.jpeg_path}: unsupported dimensions", file=sys.stderr)
        return 1

    mode_str = "L"
    raw_mode = RAW_MODE_L
    pixel_bytes_for_raw = w * h

    if im.mode == "L":
        pixels_1 = im.tobytes()
        hash_bytes = pixels_1
    elif im.mode in ("RGB", "RGBA"):
        mode_str = "RGBA"
        raw_mode = RAW_MODE_RGB
        pixel_bytes_for_raw = w * h * 3
        rgb = im.convert("RGB")
        rgb_bytes = rgb.tobytes()
        # Hash over RGBA (A=255), same as dump_jpeg_pixels_ref
        hash_bytes = bytearray(w * h * 4)
        for i in range(w * h):
            hash_bytes[i * 4 + 0] = rgb_bytes[i * 3 + 0]
            hash_bytes[i * 4 + 1] = rgb_bytes[i * 3 + 1]
            hash_bytes[i * 4 + 2] = rgb_bytes[i * 3 + 2]
            hash_bytes[i * 4 + 3] = 255
        hash_bytes = bytes(hash_bytes)
    elif im.mode == "CMYK":
        mode_str = "CMYK"
        raw_mode = RAW_MODE_CMYK
        pixel_bytes_for_raw = w * h * 4
        hash_bytes = im.tobytes()
    else:
        print(f"{args.jpeg_path}: unsupported mode {im.mode}", file=sys.stderr)
        return 1

    fnv = fnv1a(hash_bytes)

    if args.raw_path:
        with open(args.raw_path, "wb") as f:
            f.write(struct.pack("<BII", raw_mode, w, h))
            if raw_mode == RAW_MODE_L:
                f.write(im.tobytes())
            elif raw_mode == RAW_MODE_RGB:
                f.write(im.convert("RGB").tobytes())
            else:
                # CMYK: Pillow gives 0=no ink; file/libjpeg use 255=no ink. Invert
                # so .raw matches raw decoder output.
                cmyk = im.tobytes()
                f.write(bytes(255 - b for b in cmyk))

    if not args.raw_path:
        print(f"HASH {fnv:016x} WIDTH {w} HEIGHT {h} MODE {mode_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
