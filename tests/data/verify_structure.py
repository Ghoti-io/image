#!/usr/bin/env python3
"""
Check the *structure* of files this library writes, against the format specs.

The four verify_<fmt>_output.py scripts beside this one read our output back
with an outside decoder and compare pixels. That catches pixel bugs and
nothing else, and the decoders in question are lenient about everything else.
Measured, by breaking one thing in a known-good file and asking:

    a JPEG whose RST2 is renumbered RST5      Pillow and ImageMagick: accept
    a GIF background index not in its table   ImageMagick: accepts
    a GIF LZW minimum code size of 9          ImageMagick: accepts
    a GIF with its trailer removed            ImageMagick: accepts

So a writer can emit a malformed file, every oracle can read it, the pixels
can be right, and every gate stays green. That is not hypothetical: the APNG
sequence numbers were wrong for every animation whose frame data needed more
than one fdAT chunk, Pillow and ImageMagick both read those files without
complaint, and the only reader strict enough to object was this library's own.

This script is the missing gate. It decodes nothing and compares nothing; it
reads the bytes and checks what the specifications require.

**It self-tests first.** A structural checker that quietly stops parsing
reports zero problems on everything, which is indistinguishable from success -
the same trap as an oracle that skips instead of failing. So before it
certifies anything it builds known-bad inputs of its own and requires that it
catches every one. If the self-test fails, nothing else is reported and the
exit status is non-zero.

Usage:
  python3 tests/data/verify_structure.py [DIR|FILE]...
  With no argument, checks tests/out/{png,jpeg,bmp,gif}.
Exit: 0 when the self-test passed and no file has a structural problem.
"""
import os
import struct
import sys
import zlib

# --------------------------------------------------------------------------
# PNG and APNG (W3C PNG 2nd edition; APNG as published by Mozilla)
# --------------------------------------------------------------------------

PNG_SIG = b'\x89PNG\r\n\x1a\n'
# 5.6 Table 5.3: what must precede IDAT, and what must sit either side of PLTE.
PNG_BEFORE_IDAT = {b'cHRM', b'gAMA', b'iCCP', b'sBIT', b'sRGB', b'PLTE',
                   b'bKGD', b'hIST', b'tRNS', b'pHYs', b'sPLT', b'eXIf',
                   b'cICP', b'mDCv', b'cLLi', b'acTL'}
PNG_AFTER_PLTE = {b'bKGD', b'hIST', b'tRNS'}
PNG_BEFORE_PLTE = {b'cHRM', b'gAMA', b'iCCP', b'sBIT', b'sRGB', b'cICP',
                   b'mDCv', b'cLLi'}


