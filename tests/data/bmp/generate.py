#!/usr/bin/env python3
"""Generate the BMP test fixtures.

Pillow covers the variants it can write; everything else is assembled byte by
byte here, because the interesting cases for a decoder are exactly the ones
common writers never produce: 16-bit 5-5-5 versus 5-6-5, top-down rows,
BITMAPCOREHEADER, the RLE encodings, and deliberately malformed files.

Run from this directory:  python3 generate.py
"""

import io
import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).parent

# A 4x4 test pattern.  Distinct, non-symmetric, and every component differs so
# a channel swap or a row flip cannot go unnoticed.
PATTERN = [
    [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)],
    [(255, 0, 255), (0, 255, 255), (255, 255, 255), (0, 0, 0)],
    [(128, 0, 0), (0, 128, 0), (0, 0, 128), (128, 128, 0)],
    [(64, 32, 16), (16, 32, 64), (200, 100, 50), (50, 100, 200)],
]


def write(name: str, data: bytes) -> None:
    (HERE / name).write_bytes(data)
    print(f"  {name} ({len(data)} bytes)")


# ---------------------------------------------------------------------------
# Byte-level BMP assembly
# ---------------------------------------------------------------------------

def file_header(file_size: int, data_offset: int) -> bytes:
    return struct.pack("<2sIHHI", b"BM", file_size, 0, 0, data_offset)


def info_header(width, height, bit_count, compression=0, image_size=0,
                clr_used=0, extra=b"") -> bytes:
    size = 40 + len(extra)
    return struct.pack("<IiiHHIIiiII", size, width, height, 1, bit_count,
                       compression, image_size, 2835, 2835, clr_used, 0) + extra


def assemble(dib: bytes, palette: bytes, pixels: bytes) -> bytes:
    offset = 14 + len(dib) + len(palette)
    return (file_header(offset + len(pixels), offset) + dib + palette + pixels)


def pad_row(row: bytes) -> bytes:
    """Pad a row of pixel bytes up to a 4-byte boundary."""
    return row + b"\x00" * ((-len(row)) % 4)


def bottom_up(rows):
    """BMP stores the last row first unless the height is negative."""
    return list(reversed(rows))


# ---------------------------------------------------------------------------
# Pillow-written fixtures
# ---------------------------------------------------------------------------

def pillow_fixtures() -> None:
    img = Image.new("RGB", (4, 4))
    for y, row in enumerate(PATTERN):
        for x, rgb in enumerate(row):
            img.putpixel((x, y), rgb)
    img.save(HERE / "bmp_4x4_24bit.bmp")
    print("  bmp_4x4_24bit.bmp")

    img.convert("P", palette=Image.ADAPTIVE, colors=16).save(
        HERE / "bmp_4x4_8bit_palette.bmp")
    print("  bmp_4x4_8bit_palette.bmp")

    Image.new("RGB", (1, 1), (17, 34, 51)).save(HERE / "bmp_1x1_24bit.bmp")
    print("  bmp_1x1_24bit.bmp")

    # 1-bit: a checkerboard, so a shift in the bit unpacking is visible.
    mono = Image.new("1", (8, 4))
    for y in range(4):
        for x in range(8):
            mono.putpixel((x, y), 255 if (x + y) % 2 == 0 else 0)
    mono.save(HERE / "bmp_8x4_1bit.bmp")
    print("  bmp_8x4_1bit.bmp")


# ---------------------------------------------------------------------------
# Hand-assembled fixtures
# ---------------------------------------------------------------------------

def bgr24(rows) -> bytes:
    out = b""
    for row in rows:
        out += pad_row(b"".join(bytes([b, g, r]) for (r, g, b) in row))
    return out


