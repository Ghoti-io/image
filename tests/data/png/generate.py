#!/usr/bin/env python3
"""
Generate PNG test data for decode tests. Single source of truth for all PNG
variants. Writes to the same directory as this script (tests/data/png/).

Run from repo root: python3 tests/data/png/generate.py
Or from this dir: python3 generate.py
"""
import os
import struct
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))


def png_chunk(ctype: bytes, payload: bytes) -> bytes:
    """Build a PNG chunk: 4-byte length (big-endian), 4-byte type, payload, 4-byte CRC."""
    data = ctype + payload
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", crc)


def adler32_be(data: bytes) -> bytes:
    """Adler-32 of data, 4 big-endian bytes (zlib trailer)."""
    v = zlib.adler32(data) & 0xFFFFFFFF
    return struct.pack(">I", v)


def deflate_stored_block(data: bytes) -> bytes:
    """Single DEFLATE stored block (BTYPE=00, BFINAL=1): LEN, NLEN, payload."""
    n = len(data)
    nlen = 0xFFFF ^ n
    return bytes([0x01, n & 0xFF, (n >> 8) & 0xFF, nlen & 0xFF, (nlen >> 8) & 0xFF]) + data


def idat_zlib(raw_rows: bytes) -> bytes:
    """Wrap raw row data in zlib (78 01 + stored block + Adler-32)."""
    raw_deflate = deflate_stored_block(raw_rows)
    zlib_header = bytes([0x78, 0x01])
    zlib_trailer = adler32_be(raw_rows)
    return zlib_header + raw_deflate + zlib_trailer


def write_png(name: str, png_bytes: bytes) -> None:
    path = os.path.join(SCRIPT_DIR, name)
    with open(path, "wb") as f:
        f.write(png_bytes)
    print("Wrote", path)


