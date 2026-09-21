#!/usr/bin/env python3
"""Generate the GIF test fixtures.

Pillow writes the ordinary cases.  Everything a common writer never produces is
assembled byte by byte here, because those are the cases a decoder gets wrong:
a file with no Global Color Table, a code stream that names an index its colour
table does not have, extensions that must be walked past rather than parsed, and
the 87a header.

Run from this directory:  python3 generate.py
"""

import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).parent


def write(name: str, data: bytes) -> None:
    (HERE / name).write_bytes(data)
    print(f"  {name} ({len(data)} bytes)")


# ---------------------------------------------------------------------------
# Byte-level GIF assembly
# ---------------------------------------------------------------------------

def header(version: bytes = b"89a") -> bytes:
    return b"GIF" + version


def lsd(width, height, gct_bits=None, background=0, aspect=0) -> bytes:
    """Logical Screen Descriptor (89a 18).

    gct_bits is the size exponent: a table of 2**(gct_bits+1) entries, or None
    for a file that carries no Global Color Table at all.
    """
    packed = 0
    if gct_bits is not None:
        packed |= 0x80 | (gct_bits & 0x07)
        packed |= 0x70  # colour resolution, which no decoder reads
    return struct.pack("<HHBBB", width, height, packed, background, aspect)


def table(colors, bits) -> bytes:
    """A colour table padded out to the 2**(bits+1) entries the size implies."""
    entries = 1 << (bits + 1)
    out = bytearray()
    for i in range(entries):
        r, g, b = colors[i] if i < len(colors) else (0, 0, 0)
        out += bytes((r, g, b))
    return bytes(out)


def image_descriptor(left, top, width, height, lct_bits=None,
                     interlaced=False) -> bytes:
    packed = 0
    if lct_bits is not None:
        packed |= 0x80 | (lct_bits & 0x07)
    if interlaced:
        packed |= 0x40
    return b"\x2C" + struct.pack("<HHHHB", left, top, width, height, packed)


def gce(delay=0, disposal=0, transparent=None) -> bytes:
    """Graphic Control Extension (89a 23)."""
    packed = (disposal & 0x07) << 2
    if transparent is not None:
        packed |= 0x01
    return (b"\x21\xF9\x04" + bytes((packed,)) + struct.pack("<H", delay)
            + bytes((transparent if transparent is not None else 0,))
            + b"\x00")


def sub_blocks(data: bytes) -> bytes:
    """Chop a payload into the length-prefixed chain the format uses (89a 15)."""
    out = bytearray()
    for i in range(0, len(data), 255):
        piece = data[i:i + 255]
        out += bytes((len(piece),)) + piece
    out += b"\x00"
    return bytes(out)


def lzw_encode(indices: bytes, min_code_size: int) -> bytes:
    """Encode to GIF LZW (89a 22).

    Written out here rather than taken from a library so the fixtures do not
    depend on the same code the decoder is checked against.  It is the textbook
    encoder: emit Clear, build the table, widen the code as it fills, and reset
    when it is full.
    """
    clear = 1 << min_code_size
    end = clear + 1

    out = bytearray()
    bit_buffer = 0
    bit_count = 0

    def emit(code, width):
        nonlocal bit_buffer, bit_count
        bit_buffer |= code << bit_count       # GIF packs least-significant first
        bit_count += width
        while bit_count >= 8:
            out.append(bit_buffer & 0xFF)
            bit_buffer >>= 8
            bit_count -= 8

    code_size = min_code_size + 1
    dictionary = {bytes((i,)): i for i in range(clear)}
    next_code = end + 1

    emit(clear, code_size)
    current = b""
    for value in indices:
        candidate = current + bytes((value,))
        if candidate in dictionary:
            current = candidate
            continue
        emit(dictionary[current], code_size)
        if next_code < 4096:
            dictionary[candidate] = next_code
            next_code += 1
            if next_code - 1 == (1 << code_size) and code_size < 12:
                code_size += 1
        else:
            emit(clear, code_size)
            dictionary = {bytes((i,)): i for i in range(clear)}
            next_code = end + 1
            code_size = min_code_size + 1
        current = bytes((value,))
    if current:
        emit(dictionary[current], code_size)
    emit(end, code_size)
    if bit_count:
        out.append(bit_buffer & 0xFF)
    return bytes(out)


