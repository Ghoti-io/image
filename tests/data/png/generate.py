#!/usr/bin/env python3
"""
Generate PNG test data for decode tests. Single source of truth for all PNG
variants. Writes to the same directory as this script (tests/data/png/).

Also writes the APNG 16-bit blend expected file using Pillow as oracle (opens
APNG, composites frames, reads first pixel of each). Pillow returns 8-bit
for 16-bit PNGs; we scale to 16-bit. The decode test allows a small per-
component tolerance to account for 8- vs 16-bit rounding. Requires Pillow.

Run from repo root: python3 tests/data/png/generate.py
Or from this dir: python3 generate.py
"""
import os
import struct
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Paths for the 16-bit APNG and its oracle expected file
PNG_APNG_16BIT = os.path.join(SCRIPT_DIR, "png_apng_2frame_16bit_rgba.png")
APNG_16BIT_EXPECTED = os.path.join(SCRIPT_DIR, "png_apng_2frame_16bit_rgba_expected.bin")


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

    # ---- 2x2 grayscale 8-bit (for max_decoded_pixels limit test) ----
    # Row format: filter (0) + 2 pixels; 2 rows = 6 bytes.
    raw_2x2_gray = bytes([0x00, 0x00, 0x00, 0x00, 0x00, 0x00])
    idat_2x2_gray = idat_zlib(raw_2x2_gray)
    ihdr_2x2_gray = struct.pack(">IIBBBBB", 2, 2, 8, 0, 0, 0, 0)
    png_2x2_gray = (
        signature
        + png_chunk(b"IHDR", ihdr_2x2_gray)
        + png_chunk(b"IDAT", idat_2x2_gray)
        + iend
    )
    write_png("png_2x2_gray.png", png_2x2_gray)

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

    # ---- 1x1 gray + eXIf with orientation tag (for meta_common / apply_orientation test) ----
    # Minimal valid TIFF/Exif: II, 42, IFD0 at 8; IFD0 has one entry: Orientation (0x0112) = 6 (90 CW).
    # Layout: 8-byte header + 2 (num_entries) + 12 (one dir entry) + 4 (next IFD) = 26 bytes.
    exif_orientation_payload = (
        b"II\x2a\x00\x08\x00\x00\x00"  # little-endian, 42, first IFD at 8
        b"\x01\x00"  # 1 entry
        b"\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00"  # tag 0x0112, SHORT, 1, value 6
        b"\x00\x00\x00\x00"  # next IFD
    )
    png_exif_orientation = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"eXIf", exif_orientation_payload)
        + png_chunk(b"IDAT", idat_1x1_gray)
        + iend
    )
    write_png("png_exif_orientation.png", png_exif_orientation)

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

    # ---- 2-frame APNG 16-bit RGBA with blend OVER (tests 16-bit alpha blend) ----
    # Layout per APNG spec (wiki.mozilla.org/APNG_Specification): acTL before IDAT;
    # fcTL(seq=0), IDAT, fcTL(seq=1), fdAT(seq=2). Frame 0: black opaque. Frame 1:
    # red 50% alpha, blend=OVER, dispose=NONE. If a decoder disagrees on frame 1,
    # validate this file externally (e.g. littlesvr.ca/apng) to rule out generator bugs.
    ihdr_16rgba_1x1 = struct.pack(">IIBBBBB", 1, 1, 16, 6, 0, 0, 0)
    # Raw row: filter 0, then Rhi Rlo Ghi Glo Bhi Blo Ahi Alo (big-endian)
    raw_apng16_f0 = bytes([
        0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF,  # black opaque
    ])
    raw_apng16_f1 = bytes([
        0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,  # red, alpha 32768
    ])
    idat_apng16_f0 = idat_zlib(raw_apng16_f0)
    fctl_apng16_0 = struct.pack(
        ">IIIIIHHBB", 0, 1, 1, 0, 0, 50, 100, 0, 0,
    )
    fctl_apng16_1 = struct.pack(
        ">IIIIIHHBB", 1, 1, 1, 0, 0, 25, 100, 0, 1,
    )  # dispose=0 (NONE), blend=1 (OVER)
    fdat_apng16_1 = struct.pack(">I", 2) + idat_zlib(raw_apng16_f1)
    apng_16rgba = (
        signature
        + png_chunk(b"IHDR", ihdr_16rgba_1x1)
        + png_chunk(b"acTL", actl)
        + png_chunk(b"fcTL", fctl_apng16_0)
        + png_chunk(b"IDAT", idat_apng16_f0)
        + png_chunk(b"fcTL", fctl_apng16_1)
        + png_chunk(b"fdAT", fdat_apng16_1)
        + iend
    )
    write_png("png_apng_2frame_16bit_rgba.png", apng_16rgba)
    _write_subbyte_and_interlace_fixtures()
    _write_zlib_integrity_fixtures()
    _write_suggested_palette_fixtures()
    _write_apng16_oracle_expected()


