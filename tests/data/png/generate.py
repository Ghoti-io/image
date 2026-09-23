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

    # ---- The same 2-frame APNG, but asking to be played a finite 3 times ----
    # Every other APNG fixture here carries num_plays=0, which is also what a
    # zeroed structure holds, so none of them can tell "the count was read" from
    # "the count was never set".  This one can.
    apng_3plays = (
        signature
        + png_chunk(b"IHDR", ihdr_1x1_gray)
        + png_chunk(b"acTL", struct.pack(">II", 2, 3))
        + png_chunk(b"fcTL", fctl0)
        + png_chunk(b"IDAT", idat_frame0)
        + png_chunk(b"fcTL", fctl1)
        + png_chunk(b"fdAT", fdat_payload)
        + iend
    )
    write_png("png_apng_3plays.png", apng_3plays)

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
    _write_grayalpha_fixtures()
    _write_zlib_integrity_fixtures()
    _write_suggested_palette_fixtures()
    _write_filter_fixtures()
    _write_third_edition_fixtures()
    _write_color_typed_ancillary_fixtures()
    _write_filter_validity_fixtures()
    _write_apng_frame_bounds_fixtures()
    _write_png_aspect_only()
    _write_jpeg_with_resolution()
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
    """A palette of 2^depth entries, each a distinct color."""
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

            # ---- grayscale, color type 0 ----
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

            # ---- palette, color type 3 ----
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


def _write_grayalpha_fixtures() -> None:
    """Gray+alpha (color type 4) large enough for Adam7 to have seven passes.

    The only color type 4 fixtures were 1x1, where six of Adam7's seven passes
    are empty and the writer's per-pixel path for the type runs once. A
    re-save keeps the color type it was given, so a type 4 fixture is the only
    way to make the encoder write one at all - which left that path, and the
    same one for color type 2 at 16 bits, never executed.

    The pattern varies along both axes and in both channels, so a pass written
    to the wrong place shows up as a pixel and not as a coincidence.
    """
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    w = h = 16

    rows8 = bytearray()
    for y in range(h):
        rows8.append(0)  # filter type 0
        for x in range(w):
            rows8.append((x * 17 + y * 3) & 0xFF)          # gray
            rows8.append((255 - (x * 5 + y * 11)) & 0xFF)  # alpha
    png8 = (
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 4, 0, 0, 0))
        + png_chunk(b"IDAT", idat_zlib(bytes(rows8)))
        + iend
    )
    write_png("png_grayalpha8_16x16.png", png8)

    rows16 = bytearray()
    for y in range(h):
        rows16.append(0)
        for x in range(w):
            g = (x * 4097 + y * 257) & 0xFFFF
            a = (0xFFFF - (x * 1031 + y * 2063)) & 0xFFFF
            rows16 += struct.pack(">HH", g, a)
    png16 = (
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 4, 0, 0, 0))
        + png_chunk(b"IDAT", idat_zlib(bytes(rows16)))
        + iend
    )
    write_png("png_grayalpha16_16x16.png", png16)


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
# A suggested palette on a truecolor image (PNG 11.2.2).
#
# PLTE is required for color type 3 and "shall not appear" for color types 0
# and 4, but for 2 and 6 it *may* appear as a suggested palette for a viewer
# that cannot display truecolor. A decoder that can display truecolor ignores
# it. Rejecting such a file is not one of the choices the spec offers, and the
# conformance suite carries two of them (pp0n2c16, pp0n6a08).
# ---------------------------------------------------------------------------


def _write_suggested_palette_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    # A 4x4 RGB image whose pixels deliberately do *not* all appear in the
    # suggested palette: if the palette were used to decode, the result would
    # differ from the truecolor samples and the test would see it.
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

    # The same suggested palette on color type 6, which PNG 11.2.2 allows too.
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

    # PLTE on color type 0, which the spec forbids outright.
    write_png("png_gray_forbidden_palette.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0))
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"IDAT", idat_zlib(bytes(b"".join(
            bytes([0]) + bytes((x * 17) & 0xFF for x in range(4)) for _ in range(4)))))
        + iend)


# ---------------------------------------------------------------------------
# An image with structure for the row filters to find (PNG 9).
#
# Filtering subtracts a prediction from each byte, so it only pays on data that
# is predictable from the pixel above or to the left. A smooth gradient is the
# clearest case: filter None leaves it as it is, while Sub, Up and Paeth reduce
# most of it to small numbers that DEFLATE codes in far fewer bits. An encoder
# choosing per row (PNG 12.8) should beat any single filter forced on all rows.
# ---------------------------------------------------------------------------