def check_png(d):
    p = []
    i = 8
    chunks = []
    while i + 8 <= len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        t = d[i + 4:i + 8]
        if n > 0x7FFFFFFF:
            p.append("chunk %s declares length %d, above 2^31-1 (5.3)"
                     % (t.decode('latin1', 'replace'), n))
            break
        if i + 12 + n > len(d):
            p.append("chunk %s at %d runs past the end of the file"
                     % (t.decode('latin1', 'replace'), i))
            break
        payload = d[i + 8:i + 8 + n]
        want = struct.unpack('>I', d[i + 8 + n:i + 12 + n])[0]
        if want != (zlib.crc32(t + payload) & 0xFFFFFFFF):
            p.append("bad CRC on %s at %d (5.5)"
                     % (t.decode('latin1', 'replace'), i))
        chunks.append((t, payload, i))
        i += 12 + n
        if t == b'IEND':
            break
    if not chunks:
        return ["no chunks"]
    types = [c[0] for c in chunks]
    if types[0] != b'IHDR':
        p.append("first chunk is %s, not IHDR (5.6)"
                 % types[0].decode('latin1', 'replace'))
    if types[-1] != b'IEND':
        p.append("last chunk is %s, not IEND (5.6)"
                 % types[-1].decode('latin1', 'replace'))
    for req in (b'IHDR', b'IEND'):
        if types.count(req) != 1:
            p.append("%s appears %d times, must appear once (5.6)"
                     % (req.decode('latin1'), types.count(req)))
    if b'IDAT' not in types:
        return p + ["no IDAT"]
    if len(chunks[0][1]) != 13:
        return p + ["IHDR is %d bytes, not 13 (11.2.1)" % len(chunks[0][1])]
    w, h, depth, ct, comp, filt, ilace = struct.unpack('>IIBBBBB', chunks[0][1])
    idat = [k for k, t in enumerate(types) if t == b'IDAT']
    if idat != list(range(idat[0], idat[0] + len(idat))):
        p.append("IDAT chunks are not consecutive (5.6)")
    first_idat = idat[0]
    if ct == 3 and b'PLTE' not in types:
        p.append("color type 3 with no PLTE (11.2.2)")
    if ct in (0, 4) and b'PLTE' in types:
        p.append("PLTE present for grayscale color type %d (11.2.2)" % ct)
    plte_at = types.index(b'PLTE') if b'PLTE' in types else None
    for k, t in enumerate(types):
        if t in PNG_BEFORE_IDAT and k > first_idat:
            p.append("%s appears after IDAT (5.6 Table 5.3)"
                     % t.decode('latin1'))
        if plte_at is not None:
            if t in PNG_AFTER_PLTE and k < plte_at:
                p.append("%s appears before PLTE (5.6 Table 5.3)"
                         % t.decode('latin1'))
            if t in PNG_BEFORE_PLTE and k > plte_at:
                p.append("%s appears after PLTE (5.6 Table 5.3)"
                         % t.decode('latin1'))
    try:
        zlib.decompress(b''.join(c[1] for c in chunks if c[0] == b'IDAT'))
    except Exception as e:
        p.append("IDAT is not a valid zlib stream: %s" % e)
    # APNG. The sequence numbers are one counter shared by fcTL and fdAT,
    # starting at 0 and incrementing by one per chunk; IDAT carries none.
    if b'acTL' in types:
        actl = [c[1] for c in chunks if c[0] == b'acTL'][0]
        if len(actl) != 8:
            p.append("acTL is %d bytes, not 8" % len(actl))
        else:
            nframes = struct.unpack('>I', actl[:4])[0]
            if types.index(b'acTL') > first_idat:
                p.append("acTL appears after IDAT")
            nfctl = types.count(b'fcTL')
            if nfctl != nframes:
                p.append("acTL says %d frames but there are %d fcTL chunks"
                         % (nframes, nfctl))
        seq = [(struct.unpack('>I', pay[:4])[0], t, off)
               for t, pay, off in chunks
               if t in (b'fcTL', b'fdAT') and len(pay) >= 4]
        for k, (s, t, off) in enumerate(seq):
            if s != k:
                p.append("%s at %d has sequence number %d, expected %d"
                         % (t.decode('latin1'), off, s, k))
                break
        seen_fctl = False
        for t, _, _ in chunks:
            if t == b'fcTL':
                seen_fctl = True
            elif t == b'fdAT' and not seen_fctl:
                p.append("fdAT appears before any fcTL")
                break
        for k, (t, pay, _) in enumerate(chunks):
            if t == b'fcTL' and k < first_idat:
                if len(pay) >= 20:
                    _, fw, fh, fx, fy = struct.unpack('>IIIII', pay[:20])
                    if (fw, fh, fx, fy) != (w, h, 0, 0):
                        p.append(
                            "the fcTL before IDAT is %dx%d+%d+%d, but a frame "
                            "sharing the default image must cover the whole "
                            "%dx%d canvas" % (fw, fh, fx, fy, w, h))
                break
    elif b'fcTL' in types or b'fdAT' in types:
        p.append("fcTL/fdAT present without acTL")
    return p


