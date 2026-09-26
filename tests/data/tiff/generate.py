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
    "ColorMap": 320,
    "TileWidth": 322,
    "TileLength": 323,
    "TileOffsets": 324,
    "TileByteCounts": 325,
    "NewSubfileType": 254,
    "SubIFDs": 330,
    "YCbCrSubSampling": 530,
    "ImageDescription": 270,
    "Orientation": 274,
    "Predictor": 317,
    "XMP": 700,
    "ICCProfile": 34675,
    "ExtraSamples": 338,
    "SampleFormat": 339,
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
    meta.append((TAGS["Orientation"], SHORT, [6]))
    meta.append((TAGS["XMP"], BYTE, list(xmp)))
    meta.append((TAGS["ICCProfile"], UNDEFINED, list(profile)))
    write("tiff_4x4_metadata.tif", build("II", [(meta, rgb)]))

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

    # ---- Refusals ----
    write("tiff_bad_magic.tif", b"II\x2b\x00" + b"\x00" * 12)
    # CCITT Group 3, which this codec does not undo. It was LZW here until
    # LZW landed; a refusal fixture has to name something still refused, or
    # the test that asserts the refusal starts asserting nothing.
    ccitt = strip_fields(W, H, gray, 1)
    ccitt = [(t, ty, v) if t != TAGS["Compression"] else (t, ty, [3])
             for (t, ty, v) in ccitt]
    write("tiff_ccitt_unsupported.tif", build("II", [(ccitt, gray)]))
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


def build_planar(endian, w, h, data, spp, photometric, plane_len):
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
        (TAGS["BitsPerSample"], SHORT, [8] * spp),
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