# ---------------------------------------------------------------------------
# Sub-byte sample depths (1/2/4) crossed with Adam7 interlacing.
#
# PNG 7.2 packs samples of depth 1, 2 and 4 several to a byte, MSB first, and
# pads each scanline to a byte boundary. Adam7 (PNG 8.2, and the same table in
# png_common.c) then splits the image into seven passes, each of which is an
# independent, separately padded set of scanlines. The two together are the
# case a decoder is most likely to get wrong, because a pass row is not a
# sub-range of an image row: pixel i of a pass lands at image column
# x_offset + i * x_step, which for depth < 8 is a *bit* position and not a byte
# one.
#
# Each image is emitted twice, once non-interlaced and once interlaced, from
# the same sample array. Adam7 is a pure reordering, so the two must decode to
# identical pixels - a property that needs no reference decoder to check. The
# .raw files alongside give the absolute expected pixels, computed here from
# the spec's own rescaling rule (PNG 13.12: a sample of depth d scales to
# 8 bits as round(s * 255 / (2^d - 1))), so a failure says which pixel and not
# merely "different".
# ---------------------------------------------------------------------------

# Adam7 pass table: (x_offset, y_offset, x_step, y_step). PNG 8.2 Figure 8.1.
ADAM7_PASSES = [
    (0, 0, 8, 8),
    (4, 0, 8, 8),
    (0, 4, 4, 8),
    (2, 0, 4, 4),
    (0, 2, 2, 4),
    (1, 0, 2, 2),
    (0, 1, 1, 2),
]


def adam7_pass_dims(width: int, height: int, pass_index: int):
    """Pass width and height (0 if the pass is empty). PNG 8.2."""
    xo, yo, xs, ys = ADAM7_PASSES[pass_index]
    pw = 0 if width <= xo else (width - xo + xs - 1) // xs
    ph = 0 if height <= yo else (height - yo + ys - 1) // ys
    return pw, ph


def pack_samples(samples, depth: int) -> bytes:
    """Pack samples into a scanline, MSB first, padded to a byte. PNG 7.2."""
    if depth == 8:
        return bytes(samples)
    out = bytearray()
    acc = 0
    nbits = 0
    mask = (1 << depth) - 1
    for s in samples:
        acc = (acc << depth) | (s & mask)
        nbits += depth
        if nbits == 8:
            out.append(acc)
            acc = 0
            nbits = 0
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def raw_rows_plain(samples, width: int, height: int, depth: int) -> bytes:
    """Filter-None scanlines for a non-interlaced image."""
    out = bytearray()
    for y in range(height):
        out.append(0)
        out += pack_samples([samples[y][x] for x in range(width)], depth)
    return bytes(out)


def raw_rows_adam7(samples, width: int, height: int, depth: int) -> bytes:
    """Filter-None scanlines for the seven Adam7 passes, in order. PNG 8.2."""
    out = bytearray()
    for pass_index in range(7):
        pw, ph = adam7_pass_dims(width, height, pass_index)
        if pw == 0 or ph == 0:
            continue
        xo, yo, xs, ys = ADAM7_PASSES[pass_index]
        for j in range(ph):
            y = yo + j * ys
            out.append(0)
            out += pack_samples(
                [samples[y][xo + i * xs] for i in range(pw)], depth)
    return bytes(out)


