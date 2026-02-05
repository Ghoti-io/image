#!/usr/bin/env python3
"""
Generate the minimal 1x1 grayscale PNG used by test_png_decode.cpp.

This script is the single source of truth for the test bytes. Run:
  python3 gen_1x1_png.py
to print hex and write png_1x1_gray.png. The test uses the STORED_BLOCK form
(no compression) so the IDAT payload is deterministic and matches the C++ array.

Note: zlib.compress() uses default compression and produces different bytes
(78 9C ...). We build a zlib stream with a stored block (78 01 + 7-byte deflate
+ Adler-32) so the output matches test_png_decode.cpp exactly.
"""
import struct
import zlib

def png_chunk(ctype: bytes, payload: bytes) -> bytes:
    """Build a PNG chunk: 4-byte length (big-endian), 4-byte type, payload, 4-byte CRC."""
    data = ctype + payload
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", crc)

def adler32_be(data: bytes) -> bytes:
    """Adler-32 of data, returned as 4 big-endian bytes (for zlib trailer)."""
    v = zlib.adler32(data) & 0xFFFFFFFF
    return struct.pack(">I", v)

def main():
    # 1x1 grayscale 8-bit: filter byte 0, pixel 0 (black)
    raw_rows = bytes([0x00, 0x00])

    # Build zlib stream with a STORED block (BTYPE=00, BFINAL=1, LEN=2, NLEN=~2, payload 00 00).
    # Raw DEFLATE: 01 02 00 FD FF 00 00 (7 bytes). No Python XOR or signed quirks.
    raw_deflate = bytes([0x01, 0x02, 0x00, 0xFD, 0xFF, 0x00, 0x00])
    zlib_header = bytes([0x78, 0x01])  # CM=8, CINFO=0, FLEVEL=0, FDICT=0, FCHECK
    zlib_trailer = adler32_be(raw_rows)
    idat_payload = zlib_header + raw_deflate + zlib_trailer

    # IHDR: width=1, height=1, bit_depth=8, color_type=0 (gray), compression=0, filter=0, interlace=0
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    signature = b"\x89PNG\r\n\x1a\n"
    ihdr_chunk = png_chunk(b"IHDR", ihdr)
    idat_chunk = png_chunk(b"IDAT", idat_payload)
    iend_chunk = png_chunk(b"IEND", b"")

    png_bytes = signature + ihdr_chunk + idat_chunk + iend_chunk

    print("Full PNG (%d bytes) — use this to verify test_png_decode.cpp kPng1x1Gray:" % len(png_bytes))
    hex_str = " ".join(f"{b:02X}" for b in png_bytes)
    print(hex_str)
    print()
    print("IDAT payload (13 bytes):", " ".join(f"{b:02X}" for b in idat_payload))
    print("Raw DEFLATE (7 bytes):  ", " ".join(f"{b:02X}" for b in raw_deflate))
    assert zlib.decompress(raw_deflate, -15) == raw_rows, "DEFLATE round-trip"

    with open("png_1x1_gray.png", "wb") as f:
        f.write(png_bytes)
    print()
    print("Wrote png_1x1_gray.png")

    # Palette 1x1: IHDR color_type=3, PLTE 1 entry (11 22 33), tRNS 1 byte (80), same IDAT
    ihdr_palette = struct.pack(">IIBBBBB", 1, 1, 8, 3, 0, 0, 0)
    plte_payload = bytes([0x11, 0x22, 0x33])
    trns_payload = bytes([0x80])
    ihdr_palette_chunk = png_chunk(b"IHDR", ihdr_palette)
    plte_chunk = png_chunk(b"PLTE", plte_payload)
    trns_chunk = png_chunk(b"tRNS", trns_payload)
    png_palette = (
        signature + ihdr_palette_chunk + plte_chunk + trns_chunk + idat_chunk + iend_chunk
    )
    print("Palette 1x1 PNG (%d bytes) — for Decode1x1PaletteWithTrnsSucceeds:" % len(png_palette))
    print(" ".join(f"{b:02X}" for b in png_palette))
    print()
    print("kIhdr1x1Palette:", " ".join(f"0x{b:02X}" for b in ihdr_palette_chunk))
    print("kPlte1Entry:   ", " ".join(f"0x{b:02X}" for b in plte_chunk))
    print("kTrns1Entry:   ", " ".join(f"0x{b:02X}" for b in trns_chunk))
    with open("png_1x1_palette.png", "wb") as f:
        f.write(png_palette)
    print()
    print("Wrote png_1x1_palette.png")
    return png_bytes

if __name__ == "__main__":
    main()