# --------------------------------------------------------------------------
# GIF 89a
# --------------------------------------------------------------------------

def check_gif(d):
    p = []
    if len(d) < 13:
        return ["shorter than a screen descriptor"]
    w = d[6] | (d[7] << 8)
    h = d[8] | (d[9] << 8)
    packed, bg = d[10], d[11]
    i = 13
    gct = 0
    if packed & 0x80:
        gct = 2 << (packed & 7)
        i += gct * 3
        if i > len(d):
            return ["global colour table runs past the end of the file"]
        if bg >= gct:
            p.append("background index %d but the global table has %d entries "
                     "(89a 18)" % (bg, gct))
    elif bg != 0:
        p.append("background index %d with no global colour table; 89a 18 "
                 "says the field is then zero" % bg)

    def subblocks(j):
        while j < len(d):
            n = d[j]
            if n == 0:
                return j + 1, True
            j += 1 + n
        return j, False

    images = 0
    trailer = False
    while i < len(d):
        b = d[i]
        if b == 0x3B:
            trailer = True
            i += 1
            break
        if b == 0x21:
            if i + 2 > len(d):
                p.append("extension introducer with no label")
                break
            label = d[i + 1]
            i, ok = subblocks(i + 2)
            if not ok:
                p.append("extension 0x%02X has an unterminated sub-block chain "
                         "(89a 15)" % label)
                break
        elif b == 0x2C:
            if i + 10 > len(d):
                p.append("image descriptor truncated")
                break
            x = d[i + 1] | (d[i + 2] << 8)
            y = d[i + 3] | (d[i + 4] << 8)
            iw = d[i + 5] | (d[i + 6] << 8)
            ih = d[i + 7] | (d[i + 8] << 8)
            ip = d[i + 9]
            if x + iw > w or y + ih > h:
                p.append(
                    "image %d is %dx%d at %d,%d, outside the %dx%d logical "
                    "screen (89a 20)" % (images, iw, ih, x, y, w, h))
            i += 10
            lct = 0
            if ip & 0x80:
                lct = 2 << (ip & 7)
                i += lct * 3
            if i >= len(d):
                p.append("image %d has no LZW minimum code size" % images)
                break
            mcs = d[i]
            i += 1
            if mcs < 2 or mcs > 8:
                p.append("image %d has LZW minimum code size %d, outside 2..8 "
                         "(89a 22)" % (images, mcs))
            table = lct or gct
            if table == 0:
                p.append("image %d has neither a local nor a global colour "
                         "table (89a 20)" % images)
            elif (1 << mcs) < table:
                p.append("image %d has code size %d, too narrow for %d table "
                         "entries (89a 22)" % (images, mcs, table))
            i, ok = subblocks(i)
            if not ok:
                p.append("image %d has an unterminated LZW sub-block chain "
                         "(89a 15)" % images)
                break
            images += 1
        else:
            p.append("unknown block introducer 0x%02X at %d" % (b, i))
            break
    if not trailer:
        p.append("no trailer (89a 27)")
    if images == 0:
        p.append("no image blocks")
    return p


# --------------------------------------------------------------------------
# JPEG (ITU-T T.81)
# --------------------------------------------------------------------------

def check_jpeg(d):
    p = []
    i = 2
    scan = 0
    expect = 0
    while i + 1 < len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m == 0x00:
            i += 2
            continue
        if 0xD0 <= m <= 0xD7:
            if (m - 0xD0) != expect:
                p.append("scan %d: RST%d at %d where RST%d was due; T.81 4.10 "
                         "cycles them 0..7 in order"
                         % (scan, m - 0xD0, i, expect))
                return p
            expect = (expect + 1) & 7
            i += 2
            continue
        if m in (0xD8, 0x01):
            i += 2
            continue
        if m == 0xD9:
            break
        if i + 3 >= len(d):
            p.append("marker 0x%02X at %d has no length" % (m, i))
            break
        ln = (d[i + 2] << 8) | d[i + 3]
        if ln < 2 or i + 2 + ln > len(d):
            p.append("marker 0x%02X at %d declares length %d" % (m, i, ln))
            break
        if m == 0xDA:
            # A new scan restarts the counter (T.81 4.10).
            scan += 1
            expect = 0
        i += 2 + ln
    if scan == 0:
        p.append("no SOS: the file carries no scan")
    if d[-2:] != b'\xff\xd9':
        p.append("does not end with EOI (T.81 B.2.1)")
    return p


