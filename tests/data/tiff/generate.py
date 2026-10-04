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

import os
import struct
import zlib

# Field types, TIFF 6.0 section 2.
BYTE, ASCII, SHORT, LONG, RATIONAL = 1, 2, 3, 4, 5
# UNDEFINED, which is how an ICC profile is stored: a byte string the format
# has no opinion about.
UNDEFINED = 7

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
    "WhitePoint": 318,
    "PrimaryChromaticities": 319,
    "ColorMap": 320,
    "TileWidth": 322,
    "TileLength": 323,
    "TileOffsets": 324,
    "TileByteCounts": 325,
    "NewSubfileType": 254,
    "SubIFDs": 330,
    "YCbCrSubSampling": 530,
    "ImageDescription": 270,
    "DocumentName": 269,
    "Make": 271,
    "Model": 272,
    "PageName": 285,
    "Software": 305,
    "DateTime": 306,
    "Artist": 315,
    "HostComputer": 316,
    "Copyright": 33432,
    "Orientation": 274,
    "Predictor": 317,
    "XMP": 700,
    "ICCProfile": 34675,
    "ExtraSamples": 338,
    "SampleFormat": 339,
    "JPEGProc": 512,
    "JPEGInterchangeFormat": 513,
    "JPEGInterchangeFormatLength": 514,
    "JPEGQTables": 519,
    "JPEGDCTables": 520,
    "JPEGACTables": 521,
    "JPEGTables": 347,
}

TYPE_SIZE = {BYTE: 1, ASCII: 1, UNDEFINED: 1, SHORT: 2, LONG: 4,
             RATIONAL: 8}


def pack_values(endian, ftype, values):
    """The bytes of one field's values, in file order."""
    if ftype in (BYTE, ASCII, UNDEFINED):
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


def packbits(data):
    """PackBits, as TIFF 6.0 section 9 defines it.

    A literal run is a count byte 0..127 followed by count+1 bytes; a repeat
    is 257-count for 2..128 copies of the byte that follows. 128 is a no-op
    and is never written.
    """
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        # A run of three or more identical bytes is worth encoding as one.
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 128:
            run += 1
        if run >= 3:
            out.append(257 - run)
            out.append(data[i])
            i += run
            continue
        # Otherwise gather literals until a run of three shows up.
        start = i
        i += 1
        while i < n and i - start < 128:
            if (i + 2 < n and data[i] == data[i + 1] == data[i + 2]):
                break
            i += 1
        out.append(i - start - 1)
        out.extend(data[start:i])
    return bytes(out)


def tiff_lzw(data):
    """TIFF 6.0 section 13 LZW: 8-bit literals, codes packed MSB first.

    Written out rather than taken from a library because a fixture generated
    by the same code that reads it proves nothing. The two ends here are
    independent: this is the specification's algorithm, and the decoder is
    Ghoti.io Compress's.

    The width increases one code *early* - at 511, 1023 and 2047 rather than
    512, 1024 and 2048 - which is the detail every LZW-in-TIFF implementation
    has had to discover, and getting it wrong shifts every code after the
    first 254 by one bit.
    """
    CLEAR, EOI = 256, 257
    out = bytearray()
    acc = 0
    nbits = 0

    def emit(code, width):
        nonlocal acc, nbits
        acc = (acc << width) | code
        nbits += width
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)

    table = {bytes([i]): i for i in range(256)}
    nxt = 258
    width = 9
    emit(CLEAR, width)
    w = b""
    for ch in data:
        wc = w + bytes([ch])
        if wc in table:
            w = wc
            continue
        emit(table[w], width)
        table[wc] = nxt
        nxt += 1
        # Early change: the width goes up one code before the table is full.
        if nxt + 1 > (1 << width) and width < 12:
            width += 1
        elif nxt + 1 > (1 << 12):
            emit(CLEAR, width)
            table = {bytes([i]): i for i in range(256)}
            nxt = 258
            width = 9
        w = bytes([ch])
    if w:
        emit(table[w], width)
    emit(EOI, width)
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def horizontal_difference(data, w, h, spp=1):
    """Predictor 2: each sample minus the one a pixel to its left."""
    out = bytearray(data)
    row_bytes = w * spp
    for y in range(h):
        base = y * row_bytes
        for i in range(row_bytes - 1, spp - 1, -1):
            out[base + i] = (out[base + i] - out[base + i - spp]) & 0xFF
    return bytes(out)