def image_block(indices, width, height, min_code_size, **kwargs) -> bytes:
    desc = image_descriptor(kwargs.pop("left", 0), kwargs.pop("top", 0),
                            width, height,
                            lct_bits=kwargs.pop("lct_bits", None),
                            interlaced=kwargs.pop("interlaced", False))
    lct = kwargs.pop("lct", b"")
    assert not kwargs, kwargs
    return (desc + lct + bytes((min_code_size,))
            + sub_blocks(lzw_encode(bytes(indices), min_code_size)))


TRAILER = b"\x3B"

# A palette whose entries are all distinct in all three channels, so a channel
# swap cannot look like a match.
PALETTE = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
           (255, 0, 255), (0, 255, 255), (30, 60, 90), (200, 180, 160)]


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def no_global_table():
    """Every image carries its own table and the screen carries none.

    Legal (89a 18 makes the Global Color Table optional) and rare enough that
    a decoder which assumes a global table is always present reads garbage.
    """
    w, h = 8, 4
    idx = [(x + y) % 4 for y in range(h) for x in range(w)]
    lct = table(PALETTE[:4], 1)
    data = (header() + lsd(w, h, gct_bits=None)
            + image_block(idx, w, h, 2, lct_bits=1, lct=lct) + TRAILER)
    write("gif_8x4_no_global_table.gif", data)


def index_past_palette():
    """A code stream naming index 7 against a four-entry table.

    The LZW code size is set independently of the table size, so this is
    expressible and appears in the wild.  Nothing is drawn for those pixels;
    the file is not refused.
    """
    w, h = 8, 2
    idx = [0, 1, 2, 3, 7, 7, 7, 7] + [1] * 8
    data = (header() + lsd(w, h, gct_bits=1)
            + table(PALETTE[:4], 1)
            + image_block(idx, w, h, 3) + TRAILER)
    write("gif_8x2_index_past_palette.gif", data)


def extensions_to_skip():
    """A Comment and a Plain Text block around the image.

    Neither is rendered by any decoder in use.  What matters is that they are
    walked past by their sub-block chain rather than parsed, so the image after
    them is still found.
    """
    w, h = 6, 3
    idx = [(x * y) % 6 for y in range(h) for x in range(w)]
    comment = b"\x21\xFE" + sub_blocks(b"a comment nothing renders")
    # Plain Text: a 12-byte header block, then a sub-block chain of text.
    plain = (b"\x21\x01\x0C" + struct.pack("<HHHH", 0, 0, 10, 10)
             + bytes((4, 6, 1, 0)) + sub_blocks(b"TEXT"))
    data = (header() + lsd(w, h, gct_bits=2) + table(PALETTE[:6], 2)
            + comment + plain
            + image_block(idx, w, h, 3)
            + b"\x21\xFE" + sub_blocks(b"trailing comment") + TRAILER)
    write("gif_6x3_extensions.gif", data)


def gif87a_header():
    """An 87a file, which must read exactly as an 89a one does."""
    w, h = 4, 4
    idx = [(x + y * 2) % 8 for y in range(h) for x in range(w)]
    data = (header(b"87a") + lsd(w, h, gct_bits=2) + table(PALETTE, 2)
            + image_block(idx, w, h, 3) + TRAILER)
    write("gif_4x4_87a.gif", data)