def _write_filter_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    w = h = 64
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # stored unfiltered; the encoder's choice is the subject
        for x in range(w):
            raw += bytes([(x * 4) & 0xFF, (y * 4) & 0xFF, ((x + y) * 2) & 0xFF])
    write_png("png_gradient_64x64_rgb.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + png_chunk(b"IDAT", idat_zlib(bytes(raw)))
        + iend)


# ---------------------------------------------------------------------------
# PNG Third Edition color chunks: cICP, mDCv, cLLi.
#
# cICP carries coding-independent code points (ITU-T H.273): color primaries,
# transfer function, matrix coefficients, and a full-range flag. The Third
# Edition puts it ahead of sRGB, iCCP and gAMA+cHRM - where it appears, it is
# what the samples mean.
#
# The first file names the sRGB pair (primaries 1, transfer 13, identity
# matrix, full range) and also carries a gAMA claiming 1.0, so a decoder that
# honors the precedence and one that does not give different answers.
# ---------------------------------------------------------------------------


def _write_third_edition_fixtures() -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
    idat = png_chunk(b"IDAT", idat_zlib(bytes([0x00, 0x40, 0x80, 0xC0])))

    # cICP naming sRGB, with a gAMA that disagrees. cICP wins (PNG 3rd ed.).
    write_png("png_cicp_srgb.png",
        signature + ihdr
        + png_chunk(b"cICP", bytes([1, 13, 0, 1]))
        + png_chunk(b"gAMA", struct.pack(">I", 100000))
        + idat + iend)

    # cICP naming BT.2020 primaries with the PQ transfer: a perfectly legal
    # file that this library's color model cannot describe, so it reports no
    # color information rather than guessing.
    # A gAMA rides along so that "left unknown" is a real assertion: a decoder
    # that ignored cICP would report the gamma instead of reporting nothing.
    write_png("png_cicp_bt2020_pq.png",
        signature + ihdr
        + png_chunk(b"cICP", bytes([9, 16, 0, 1]))
        + png_chunk(b"gAMA", struct.pack(">I", 45455))
        + idat + iend)

    # The HDR mastering chunks, which are preserved and not interpreted.
    mdcv = struct.pack(">8H", 34000, 16000, 13250, 34500, 7500, 3000,
        15635, 16450) + struct.pack(">II", 10000000, 1)
    write_png("png_mdcv_clli.png",
        signature + ihdr
        + png_chunk(b"mDCv", mdcv)
        + png_chunk(b"cLLi", struct.pack(">II", 10000000, 1000000))
        + idat + iend)