def main() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")

    # ---- 1x1 grayscale 8-bit (black) ----
    raw_1x1_gray = bytes([0x00, 0x00])
    idat_1x1_gray = idat_zlib(raw_1x1_gray)
    ihdr_1x1_gray = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    png_1x1_gray = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"IDAT", idat_1x1_gray)
        + iend
    )
    write_png("png_1x1_gray.png", png_1x1_gray)

    # ---- 1x1 palette + tRNS ----
    ihdr_palette = struct.pack(">IIBBBBB", 1, 1, 8, 3, 0, 0, 0)
    plte = bytes([0x11, 0x22, 0x33])
    trns = bytes([0x80])
    raw_palette = bytes([0x00, 0x00])
    idat_palette = idat_zlib(raw_palette)
    png_palette = (
        signature
        + png_chunk(b"IHDR", ihdr_palette)
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"tRNS", trns)
        + png_chunk(b"IDAT", idat_palette)
        + iend
    )
    write_png("png_1x1_palette.png", png_palette)

    # ---- 1x1 16-bit grayscale (pixel value 0x1234) ----
    raw_16gray = bytes([0x00, 0x12, 0x34])
    idat_16gray = idat_zlib(raw_16gray)
    ihdr_16gray = struct.pack(">IIBBBBB", 1, 1, 16, 0, 0, 0, 0)
    png_16gray = (
        signature
        + png_chunk(b"IHDR", ihdr_16gray)
        + png_chunk(b"IDAT", idat_16gray)
        + iend
    )
    write_png("png_16bit_gray.png", png_16gray)

    # ---- 1x1 RGBA 8-bit (R=0x11 G=0x22 B=0x33 A=0x80) ----
    raw_rgba = bytes([0x00, 0x11, 0x22, 0x33, 0x80])
    idat_rgba = idat_zlib(raw_rgba)
    ihdr_rgba = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    png_rgba = (
        signature
        + png_chunk(b"IHDR", ihdr_rgba)
        + png_chunk(b"IDAT", idat_rgba)
        + iend
    )
    write_png("png_1x1_rgba.png", png_rgba)

    # ---- 1x1 16-bit RGBA (R=0x1234 G=0x5678 B=0x9ABC A=0xDEF0) ----
    raw_16rgba = bytes([
        0x00,
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
    ])
    idat_16rgba = idat_zlib(raw_16rgba)
    ihdr_16rgba = struct.pack(">IIBBBBB", 1, 1, 16, 6, 0, 0, 0)
    png_16rgba = (
        signature
        + png_chunk(b"IHDR", ihdr_16rgba)
        + png_chunk(b"IDAT", idat_16rgba)
        + iend
    )
    write_png("png_16bit_rgba.png", png_16rgba)

    # ---- 1x1 RGB 8-bit (color_type 2): R=0x11 G=0x22 B=0x33 ----
    raw_rgb = bytes([0x00, 0x11, 0x22, 0x33])
    idat_rgb = idat_zlib(raw_rgb)
    ihdr_rgb = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    png_rgb = (
        signature
        + png_chunk(b"IHDR", ihdr_rgb)
        + png_chunk(b"IDAT", idat_rgb)
        + iend
    )
    write_png("png_1x1_rgb.png", png_rgb)

    # ---- 1x1 RGB 16-bit (color_type 2): R=0x1234 G=0x5678 B=0x9ABC ----
    raw_16rgb = bytes([
        0x00,
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
    ])
    idat_16rgb = idat_zlib(raw_16rgb)
    ihdr_16rgb = struct.pack(">IIBBBBB", 1, 1, 16, 2, 0, 0, 0)
    png_16rgb = (
        signature
        + png_chunk(b"IHDR", ihdr_16rgb)
        + png_chunk(b"IDAT", idat_16rgb)
        + iend
    )
    write_png("png_16bit_rgb.png", png_16rgb)

    # ---- 1x1 grayscale+alpha 8-bit (color_type 4): G=0x80 A=0xCC ----
    raw_ga = bytes([0x00, 0x80, 0xCC])
    idat_ga = idat_zlib(raw_ga)
    ihdr_ga = struct.pack(">IIBBBBB", 1, 1, 8, 4, 0, 0, 0)
    png_ga = (
        signature
        + png_chunk(b"IHDR", ihdr_ga)
        + png_chunk(b"IDAT", idat_ga)
        + iend
    )
    write_png("png_1x1_grayalpha.png", png_ga)

    # ---- 1x1 grayscale+alpha 16-bit (color_type 4): G=0x1234 A=0xDEF0 ----
    raw_16ga = bytes([
        0x00,
        0x12, 0x34, 0xDE, 0xF0,
    ])
    idat_16ga = idat_zlib(raw_16ga)
    ihdr_16ga = struct.pack(">IIBBBBB", 1, 1, 16, 4, 0, 0, 0)
    png_16ga = (
        signature
        + png_chunk(b"IHDR", ihdr_16ga)
        + png_chunk(b"IDAT", idat_16ga)
        + iend
    )
    write_png("png_16bit_grayalpha.png", png_16ga)

    # ---- 1x1 gray + sRGB chunk (rendering intent 0 = Perceptual) ----
    srgb_payload = bytes([0x00])
    png_srgb = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"sRGB", srgb_payload)
        + png_chunk(b"IDAT", idat_1x1_gray)
        + iend
    )
    write_png("png_srgb.png", png_srgb)

    # ---- 1x1 gray + eXIf chunk (minimal EXIF-like bytes for round-trip test) ----
    # Minimal eXIf: a few bytes so we can verify attach to meta_raw.
    exif_payload = bytes([0x00, 0x01, 0x02, 0x03, 0x04, 0x05])
    png_exif = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"eXIf", exif_payload)
        + png_chunk(b"IDAT", idat_1x1_gray)
        + iend
    )
    write_png("png_exif.png", png_exif)

    # ---- 1x1 gray + iCCP chunk (tiny zlib-compressed "profile" for color info test) ----
    # iCCP: profile name (null-term) + compression method (0 = deflate) + zlib stream.
    profile_data = b"minimal_icc"
    iccp_name = b"0\0"
    iccp_comp = bytes([0x00])
    iccp_zlib = zlib.compress(profile_data)
    iccp_payload = iccp_name + iccp_comp + iccp_zlib
    png_iccp = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"iCCP", iccp_payload)
        + png_chunk(b"IDAT", idat_1x1_gray)
        + iend
    )
    write_png("png_iccp.png", png_iccp)

    # ---- 2-frame APNG (default image is first frame): 1x1 gray frame 0 (black), frame 1 (gray 0x80) ----
    # acTL: num_frames=2, num_plays=0 (infinite)
    actl = struct.pack(">II", 2, 0)
    # fcTL frame 0: seq=0, 1x1, x_off=0, y_off=0, delay_num=50, delay_den=100, dispose=0, blend=0
    fctl0 = struct.pack(
        ">IIIIIHHBB",
        0, 1, 1, 0, 0, 50, 100, 0, 0,
    )
    # Frame 0 image data (same as 1x1 gray black)
    idat_frame0 = idat_zlib(raw_1x1_gray)
    # fcTL frame 1: seq=1, 1x1, delay_num=25, delay_den=100, dispose=1 (background), blend=1 (over)
    fctl1 = struct.pack(
        ">IIIIIHHBB",
        1, 1, 1, 0, 0, 25, 100, 1, 1,
    )
    # Frame 1 image data: 1x1 gray value 0x80
    raw_frame1 = bytes([0x00, 0x80])
    frame1_zlib = idat_zlib(raw_frame1)
    fdat_payload = struct.pack(">I", 2) + frame1_zlib  # seq=2
    apng_2frame = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"acTL", actl)
        + png_chunk(b"fcTL", fctl0)
        + png_chunk(b"IDAT", idat_frame0)
        + png_chunk(b"fcTL", fctl1)
        + png_chunk(b"fdAT", fdat_payload)
        + iend
    )
    write_png("png_apng_2frame.png", apng_2frame)

    # ---- 3-frame APNG (dispose/blend variants): gray 0, 0x80, 0xC0 ----
    # Frame 0: NONE/SOURCE; frame 1: BACKGROUND/OVER; frame 2: PREVIOUS/OVER
    actl3 = struct.pack(">II", 3, 0)
    fctl0_3 = struct.pack(">IIIIIHHBB", 0, 1, 1, 0, 0, 50, 100, 0, 0)
    fctl1_3 = struct.pack(">IIIIIHHBB", 1, 1, 1, 0, 0, 25, 100, 1, 1)
    # fcTL sequence numbers share counter with fdAT: 0,1, then fdAT=2, then fcTL=3, fdAT=4
    fctl2_3 = struct.pack(">IIIIIHHBB", 3, 1, 1, 0, 0, 10, 100, 2, 1)
    raw_f2 = bytes([0x00, 0xC0])
    frame2_zlib = idat_zlib(raw_f2)
    # fdAT seq: after fcTL(0),fcTL(1) next_sequence=2; after fdAT next_sequence=3; after fcTL(2) next_sequence=4
    fdat1 = struct.pack(">I", 2) + frame1_zlib   # first fdAT (frame 1) seq=2
    fdat2 = struct.pack(">I", 4) + frame2_zlib   # second fdAT (frame 2) seq=4
    apng_3frame = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"acTL", actl3)
        + png_chunk(b"fcTL", fctl0_3)
        + png_chunk(b"IDAT", idat_frame0)
        + png_chunk(b"fcTL", fctl1_3)
        + png_chunk(b"fdAT", fdat1)
        + png_chunk(b"fcTL", fctl2_3)
        + png_chunk(b"fdAT", fdat2)
        + iend
    )
    write_png("png_apng_3frame.png", apng_3frame)


if __name__ == "__main__":
    main()
