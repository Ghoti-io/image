#!/usr/bin/env python3
"""Generate the TIFF fixtures.

Written rather than vendored, for the reason the PNG and GIF generators give:
a fixture whose bytes were produced by a script can be read back from the
script, and a property asserted about it - that two byte orders decode alike,
that a tiled file and a stripped file of the same picture agree - is pinned to
something other than this library's own opinion.

Every file here is uncompressed, because that is what the codec reads so far.
The compressed ones will arrive with the code that decompresses them.
"""

import struct

# Field types, TIFF 6.0 section 2.
BYTE, ASCII, SHORT, LONG, RATIONAL = 1, 2, 3, 4, 5

TAGS = {
    "NewSubfileType": 254,
    "ImageWidth": 256,
    "ImageLength": 257,
    "BitsPerSample": 258,
    "Compression": 259,
    "Photometric": 262,
    "StripOffsets": 273,
    "SamplesPerPixel": 277,
    "RowsPerStrip": 278,
    "StripByteCounts": 279,
    "XResolution": 282,
    "YResolution": 283,
    "PlanarConfig": 284,
    "ResolutionUnit": 296,
    "ColorMap": 320,
    "TileWidth": 322,
    "TileLength": 323,
    "TileOffsets": 324,
    "TileByteCounts": 325,
    "ExtraSamples": 338,
    "SampleFormat": 339,
}

TYPE_SIZE = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8}


def pack_values(endian, ftype, values):
    """The bytes of one field's values, in file order."""
    if ftype in (BYTE, ASCII):
        return bytes(values)
    if ftype == SHORT:
        return b"".join(struct.pack(endian + "H", v) for v in values)
    if ftype == LONG:
        return b"".join(struct.pack(endian + "I", v) for v in values)
    if ftype == RATIONAL:
        return b"".join(struct.pack(endian + "II", n, d) for n, d in values)
    raise ValueError(ftype)


def build(endian, ifds, version=42):
    """Lay out a whole TIFF.

    `ifds` is a list of (fields, data) where fields is a list of
    (tag, type, values) and data is the strip or tile bytes for that IFD.
    A field whose values do not fit in four bytes goes into an overflow area,
    and so does each IFD's pixel data; the directories follow both.

    Two passes, because the directories' own offsets depend on how much
    overflow there is and the overflow is only known once every field has been
    packed. Getting that wrong is not loud: the first draft laid the
    directories out before stashing the long fields, which moved every
    directory by the length of a ColorMap and made four fixtures load as
    "the offsets and the byte counts do not describe the same blocks".
    """
    e = "<" if endian == "II" else ">"
    pool = bytearray()
    pool_base = 8

    def stash(raw):
        at = pool_base + len(pool)
        pool.extend(raw)
        if len(pool) % 2:
            pool.append(0)
        return at

    data_at = [stash(data) for _fields, data in ifds]

    # Pack every field, resolving "@data" to where the pixels landed.
    entries = []
    for (fields, _data), at in zip(ifds, data_at):
        rows = []
        for tag, ftype, values in sorted(fields, key=lambda f: f[0]):
            if values == "@data":
                values = [at]
            raw = pack_values(e, ftype, values)
            rows.append((tag, ftype, len(values), raw))
        entries.append(rows)

    overflow_total = sum(len(raw) + (len(raw) % 2)
                         for rows in entries for *_x, raw in rows
                         if len(raw) > 4)
    sizes = [2 + 12 * len(rows) + 4 for rows in entries]
    cursor = pool_base + len(pool) + overflow_total
    ifd_at = []
    for size in sizes:
        ifd_at.append(cursor)
        cursor += size

    overflow = bytearray()
    directories = bytearray()
    for i, rows in enumerate(entries):
        out = bytearray(struct.pack(e + "H", len(rows)))
        for tag, ftype, count, raw in rows:
            out += struct.pack(e + "HHI", tag, ftype, count)
            if len(raw) <= 4:
                out += raw + b"\x00" * (4 - len(raw))
            else:
                place = pool_base + len(pool) + len(overflow)
                overflow.extend(raw)
                if len(overflow) % 2:
                    overflow.append(0)
                out += struct.pack(e + "I", place)
        nxt = ifd_at[i + 1] if i + 1 < len(ifd_at) else 0
        out += struct.pack(e + "I", nxt)
        directories += out

    assert len(overflow) == overflow_total, (len(overflow), overflow_total)
    header = endian.encode() + struct.pack(e + "H", version)
    return bytes(header + struct.pack(e + "I", ifd_at[0]) + bytes(pool) +
                 bytes(overflow) + bytes(directories))


