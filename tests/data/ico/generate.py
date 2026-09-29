#!/usr/bin/env python3
"""Generate ICO/CUR test fixtures.

Uses Pillow where it can write icons; assembles the rest byte by byte for
cases writers refuse (zero-alpha+AND, directory mismatch, corrupt entries).

Run from this directory:  python3 generate.py
"""

from __future__ import annotations

import os as _os
import struct
import sys as _sys
from pathlib import Path

_sys.path.insert(0, _os.path.join(
    _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow",
    scratch=[_os.path.dirname(_os.path.abspath(__file__))])

from PIL import Image  # noqa: E402

HERE = Path(__file__).parent


def write(name: str, data: bytes) -> None:
    (HERE / name).write_bytes(data)
    print(f"  {name} ({len(data)} bytes)")


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def icondir(type_: int, count: int) -> bytes:
    return u16(0) + u16(type_) + u16(count)


def direntry(w: int, h: int, size: int, offset: int, *,
             hotspot: tuple[int, int] | None = None, bpp: int = 32,
             colors: int = 0) -> bytes:
    wb = 0 if w >= 256 else w
    hb = 0 if h >= 256 else h
    if hotspot is None:
        planes, bitcount = 1, bpp
    else:
        planes, bitcount = hotspot
    return bytes([wb, hb, colors, 0]) + u16(planes) + u16(bitcount) + u32(size) + u32(offset)


def dib_32(w: int, h: int, pixels_rgba: list[tuple[int, int, int, int]],
           and_mask: list[int] | None = None) -> bytes:
    """Bottom-up 32-bpp BI_RGB DIB; biHeight = 2*h for AND mask."""
    xor_stride = (w * 4 + 3) & ~3
    mask_stride = ((w + 31) // 32) * 4
    xor = bytearray(xor_stride * h)
    for y in range(h):
        src_y = h - 1 - y
        for x in range(w):
            r, g, b, a = pixels_rgba[src_y * w + x]
            o = y * xor_stride + x * 4
            xor[o:o + 4] = bytes([b, g, r, a])
    mask = bytearray(mask_stride * h)
    if and_mask is not None:
        for y in range(h):
            src_y = h - 1 - y
            for x in range(w):
                if and_mask[src_y * w + x]:
                    mask[y * mask_stride + x // 8] |= 0x80 >> (x % 8)
    header = bytearray(40)
    header[0:4] = u32(40)
    header[4:8] = u32(w)
    header[8:12] = u32(h * 2)
    header[12:14] = u16(1)
    header[14:16] = u16(32)
    header[20:24] = u32(len(xor) + len(mask))
    return bytes(header) + bytes(xor) + bytes(mask)


def build_ico(entries: list[tuple[bytes, dict]]) -> bytes:
    """entries: list of (payload, meta dict with w,h,hotspot?,bpp?)."""
    type_ = 2 if any(e[1].get("hotspot") for e in entries) else 1
    n = len(entries)
    dir_end = 6 + 16 * n
    offset = dir_end
    body = bytearray()
    table = bytearray()
    for payload, meta in entries:
        table += direntry(
            meta["w"], meta["h"], len(payload), offset,
            hotspot=meta.get("hotspot"), bpp=meta.get("bpp", 32))
        body += payload
        offset += len(payload)
    return icondir(type_, n) + bytes(table) + bytes(body)


def solid(w: int, h: int, rgba: tuple[int, int, int, int]) -> list:
    return [rgba] * (w * h)


def main() -> None:
    # 16x16 opaque red DIB
    px = solid(16, 16, (255, 0, 0, 255))
    write("ico_16_dib_32.ico", build_ico([
        (dib_32(16, 16, px), {"w": 16, "h": 16}),
    ]))

    # Multi-entry: 16 and 32 DIB
    write("ico_multi_dib.ico", build_ico([
        (dib_32(16, 16, solid(16, 16, (255, 0, 0, 255))), {"w": 16, "h": 16}),
        (dib_32(32, 32, solid(32, 32, (0, 255, 0, 255))), {"w": 32, "h": 32}),
    ]))

    # 32-bpp zero alpha + meaningful AND (checkerboard transparency)
    w = h = 8
    px = solid(w, h, (0, 0, 255, 0))  # all alpha zero
    and_mask = [1 if (x + y) % 2 else 0 for y in range(h) for x in range(w)]
    write("ico_zero_alpha_and.ico", build_ico([
        (dib_32(w, h, px, and_mask), {"w": w, "h": h}),
    ]))

    # Directory says 16x16; DIB is 8x8
    small = dib_32(8, 8, solid(8, 8, (128, 128, 0, 255)))
    write("ico_dir_mismatch.ico", build_ico([
        (small, {"w": 16, "h": 16}),
    ]))

    # CUR with hotspot
    write("cur_hotspot.cur", build_ico([
        (dib_32(16, 16, solid(16, 16, (0, 0, 0, 255))),
         {"w": 16, "h": 16, "hotspot": (3, 5)}),
    ]))

    # PNG payload via Pillow
    img = Image.new("RGBA", (32, 32), (0, 128, 255, 200))
    from io import BytesIO
    bio = BytesIO()
    img.save(bio, format="PNG")
    png = bio.getvalue()
    write("ico_png_32.ico", build_ico([
        (png, {"w": 32, "h": 32}),
    ]))

    # Mixed PNG + DIB
    write("ico_mixed.ico", build_ico([
        (dib_32(16, 16, solid(16, 16, (255, 255, 0, 255))), {"w": 16, "h": 16}),
        (png, {"w": 32, "h": 32}),
    ]))

    # 256x256 encoded as 0 in directory (Pillow PNG)
    img256 = Image.new("RGBA", (256, 256), (40, 40, 40, 255))
    bio = BytesIO()
    img256.save(bio, format="PNG")
    png256 = bio.getvalue()
    write("ico_256_png.ico", build_ico([
        (png256, {"w": 256, "h": 256}),
    ]))

    # Corrupt: entry past EOF
    bad = bytearray(icondir(1, 1))
    bad += direntry(16, 16, 100, 1000)  # offset past end
    write("ico_corrupt_overflow.ico", bytes(bad) + b"\0" * 20)

    # Corrupt: count zero (should not probe as ICO)
    write("ico_corrupt_count0.ico", u16(0) + u16(1) + u16(0) + b"\0" * 16)

    # Truncated after directory
    write("ico_corrupt_truncated.ico",
          icondir(1, 1) + direntry(16, 16, 100, 22))

    print("done")


if __name__ == "__main__":
    main()