def true_color_fixtures() -> None:
    # 24-bit, top-down (negative height).  Same pixels as the Pillow file, so
    # a decoder that ignores the sign produces a visibly flipped image.
    pixels = bgr24(PATTERN)  # Already top-down order.
    write("bmp_4x4_24bit_topdown.bmp",
          assemble(info_header(4, -4, 24), b"", pixels))

    # 16-bit BI_RGB: 5-5-5 with the top bit unused.  A decoder that assumes
    # 5-6-5 reads green and red from the wrong bit positions.
    def rgb555(r, g, b):
        return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)

    rows = []
    for row in bottom_up(PATTERN):
        rows.append(pad_row(b"".join(
            struct.pack("<H", rgb555(r, g, b)) for (r, g, b) in row)))
    write("bmp_4x4_16bit_555.bmp",
          assemble(info_header(4, 4, 16), b"", b"".join(rows)))

    # 16-bit BI_BITFIELDS declaring 5-6-5.
    def rgb565(r, g, b):
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    masks = struct.pack("<III", 0xF800, 0x07E0, 0x001F)
    rows = []
    for row in bottom_up(PATTERN):
        rows.append(pad_row(b"".join(
            struct.pack("<H", rgb565(r, g, b)) for (r, g, b) in row)))
    write("bmp_4x4_16bit_565.bmp",
          assemble(info_header(4, 4, 16, compression=3) + masks, b"",
                   b"".join(rows)))

    # 32-bit BI_RGB with every high byte zero: must decode fully opaque, not
    # fully transparent.
    rows = []
    for row in bottom_up(PATTERN):
        rows.append(b"".join(bytes([b, g, r, 0]) for (r, g, b) in row))
    write("bmp_4x4_32bit_zero_high_byte.bmp",
          assemble(info_header(4, 4, 32), b"", b"".join(rows)))

    # 32-bit BI_RGB whose spare high bytes are *not* all zero.  BI_RGB does
    # not define that byte, so this must still decode opaque by default; with
    # GIMG_BMP_RGB32_ALPHA_HEURISTIC it becomes an alpha ramp instead.  This
    # is bmpsuite's q/rgb32fakealpha.bmp in miniature.
    rows = []
    for y, row in enumerate(bottom_up(PATTERN)):
        spare = [0, 85, 170, 255][y]
        rows.append(b"".join(bytes([b, g, r, spare]) for (r, g, b) in row))
    write("bmp_4x4_32bit_dirty_high_byte.bmp",
          assemble(info_header(4, 4, 32), b"", b"".join(rows)))

    # 32-bit BI_BITFIELDS with an explicit alpha mask and a real alpha ramp.
    # The four masks live inside the header, which makes it a 56-byte
    # BITMAPV3INFOHEADER; a plain 40-byte header with BI_BITFIELDS carries
    # only the three color masks.
    masks = struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF,
                        0xFF000000)
    rows = []
    for y, row in enumerate(bottom_up(PATTERN)):
        # Iterating bottom-up, so index y here is image row 3 - y.  The result
        # is an alpha ramp of 255, 170, 85, 0 reading down from the top.
        alpha = [0, 85, 170, 255][y]
        rows.append(b"".join(bytes([b, g, r, alpha]) for (r, g, b) in row))
    write("bmp_4x4_32bit_alpha.bmp",
          assemble(info_header(4, 4, 32, compression=3, extra=masks), b"",
                   b"".join(rows)))


def palette_fixtures() -> None:
    # A 6-entry palette; indices 0..5 only.
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255),
              (255, 255, 0), (255, 0, 255), (0, 255, 255)]
    palette = b"".join(bytes([b, g, r, 0]) for (r, g, b) in colors)

    # 4-bit, 8x2: two indices per byte.
    indices = [
        [0, 1, 2, 3, 4, 5, 0, 1],
        [5, 4, 3, 2, 1, 0, 5, 4],
    ]
    rows = []
    for row in bottom_up(indices):
        packed = bytearray()
        for i in range(0, len(row), 2):
            packed.append((row[i] << 4) | row[i + 1])
        rows.append(pad_row(bytes(packed)))
    write("bmp_8x2_4bit.bmp",
          assemble(info_header(8, 2, 4, clr_used=6), palette, b"".join(rows)))

    # 8-bit with the same palette.
    rows = [pad_row(bytes(row)) for row in bottom_up(indices)]
    write("bmp_8x2_8bit.bmp",
          assemble(info_header(8, 2, 8, clr_used=6), palette, b"".join(rows)))

    # 2 bits per pixel: four indices, four to a byte.  A Windows CE addition
    # that the desktop API never accepted, so it is read and never written -
    # and until this fixture existed nothing here decoded one at all.
    two_bit = [
        [0, 1, 2, 3, 3, 2, 1, 0],
        [3, 2, 1, 0, 0, 1, 2, 3],
    ]
    rows = []
    for row in bottom_up(two_bit):
        packed = bytearray()
        for i in range(0, len(row), 4):
            packed.append((row[i] << 6) | (row[i + 1] << 4) |
                          (row[i + 2] << 2) | row[i + 3])
        rows.append(pad_row(bytes(packed)))
    write("bmp_8x2_2bit.bmp",
          assemble(info_header(8, 2, 2, clr_used=4), palette, b"".join(rows)))

    # BITMAPCOREHEADER: 12-byte header, 16-bit dimensions, 3-byte palette
    # entries, and no biClrUsed field (so the palette is the full 2^bpp).
    core = struct.pack("<IHHHH", 12, 8, 2, 1, 4)
    core_palette = b"".join(bytes([b, g, r]) for (r, g, b) in colors)
    core_palette += b"\x00" * 3 * (16 - len(colors))
    rows = []
    for row in bottom_up(indices):
        packed = bytearray()
        for i in range(0, len(row), 2):
            packed.append((row[i] << 4) | row[i + 1])
        rows.append(pad_row(bytes(packed)))
    write("bmp_8x2_core.bmp", assemble(core, core_palette, b"".join(rows)))