def _write_color_typed_ancillary_fixtures() -> None:
    """Images carrying bKGD, sBIT and hIST, for the save-side retargeting.

    These three chunks are laid out according to the color type in the IHDR
    beside them (PNG 11.3.4.1, 11.3.2.4, 11.3.4.2), so they cannot be copied
    across when the writer emits a different color type than the frame arrived
    as - and it does, whenever a tRNS has to become an alpha channel.

    Each fixture is a case where that happens, or a control where it must not.
    """
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")

    # ---- 4-bit grayscale + tRNS + bKGD + sBIT ------------------------------
    #
    # Saving this promotes it to color type 6: the transparent gray level has
    # to become an alpha channel. bKGD must be rewritten from one 2-byte gray
    # to three 16-bit samples, rescaled from 4 bits to 8 by 13.12, and sBIT
    # must be dropped - rescaling spreads each 4-bit value over 8 bits, so a
    # count taken before it no longer describes what is stored.
    #
    # The background is gray 7 of 15, which is 119 at 8 bits
    # (round(7 * 255 / 15)) - a value that is wrong in every way the rescaling
    # could be got wrong: not 7, not 112, not 127.
    width, height, depth = 4, 4, 4
    samples = [[(x + y) % 16 for x in range(width)] for y in range(height)]
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, 0, 0, 0, 0))
    write_png("png_gray4_trns_bkgd_sbit.png",
        signature + ihdr
        + png_chunk(b"tRNS", struct.pack(">H", 15))
        + png_chunk(b"bKGD", struct.pack(">H", 7))
        + png_chunk(b"sBIT", bytes([3]))
        + png_chunk(b"IDAT", idat_zlib(raw_rows_plain(samples, width, height, depth)))
        + iend)

    # ---- 8-bit grayscale + bKGD + sBIT, no tRNS ---------------------------
    #
    # The control. Nothing forces a change of color type, so both chunks must
    # come back byte for byte. A writer that rewrote them unconditionally would
    # pass the fixture above and fail this one.
    gray8 = bytes([0x00] + [0x11, 0x22, 0x33, 0x44]) * 4
    write_png("png_gray8_bkgd_sbit.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0))
        + png_chunk(b"bKGD", struct.pack(">H", 0x0080))
        + png_chunk(b"sBIT", bytes([5]))
        + png_chunk(b"IDAT", idat_zlib(gray8))
        + iend)

    # ---- palette + tRNS + bKGD + hIST -------------------------------------
    #
    # A palette whose tRNS gives one entry partial alpha, which no tRNS on a
    # truecolor image can express, so saving promotes this to color type 6.
    # bKGD names palette entry 1 and must become that entry's color; hIST is
    # one frequency per palette entry and has nothing to be about once the
    # palette is gone, so it must be dropped (11.3.4.2 requires PLTE).
    #
    # Entry 1 is (0x20, 0x40, 0x60): three different samples, so a writer that
    # collapsed the color to gray, or took the wrong entry, is visible.
    plte = bytes([0xFF, 0x00, 0x00,   # 0
                  0x20, 0x40, 0x60,   # 1  <- bKGD names this one
                  0x00, 0xFF, 0x00,   # 2
                  0x00, 0x00, 0xFF])  # 3
    indices = [[0, 1, 2, 3] for _ in range(4)]
    write_png("png_palette_trns_bkgd_hist.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 3, 0, 0, 0))
        + png_chunk(b"PLTE", plte)
        + png_chunk(b"tRNS", bytes([0xFF, 0x80, 0xFF, 0xFF]))
        + png_chunk(b"bKGD", bytes([1]))
        + png_chunk(b"hIST", struct.pack(">4H", 100, 50, 25, 10))
        + png_chunk(b"IDAT", idat_zlib(raw_rows_plain(indices, 4, 4, 8)))
        + iend)

    # ---- 8-bit grayscale + tRNS + a bKGD of the wrong length --------------
    #
    # Three bytes where color type 0 calls for two. The file is already
    # malformed; the point is that it is not carried forward into a new one.
    # The tRNS forces a color type change so the chunk is examined at all.
    write_png("png_gray8_bad_bkgd.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 0, 0, 0, 0))
        + png_chunk(b"tRNS", struct.pack(">H", 0x0011))
        + png_chunk(b"bKGD", bytes([0x00, 0x80, 0x00]))
        + png_chunk(b"IDAT", idat_zlib(gray8))
        + iend)


    # ---- 16-bit RGB + bKGD ------------------------------------------------
    #
    # A background stated at sixteen bits a sample, which the document model
    # cannot hold: gimg_doc_background_color() reports eight bits, because that
    # is what a caller with a decoded raster can use.
    #
    # So the writer must not rebuild this chunk from the document when nothing
    # about the background has changed - it has to put back the bytes the file
    # came with, or a 16-bit background quietly becomes an 8-bit one on a round
    # trip that changed nothing.  The three samples are chosen so that the low
    # byte is not a copy of the high byte and not zero: 0x1234 read as 8-bit is
    # 0x12, and a writer that rebuilt it would emit 0x1212.
    rgb16 = b""
    for y in range(4):
        rgb16 += b"\x00"
        for x in range(4):
            rgb16 += struct.pack(">HHH", 0x1000 + x, 0x2000 + y, 0x3000)
    write_png("png_rgb16_bkgd.png",
        signature
        + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 16, 2, 0, 0, 0))
        + png_chunk(b"bKGD", struct.pack(">HHH", 0x1234, 0x5678, 0x9ABC))
        + png_chunk(b"IDAT", idat_zlib(rgb16))
        + iend)



def _write_filter_validity_fixtures() -> None:
    """Rows whose filter byte is not one of the five clause 9 defines.

    Table 9.1 lists filter types 0 through 4 and nothing else, so a byte above
    4 is not a filter type this format has. A decoder that treated it as None
    would reconstruct the row from the wrong predictor and report success.
    libpng refuses such a file ("bad adaptive filter value") and so does
    Pillow ("unrecognized data stream contents").
    """
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 2, 8, 0, 0, 0, 0))

    # First row filtered None, second row claiming filter type 5.
    raw = b"\x00" + bytes([10, 20, 30, 40]) + b"\x05" + bytes([1, 1, 1, 1])
    write_png("png_bad_filter_type.png",
        signature + ihdr + png_chunk(b"IDAT", idat_zlib(raw)) + iend)

    # The same image with that row filtered Sub, which is the largest type
    # Table 9.1 does define. The pair is the point: one must decode and one
    # must not, so a decoder that refused both would fail this fixture.
    raw_ok = b"\x00" + bytes([10, 20, 30, 40]) + b"\x01" + bytes([1, 1, 1, 1])
    write_png("png_good_filter_type.png",
        signature + ihdr + png_chunk(b"IDAT", idat_zlib(raw_ok)) + iend)

    # Interlaced, so the check is reached on the Adam7 path as well. Pass 1 of
    # a 4x2 image is one pixel; its filter byte is the invalid one.
    ihdr_i = png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 2, 8, 0, 0, 0, 1))
    samples = [[10, 20, 30, 40], [50, 60, 70, 80]]
    rows = bytearray(raw_rows_adam7(samples, 4, 2, 8))
    rows[0] = 5
    write_png("png_bad_filter_type_interlaced.png",
        signature + ihdr_i + png_chunk(b"IDAT", idat_zlib(bytes(rows))) + iend)