# --------------------------------------------------------------------------
# BMP: the header has to agree with the file around it.
# --------------------------------------------------------------------------

def check_bmp(d):
    p = []
    if len(d) < 54:
        return ["shorter than a BITMAPINFOHEADER file"]
    fsize = struct.unpack('<I', d[2:6])[0]
    off = struct.unpack('<I', d[10:14])[0]
    if fsize != len(d):
        p.append("bfSize is %d but the file is %d bytes" % (fsize, len(d)))
    if off > len(d):
        p.append("bfOffBits is %d, past the end of a %d-byte file"
                 % (off, len(d)))
    hsz = struct.unpack('<I', d[14:18])[0]
    if hsz < 12:
        return p + ["DIB header size %d is below the smallest defined" % hsz]
    if hsz >= 40:
        w, h = struct.unpack('<ii', d[18:26])
        planes, bpp = struct.unpack('<HH', d[26:30])
        comp, szimg = struct.unpack('<II', d[30:38])
        clrused = struct.unpack('<I', d[46:50])[0]
        if planes != 1:
            p.append("biPlanes is %d; the format defines only 1" % planes)
        if bpp not in (1, 2, 4, 8, 16, 24, 32):
            p.append("biBitCount %d is not a defined depth" % bpp)
        if bpp and bpp <= 8 and clrused > (1 << bpp):
            p.append("biClrUsed %d exceeds the %d colours %d bits can index"
                     % (clrused, 1 << bpp, bpp))
        if comp == 0 and bpp:
            row = ((w * bpp + 31) // 32) * 4
            want = row * abs(h)
            if szimg not in (0, want):
                p.append("biSizeImage is %d but %d rows of %d bytes need %d"
                         % (szimg, abs(h), row, want))
            if off + want > len(d):
                p.append("pixel data needs %d bytes from offset %d, past the "
                         "end of a %d-byte file" % (want, off, len(d)))
    return p


# --------------------------------------------------------------------------

def check(data):
    """Dispatch on the signature. Returns (format name, problems)."""
    if data[:8] == PNG_SIG:
        return "png", check_png(data)
    if data[:6] in (b'GIF87a', b'GIF89a'):
        return "gif", check_gif(data)
    if data[:2] == b'\xff\xd8':
        return "jpeg", check_jpeg(data)
    if data[:2] == b'BM':
        return "bmp", check_bmp(data)
    return None, ["unrecognized signature"]


# --------------------------------------------------------------------------
# Self-test
#
# Every checker above is asked to catch a file broken on purpose, before any
# real file is judged. A parser that silently stops finding chunks reports no
# problems on everything it is shown, which looks exactly like success - so
# the gate has to prove it still bites. Each case names the rule it breaks and
# a substring the report must contain, so a checker that fails for some
# unrelated reason does not count as having caught it.
# --------------------------------------------------------------------------

def _png_chunk(t, payload):
    return (struct.pack('>I', len(payload)) + t + payload
            + struct.pack('>I', zlib.crc32(t + payload) & 0xFFFFFFFF))


def _good_png(apng=False):
    w = h = 4
    raw = b''.join(b'\x00' + bytes([(x * 7 + y * 3) & 0xFF] * 4 * w)
                   for y in range(h) for x in [0])
    raw = b''
    for y in range(h):
        raw += b'\x00' + bytes([(y * 31) & 0xFF]) * (w * 4)
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    out = PNG_SIG + _png_chunk(b'IHDR', ihdr)
    if apng:
        out += _png_chunk(b'acTL', struct.pack('>II', 2, 0))
        out += _png_chunk(b'fcTL', struct.pack('>IIIIIHHBB', 0, w, h, 0, 0,
                                               5, 100, 0, 0))
    out += _png_chunk(b'IDAT', zlib.compress(raw))
    if apng:
        out += _png_chunk(b'fcTL', struct.pack('>IIIIIHHBB', 1, w, h, 0, 0,
                                               5, 100, 0, 0))
        out += _png_chunk(b'fdAT', struct.pack('>I', 2) + zlib.compress(raw))
    out += _png_chunk(b'IEND', b'')
    return out


def _good_gif():
    # 4x4, a 4-entry global table, one image, background index 0.
    out = b'GIF89a' + struct.pack('<HH', 4, 4)
    out += bytes([0x80 | 0x01, 0x00, 0x00])
    out += bytes([0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255])
    out += b'\x21\xF9\x04\x00\x0A\x00\x00\x00'
    out += b'\x2C' + struct.pack('<HHHH', 0, 0, 4, 4) + b'\x00'
    out += bytes([0x02, 0x03, 0x8C, 0x2D, 0x99, 0x00])
    out += b'\x3B'
    return out


def _good_jpeg():
    # Marker structure only: this checker never decodes entropy data.
    out = b'\xff\xd8'
    out += b'\xff\xdb' + struct.pack('>H', 67) + b'\x00' + bytes(64)
    out += b'\xff\xc0' + struct.pack('>H', 11)
    out += bytes([8, 0, 8, 0, 8, 1, 1, 0x11, 0])
    out += b'\xff\xda' + struct.pack('>H', 8) + bytes([1, 1, 0, 0, 63, 0])
    out += b'\x12\x34' + b'\xff\xd0' + b'\x56\x78' + b'\xff\xd1' + b'\x9a'
    out += b'\xff\xd9'
    return out


def _good_bmp():
    w = h = 4
    row = ((w * 24 + 31) // 32) * 4
    pixels = bytes(row * h)
    off = 14 + 40
    out = b'BM' + struct.pack('<IHHI', off + len(pixels), 0, 0, off)
    out += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(pixels),
                       2835, 2835, 0, 0)
    return out + pixels


def _patch(data, index, value):
    b = bytearray(data)
    b[index] = value
    return bytes(b)


def _patch_png_chunk(data, chunk_index, byte_in_payload, value):
    """Change one payload byte of a PNG chunk and repair its CRC.

    The CRC has to be rebuilt or the case tests two rules at once: the real
    APNG bug produced chunks whose CRCs were perfectly valid and whose
    sequence numbers were wrong, and a case that also breaks the CRC would be
    caught by the CRC rule even with the sequence rule switched off.
    """
    b = bytearray(data)
    n = struct.unpack('>I', b[chunk_index - 4:chunk_index])[0]
    b[chunk_index + 4 + byte_in_payload] = value
    body = bytes(b[chunk_index:chunk_index + 4 + n])
    b[chunk_index + 4 + n:chunk_index + 8 + n] = struct.pack(
        '>I', zlib.crc32(body) & 0xFFFFFFFF)
    return bytes(b)


def _self_test():
    """Return a list of cases the checkers failed to catch."""
    missed = []
    apng = _good_apng = _good_png(apng=True)
    cases = []

    # PNG: a wrong CRC on IDAT.
    i = apng.index(b'IDAT')
    n = struct.unpack('>I', apng[i - 4:i])[0]
    cases.append(("PNG bad CRC",
                  _patch(apng, i + 4 + n, apng[i + 4 + n] ^ 0xFF), "bad CRC"))
    # PNG: APNG sequence numbers out of order - the bug this gate exists for.
    # The CRC is repaired so this breaks that rule and no other.
    j = apng.rindex(b'fcTL')
    cases.append(("APNG sequence out of order",
                  _patch_png_chunk(apng, j, 3, 7), "sequence number"))
    # PNG: acTL disagreeing with the number of fcTL chunks.
    k = apng.index(b'acTL')
    cases.append(("APNG acTL frame count",
                  _patch_png_chunk(apng, k, 3, 9), "acTL says"))
    # PNG: no IEND.
    cases.append(("PNG missing IEND", apng[:apng.rindex(b'IEND') - 4],
                  "IEND"))
    # GIF: background index outside the global colour table.
    gif = _good_gif()
    cases.append(("GIF background index", _patch(gif, 11, 200),
                  "background index"))
    # GIF: an LZW minimum code size the format does not define.
    m = gif.index(b'\x2C') + 10
    cases.append(("GIF LZW code size", _patch(gif, m, 9), "code size"))
    # GIF: no trailer.
    cases.append(("GIF missing trailer", gif[:-1], "trailer"))
    # JPEG: a restart marker out of turn.
    jpg = _good_jpeg()
    r = jpg.index(b'\xff\xd1')
    cases.append(("JPEG restart out of order", _patch(jpg, r + 1, 0xD5),
                  "RST"))
    # JPEG: no EOI.
    cases.append(("JPEG missing EOI", jpg[:-2], "EOI"))
    # BMP: bfSize disagreeing with the file.
    bmp = _good_bmp()
    cases.append(("BMP bfSize", _patch(bmp, 2, (bmp[2] + 7) & 0xFF), "bfSize"))

    # Every good file must come back clean, or the checker is crying wolf.
    for name, data in (("PNG", _good_png()), ("APNG", apng),
                       ("GIF", _good_gif()), ("JPEG", _good_jpeg()),
                       ("BMP", _good_bmp())):
        fmt, problems = check(data)
        if problems:
            missed.append("%s: a well-formed file was reported as broken: %s"
                          % (name, problems[0]))

    for name, data, want in cases:
        fmt, problems = check(data)
        if not any(want in x for x in problems):
            missed.append("%s: not caught (wanted %r, got %r)"
                          % (name, want, problems[:2]))
    return missed


def main(argv):
    missed = _self_test()
    if missed:
        sys.stderr.write("structure check SELF-TEST FAILED; nothing was "
                         "verified:\n")
        for m in missed:
            sys.stderr.write("    %s\n" % m)
        return 2

    targets = argv[1:]
    if not targets:
        here = os.path.dirname(os.path.abspath(__file__))
        root = os.path.dirname(os.path.dirname(here))
        targets = [os.path.join(root, "tests", "out", f)
                   for f in ("png", "jpeg", "bmp", "gif")]
    files = []
    for t in targets:
        if os.path.isdir(t):
            for name in sorted(os.listdir(t)):
                path = os.path.join(t, name)
                if os.path.isfile(path) and not name.endswith(".rgba"):
                    files.append(path)
        elif os.path.isfile(t):
            files.append(t)

    counts = {}
    bad = 0
    for path in files:
        with open(path, "rb") as fh:
            data = fh.read()
        if len(data) < 16:
            continue
        fmt, problems = check(data)
        if fmt is None:
            continue
        counts[fmt] = counts.get(fmt, 0) + 1
        if problems:
            bad += 1
            print("  %s" % os.path.relpath(path))
            for x in problems[:6]:
                print("      %s" % x)
    if not counts:
        sys.stderr.write("structure check: no files found in %s\n"
                         % ", ".join(targets))
        return 1
    print("  %s structurally checked (%s), problems in %d"
          % (sum(counts.values()),
             ", ".join("%d %s" % (v, k) for k, v in sorted(counts.items())),
             bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