def os2_fixtures() -> None:
    """OS/2 2.x BITMAPCOREHEADER2 forms, and the RLE24 only OS/2 has.

    The header may stop at any multiple of 4 from 16 to 64, and every field it
    stops short of reads as zero.  Its first 40 bytes are byte-for-byte a
    BITMAPINFOHEADER, so the same picture written at 16, 40 and 64 bytes must
    decode identically - which is a property, and needs no reference decoder
    to check.
    """
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255),
              (255, 255, 0), (255, 0, 255), (0, 255, 255)]
    palette = b"".join(bytes([b, g, r, 0]) for (r, g, b) in colors)
    indices = [
        [0, 1, 2, 3, 4, 5, 0, 1],
        [5, 4, 3, 2, 1, 0, 5, 4],
    ]
    rows = b"".join(pad_row(bytes(row)) for row in bottom_up(indices))

    def os2v2_header(size, width, height, bit_count, compression=0,
                     clr_used=0):
        """A BITMAPCOREHEADER2 truncated to `size` bytes."""
        full = struct.pack("<IiiHHIIiiII", size, width, height, 1, bit_count,
                           compression, 0, 0, 0, clr_used, 0)
        full += struct.pack("<HHHHIIII", 0, 0, 0, 0, 0, 0, 0, 0)
        assert len(full) == 64
        return full[:size]

    # The full 64-byte form, and the smallest one there is.  A 16-byte header
    # stops before biCompression and biClrUsed, so the palette is the depth's
    # full 256 entries.
    write("bmp_8x2_os2v2_64.bmp",
          assemble(os2v2_header(64, 8, 2, 8, clr_used=6), palette, rows))
    full_palette = palette + b"\x00" * 4 * (256 - len(colors))
    write("bmp_8x2_os2v2_16.bmp",
          assemble(os2v2_header(16, 8, 2, 8), full_palette, rows))

    # ulCompression 3 is Huffman 1D to OS/2, not BI_BITFIELDS.  Reading it as
    # bitfields would look for masks that are not there.
    write("bmp_8x2_os2v2_huffman.bmp",
          assemble(os2v2_header(64, 8, 2, 1, compression=3), palette,
                   b"\x00" * 8))

    # ulCompression 4 is RLE24: an escape structure like RLE8's, but each
    # pixel is a BGR triple and there is no palette at all.  The fixture runs
    # every form through in one 8x2 image - encoded run, absolute run, delta,
    # end of line, end of bitmap - and the expected pixels are spelled out in
    # the test rather than derived from PATTERN.
    def rle24_run(count, rgb):
        r, g, b = rgb
        return bytes([count, b, g, r])

    def rle24_absolute(pixels):
        assert len(pixels) >= 3, "counts below 3 are escapes, not runs"
        body = b"".join(bytes([b, g, r]) for (r, g, b) in pixels)
        return bytes([0, len(pixels)]) + body + (b"\x00" * (len(body) & 1))

    red, green, blue = (255, 0, 0), (0, 255, 0), (0, 0, 255)
    yellow, magenta, cyan = (255, 255, 0), (255, 0, 255), (0, 255, 255)
    white = (255, 255, 255)

    # Rows are emitted bottom-up, so the image's last row is encoded first.
    #   row 1: R R R | G B Y | M M
    #   row 0: C C | skip 2 | W W | (the rest never reached, so black)
    rle24 = (rle24_run(3, red)
             + rle24_absolute([green, blue, yellow])
             + rle24_run(2, magenta)
             + bytes([0, 0])                  # End of line.
             + rle24_run(2, cyan)
             + bytes([0, 2, 2, 0])            # Delta: +2 columns, +0 rows.
             + rle24_run(2, white)
             + bytes([0, 1]))                 # End of bitmap.
    write("bmp_8x2_rle24.bmp",
          assemble(os2v2_header(64, 8, 2, 24, compression=4), b"", rle24))