def scale_sample_to_8(sample: int, depth: int) -> int:
    """PNG 13.12 sample depth rescaling, rounded."""
    max_val = (1 << depth) - 1
    return (sample * 255 + max_val // 2) // max_val


def _subbyte_samples(width: int, height: int, depth: int):
    """A pattern that varies along both axes and uses every value of the depth."""
    n = 1 << depth
    return [[(x * 3 + y * 5) % n for x in range(width)] for y in range(height)]


def _subbyte_palette(depth: int) -> bytes:
    """A palette of 2^depth entries, each a distinct colour."""
    n = 1 << depth
    out = bytearray()
    for i in range(n):
        out += bytes([(i * 37 + 11) & 0xFF, (i * 91 + 3) & 0xFF, (i * 53 + 199) & 0xFF])
    return bytes(out)


def _write_raw(name: str, data: bytes) -> None:
    path = os.path.join(SCRIPT_DIR, name)
    with open(path, "wb") as f:
        f.write(data)
    print("Wrote", path, f"({len(data)} bytes)")


def _write_subbyte_and_interlace_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    # 32x8 exercises every Adam7 pass with a whole number of bytes per row;
    # 33x9 additionally leaves a partly-used final byte in several passes.
    for width, height in ((32, 8), (33, 9)):
        for depth in (1, 2, 4):
            samples = _subbyte_samples(width, height, depth)

            # ---- grayscale, colour type 0 ----
            base = f"png_gray{depth}_{width}x{height}"
            for interlace in (0, 1):
                raw = (raw_rows_adam7 if interlace else raw_rows_plain)(
                    samples, width, height, depth)
                ihdr = struct.pack(">IIBBBBB", width, height, depth, 0, 0, 0,
                    interlace)
                name = base + ("_interlaced.png" if interlace else ".png")
                write_png(name, signature + png_chunk(b"IHDR", ihdr)
                    + png_chunk(b"IDAT", idat_zlib(raw)) + iend)
            # Expected pixels: GRAY8, one byte per pixel.
            _write_raw(base + ".raw", bytes(
                scale_sample_to_8(samples[y][x], depth)
                for y in range(height) for x in range(width)))

            # ---- palette, colour type 3 ----
            base = f"png_pal{depth}_{width}x{height}"
            plte = _subbyte_palette(depth)
            for interlace in (0, 1):
                raw = (raw_rows_adam7 if interlace else raw_rows_plain)(
                    samples, width, height, depth)
                ihdr = struct.pack(">IIBBBBB", width, height, depth, 3, 0, 0,
                    interlace)
                name = base + ("_interlaced.png" if interlace else ".png")
                write_png(name, signature + png_chunk(b"IHDR", ihdr)
                    + png_chunk(b"PLTE", plte)
                    + png_chunk(b"IDAT", idat_zlib(raw)) + iend)
            # Expected pixels: RGBA8 from the palette, alpha 255 (no tRNS).
            expected = bytearray()
            for y in range(height):
                for x in range(width):
                    i = samples[y][x]
                    expected += plte[i * 3:i * 3 + 3] + b"\xff"
            _write_raw(base + ".raw", bytes(expected))



# ---------------------------------------------------------------------------
# zlib streams whose wrapper is wrong (PNG 10.3, RFC 1950).
#
# PNG carries two independent integrity checks: the CRC on each chunk, which
# catches damage to the stored bytes, and the Adler-32 at the end of each zlib
# stream, which catches a stream that still inflates but to the wrong bytes.
# A decoder that skips the two header bytes and the four trailer bytes without
# reading them implements only the first.
#
# All four files below have correct chunk CRCs, so nothing but the zlib wrapper
# distinguishes them from the control.
# ---------------------------------------------------------------------------


def _gray_8x8_raw() -> bytes:
    """Filter-None scanlines for an 8x8 grayscale ramp."""
    out = bytearray()
    for y in range(8):
        out.append(0)
        out += bytes((x * 8 + y * 3) & 0xFF for x in range(8))
    return bytes(out)


def _write_zlib_integrity_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 0, 0, 0, 0))
    raw = _gray_8x8_raw()
    body = deflate_stored_block(raw)

    def emit(name: str, zlib_stream: bytes) -> None:
        write_png(name, signature + ihdr + png_chunk(b"IDAT", zlib_stream) + iend)

    # The control: everything correct. Every other file here differs from it
    # only in the bytes named, so a test that rejects them all must accept this.
    emit("png_zlib_ok.png", bytes([0x78, 0x01]) + body + adler32_be(raw))

    # Adler-32 of the wrong bytes: inflates cleanly, produces the wrong image.
    emit("png_zlib_bad_adler.png",
        bytes([0x78, 0x01]) + body + adler32_be(raw + b"\x00"))

    # CMF/FLG that is not a multiple of 31 (RFC 1950 FCHECK) and whose
    # compression method is 9 rather than the 8 PNG 10.3 requires.
    emit("png_zlib_bad_header.png", bytes([0x99, 0x99]) + body + adler32_be(raw))

    # FDICT set. 0x7820 passes the FCHECK multiple-of-31 test, so only the flag
    # itself marks this stream as one PNG 10.3 forbids - and a decoder that
    # ignores it reads the DICTID as DEFLATE data.
    emit("png_zlib_preset_dict.png",
        bytes([0x78, 0x20]) + struct.pack(">I", 0x1234) + body + adler32_be(raw))