def pack_rows(values, w, h, bps, spp=1):
    """Pack samples most significant bit first, each row starting on a byte.

    TIFF 6.0 section 3: a row is padded to a byte boundary, which is why a
    6-pixel 4-bit row is three bytes and not two and a half.
    """
    out = bytearray()
    for y in range(h):
        acc = 0
        nbits = 0
        for x in range(w * spp):
            acc = (acc << bps) | (values[y * w * spp + x] & ((1 << bps) - 1))
            nbits += bps
            while nbits >= 8:
                nbits -= 8
                out.append((acc >> nbits) & 0xFF)
        if nbits:
            out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def strip_fields(w, h, data, photometric, spp=1, extra=None, rows=None,
                 colormap=None, bps=8):
    rows = h if rows is None else rows
    row_bytes = (w * spp * bps + 7) // 8
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

    # ---- Every bit depth this codec reads, at both ends of the range ----
    #
    # The sub-byte depths are where a row's byte padding shows: 6 pixels at 4
    # bits is three bytes, and a decoder that computed two and a half shears
    # the picture one pixel further left on every row.
    for bps in (1, 2, 4):
        top = (1 << bps) - 1
        vals = [((y * 6 + x) * top // (6 * 5 - 1)) for y in range(5)
                for x in range(6)]
        packed = pack_rows(vals, 6, 5, bps)
        write("tiff_6x5_gray%d.tif" % bps,
              build("II", [(strip_fields(6, 5, packed, 1, bps=bps), packed)]))

    # 16-bit, in both byte orders, because a 16-bit sample is the one place a
    # file's declared order reaches past the header and into the pixels.
    wide = bytearray()
    for i in range(W * H):
        wide += struct.pack("<H", i * 65535 // (W * H - 1))
    wide_be = bytearray()
    for i in range(W * H):
        wide_be += struct.pack(">H", i * 65535 // (W * H - 1))
    write("tiff_4x4_gray16_le.tif",
          build("II", [(strip_fields(W, H, bytes(wide), 1, bps=16),
                        bytes(wide))]))
    write("tiff_4x4_gray16_be.tif",
          build("MM", [(strip_fields(W, H, bytes(wide_be), 1, bps=16),
                        bytes(wide_be))]))

    rgb16 = bytearray()
    for y in range(H):
        for x in range(W):
            for v in (x * 65535 // (W - 1), y * 65535 // (H - 1),
                      (x + y) * 65535 // (W + H - 2)):
                rgb16 += struct.pack("<H", v)
    write("tiff_4x4_rgb16.tif",
          build("II", [(strip_fields(W, H, bytes(rgb16), 2, spp=3, bps=16),
                        bytes(rgb16))]))

    # A 4-bit palette: sixteen entries, and the indices packed two to a byte.
    idx4 = [((x + y) % 4) for y in range(H) for x in range(W)]
    packed4 = pack_rows(idx4, W, H, 4)
    entries4 = 1 << 4
    r4 = [0] * entries4
    g4 = [0] * entries4
    b4 = [0] * entries4
    for i, (r, g, b) in enumerate(
            [(0, 0, 0), (65535, 0, 0), (0, 65535, 0), (32768, 32768, 65535)]):
        r4[i], g4[i], b4[i] = r, g, b
    write("tiff_4x4_palette4.tif",
          build("II", [(strip_fields(W, H, packed4, 3, bps=4,
                                     colormap=r4 + g4 + b4), packed4)]))

    # ---- The same RGB picture, with the channels stored apart ----
    # PlanarConfiguration 2 is a storage layout and nothing else, so this file
    # and tiff_4x4_rgb.tif must decode to the same bytes - a property that
    # needs no reference decoder, and the same shape as the byte-order pair
    # and the tiled/stripped pair.
    planes = bytearray()
    for c in range(3):
        for i in range(W * H):
            planes.append(rgb[i * 3 + c])
    plane_len = W * H
    write("tiff_4x4_rgb_planar.tif",
          build_planar("II", W, H, bytes(planes), 3, 2, plane_len))

    # ---- Separated: four inks, which is CMYK in every file that uses it ----
    cmyk = bytearray()
    for y in range(H):
        for x in range(W):
            cmyk += bytes([x * 255 // (W - 1), y * 255 // (H - 1),
                           (x + y) * 255 // (W + H - 2), 16])
    cmyk = bytes(cmyk)
    write("tiff_4x4_cmyk.tif",
          build("II", [(strip_fields(W, H, cmyk, 5, spp=4), cmyk)]))

    # ---- The same picture under every compression this codec undoes ----
    #
    # One picture, five spellings: a family where each member must decode to
    # the same bytes as the stored one, so a compression that is subtly wrong
    # fails rather than merely looking plausible. The predictor pair is the
    # same idea one level down - horizontal differencing is reversible, so it
    # cannot change the picture either.
    big8 = gray_ramp(16, 8)
    variants = [
        ("tiff_16x8_none.tif", 1, big8, 1),
        ("tiff_16x8_packbits.tif", 32773, packbits(big8), 1),
        ("tiff_16x8_lzw.tif", 5, tiff_lzw(big8), 1),
        ("tiff_16x8_deflate.tif", 8, zlib.compress(big8, 6), 1),
        ("tiff_16x8_lzw_predictor.tif", 5,
         tiff_lzw(horizontal_difference(big8, 16, 8)), 2),
        ("tiff_16x8_deflate_predictor.tif", 8,
         zlib.compress(horizontal_difference(big8, 16, 8), 6), 2),
    ]
    for name, comp, payload, predictor in variants:
        fields = strip_fields(16, 8, payload, 1)
        fields = [(t, ty, [comp] if t == TAGS["Compression"] else v)
                  for (t, ty, v) in fields]
        fields = [(t, ty, [len(payload)] if t == TAGS["StripByteCounts"] else v)
                  for (t, ty, v) in fields]
        if predictor != 1:
            fields.append((TAGS["Predictor"], SHORT, [predictor]))
        write(name, build("II", [(fields, payload)]))

    # ---- Metadata: a profile, an orientation, a description, an XMP packet --
    #
    # The profile is opaque to this library - it is carried, never
    # interpreted - so a blob with a plausible ICC header is enough to check
    # that it survives a load and a save unchanged. That is the whole claim:
    # a TIFF is what professional colour work is stored in, and a profile
    # dropped in passing makes a file's colours mean something else.
    profile = bytearray(b"\x00\x00\x01\x28")        # Size, as a profile has
    profile += b"ADBEmntrRGB XYZ "                     # Signatures
    profile += bytes(range(256)) * 1
    profile = bytes(profile[:296])
    xmp = (b'<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?>'
           b'<x:xmpmeta xmlns:x="adobe:ns:meta/"></x:xmpmeta>'
           b'<?xpacket end="w"?>')
    meta = strip_fields(W, H, rgb, 2, spp=3)
    meta.append((TAGS["ImageDescription"], ASCII, list(b"a fixture\x00")))
    # Short enough that some land in the entry itself (four bytes or fewer)
    # and some in the pool, which is the split a round trip has to survive.
    meta.append((TAGS["DocumentName"], ASCII, list(b"ab\x00")))
    meta.append((TAGS["Make"], ASCII, list(b"Acme\x00")))
    meta.append((TAGS["Model"], ASCII, list(b"Flatbed\x00")))
    meta.append((TAGS["PageName"], ASCII, list(b"p1\x00")))
    meta.append((TAGS["Software"], ASCII, list(b"ghoti\x00")))
    meta.append((TAGS["DateTime"], ASCII, list(b"2026:10:04 01:09:00\x00")))
    meta.append((TAGS["Artist"], ASCII, list(b"Ada\x00")))
    meta.append((TAGS["HostComputer"], ASCII, list(b"desk\x00")))
    # Two notices, photographer then editor, separated by a NUL. A reader
    # that stops at the first NUL keeps only "photo".
    meta.append((TAGS["Copyright"], ASCII, list(b"photo\x00editor\x00")))
    meta.append((TAGS["Orientation"], SHORT, [6]))
    meta.append((TAGS["XMP"], BYTE, list(xmp)))
    meta.append((TAGS["ICCProfile"], UNDEFINED, list(profile)))
    write("tiff_4x4_metadata.tif", build("II", [(meta, rgb)]))

    # ---- WhitePoint (318) and PrimaryChromaticities (319) ----
    #
    # TIFF 6.0 states both as RATIONALs, so a gamut written this way is exact
    # and needs no matching against a table. These are Adobe RGB's, which
    # gimg_gamut_identify() has a name for; the point of the fixture is that
    # the two tags are read at all, which they were not until the colour model
    # could hold what they say.
    #
    # The second file carries a white point and no primaries. A gamut needs
    # all three primaries, so that file states no gamut - but the white point
    # is still what the file said, and dropping it would be a loss no error
    # reports.
    def rat(x, den=10000):
        return (int(round(x * den)), den)

    adobe = [rat(0.6400), rat(0.3300), rat(0.2100), rat(0.7100),
             rat(0.1500), rat(0.0600)]
    d65 = [rat(0.3127), rat(0.3290)]

    chroma = strip_fields(W, H, rgb, 2, spp=3)
    chroma.append((TAGS["WhitePoint"], RATIONAL, d65))
    chroma.append((TAGS["PrimaryChromaticities"], RATIONAL, adobe))
    write("tiff_chromaticities.tif", build("II", [(chroma, rgb)]))

    white_only = strip_fields(W, H, rgb, 2, spp=3)
    white_only.append((TAGS["WhitePoint"], RATIONAL, d65))
    write("tiff_white_point_only.tif", build("II", [(white_only, rgb)]))

    # ---- A real ICC profile in tag 34675 ----
    #
    # tiff_4x4_metadata.tif above carries a plausible-looking blob, which is
    # the right fixture for "an opaque profile survives a round trip". This
    # one carries a profile littleCMS parses and transforms with, and it is
    # the swap-red-and-green one: a reader that applies it shows the two
    # channels exchanged, and a reader that only carries it shows the pixels
    # unchanged. See tests/data/icc/generate.py.
    here = os.path.dirname(os.path.abspath(__file__))
    icc_path = os.path.join(os.path.dirname(here), "icc", "swap_rg.icc")
    if not os.path.exists(icc_path):
        raise SystemExit(
            "missing %s; run tests/data/icc/generate.py first" % icc_path)
    with open(icc_path, "rb") as f:
        real_icc = f.read()
    icc_fields = strip_fields(W, H, rgb, 2, spp=3)
    icc_fields.append((TAGS["ICCProfile"], UNDEFINED, list(real_icc)))
    write("tiff_icc_swap_rg.tif", build("II", [(icc_fields, rgb)]))

    # ---- Sixteen bits under the predictor, in both byte orders ----
    #
    # Horizontal differencing at sixteen bits is the one path where the
    # differences are taken in the *file's* byte order: swapping first and
    # adding afterwards gives a different number, so a decoder that reorders
    # before summing produces noise on a big-endian file and the right
    # picture on a little-endian one. Both are here for that reason.
    def diff16(values, w, h, endian):
        out = bytearray()
        for y in range(h):
            prev = 0
            for x in range(w):
                v = values[y * w + x]
                out += struct.pack(endian + "H", (v - prev) & 0xFFFF)
                prev = v
        return bytes(out)

    wide8 = [((y * 8 + x) * 65535) // 63 for y in range(8) for x in range(8)]
    for endian, suffix in (("<", "le"), (">", "be")):
        plain = b"".join(struct.pack(endian + "H", v) for v in wide8)
        order = "II" if endian == "<" else "MM"
        write("tiff_8x8_gray16_%s_plain.tif" % suffix,
              build(order, [(strip_fields(8, 8, plain, 1, bps=16), plain)]))
        packed = zlib.compress(diff16(wide8, 8, 8, endian), 6)
        fields = strip_fields(8, 8, packed, 1, bps=16)
        fields = [(t, ty, [8] if t == TAGS["Compression"] else v)
                  for (t, ty, v) in fields]
        fields = [(t, ty, [len(packed)]
                   if t == TAGS["StripByteCounts"] else v)
                  for (t, ty, v) in fields]
        fields.append((TAGS["Predictor"], SHORT, [2]))
        write("tiff_8x8_gray16_%s_predictor.tif" % suffix,
              build(order, [(fields, packed)]))

    # ---- YCbCr, at both subsamplings, carrying a grey picture ----
    #
    # Cb and Cr held at 128 means no colour at all, and the conversion then
    # has to give R = G = B = Y exactly, whatever the coefficients are and
    # whatever the subsampling is. That is a property worth more than a
    # comparison: it needs no reference decoder, it is exact rather than
    # approximate, and it fails loudly if the chroma is read from the wrong
    # place - which is the mistake subsampling invites.
    ramp8 = gray_ramp(8, 8)
    # 1x1: every pixel carries its own Cb and Cr, so the layout is plain
    # interleaved YCbCr.
    ycc11 = bytearray()
    for v in ramp8:
        ycc11 += bytes([v, 128, 128])
    f11 = strip_fields(8, 8, bytes(ycc11), 6, spp=3)
    f11.append((TAGS["YCbCrSubSampling"], SHORT, [1, 1]))
    write("tiff_8x8_ycbcr_11.tif", build("II", [(f11, bytes(ycc11))]))

    # 2x2: four luma samples then one Cb and one Cr, a unit row spanning two
    # image rows.
    ycc22 = bytearray()
    for uy in range(4):
        for ux in range(4):
            for iy in range(2):
                for ix in range(2):
                    ycc22.append(ramp8[(uy * 2 + iy) * 8 + (ux * 2 + ix)])
            ycc22 += bytes([128, 128])
    f22 = strip_fields(8, 8, bytes(ycc22), 6, spp=3)
    f22 = [(t, ty, [len(ycc22)] if t == TAGS["StripByteCounts"] else v)
           for (t, ty, v) in f22]
    f22.append((TAGS["YCbCrSubSampling"], SHORT, [2, 2]))
    write("tiff_8x8_ycbcr_22.tif", build("II", [(f22, bytes(ycc22))]))

    # ---- A pyramid, in both of the format's spellings ----
    #
    # TIFF says a reduced-resolution version of an image two ways: a
    # directory in the main chain whose NewSubfileType has bit 0 set, and a
    # SubIFD hanging off the full-size page (Technical Note 1). Both are
    # pyramids and neither is a second picture, which is what
    # GIMG_ITEM_LEVEL exists to say.
    full = gray_ramp(8, 8)
    half = bytes(full[(y * 2) * 8 + (x * 2)] for y in range(4)
                 for x in range(4))
    chain_full = strip_fields(8, 8, full, 1)
    chain_half = strip_fields(4, 4, half, 1)
    chain_half.append((TAGS["NewSubfileType"], LONG, [1]))
    write("tiff_pyramid_chain.tif",
          build("II", [(chain_full, full), (chain_half, half)]))
    write("tiff_pyramid_subifd.tif", build_subifd("II", full, half))

    # ---- What the fuzzer found ----
    #
    # A consistent-looking file - one strip, one offset, one byte count -
    # that declares 536,870,920 rows per strip for an 8-row image. Nothing
    # about it is contradictory, and sizing the decompression buffer by the
    # tag rather than by the picture asked for 8.6 GB. It decodes now; the
    # fixture is here so that it keeps doing so.
    huge_rows = strip_fields(16, 8, gray_ramp(16, 8), 1)
    huge_rows = [(t, ty, [0x20000008] if t == TAGS["RowsPerStrip"] else v)
                 for (t, ty, v) in huge_rows]
    write("tiff_16x8_absurd_rows_per_strip.tif",
          build("II", [(huge_rows, gray_ramp(16, 8))]))

    # ---- JPEG in a TIFF ----
    #
    # Grayscale on purpose. Both corpus files that use the 1992 spelling are
    # YCbCr, and there libtiff and this codec disagree about the colour model
    # for a reason that has nothing to do with the JPEG: libtiff applies
    # ReferenceBlackWhite for compression 6 and ignores it for compression 7,
    # which a minimal pair in notes/image/status.md pins down. A grayscale
    # file has no such question, so it is the one shape where the sweep can
    # demand an exact match and get one.
    #
    # The JPEG is a fixture of the JPEG codec, re-cut rather than re-encoded:
    # what these files test is the cutting.
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "jpeg", "baseline_640x480_gray.jpg"),
               "rb").read()
    dqt, dht, qbody, dcbody, acbody, sof, sos, entropy = split_jpeg(src)
    jh, jw = struct.unpack(">HH", sof[5:9])

    # Compression 7: the tables in one tag as whole segments, the frame in
    # the strip. This is what Technical Note 2 defines and what everything
    # written since 1995 uses.
    tables = b"\xff\xd8" + b"".join(dqt) + b"".join(dht) + b"\xff\xd9"
    frame = b"\xff\xd8" + sof + sos + entropy + b"\xff\xd9"
    write("tiff_jpeg_gray.tif", build_flat("II", [
        (TAGS["ImageWidth"], LONG, [jw]),
        (TAGS["ImageLength"], LONG, [jh]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [7]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [jh]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["StripOffsets"], LONG, ("@blob", 0)),
        (TAGS["StripByteCounts"], LONG, [len(frame)]),
        (TAGS["JPEGTables"], UNDEFINED, tables),
    ], [frame]))

    # Compression 6, the 1992 spelling, in the shape that carries no JPEG at
    # all: the tables are bare arrays at file offsets and the strip holds
    # entropy-coded data with no frame header in front of it, so a decoder
    # has to write the JPEG the file did not.
    write("tiff_ojpeg_gray.tif", build_flat("II", [
        (TAGS["ImageWidth"], LONG, [jw]),
        (TAGS["ImageLength"], LONG, [jh]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [6]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [jh]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["StripOffsets"], LONG, ("@blob", 0)),
        (TAGS["StripByteCounts"], LONG, [len(entropy)]),
        (TAGS["JPEGProc"], SHORT, [1]),
        (TAGS["JPEGQTables"], LONG, ("@blob", 1)),
        (TAGS["JPEGDCTables"], LONG, ("@blob", 2)),
        (TAGS["JPEGACTables"], LONG, ("@blob", 3)),
    ], [entropy, qbody[0], dcbody[0], acbody[0]]))

    # The other shape of compression 6: tags 513 and 514 point at a complete
    # JPEG datastream and the strips are beside the point. Its scan header
    # gets the zeros a 1992 encoder wrote, because that is the thing the
    # reader has to survive - T.81 B.2.3 reads Ss=0 Se=0 as a progressive DC
    # scan, and this frame is baseline.
    sos_1992 = sos[:-3] + b"\x00\x00\x00"
    whole = b"\xff\xd8" + b"".join(dqt) + b"".join(dht) + sof + sos_1992 + \
        entropy + b"\xff\xd9"
    write("tiff_ojpeg_gray_interchange.tif", build_flat("II", [
        (TAGS["ImageWidth"], LONG, [jw]),
        (TAGS["ImageLength"], LONG, [jh]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [6]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [jh]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["StripOffsets"], LONG, ("@blob", 0)),
        (TAGS["StripByteCounts"], LONG, [len(whole)]),
        (TAGS["JPEGProc"], SHORT, [1]),
        (TAGS["JPEGInterchangeFormat"], LONG, ("@blob", 0)),
        (TAGS["JPEGInterchangeFormatLength"], LONG, [len(whole)]),
    ], [whole]))

    # ---- ThunderScan ----
    #
    # The sample set's only ThunderScan file has a short last strip, where
    # libtiff hands back two rows of its previous strip's buffer rather than
    # an error (measured: they are byte-identical to rows 293 and 294). So it
    # cannot be the whole evidence for this compression, and this fixture is
    # the rest of it: a file that is not truncated, which libtiff and this
    # codec must agree about exactly.
    #
    # The encoder below is deliberately unoptimised - it just has to use all
    # four of the codes, because those are what the decoder chooses between.
    def thunderscan(rows_of_pixels, w):
        """Encode 4-bit rows, each a list of w values in 0..15."""
        out = bytearray()
        for row in rows_of_pixels:
            last = 0
            i = 0
            while i < w:
                run = 0
                while i + run < w and row[i + run] == last and run < 63:
                    run += 1
                if run >= 2:
                    out.append(run)      # 00nnnnnn: repeat the last pixel.
                    i += run
                    continue
                # 10aaabbb: two three-bit deltas. 1..3 are 1,2,3 and -1..-3
                # are 7,6,5; 4 is the filler that emits nothing.
                three = [4, 4]
                ok = True
                probe = last
                taken = 0
                for k in range(2):
                    if i + k >= w:
                        break
                    d = row[i + k] - probe
                    if d == 0:
                        three[k] = 0
                    elif 1 <= d <= 3:
                        three[k] = d
                    elif -3 <= d <= -1:
                        three[k] = 8 + d
                    else:
                        ok = k > 0
                        break
                    probe = row[i + k]
                    taken += 1
                if ok and taken > 0:
                    out.append(0x80 | (three[0] << 3) | three[1])
                    i += taken
                    last = row[i - 1]
                    continue
                out.append(0xC0 | row[i])  # 11xxxxxx: stored outright.
                last = row[i]
                i += 1
        return bytes(out)

    TW, TH = 37, 11
    # A picture with runs, small steps and jumps, so every code gets used.
    trows = [[(x * 3 + y) % 16 if (x // 5) % 2 else (y % 16)
              for x in range(TW)] for y in range(TH)]
    tdata = thunderscan(trows, TW)
    tpixels = bytearray()
    for row in trows:
        packed = bytearray((TW + 1) // 2)
        for x, v in enumerate(row):
            if x % 2 == 0:
                packed[x // 2] = v << 4
            else:
                packed[x // 2] |= v
        tpixels += packed

    def four_bit_fields(compression, blob):
        return [
            (TAGS["ImageWidth"], LONG, [TW]),
            (TAGS["ImageLength"], LONG, [TH]),
            (TAGS["BitsPerSample"], SHORT, [4]),
            (TAGS["Compression"], SHORT, [compression]),
            (TAGS["Photometric"], SHORT, [0]),
            (TAGS["SamplesPerPixel"], SHORT, [1]),
            (TAGS["RowsPerStrip"], LONG, [TH]),
            (TAGS["PlanarConfig"], SHORT, [1]),
            (TAGS["StripOffsets"], LONG, ("@blob", 0)),
            (TAGS["StripByteCounts"], LONG, [len(blob)]),
        ]

    write("tiff_thunderscan.tif",
          build_flat("II", four_bit_fields(32809, tdata), [tdata]))
    # The same picture stored plainly, so the fixture set can check the
    # compression without asking libtiff anything.
    write("tiff_thunderscan_plain.tif",
          build_flat("II", four_bit_fields(1, bytes(tpixels)),
                     [bytes(tpixels)]))

    # ---- Refusals ----
    write("tiff_bad_magic.tif", b"II\x2b\x00" + b"\x00" * 12)
    # A compression this codec does not undo. It was LZW here until LZW
    # landed, CCITT Group 3 until Group 3 landed, and JPEG until JPEG landed;
    # a refusal fixture has to name something still refused, or the test that
    # asserts the refusal starts asserting nothing. 34712 is JPEG 2000, which
    # is a different image format wearing a TIFF wrapper and is not on the
    # list - so this one should outlast the others.
    unsup = strip_fields(W, H, gray, 1)
    unsup = [(t, ty, v) if t != TAGS["Compression"] else (t, ty, [34712])
             for (t, ty, v) in unsup]
    write("tiff_unknown_compression.tif", build("II", [(unsup, gray)]))
    # CCITT Group 4 over bytes that are not Group 4 at all. The compression
    # is read now, so what this exercises is the other half: a block whose
    # coding this codec knows and whose contents decode to nothing.
    #
    # The bytes are zeros rather than anything more interesting, and that is
    # measured rather than chosen: 0xA5 filler was the first attempt and it
    # decoded cleanly, because an arbitrary bit pattern is usually a legal
    # sequence of mode codes. A run of zeros is not - it is a fragment of an
    # end-of-line, in the middle of a page that has none.
    blank = bytes(len(gray))
    undec = strip_fields(W, H, blank, 1)
    undec = [(t, ty, [4] if t == TAGS["Compression"] else v)
             for (t, ty, v) in undec]
    undec = [(t, ty, [1] if t == TAGS["BitsPerSample"] else v)
             for (t, ty, v) in undec]
    write("tiff_ccitt_undecodable.tif", build("II", [(undec, blank)]))
    no_photo = [f for f in strip_fields(W, H, gray, 1)
                if f[0] != TAGS["Photometric"]]
    write("tiff_no_photometric.tif", build("II", [(no_photo, gray)]))

    # CIE L*a*b*. Four pixels chosen so a* and b* are positive, negative and
    # at the signed extremes; an eight-bit encoding and the sixteen-bit
    # encoding of the same 1976 values must decode to the same RGBA, and so
    # must photometric 9, which stores a* and b* with a bias added.
    lab = [(255, 0, 0), (0, 0, 0), (128, 40, -20), (50, -128, 127)]

    def lab_bytes(icc, bits, endian):
        out = bytearray()
        for L, a, b in lab:
            if bits == 8:
                aa = (a + 128) & 255 if icc else a & 255
                bb = (b + 128) & 255 if icc else b & 255
                out += bytes((L, aa, bb))
            elif icc:
                out += struct.pack(endian + "HHH", L * 256,
                                   (a * 256 + 32768) & 0xFFFF,
                                   (b * 256 + 32768) & 0xFFFF)
            else:
                out += struct.pack(endian + "Hhh", L * 257, a * 256, b * 256)
        return bytes(out)

    def planes_of(chunky, spp, sample_bytes):
        n = len(chunky) // (spp * sample_bytes)
        out = bytearray()
        for c in range(spp):
            for i in range(n):
                at = (i * spp + c) * sample_bytes
                out += chunky[at:at + sample_bytes]
        return bytes(out)

    cie8 = lab_bytes(False, 8, "<")
    icc8 = lab_bytes(True, 8, "<")
    cie16 = lab_bytes(False, 16, "<")
    icc16 = lab_bytes(True, 16, "<")
    cie16be = lab_bytes(False, 16, ">")
    write("tiff_lab_cielab8.tif",
          build("II", [(strip_fields(2, 2, cie8, 8, spp=3), cie8)]))
    write("tiff_lab_icclab8.tif",
          build("II", [(strip_fields(2, 2, icc8, 9, spp=3), icc8)]))
    write("tiff_lab_cielab16.tif",
          build("II", [(strip_fields(2, 2, cie16, 8, spp=3, bps=16), cie16)]))
    write("tiff_lab_icclab16.tif",
          build("II", [(strip_fields(2, 2, icc16, 9, spp=3, bps=16), icc16)]))
    write("tiff_lab_cielab16_be.tif",
          build("MM", [(strip_fields(2, 2, cie16be, 8, spp=3, bps=16),
                        cie16be)]))
    cie8_planes = planes_of(cie8, 3, 1)
    cie16_planes = planes_of(cie16, 3, 2)
    write("tiff_lab_cielab8_planar.tif",
          build_planar("II", 2, 2, cie8_planes, 3, 8, 4))
    write("tiff_lab_cielab16_planar.tif",
          build_planar("II", 2, 2, cie16_planes, 3, 8, 8, bps=16))

    alphas = (255, 200, 10, 0)
    cie8_alpha = bytearray()
    for i, (L, a, b) in enumerate(lab):
        cie8_alpha += bytes((L, a & 255, b & 255, alphas[i]))
    cie8_alpha = bytes(cie8_alpha)
    write("tiff_lab_cielab8_alpha.tif",
          build("II", [(strip_fields(2, 2, cie8_alpha, 8, spp=4, extra=2),
                        cie8_alpha)]))
    # Associated: the same Lab, and one pixel whose alpha is not opaque, so
    # the divide-back-out has something to do.
    assoc_alpha = (255, 128, 255, 255)
    cie8_assoc = bytearray()
    for i, (L, a, b) in enumerate(lab):
        cie8_assoc += bytes((L, a & 255, b & 255, assoc_alpha[i]))
    cie8_assoc = bytes(cie8_assoc)
    write("tiff_lab_cielab8_assoc.tif",
          build("II", [(strip_fields(2, 2, cie8_assoc, 8, spp=4, extra=1),
                        cie8_assoc)]))

    white = strip_fields(2, 2, cie8, 8, spp=3)
    white.append((TAGS["WhitePoint"], RATIONAL, [(1, 3), (1, 3)]))
    write("tiff_lab_cielab8_white.tif", build("II", [(white, cie8)]))

    def compression(fields, comp):
        return [(t, ty, [comp] if t == TAGS["Compression"] else v)
                for (t, ty, v) in fields]

    jpeg = compression(strip_fields(1, 1, b"\x00\x00\x00", 8, spp=3), 7)
    write("tiff_lab_jpeg.tif", build("II", [(jpeg, b"\x00\x00\x00")]))
    jpeg_old = compression(strip_fields(1, 1, b"\x00\x00\x00", 8, spp=3), 6)
    write("tiff_lab_jpeg_old.tif",
          build("II", [(jpeg_old, b"\x00\x00\x00")]))
    write("tiff_lab_itu.tif",
          build("II", [(strip_fields(1, 1, b"\x00\x00\x00", 10, spp=3),
                        b"\x00\x00\x00")]))
    wide = bytes(12)
    write("tiff_lab_depth32.tif",
          build("II", [(strip_fields(1, 1, wide, 8, spp=3, bps=32), wide)]))
    write("tiff_lab_one_sample.tif",
          build("II", [(strip_fields(1, 1, b"\x80", 8, spp=1), b"\x80")]))


def split_jpeg(blob):
    """Take a baseline JPEG apart into the pieces the TIFF tags want.

    Both ways of putting JPEG in a TIFF store the same bytes cut differently:
    compression 7 keeps whole table *segments* in one tag and the frame in the
    strips, compression 6 keeps the table *contents* at six file offsets and
    the strips hold nothing but entropy-coded data. So one parse serves both.

    @return (dqt_segments, dht_segments, qtable_bodies, dc_bodies,
             ac_bodies, sof_segment, sos_segment, entropy)
    """
    assert blob[:2] == b"\xff\xd8", "not a JPEG"
    dqt, dht, qbody, dcbody, acbody = [], [], [], [], []
    sof = sos = None
    i = 2
    while i + 3 < len(blob):
        assert blob[i] == 0xFF, f"marker expected at {i}"
        m = blob[i + 1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        ln = struct.unpack(">H", blob[i + 2:i + 4])[0]
        seg = blob[i:i + 2 + ln]
        payload = blob[i + 4:i + 2 + ln]
        if m == 0xDB:
            dqt.append(seg)
            # Pq|Tq, then 64 bytes; more than one table may share a segment.
            at = 0
            while at < len(payload):
                assert payload[at] >> 4 == 0, "16-bit quantization"
                qbody.append(payload[at + 1:at + 65])
                at += 65
        elif m == 0xC4:
            dht.append(seg)
            at = 0
            while at < len(payload):
                counts = payload[at + 1:at + 17]
                n = sum(counts)
                body = payload[at + 1:at + 17 + n]
                (dcbody if payload[at] >> 4 == 0 else acbody).append(body)
                at += 17 + n
        elif m == 0xC0:
            sof = seg
        elif m == 0xDA:
            sos = seg
            i += 2 + ln
            break
        i += 2 + ln
    end = blob.rfind(b"\xff\xd9")
    entropy = blob[i:end if end > i else len(blob)]
    assert sof and sos and qbody and dcbody and acbody, "not baseline"
    return dqt, dht, qbody, dcbody, acbody, sof, sos, entropy


def build_flat(endian, fields, blobs):
    """A one-IFD file whose data blobs go at known offsets.

    build() places a strip for you and nothing else; these JPEG fixtures need
    several independent runs of bytes whose offsets appear in different tags,
    so they are laid out here instead. A field's values may be ("@blob", i),
    which becomes the offset of blobs[i] once the layout is known.
    """
    e = "<" if endian == "II" else ">"
    # Header, then the blobs, then the directory and its value pool.
    at = 8
    offsets = []
    data = bytearray()
    for b in blobs:
        offsets.append(at + len(data))
        data += b
        if len(data) % 2:
            data += b"\x00"  # Keep every offset even, as TIFF 6.0 asks.
    dir_at = 8 + len(data)

    resolved = []
    for (tag, ftype, values) in fields:
        if isinstance(values, tuple) and values and values[0] == "@blob":
            values = [offsets[i] for i in values[1:]]
        resolved.append((tag, ftype, values))
    resolved.sort(key=lambda f: f[0])

    pool_at = dir_at + 2 + (12 * len(resolved)) + 4
    entries = bytearray()
    pool = bytearray()
    for (tag, ftype, values) in resolved:
        raw = pack_values(e, ftype, values)
        entries += struct.pack(e + "HHI", tag, ftype, len(values))
        if len(raw) <= 4:
            entries += raw + bytes(4 - len(raw))
        else:
            entries += struct.pack(e + "I", pool_at + len(pool))
            pool += raw
            if len(pool) % 2:
                pool += b"\x00"
    header = endian.encode() + struct.pack(e + "H", 42) + \
        struct.pack(e + "I", dir_at)
    directory = struct.pack(e + "H", len(resolved)) + bytes(entries) + \
        struct.pack(e + "I", 0)
    return bytes(header + bytes(data) + directory + bytes(pool))


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


def build_planar(endian, w, h, data, spp, photometric, plane_len, bps=8):
    """A file whose channels are stored one plane after another.

    Every plane gets its own strip, which is what PlanarConfiguration 2 means
    in the simplest case: StripOffsets holds spp entries, plane after plane.
    """
    e = "<" if endian == "II" else ">"
    pool = bytearray()
    offsets, counts = [], []
    for c in range(spp):
        offsets.append(8 + len(pool))
        counts.append(plane_len)
        pool.extend(data[c * plane_len:(c + 1) * plane_len])
        if len(pool) % 2:
            pool.append(0)
    fields = [
        (TAGS["ImageWidth"], LONG, [w]),
        (TAGS["ImageLength"], LONG, [h]),
        (TAGS["BitsPerSample"], SHORT, [bps] * spp),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [photometric]),
        (TAGS["SamplesPerPixel"], SHORT, [spp]),
        (TAGS["RowsPerStrip"], LONG, [h]),
        (TAGS["PlanarConfig"], SHORT, [2]),
        (TAGS["StripOffsets"], LONG, offsets),
        (TAGS["StripByteCounts"], LONG, counts),
    ]
    return _finish(e, pool, [fields])


def build_subifd(endian, full, half):
    """A full-size page whose SubIFDs tag names one reduced-resolution one.

    Laid out by hand because the SubIFD is a directory the main chain does
    not contain: its offset lives in a tag, and the page that names it has to
    know where it landed.
    """
    e = "<" if endian == "II" else ">"
    pool = bytearray()
    full_at = 8 + len(pool)
    pool.extend(full)
    if len(pool) % 2:
        pool.append(0)
    half_at = 8 + len(pool)
    pool.extend(half)
    if len(pool) % 2:
        pool.append(0)

    sub_fields = [
        (TAGS["ImageWidth"], LONG, [4]),
        (TAGS["ImageLength"], LONG, [4]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [4]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["NewSubfileType"], LONG, [1]),
        (TAGS["StripOffsets"], LONG, [half_at]),
        (TAGS["StripByteCounts"], LONG, [len(half)]),
    ]
    main_fields = [
        (TAGS["ImageWidth"], LONG, [8]),
        (TAGS["ImageLength"], LONG, [8]),
        (TAGS["BitsPerSample"], SHORT, [8]),
        (TAGS["Compression"], SHORT, [1]),
        (TAGS["Photometric"], SHORT, [1]),
        (TAGS["SamplesPerPixel"], SHORT, [1]),
        (TAGS["RowsPerStrip"], LONG, [8]),
        (TAGS["PlanarConfig"], SHORT, [1]),
        (TAGS["StripOffsets"], LONG, [full_at]),
        (TAGS["StripByteCounts"], LONG, [len(full)]),
        (TAGS["SubIFDs"], LONG, [0]),  # Patched once the layout is known.
    ]
    main_size = 2 + 12 * len(main_fields) + 4
    main_at = 8 + len(pool)
    sub_at = main_at + main_size
    main_fields = [(t, ty, [sub_at] if t == TAGS["SubIFDs"] else v)
                   for (t, ty, v) in main_fields]

    def directory(fields, nxt):
        out = bytearray(struct.pack(e + "H", len(fields)))
        for tag, ftype, values in sorted(fields, key=lambda f: f[0]):
            raw = pack_values(e, ftype, values)
            out += struct.pack(e + "HHI", tag, ftype, len(values))
            assert len(raw) <= 4, "this layout keeps every value inline"
            out += raw + b"\x00" * (4 - len(raw))
        out += struct.pack(e + "I", nxt)
        return bytes(out)

    header = endian.encode() + struct.pack(e + "H", 42)
    return bytes(header + struct.pack(e + "I", main_at) + bytes(pool) +
                 directory(main_fields, 0) + directory(sub_fields, 0))


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