def rle_fixtures() -> None:
    colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255),
              (255, 255, 0), (255, 0, 255), (0, 255, 255)]
    palette = b"".join(bytes([b, g, r, 0]) for (r, g, b) in colors)

    # RLE8 over an 8x2 image.  Rows are emitted bottom-up, so the first
    # encoded row is the image's last row.
    #   row 1 (bottom): 4 x index 2, then an absolute run of 1,2,3,4
    #   row 0 (top):    8 x index 0
    rle8 = bytes([
        4, 2,                       # Encoded run.
        0, 4, 1, 2, 3, 4,           # Absolute run of 4 (even, no padding).
        0, 0,                       # End of line.
        8, 0,                       # Encoded run.
        0, 1,                       # End of bitmap.
    ])
    write("bmp_8x2_rle8.bmp",
          assemble(info_header(8, 2, 8, compression=1, clr_used=6),
                   palette, rle8))

    # RLE4 over the same shape.  An encoded run alternates the two nibbles of
    # the value byte, so 0x12 means 1,2,1,2,...
    rle4 = bytes([
        4, 0x12,                    # 1,2,1,2
        0, 4, 0x34, 0x50,           # Absolute run of 4 nibbles: 3,4,5,0
        0, 0,                       # End of line.
        8, 0x00,                    # 0,0,0,0,0,0,0,0
        0, 1,                       # End of bitmap.
    ])
    write("bmp_8x2_rle4.bmp",
          assemble(info_header(8, 2, 4, compression=2, clr_used=6),
                   palette, rle4))

    # RLE8 with a delta that skips the rest of a row.  Skipped pixels must be
    # left at the zeroed initial value rather than filled or read past.
    rle_delta = bytes([
        2, 1,                       # Two pixels of index 1.
        0, 2, 3, 0,                 # Delta: +3 columns, +0 rows.
        3, 2,                       # Three pixels of index 2.
        0, 0,                       # End of line.
        8, 5,
        0, 1,                       # End of bitmap.
    ])
    write("bmp_8x2_rle8_delta.bmp",
          assemble(info_header(8, 2, 8, compression=1, clr_used=6),
                   palette, rle_delta))