# ---------------------------------------------------------------------------
# A suggested palette on a truecolour image (PNG 11.2.2).
#
# PLTE is required for colour type 3 and "shall not appear" for colour types 0
# and 4, but for 2 and 6 it *may* appear as a suggested palette for a viewer
# that cannot display truecolour. A decoder that can display truecolour ignores
# it. Rejecting such a file is not one of the choices the spec offers, and the
# conformance suite carries two of them (pp0n2c16, pp0n6a08).
# ---------------------------------------------------------------------------


def _write_suggested_palette_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    # A 4x4 RGB image whose pixels deliberately do *not* all appear in the
    # suggested palette: if the palette were used to decode, the result would
    # differ from the truecolour samples and the test would see it.
    raw = bytearray()
    for y in range(4):
        raw.append(0)
        for x in range(4):
            raw += bytes([(x * 60 + 3) & 0xFF, (y * 70 + 9) & 0xFF, (x * y * 13 + 31) & 0xFF])
    raw = bytes(raw)
    plte = bytes([0, 0, 0, 255, 255, 255, 128, 64, 32])

    write_png("png_rgb_suggested_palette.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"IDAT", idat_zlib(raw))
        + iend)

    # The same suggested palette on colour type 6, which PNG 11.2.2 allows too.
    raw_a = bytearray()
    for y in range(4):
        raw_a.append(0)
        for x in range(4):
            raw_a += bytes([(x * 60 + 3) & 0xFF, (y * 70 + 9) & 0xFF,
                (x * y * 13 + 31) & 0xFF, (200 + x * 5) & 0xFF])
    write_png("png_rgba_suggested_palette.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"IDAT", idat_zlib(bytes(raw_a)))
        + iend)

    # The same image with no PLTE at all. A suggested palette is advisory, so
    # this must decode to exactly what the file above does - a property that
    # needs no reference decoder, and one the fixture is built to expose,
    # because the pixels above are not all in the palette.
    write_png("png_rgb_no_palette.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", idat_zlib(raw))
        + iend)

    # PLTE on colour type 0, which the spec forbids outright.
    write_png("png_gray_forbidden_palette.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0))
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"IDAT", idat_zlib(bytes(b"".join(
            bytes([0]) + bytes((x * 17) & 0xFF for x in range(4)) for _ in range(4)))))
        + iend)

def _write_apng16_oracle_expected() -> None:
    """Write expected pixels for the 16-bit APNG blend test using Pillow as oracle.

    Pillow opens the APNG, composites frames (blend OVER), and we read the first
    pixel of frame 0 and frame 1. Pillow typically returns 8-bit; we scale to
    16-bit with (v*65535+127)//255. The decode test compares with a per-component
    tolerance to account for 8- vs 16-bit rounding.
    """
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit(
            "Pillow is required to generate the APNG 16-bit blend expected file. "
            "Install with: pip install Pillow"
        ) from None
    if not os.path.isfile(PNG_APNG_16BIT):
        return
    img = Image.open(PNG_APNG_16BIT)
    n_frames = getattr(img, "n_frames", 1)
    if n_frames < 2:
        raise SystemExit(
            f"APNG has only {n_frames} frame(s); need 2. "
            "Ensure the file is the 2-frame 16-bit RGBA APNG from generate.py."
        )
    pixels_16 = []
    for frame_idx in (0, 1):
        img.seek(frame_idx)
        img.load()
        px = img.getpixel((0, 0))
        if len(px) == 3:
            px = (px[0], px[1], px[2], 65535)
        elif len(px) != 4:
            raise SystemExit(
                f"Frame {frame_idx}: unexpected pixel format (expected RGBA)"
            )
        if max(px) <= 255:
            px = tuple((v * 65535 + 127) // 255 for v in px)
        pixels_16.append(px)
    data = struct.pack("<4H", *pixels_16[0]) + struct.pack("<4H", *pixels_16[1])
    with open(APNG_16BIT_EXPECTED, "wb") as f:
        f.write(data)
    print("Wrote", APNG_16BIT_EXPECTED, f"({len(data)} bytes, from Pillow)")


if __name__ == "__main__":
    main()