def offset_frames():
    """Two frames, the second a patch smaller than the canvas.

    The canvas is 12x8 and the second image is 4x4 at (6,2), so a decoder that
    ignores the image descriptor's position writes it in the wrong place and a
    decoder that ignores the canvas size returns the wrong dimensions.
    """
    cw, ch = 12, 8
    base = [(x // 3 + y // 2) % 6 for y in range(ch) for x in range(cw)]
    patch = [7, 7, 7, 7, 7, 3, 3, 7, 7, 3, 3, 7, 7, 7, 7, 7]
    data = (header() + lsd(cw, ch, gct_bits=2) + table(PALETTE, 2)
            + gce(delay=10, disposal=1)
            + image_block(base, cw, ch, 3)
            + gce(delay=10, disposal=1)
            + image_block(patch, 4, 4, 3, left=6, top=2)
            + TRAILER)
    write("gif_12x8_offset_frame.gif", data)


def disposal_previous():
    """Three frames whose middle one asks to be undone (disposal 3).

    The last frame must therefore composite over the first, not the second.
    Disposal 3 is the one writers use least and decoders get wrong most.
    """
    cw, ch = 8, 8
    first = [0] * (cw * ch)
    middle = [4] * 16
    last = [2, 2, 2, 2] * 4
    data = (header() + lsd(cw, ch, gct_bits=2) + table(PALETTE, 2)
            + gce(delay=5, disposal=1)
            + image_block(first, cw, ch, 3)
            + gce(delay=5, disposal=3)
            + image_block(middle, 4, 4, 3, left=0, top=0)
            + gce(delay=5, disposal=1)
            + image_block(last, 4, 4, 3, left=2, top=2)
            + TRAILER)
    write("gif_8x8_disposal_previous.gif", data)


def disposal_cycle():
    """Seven frames that use every disposal method, over an offset patch.

    A GIF frame is decoded by replaying the frames before it, so the canvas
    each frame starts from is the previous frame *after* its disposal was
    applied.  This fixture is the one that makes that chain visible: every
    disposal value appears, transparency appears with each of them, and the
    patches overlap so that getting the order wrong shows up as a pixel rather
    than as nothing.  A decoder that caches the replayed canvas has to arrive
    at the same picture as one that replays from the beginning every time, and
    that is what the tests check it against.
    """
    cw, ch = 10, 6
    full = [(x + y) % 6 for y in range(ch) for x in range(cw)]
    # (disposal, transparent index, left, top, w, h, fill index)
    patches = [
        (0, None, 0, 0, 4, 4, 1),
        (1, 0, 3, 1, 5, 4, 2),
        (2, None, 1, 2, 6, 3, 3),
        (3, 4, 2, 0, 4, 5, 4),
        (2, 2, 0, 3, 9, 3, 5),
        (1, None, 5, 0, 5, 6, 6),
    ]
    data = (header() + lsd(cw, ch, gct_bits=2) + table(PALETTE, 2)
            + gce(delay=3, disposal=1) + image_block(full, cw, ch, 3))
    for disposal, transparent, left, top, w, h, fill in patches:
        # A chequer rather than a flat fill, so that a frame written at the
        # wrong offset cannot line up with the one it replaced.
        idx = [fill if (x + y) % 2 == 0 else (fill + 3) % 8
               for y in range(h) for x in range(w)]
        data += (gce(delay=3, disposal=disposal, transparent=transparent)
                 + image_block(idx, w, h, 3, left=left, top=top))
    write("gif_10x6_disposal_cycle.gif", data + TRAILER)


def netscape_loop():
    """The Application Extension everything uses to say "repeat"."""
    w, h = 4, 2
    idx = [0, 1, 2, 3, 3, 2, 1, 0]
    loop = (b"\x21\xFF\x0BNETSCAPE2.0"
            + sub_blocks(b"\x01" + struct.pack("<H", 5)))
    data = (header() + lsd(w, h, gct_bits=2) + table(PALETTE, 2) + loop
            + gce(delay=7) + image_block(idx, w, h, 3) + TRAILER)
    write("gif_4x2_netscape_loop.gif", data)


def transparent_over_previous():
    """A second frame whose transparent pixels must leave the first showing."""
    cw, ch = 6, 4
    first = [1] * (cw * ch)
    second = [0 if (x + y) % 2 else 5 for y in range(ch) for x in range(cw)]
    data = (header() + lsd(cw, ch, gct_bits=2) + table(PALETTE, 2)
            + gce(delay=4, disposal=1)
            + image_block(first, cw, ch, 3)
            + gce(delay=4, disposal=1, transparent=0)
            + image_block(second, cw, ch, 3)
            + TRAILER)
    write("gif_6x4_transparent_over_previous.gif", data)


def truncated():
    """A file that stops in the middle of its code stream."""
    w, h = 16, 16
    idx = [(x * 3 + y) % 8 for y in range(h) for x in range(w)]
    whole = (header() + lsd(w, h, gct_bits=2) + table(PALETTE, 2)
             + image_block(idx, w, h, 3) + TRAILER)
    write("gif_16x16_truncated.gif", whole[:len(whole) - 12])


def pillow_fixtures():
    """The ordinary cases, from a writer that is not this file."""
    im = Image.new("P", (16, 8))
    im.putpalette([c for rgb in PALETTE for c in rgb] + [0] * (768 - 24))
    im.putdata([(x + y) % 8 for y in range(8) for x in range(16)])
    im.save(HERE / "gif_16x8_interlaced.gif", interlace=True)
    print("  gif_16x8_interlaced.gif")
    im.save(HERE / "gif_16x8_plain.gif", interlace=False)
    print("  gif_16x8_plain.gif")


def main():
    print("Generating GIF fixtures:")
    pillow_fixtures()
    no_global_table()
    index_past_palette()
    extensions_to_skip()
    gif87a_header()
    offset_frames()
    disposal_previous()
    disposal_cycle()
    netscape_loop()
    transparent_over_previous()
    truncated()


if __name__ == "__main__":
    main()