def color_fixtures() -> None:
    """BITMAPV4HEADER and BITMAPV5HEADER color fields.

    bmpsuite carries these too - g/pal8v4.bmp, g/pal8v5.bmp, q/rgb24prof.bmp -
    but the suite is not vendored, and these pin the same behaviour with
    values chosen here rather than inherited.
    """
    def fxpt2dot30(value: float) -> int:
        return int(round(value * (1 << 30)))

    def fixed16(value: float) -> int:
        return int(round(value * 65536))

    # The ITU-R BT.709 primaries, which sRGB shares, written the way every
    # writer in reach writes them: xyY chromaticities normalized to sum to 1,
    # in fields the format declares as CIEXYZ.
    SRGB_ENDPOINTS = [0.6400, 0.3300, 0.0300,
                      0.3000, 0.6000, 0.1000,
                      0.1500, 0.0600, 0.7900]
    # Adobe RGB (1998): the same red and blue, a wider green.
    ADOBE_ENDPOINTS = [0.6400, 0.3300, 0.0300,
                       0.2100, 0.7100, 0.0800,
                       0.1500, 0.0600, 0.7900]

    def v4_tail(cs_type, endpoints, gamma):
        # A V4 header carries the four channel masks at offsets 40..55
        # whatever the compression is; they are simply zero for BI_RGB.  The
        # colour fields follow them, so leaving them out shifts everything
        # after and makes a 92-byte header no version defines.
        out = struct.pack("<4I", 0, 0, 0, 0)
        out += struct.pack("<I", cs_type)
        out += struct.pack("<9i", *[fxpt2dot30(v) for v in endpoints])
        out += struct.pack("<3I", *[fixed16(g) for g in gamma])
        return out

    def v5_tail(intent, profile_offset, profile_size):
        return struct.pack("<4I", intent, profile_offset, profile_size, 0)

    pixels = bgr24(bottom_up(PATTERN))

    # V4, LCS_CALIBRATED_RGB: the space is described by the endpoints and the
    # per-channel gammas rather than named.
    write("bmp_4x4_v4_calibrated.bmp",
          assemble(info_header(4, 4, 24,
                               extra=v4_tail(0, SRGB_ENDPOINTS, [2.2] * 3)),
                   b"", pixels))

    # The same, with Adobe RGB's green.
    write("bmp_4x4_v4_adobe.bmp",
          assemble(info_header(4, 4, 24,
                               extra=v4_tail(0, ADOBE_ENDPOINTS, [2.2] * 3)),
                   b"", pixels))

    # Three gammas that disagree describe a space one transfer function cannot
    # hold, so the transfer must be left unsaid rather than averaged.
    write("bmp_4x4_v4_split_gamma.bmp",
          assemble(info_header(4, 4, 24,
                               extra=v4_tail(0, SRGB_ENDPOINTS,
                                             [2.2, 1.8, 1.0])),
                   b"", pixels))

    # V5 naming sRGB outright, with LCS_GM_GRAPHICS (relative colorimetric).
    write("bmp_4x4_v5_srgb.bmp",
          assemble(info_header(4, 4, 24,
                               extra=v4_tail(0x73524742, [0.0] * 9, [0.0] * 3)
                               + v5_tail(2, 0, 0)),
                   b"", pixels))

    # V5 with PROFILE_EMBEDDED.  The payload is not a usable profile - nothing
    # here parses one - but it is shaped like the start of an ICC profile, so
    # a test can require it to come back byte for byte.
    profile = bytearray(128)
    profile[0:4] = struct.pack(">I", 128)      # Profile size, big-endian.
    profile[36:40] = b"acsp"                   # ICC file signature.
    for i in range(40, 128):
        profile[i] = (i * 7) & 0xFF
    dib = info_header(4, 4, 24,
                      extra=v4_tail(0x4D424544, [0.0] * 9, [0.0] * 3)
                      + v5_tail(4, 0, 0))
    # bV5ProfileData counts from the start of the DIB header, and the profile
    # is placed after the pixel data.
    offset = len(dib) + len(pixels)
    dib = dib[:108] + struct.pack("<4I", 4, offset, len(profile), 0)
    write("bmp_4x4_v5_icc.bmp",
          assemble(dib, b"", pixels + bytes(profile)))

    # PROFILE_LINKED: the "profile" is a file path.  It must not be followed -
    # opening a path an image names is acting on data.
    path = b"C:\\does\\not\\exist.icc\x00"
    dib = info_header(4, 4, 24,
                      extra=v4_tail(0x4C494E4B, [0.0] * 9, [0.0] * 3)
                      + v5_tail(4, 0, 0))
    offset = len(dib) + len(pixels)
    dib = dib[:108] + struct.pack("<4I", 4, offset, len(path), 0)
    write("bmp_4x4_v5_linked_profile.bmp",
          assemble(dib, b"", pixels + path))

    # A profile whose offset and size run off the end of the file.  The image
    # decodes untagged rather than being refused: a picture is not wrong
    # because its colour annotation is.
    dib = info_header(4, 4, 24,
                      extra=v4_tail(0x4D424544, [0.0] * 9, [0.0] * 3)
                      + v5_tail(4, 0, 0))
    dib = dib[:108] + struct.pack("<4I", 4, 100000, 4096, 0)
    write("bmp_4x4_v5_icc_past_eof.bmp", assemble(dib, b"", pixels))