def write(name, blob):
    with open(name, "wb") as f:
        f.write(blob)
    print(f"  {name}: {len(blob)} bytes")


def gray_ramp(w, h):
    """A picture with no symmetry, so a transposed or mirrored decode shows."""
    return bytes(((y * w + x) * 255 // max(1, w * h - 1)) for y in range(h)
                 for x in range(w))


def rgb_ramp(w, h):
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes([x * 255 // max(1, w - 1),
                          y * 255 // max(1, h - 1),
                          (x + y) * 255 // max(1, w + h - 2)])
    return bytes(out)


def strip_fields(w, h, data, photometric, spp=1, extra=None, rows=None,
                 colormap=None, bps=8):
    rows = h if rows is None else rows
    row_bytes = w * spp
    strips = (h + rows - 1) // rows
    # One strip here, or several; the offsets are filled in by build() only
    # for the single-strip case, so multi-strip files pass explicit offsets.
    fields = [
        (TAGS["ImageWidth"], LONG, [w]),
        (TAGS["ImageLength"], LONG, [h]),
        (TAGS["BitsPerSample"], SHORT, [bps] * spp),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [photometric]),
        (TAGS["SamplesPerPixel"], SHORT, [spp]),
        (TAGS["RowsPerStrip"], LONG, [rows]),
        (TAGS["PlanarConfig"], SHORT, [1]),
    ]
    if strips == 1:
        fields.append((TAGS["StripOffsets"], LONG, "@data"))
        fields.append((TAGS["StripByteCounts"], LONG, [len(data)]))
    else:
        fields.append((TAGS["StripOffsets"], LONG, "@strips"))
        fields.append((TAGS["StripByteCounts"], LONG,
                       [min(rows, h - s * rows) * row_bytes
                        for s in range(strips)]))
    if extra is not None:
        fields.append((TAGS["ExtraSamples"], SHORT, [extra]))
    if colormap is not None:
        fields.append((TAGS["ColorMap"], SHORT, colormap))
    return fields


def main():
    W = H = 4
    gray = gray_ramp(W, H)

    # ---- The byte-order pair: the same picture written both ways ----
    for endian, name in (("II", "tiff_4x4_gray8_le.tif"),
                         ("MM", "tiff_4x4_gray8_be.tif")):
        write(name, build(endian,
                          [(strip_fields(W, H, gray, 1), gray)]))

    # ---- WhiteIsZero: the complement of the same samples ----
    write("tiff_4x4_whitezero.tif",
          build("II", [(strip_fields(W, H, gray, 0), gray)]))

    # ---- RGB, and RGB with an alpha channel, associated and not ----
    rgb = rgb_ramp(W, H)
    write("tiff_4x4_rgb.tif",
          build("II", [(strip_fields(W, H, rgb, 2, spp=3), rgb)]))

    rgba = bytearray()
    for i in range(W * H):
        rgba += rgb[i * 3:i * 3 + 3] + bytes([(i * 17) % 256])
    rgba = bytes(rgba)
    write("tiff_4x4_rgba.tif",
          build("II", [(strip_fields(W, H, rgba, 2, spp=4, extra=2), rgba)]))

    # Associated alpha: the colour is premultiplied, so the same picture is
    # stored darker and the decoder has to divide it back out.
    assoc = bytearray()
    for i in range(W * H):
        a = (i * 17) % 256
        for k in range(3):
            assoc.append(rgb[i * 3 + k] * a // 255)
        assoc.append(a)
    assoc = bytes(assoc)
    write("tiff_4x4_rgba_associated.tif",
          build("II", [(strip_fields(W, H, assoc, 2, spp=4, extra=1),
                        assoc)]))

    # ---- Palette: indices into a 16-bit ColorMap ----
    idx = bytes((x + y) % 4 for y in range(H) for x in range(W))
    # TIFF 6.0 section 8: a ColorMap has 3 * 2**BitsPerSample entries, reds
    # then greens then blues, 16-bit, whether or not the picture uses them
    # all. A first draft of this fixture stored four entries per channel and
    # the codec refused it, correctly - the map has to be the size the depth
    # implies, because an index is only bounded by the depth.
    entries = 1 << 8
    reds = [0] * entries
    greens = [0] * entries
    blues = [0] * entries
    for i, (r, g, b) in enumerate(
            [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (32768, 32768, 65535)]):
        reds[i], greens[i], blues[i] = r, g, b
    cmap = reds + greens + blues
    write("tiff_4x4_palette.tif",
          build("II", [(strip_fields(W, H, idx, 3, colormap=cmap), idx)]))

    # ---- The same palette, written the way many real writers write it ----
    # TIFF 6.0 says a ColorMap entry is 16-bit, and plenty of encoders store
    # an 8-bit value in the field anyway. Both readings are defensible from
    # the bytes alone and they disagree by a factor of 257, so this fixture
    # exists to make whichever one this library picks visible and measured
    # rather than assumed.
    small_r = [0] * 256
    small_g = [0] * 256
    small_b = [0] * 256
    for i, (r, g, b) in enumerate(
            [(0, 0, 0), (255, 0, 0), (0, 255, 0), (128, 128, 255)]):
        small_r[i], small_g[i], small_b[i] = r, g, b
    write("tiff_4x4_palette_8bit_map.tif",
          build("II", [(strip_fields(W, H, idx, 3,
                                     colormap=small_r + small_g + small_b),
                        idx)]))

    # ---- Several strips of one picture ----
    # Written by hand because the strips need their own offsets.
    big = gray_ramp(8, 8)
    write("tiff_8x8_four_strips.tif", build_multi_strip("II", 8, 8, big, 2))

    # ---- Tiles, with the picture not a whole number of tiles across ----
    write("tiff_6x6_tiled.tif", build_tiled("II", 6, 6, gray_ramp(6, 6), 4, 4))
    # The same picture stripped, so the two can be required to agree.
    six = gray_ramp(6, 6)
    write("tiff_6x6_stripped.tif",
          build("II", [(strip_fields(6, 6, six, 1), six)]))

    # ---- Two pages ----
    a = gray_ramp(4, 4)
    b = bytes(255 - v for v in a)
    write("tiff_two_pages.tif",
          build("II", [(strip_fields(4, 4, a, 1), a),
                       (strip_fields(4, 4, b, 1), b)]))

    # ---- Refusals ----
    write("tiff_bad_magic.tif", b"II\x2b\x00" + b"\x00" * 12)
    lzw = strip_fields(W, H, gray, 1)
    lzw = [(t, ty, v) if t != TAGS["Compression"] else (t, ty, [5])
           for (t, ty, v) in lzw]
    write("tiff_lzw_unsupported.tif", build("II", [(lzw, gray)]))
    no_photo = [f for f in strip_fields(W, H, gray, 1)
                if f[0] != TAGS["Photometric"]]
    write("tiff_no_photometric.tif", build("II", [(no_photo, gray)]))


def build_multi_strip(endian, w, h, data, rows):
    """A stripped file whose strips each need their own offset."""
    e = "<" if endian == "II" else ">"
    strips = (h + rows - 1) // rows
    row_bytes = w
    pieces = [data[s * rows * row_bytes:
                   min(h, (s + 1) * rows) * row_bytes] for s in range(strips)]
    pool = bytearray()
    offsets = []
    for p in pieces:
        offsets.append(8 + len(pool))
        pool.extend(p)
        if len(pool) % 2:
            pool.append(0)
    fields = [
        (TAGS["ImageWidth"], LONG, [w]),
        (TAGS["ImageLength"], LONG, [h]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [rows]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["StripOffsets"], LONG, offsets),
        (TAGS["StripByteCounts"], LONG, [len(p) for p in pieces]),
    ]
    return _finish(e, pool, [fields])


def build_tiled(endian, w, h, data, tw, th):
    """A tiled file. Every tile is stored full size; the parts that fall off
    the right and bottom edges are padding and are dropped on read."""
    e = "<" if endian == "II" else ">"
    across = (w + tw - 1) // tw
    down = (h + th - 1) // th
    pool = bytearray()
    offsets, counts = [], []
    for ty in range(down):
        for tx in range(across):
            tile = bytearray()
            for row in range(th):
                y = ty * th + row
                for col in range(tw):
                    x = tx * tw + col
                    tile.append(data[y * w + x] if (x < w and y < h) else 0)
            offsets.append(8 + len(pool))
            counts.append(len(tile))
            pool.extend(tile)
            if len(pool) % 2:
                pool.append(0)
    fields = [
        (TAGS["ImageWidth"], LONG, [w]),
        (TAGS["ImageLength"], LONG, [h]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["TileWidth"], LONG, [tw]),
        (TAGS["TileLength"], LONG, [th]),
        (TAGS["TileOffsets"], LONG, offsets),
        (TAGS["TileByteCounts"], LONG, counts),
    ]
    return _finish(e, pool, [fields])


def _finish(e, pool, ifds):
    """Lay out directories after a pool that is already built."""
    pool = bytearray(pool)
    pool_base = 8

    def stash(raw):
        at = pool_base + len(pool)
        pool.extend(raw)
        if len(pool) % 2:
            pool.append(0)
        return at

    sizes = [2 + 12 * len(f) + 4 for f in ifds]
    cursor = pool_base + len(pool)
    at = []
    for s in sizes:
        at.append(cursor)
        cursor += s
    # Any field too long for four bytes lands in the pool, which grows the
    # gap; lay the directories out again once it has settled.
    overflow = bytearray()
    entries = []
    for fields in ifds:
        rows = []
        for tag, ftype, values in sorted(fields, key=lambda f: f[0]):
            raw = pack_values(e, ftype, values)
            rows.append((tag, ftype, len(values), raw))
        entries.append(rows)
    total_overflow = sum(len(raw) + (len(raw) % 2)
                         for rows in entries for *_x, raw in rows
                         if len(raw) > 4)
    cursor = pool_base + len(pool) + total_overflow
    at = []
    for s in sizes:
        at.append(cursor)
        cursor += s

    directories = bytearray()
    for i, rows in enumerate(entries):
        out = bytearray(struct.pack(e + "H", len(rows)))
        for tag, ftype, count, raw in rows:
            out += struct.pack(e + "HHI", tag, ftype, count)
            if len(raw) <= 4:
                out += raw + b"\x00" * (4 - len(raw))
            else:
                place = pool_base + len(pool) + len(overflow)
                overflow.extend(raw)
                if len(overflow) % 2:
                    overflow.append(0)
                out += struct.pack(e + "I", place)
        out += struct.pack(e + "I", at[i + 1] if i + 1 < len(at) else 0)
        directories += out

    header = (b"II" if e == "<" else b"MM") + struct.pack(e + "H", 42)
    return bytes(header + struct.pack(e + "I", at[0]) + bytes(pool) +
                 bytes(overflow) + bytes(directories))


if __name__ == "__main__":
    main()