def _write_apng_frame_bounds_fixtures() -> None:
    """APNG frames placed relative to the canvas the IHDR describes.

    The APNG specification requires a frame to lie inside that canvas: width
    and height above zero, x_offset + width no greater than the image width,
    and likewise for the height. It is not advisory. Compositing writes the
    frame into the canvas at that offset, so a frame declared past the edge is
    a write past the end of the canvas buffer - which is what this library
    used to do. Pillow refuses such a file ("APNG contains invalid frames").
    """
    signature = b"\x89PNG\r\n\x1a\n"
    iend = png_chunk(b"IEND", b"")
    w = h = 8

    def rgba(width, height, color):
        return b"".join(b"\x00" + bytes(list(color) * width)
                        for _ in range(height))

    ihdr = png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    actl = png_chunk(b"acTL", struct.pack(">II", 2, 0))
    fctl0 = png_chunk(b"fcTL",
        struct.pack(">IIIIIHHBB", 0, w, h, 0, 0, 1, 10, 0, 0))
    idat = png_chunk(b"IDAT", idat_zlib(rgba(w, h, (255, 0, 0, 255))))

    # Frame 1 is the full canvas size but offset to (4,4), so it runs four
    # pixels past the right and bottom edges.
    fctl_bad = png_chunk(b"fcTL",
        struct.pack(">IIIIIHHBB", 1, w, h, 4, 4, 1, 10, 0, 0))
    fdat_bad = png_chunk(b"fdAT",
        struct.pack(">I", 2) + idat_zlib(rgba(w, h, (0, 255, 0, 255)))[0:])
    write_png("png_apng_frame_outside_canvas.png",
        signature + ihdr + actl + fctl0 + idat + fctl_bad + fdat_bad + iend)

    # The control: a 4x4 frame at (4,4) fits exactly, and must still decode.
    # Without it, a decoder that refused every offset frame would pass.
    fctl_ok = png_chunk(b"fcTL",
        struct.pack(">IIIIIHHBB", 1, 4, 4, 4, 4, 1, 10, 0, 0))
    fdat_ok = png_chunk(b"fdAT",
        struct.pack(">I", 2) + idat_zlib(rgba(4, 4, (0, 255, 0, 255))))
    write_png("png_apng_frame_inside_canvas.png",
        signature + ihdr + actl + fctl0 + idat + fctl_ok + fdat_ok + iend)

    # A zero-sized frame: the spec requires width and height above zero.
    fctl_zero = png_chunk(b"fcTL",
        struct.pack(">IIIIIHHBB", 1, 0, 0, 0, 0, 1, 10, 0, 0))
    write_png("png_apng_frame_zero_size.png",
        signature + ihdr + actl + fctl0 + idat + fctl_zero + fdat_ok + iend)



def _write_png_aspect_only() -> None:
    """A PNG whose pHYs states a pixel aspect ratio and no physical size.

    Unit specifier 0 means the two numbers are a ratio and nothing else
    (11.3.4.3), which is not a density and so cannot become a DPI. The loader
    used to drop such a chunk, losing the only thing it says. 4:3 here, which
    is a shape no square-pixel default would produce by accident.
    """
    signature = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0)
    phys = png_chunk(b"pHYs", struct.pack(">IIB", 4, 3, 0))
    data = (signature + png_chunk(b"IHDR", ihdr) + phys
            + png_chunk(b"IDAT", idat_zlib(bytes([0x00, 0x00])))
            + png_chunk(b"IEND", b""))
    write_png("png_phys_aspect_4_3.png", data)


def _write_jpeg_with_resolution() -> None:
    """A small JPEG stating 300 dpi, for the PNG side of the resolution test.

    PNG measures resolution in pHYs (11.3.4.3) and JPEG in a JFIF APP0, and the
    point of the fixture is that a resolution crossing from one to the other
    arrives intact. Written with Pillow so the JFIF density is a real one and
    not this project's idea of one.
    """
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("Pillow is required to generate the resolution fixture") from None
    im = Image.new("RGB", (8, 8))
    for y in range(8):
        for x in range(8):
            im.putpixel((x, y), ((x * 32) % 256, (y * 32) % 256, 128))
    path = os.path.join(SCRIPT_DIR, "jpeg_300dpi_8x8.jpg")
    im.save(path, "JPEG", dpi=(300, 300), quality=90)
    print("Wrote", path)


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