def embedded_fixtures() -> None:
    """BI_JPEG and BI_PNG: the "pixel data" is a whole JPEG or PNG stream.

    The payloads are generated from PATTERN through Pillow, then wrapped, so
    a test can decode the wrapper and the payload separately and require the
    two to agree - which pins the wrapper without re-testing the JPEG and PNG
    codecs.
    """
    img = Image.new("RGB", (4, 4))
    for y, row in enumerate(PATTERN):
        for x, rgb in enumerate(row):
            img.putpixel((x, y), rgb)

    for name, fmt, compression in (("png", "PNG", 5), ("jpeg", "JPEG", 4)):
        buffer = io.BytesIO()
        img.save(buffer, format=fmt)
        payload = buffer.getvalue()
        # biSizeImage states the payload length, which is the only field that
        # does; the header's own width, height and depth describe the image
        # the wrapper stands in for.
        dib = info_header(4, 4, 24, compression=compression,
                          image_size=len(payload))
        write(f"bmp_4x4_embedded_{name}.bmp", assemble(dib, b"", payload))
        # The payload on its own, for the test to decode as a second opinion.
        write(f"bmp_4x4_embedded_{name}_payload.{name.replace('jpeg', 'jpg')}",
              payload)

    # A BI_PNG wrapper whose payload is not a PNG at all.  Refusing it is the
    # point: the inner stream is loaded through the codec the header named,
    # never through the prober, so this cannot become a BMP inside a BMP.
    dib = info_header(4, 4, 24, compression=5, image_size=16)
    write("bmp_embedded_png_not_a_png.bmp", assemble(dib, b"", b"not a png!!!!!!!"))


def malformed_fixtures() -> None:
    colors = [(255, 0, 0), (0, 255, 0)]
    palette = b"".join(bytes([b, g, r, 0]) for (r, g, b) in colors)

    # An 8-bit image whose pixel data references index 7 with only 2 palette
    # entries present.
    rows = [pad_row(bytes([0, 1, 7, 0]))]
    write("bmp_bad_palette_index.bmp",
          assemble(info_header(4, 1, 8, clr_used=2), palette, b"".join(rows)))

    # Declares 4x4 at 24bpp (48 bytes of pixel data) but supplies one row.
    write("bmp_truncated_pixels.bmp",
          assemble(info_header(4, 4, 24), b"", bgr24([PATTERN[0]])))

    # BI_BITFIELDS with a mask whose set bits are not contiguous.
    masks = struct.pack("<III", 0xF81F, 0x07E0, 0x001F)
    write("bmp_noncontiguous_mask.bmp",
          assemble(info_header(4, 4, 16, compression=3) + masks, b"",
                   b"\x00" * 32))

    # Zero width.
    write("bmp_zero_width.bmp", assemble(info_header(0, 4, 24), b"", b""))

    # A DIB header size no version uses.  It has to be outside 16..64 as well
    # as away from the Windows sizes: OS/2 2.x allows every multiple of 4 in
    # that range, so 44 - which this fixture used to carry - is legal there.
    bad = struct.pack("<IiiHHIIiiII", 100, 4, 4, 1, 24, 0, 0, 0, 0, 0, 0)
    bad += b"\x00" * 60
    write("bmp_unknown_header_size.bmp", assemble(bad, b"", b"\x00" * 48))

    # Correct header, wrong magic.
    data = bytearray(assemble(info_header(4, 4, 24), b"", bgr24(PATTERN)))
    data[0:2] = b"XY"
    write("bmp_bad_magic.bmp", bytes(data))

    # Pixel data offset pointing past the end of the file.
    body = assemble(info_header(4, 4, 24), b"", bgr24(PATTERN))
    data = bytearray(body)
    struct.pack_into("<I", data, 10, len(body) + 1000)
    write("bmp_offset_past_eof.bmp", bytes(data))

    # RLE with a negative height.  The two cannot be combined: an RLE stream's
    # "end of line" walks one way only, so a top-down RLE bitmap does not say
    # which way it walks.
    rle8 = bytes([4, 0, 0, 0, 4, 1, 0, 1])
    write("bmp_rle8_topdown.bmp",
          assemble(info_header(4, -2, 8, compression=1, clr_used=2),
                   palette, rle8))


if __name__ == "__main__":
    print("Pillow fixtures:")
    pillow_fixtures()
    print("True color fixtures:")
    true_color_fixtures()
    print("Palette fixtures:")
    palette_fixtures()
    print("OS/2 fixtures:")
    os2_fixtures()
    print("RLE fixtures:")
    rle_fixtures()
    print("Color fixtures:")
    color_fixtures()
    print("Embedded-stream fixtures:")
    embedded_fixtures()
    print("Malformed fixtures:")
    malformed_fixtures()
