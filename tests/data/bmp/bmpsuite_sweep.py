#!/usr/bin/env python3
"""Run Jason Summers' bmpsuite through this library's BMP decoder.

The suite is not vendored.  It is a published conformance corpus with its own
licence, and the PNG codec treats PngSuite the same way: fetched once into a
scratch directory and pointed at from here.

    curl -sLO https://entropymine.com/jason/bmpsuite/releases/bmpsuite-2.8.zip
    unzip -q bmpsuite-2.8.zip -d /tmp/bmpsuite
    make bmpsuite BMPSUITE=/tmp/bmpsuite/bmpsuite-2.8

What this checks, and against what
----------------------------------

Three external decoders may be installed here - Pillow, GdkPixbuf and netpbm's
`bmptopnm` - and no two of them agree.  Each is used where its reach extends
and declared absent where it does not, rather than one of them being treated
as the answer:

- **Pillow decodes RLE4 wrongly.**  On `g/pal4rle.bmp` it disagrees with
  GdkPixbuf, with netpbm, and with a decoder written from the specification,
  all three of which agree with this library.
- **The three scale sub-byte channels three different ways.**  For a 5-bit
  sample Pillow and netpbm truncate, GdkPixbuf replicates bits, and this
  library rounds - `round(v * 255 / 31)`, which is the rule PNG 13.12 states
  and the only one of the three that is the correctly rounded value.  All four
  agree at 0 and at the maximum, and differ by at most 1 in between, which is
  why colour is compared to a tolerance of 1 and not exactly.
- **Most of them discard alpha.**  Pillow opens a BMP with a declared alpha
  mask as RGB, so its alpha is not evidence of anything.

Because of all that, the decoders written from the specification at the bottom
of this file - for RLE4, RLE8, RLE24 and the BI_BITFIELDS channel layouts -
are the primary oracle, and the installed ones are corroboration.  Those three
files verified cases no installed decoder could: `q/rgb16-231.bmp` and
`q/rgb16-3103.bmp` have channel layouts none of the three read correctly, and
`q/rgb24rle24.bmp` is refused by all of them.

Exit status is 0 when every file behaved as this script says it should: the
ones REJECT names were refused, every other one decoded, and each decode
agreed with whatever could check it.  A file nothing can check is a failure
too - an unverified pass is not a pass.
"""

import argparse
import os
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# What each file should do
# ---------------------------------------------------------------------------
#
# "decode" - must load and decode.  "reject" - must be refused, at load or at
# decode; a decoder that accepts one of these is the thing being caught.
#
# Anything not named here must decode, which is the right default for g/ and
# for most of q/.  A file listed as "reject" carries the reason, because a
# refusal is a decision and the reason is the part worth arguing with.

REJECT = {
    # The format does not define these at all.
    "badbitcount.bmp": "an absurd biBitCount",
    "badheadersize.bmp": "a DIB header size no version uses",
    "badwidth.bmp": "a negative biWidth",
    "reallybig.bmp": "dimensions past the caller's max_decoded_pixels",
    "shortfile.bmp": "truncated inside the pixel data",
    "pal8badindex.bmp": "palette indices with no entry behind them",
    "rgb16-880.bmp": "a blue mask of zero: no way to say what blue is",
    "rletopdown.bmp": "RLE with top-down rows, which the format forbids",
    # Recognized, deliberately not implemented.  Each is listed in
    # documentation/formats/bmp.md under "Not implemented".
    "pal1huffmsb.bmp": "OS/2 Huffman 1D compression",
    "ba-bm.bmp": "the OS/2 'BA' bitmap array container",
}

# Files where an installed decoder is known to be wrong, with what is wrong
# with it.  The from-specification reference still checks these.
ORACLE_IS_WRONG = {
    "pil": {
        "pal4rle.bmp": "Pillow decodes RLE4 incorrectly",
        "pal4rlecut.bmp": "Pillow decodes RLE4 incorrectly",
        "pal4rletrns.bmp": "Pillow decodes RLE4 incorrectly",
        "badrle4.bmp": "Pillow decodes RLE4 incorrectly",
        "badrle4bis.bmp": "Pillow decodes RLE4 incorrectly",
        "badrle4ter.bmp": "Pillow decodes RLE4 incorrectly",
        "pal8rlecut.bmp": "Pillow stops emitting after a delta",
        "pal8rletrns.bmp": "Pillow stops emitting after a delta",
        "badrle.bmp": "Pillow stops emitting after a delta",
        "badrlebis.bmp": "Pillow stops emitting after a delta",
        "badrleter.bmp": "Pillow stops emitting after a delta",
    },
    "pixbuf": {
        "pal8offs.bmp": "GdkPixbuf ignores bfOffBits",
        "pal8os2sp.bmp": "GdkPixbuf ignores bfOffBits",
        "rgb16-565pal.bmp": "GdkPixbuf misreads a 16-bit file that has a palette",
        "rgb16-231.bmp": "GdkPixbuf misreads this channel layout",
        "rgb16-3103.bmp": "GdkPixbuf misreads this channel layout",
        "rgba16-1924.bmp": "GdkPixbuf misreads this channel layout",
        "rgba32-1010102.bmp": "GdkPixbuf misreads this channel layout",
    },
    # The b/badrle* files are bmpsuite's deliberate buffer-overrun attempts,
    # listed there with no expected rendering at all.  bmplib carries on
    # decoding a stream this library stops trusting, so the two disagree about
    # everything past the first malformation.  Neither is wrong: the question
    # the file asks is whether a decoder stays inside its buffer, and both do.
    "bmplib": {
        "badrle.bmp": "no correct rendering; bmplib decodes past the malformation",
        "badrlebis.bmp": "no correct rendering; bmplib decodes past the malformation",
        "badrleter.bmp": "no correct rendering; bmplib decodes past the malformation",
        "badrle4.bmp": "no correct rendering; bmplib decodes past the malformation",
        "badrle4bis.bmp": "no correct rendering; bmplib decodes past the malformation",
        "badrle4ter.bmp": "no correct rendering; bmplib decodes past the malformation",
    },
    # netpbm's bmptopnm gets the default layouts right and the declared masks
    # wrong, so it is trusted only where no BITFIELDS segment is involved.
    "netpbm": {
        "rgb16-565.bmp": "netpbm misreads a declared 5-6-5 mask",
        "rgb16-565pal.bmp": "netpbm misreads a declared 5-6-5 mask",
        "rgb32bf.bmp": "netpbm misreads declared masks",
        "rgb16-231.bmp": "netpbm misreads declared masks",
        "rgb16-3103.bmp": "netpbm misreads declared masks",
        "rgb32-111110.bmp": "netpbm misreads declared masks",
        "rgb32-7187.bmp": "netpbm misreads declared masks",
        "rgb32-xbgr.bmp": "netpbm misreads declared masks",
        "rgb32h52.bmp": "netpbm misreads a 52-byte header",
        "rgba32h56.bmp": "netpbm misreads a 56-byte header",
        "rgba32-1010102.bmp": "netpbm misreads declared masks",
        "rgba32-2.bmp": "netpbm misreads declared masks",
        "rgba32-61754.bmp": "netpbm misreads declared masks",
        "rgba32-81284.bmp": "netpbm misreads declared masks",
    },
}

# Files that must decode to exactly the picture another file in the suite
# decodes to.  bmpsuite renders one photograph many different ways, so "the
# same image, stored differently" is a property of the corpus and needs no
# decoder to confirm - which is what makes it the right check for the files
# nothing installed here will read.
EQUIVALENT = {
    # The same 8-bit image under a 16-byte and a 64-byte OS/2 2.x header.
    "pal8os2v2-16.bmp": "pal8os2v2.bmp",
    # A palette count larger than the indices can address.  The surplus
    # entries are unreachable, not wrong, so the picture is unchanged.
    "pal8oversizepal.bmp": "pal8.bmp",
    "badpalettesize.bmp": "pal8.bmp",
}

# BI_JPEG and BI_PNG wrap a whole stream of another format.  The wrapper's
# only job is to find the payload and hand it over intact, so the check is
# that the wrapped decode equals an ordinary decode of the payload extracted
# from the same file - which isolates the wrapper from the JPEG and PNG
# codecs, whose own suites cover them.
EMBEDDED = {"rgb24jpeg.bmp", "rgb24png.bmp"}

# Colour is compared to +/-1 because the rounding rule is a decoder's choice
# and three decoders make three different ones.  Alpha is compared only where
# the oracle honours it at all, which is recorded per oracle below.
COLOUR_TOLERANCE = 1

# An oracle that opens a BMP as RGB says nothing about its alpha.  Rather than
# guess per file, alpha is only ever compared against the reference decoders
# written here, which do honour a declared alpha mask.
ORACLES_HONOUR_ALPHA = False


# ---------------------------------------------------------------------------
# Installed decoders
# ---------------------------------------------------------------------------

def oracle_pil(path):
    from PIL import Image
    with Image.open(path) as im:
        im.load()
        return im.convert("RGBA").tobytes()


def oracle_pixbuf(path):
    import gi
    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf
    pb = GdkPixbuf.Pixbuf.new_from_file(path)
    w, h, rowstride = pb.get_width(), pb.get_height(), pb.get_rowstride()
    channels, data = pb.get_n_channels(), pb.get_pixels()
    out = bytearray()
    for y in range(h):
        row = data[y * rowstride:y * rowstride + w * channels]
        if channels == 4:
            out += row
        else:
            for x in range(w):
                out += row[x * 3:x * 3 + 3] + b"\xff"
    return bytes(out)


def oracle_netpbm(path):
    proc = subprocess.run(["bmptopnm", path], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    return pnm_to_rgba(proc.stdout)


def pnm_to_rgba(data):
    """Convert a P1/P2/P3/P4/P5/P6 image to packed RGBA8."""
    fields, i = [], 0
    want = 3 if data[:2] in (b"P1", b"P4") else 4
    while len(fields) < want:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    magic, w, h = fields[0], int(fields[1]), int(fields[2])
    maxval = int(fields[3]) if want == 4 else 1
    body = data[i:]
    out = bytearray()
    if magic == b"P6":
        for k in range(w * h):
            out += body[k * 3:k * 3 + 3] + b"\xff"
    elif magic == b"P5":
        for k in range(w * h):
            out += body[k:k + 1] * 3 + b"\xff"
    elif magic == b"P4":
        # Packed bits, one row at a time; 1 is black in PBM.
        stride = (w + 7) // 8
        for y in range(h):
            for x in range(w):
                bit = (body[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1
                v = 0 if bit else 255
                out += bytes((v, v, v, 255))
    else:
        raise RuntimeError("unhandled PNM type " + magic.decode())
    if maxval not in (1, 255) and magic in (b"P5", b"P6"):
        raise RuntimeError("unhandled PNM maxval %d" % maxval)
    return bytes(out)


# bmplib is not installed by any package here; it is built from source into
# third_party/bmplib by `make bmp-oracle-tools`, because it is the only decoder
# reachable from here that reads OS/2 Huffman 1D, OS/2 bitmap arrays and 64-bit
# BMPs.  When the tool has not been built, this oracle is simply absent, which
# is the same treatment the other three get when they are not installed.
BMPLIB_TOOL = os.environ.get(
    "BMP_ORACLE_BMPLIB",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "tools", "bmp-oracle", "build",
                 "dump_bmp_pixels_bmplib"))


def oracle_bmplib(path):
    if not os.path.isfile(BMPLIB_TOOL):
        raise FileNotFoundError(BMPLIB_TOOL)
    proc = subprocess.run([BMPLIB_TOOL, path], capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode(errors="replace").strip())
    data = proc.stdout
    if len(data) < 12 or data[:4] != b"BMPO":
        raise RuntimeError("tool wrote no raster")
    w = int.from_bytes(data[4:8], "little")
    h = int.from_bytes(data[8:12], "little")
    body = data[12:]
    if len(body) != w * h * 4:
        raise RuntimeError("tool wrote %d bytes for %dx%d" % (len(body), w, h))
    return bytes(body)


ORACLES = {"pil": oracle_pil, "pixbuf": oracle_pixbuf,
           "netpbm": oracle_netpbm, "bmplib": oracle_bmplib}


# ---------------------------------------------------------------------------
# Decoders written from the specification
# ---------------------------------------------------------------------------
#
# These are the primary oracle.  They are deliberately naive - no bounds
# cleverness, no shared code with the C decoder, no attempt at speed - so that
# agreeing with them means agreeing with the format rather than with an
# implementation of it.

def parse_header(data):
    """Pull out the fields the reference decoders need."""
    h = {}
    h["offset"] = struct.unpack("<I", data[10:14])[0]
    h["header_size"] = hs = struct.unpack("<I", data[14:18])[0]
    if hs == 12:
        h["width"], height = struct.unpack("<HH", data[18:22])
        h["bit_count"] = struct.unpack("<H", data[24:26])[0]
        h["compression"] = 0
        h["clr_used"] = 0
        h["entry_size"] = 3
    else:
        h["width"], height = struct.unpack("<ii", data[18:26])
        h["bit_count"] = struct.unpack("<H", data[28:30])[0]
        h["compression"] = struct.unpack("<I", data[30:34])[0] if hs >= 20 else 0
        h["clr_used"] = struct.unpack("<I", data[46:50])[0] if hs >= 40 else 0
        h["entry_size"] = 4
    h["top_down"] = height < 0
    h["height"] = abs(height)
    # OS/2 2.x spells 3 as Huffman and 4 as RLE24; Windows spells them
    # BI_BITFIELDS and BI_JPEG.  Only the sizes Windows never uses are OS/2.
    h["os2_v2"] = 16 <= hs <= 64 and hs % 4 == 0 and hs not in (40, 52, 56)
    return h


def read_palette(data, h):
    at = 14 + h["header_size"]
    count = h["clr_used"] or (1 << h["bit_count"] if h["bit_count"] <= 8 else 0)
    palette = []
    for _ in range(count):
        if at + h["entry_size"] > len(data):
            break
        palette.append((data[at + 2], data[at + 1], data[at]))
        at += h["entry_size"]
    return palette


def emit(h, pixels):
    """Pack a height-by-width grid of RGBA tuples into top-down RGBA8."""
    out = bytearray()
    for row in range(h["height"]):
        source = row if h["top_down"] else h["height"] - 1 - row
        for x in range(h["width"]):
            out += bytes(pixels[source][x])
    return bytes(out)


def reference_rle(data):
    """RLE8, RLE4 and the OS/2 RLE24, from the escape structure they share."""
    h = parse_header(data)
    palette = read_palette(data, h)
    comp = h["compression"]
    rle4 = comp == 2
    rle24 = h["os2_v2"] and comp == 4
    w, height = h["width"], h["height"]
    # Pixels the stream never reaches keep the raster's zeroed value, which is
    # transparent and not opaque black.
    pixels = [[(0, 0, 0, 0)] * w for _ in range(height)]

    def plot(x, y, rgb):
        if 0 <= x < w and 0 <= y < height:
            pixels[y][x] = (rgb[0], rgb[1], rgb[2], 255)

    s = data[h["offset"]:]
    i = x = y = 0
    while i < len(s):
        count = s[i]
        i += 1
        if count:
            if rle24:
                b, g, r = s[i], s[i + 1], s[i + 2]
                i += 3
                for _ in range(count):
                    plot(x, y, (r, g, b))
                    x += 1
            else:
                value = s[i]
                i += 1
                for k in range(count):
                    idx = ((value >> 4) if k % 2 == 0 else (value & 0xF)) \
                        if rle4 else value
                    plot(x, y, palette[idx])
                    x += 1
            continue
        value = s[i]
        i += 1
        if value == 0:
            x, y = 0, y + 1
            continue
        if value == 1:
            break
        if value == 2:
            x += s[i]
            y += s[i + 1]
            i += 2
            continue
        n = value
        encoded = n * 3 if rle24 else ((n + 1) // 2 if rle4 else n)
        for k in range(n):
            if rle24:
                b, g, r = s[i + k * 3], s[i + k * 3 + 1], s[i + k * 3 + 2]
                plot(x, y, (r, g, b))
            elif rle4:
                byte = s[i + k // 2]
                plot(x, y, palette[(byte >> 4) if k % 2 == 0 else (byte & 0xF)])
            else:
                plot(x, y, palette[s[i + k]])
            x += 1
        i += encoded + (encoded & 1)
    return emit(h, pixels)


def reference_bitfields(data):
    """A BI_BITFIELDS or BI_ALPHABITFIELDS image, from its declared masks."""
    h = parse_header(data)
    hs, bc = h["header_size"], h["bit_count"]
    if hs >= 52:
        masks = list(struct.unpack("<III", data[54:66]))
        masks.append(struct.unpack("<I", data[66:70])[0] if hs >= 56 else 0)
    else:
        n = 4 if h["compression"] == 6 else 3
        at = 14 + hs
        masks = list(struct.unpack("<" + "I" * n, data[at:at + 4 * n]))
        if n == 3:
            masks.append(0)

    def decompose(mask):
        if not mask:
            return 0, 0
        shift = 0
        while not (mask >> shift) & 1:
            shift += 1
        return shift, mask >> shift

    parts = [decompose(m) for m in masks]
    stride = ((h["width"] * bc + 31) // 32) * 4
    pixels = []
    for y in range(h["height"]):
        row = []
        src = data[h["offset"] + y * stride:]
        for x in range(h["width"]):
            raw = int.from_bytes(src[x * (bc // 8):(x + 1) * (bc // 8)],
                                 "little")
            sample = []
            for mask, (shift, top) in zip(masks, parts):
                if not mask or not top:
                    sample.append(None)
                    continue
                v = (raw & mask) >> shift
                # Exact rescale to 8 bits, rounded: the rule PNG 13.12 states.
                sample.append((v * 255 + top // 2) // top)
            r, g, b, a = sample
            row.append((r, g, b, 255 if a is None else a))
        pixels.append(row)
    # pixels is indexed in file order, which is what emit() expects.
    return emit(h, pixels)


def extract_embedded(path):
    """The JPEG or PNG stream a BI_JPEG or BI_PNG file carries as pixel data."""
    data = open(path, "rb").read()
    h = parse_header(data)
    size = struct.unpack("<I", data[34:38])[0] if h["header_size"] >= 40 else 0
    end = h["offset"] + size if size else len(data)
    return data[h["offset"]:min(end, len(data))]


def reference_for(path):
    """The reference decoder that covers this file, or None."""
    data = open(path, "rb").read()
    if len(data) < 30 or data[:2] != b"BM":
        return None, None
    try:
        h = parse_header(data)
    except struct.error:
        return None, None
    comp = h["compression"]
    if comp in (1, 2) or (h["os2_v2"] and comp == 4):
        return reference_rle, data
    if not h["os2_v2"] and comp in (3, 6):
        return reference_bitfields, data
    return None, None


# ---------------------------------------------------------------------------
# The sweep
# ---------------------------------------------------------------------------

def compare(ours, theirs, tolerance, check_alpha):
    """Return None when they agree, or a sentence saying how they do not."""
    if len(ours) != len(theirs):
        return "sizes differ (%d vs %d bytes)" % (len(ours), len(theirs))
    worst_colour = worst_alpha = 0
    where = None
    for i in range(len(ours)):
        delta = abs(ours[i] - theirs[i])
        if i % 4 == 3:
            worst_alpha = max(worst_alpha, delta)
        elif delta > worst_colour:
            worst_colour, where = delta, i // 4
    if worst_colour > tolerance:
        return ("colour differs by up to %d (tolerance %d), first at pixel %d"
                % (worst_colour, tolerance, where))
    if check_alpha and worst_alpha > tolerance:
        return "alpha differs by up to %d" % worst_alpha
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--suite", default=os.environ.get("BMPSUITE"),
                        help="an unpacked bmpsuite, holding g/ q/ b/ x/")
    parser.add_argument("--decoder", required=True,
                        help="path to dump_bmp_raster")
    parser.add_argument("--out", default=None,
                        help="where to leave the decoded rasters")
    args = parser.parse_args()

    if not args.suite or not os.path.isdir(args.suite):
        print("bmpsuite: skipped (pass --suite or set BMPSUITE to an unpacked "
              "copy of https://entropymine.com/jason/bmpsuite/)")
        return 0
    if not os.path.isfile(args.decoder):
        print("bmpsuite: %s not built; run make bmp-dump-raster"
              % args.decoder, file=sys.stderr)
        return 1

    out_dir = args.out or os.path.join(args.suite, ".sweep-out")
    os.makedirs(out_dir, exist_ok=True)

    files = []
    for group in ("g", "q", "b", "x"):
        directory = os.path.join(args.suite, group)
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if name.lower().endswith(".bmp"):
                files.append((group, name, os.path.join(directory, name)))
    if not files:
        print("bmpsuite: %s holds no g/ q/ b/ x/ images" % args.suite,
              file=sys.stderr)
        return 1

    proc = subprocess.run([args.decoder, out_dir] + [p for _, _, p in files],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print("bmpsuite: the decoder exited %d\n%s"
              % (proc.returncode, proc.stderr[-4000:]), file=sys.stderr)
        return 1
    status = {}
    for line in proc.stdout.splitlines():
        parts = line.split("\t")
        status[parts[0]] = parts[1:]

    available = {}
    for name, fn in ORACLES.items():
        try:
            fn(files[0][2])
            available[name] = fn
        except ImportError:
            pass
        except FileNotFoundError:
            pass
        except Exception:
            available[name] = fn

    failures = []
    checked_by_reference = 0
    checked_by_oracle = 0
    rejected_as_expected = 0

    for group, name, path in files:
        state = status.get(name, ["missing"])
        decoded = state and state[0] == "ok"

        if name in REJECT:
            if decoded:
                failures.append("%s/%s: decoded, but should be refused (%s)"
                                % (group, name, REJECT[name]))
            else:
                rejected_as_expected += 1
            continue
        if not decoded:
            failures.append("%s/%s: expected to decode, got %s"
                            % (group, name, state[0] if state else "nothing"))
            continue

        with open(os.path.join(out_dir, name + ".rgba"), "rb") as f:
            ours = f.read()

        reference, data = reference_for(path)
        if reference is not None:
            try:
                theirs = reference(data)
            except Exception as error:  # a reference that cannot read it
                failures.append("%s/%s: the reference decoder failed: %s"
                                % (group, name, error))
                continue
            problem = compare(ours, theirs, 0, check_alpha=True)
            if problem:
                failures.append("%s/%s: differs from the specification "
                                "reference: %s" % (group, name, problem))
            else:
                checked_by_reference += 1

        if name in EQUIVALENT:
            twin = os.path.join(out_dir, EQUIVALENT[name] + ".rgba")
            if not os.path.isfile(twin):
                failures.append("%s/%s: %s did not decode, so there is nothing "
                                "to compare it with"
                                % (group, name, EQUIVALENT[name]))
            else:
                with open(twin, "rb") as f:
                    problem = compare(ours, f.read(), 0, check_alpha=True)
                if problem:
                    failures.append("%s/%s: should decode to the same picture "
                                    "as %s, but %s"
                                    % (group, name, EQUIVALENT[name], problem))
                else:
                    checked_by_reference += 1

        if name in EMBEDDED:
            payload = extract_embedded(path)
            checked = False
            for oracle, fn in available.items():
                tmp = os.path.join(out_dir, name + ".payload")
                with open(tmp, "wb") as f:
                    f.write(payload)
                try:
                    theirs = fn(tmp)
                except Exception:
                    continue
                problem = compare(ours, theirs, COLOUR_TOLERANCE,
                                  ORACLES_HONOUR_ALPHA)
                if problem:
                    failures.append("%s/%s: the wrapped decode differs from "
                                    "%s's decode of the payload alone: %s"
                                    % (group, name, oracle, problem))
                else:
                    checked = True
            if os.path.isfile(os.path.join(out_dir, name + ".payload")):
                os.remove(os.path.join(out_dir, name + ".payload"))
            if checked:
                checked_by_oracle += 1
            else:
                failures.append("%s/%s: nothing here could decode the "
                                "embedded payload" % (group, name))
            continue

        agreed = False
        for oracle, fn in available.items():
            if name in ORACLE_IS_WRONG.get(oracle, {}):
                continue
            try:
                theirs = fn(path)
            except Exception:
                continue  # an oracle that cannot read a file is not evidence
            problem = compare(ours, theirs, COLOUR_TOLERANCE,
                              ORACLES_HONOUR_ALPHA)
            if problem:
                failures.append("%s/%s: %s says %s"
                                % (group, name, oracle, problem))
            else:
                agreed = True
        if agreed:
            checked_by_oracle += 1
        elif reference is None and name not in EQUIVALENT:
            failures.append("%s/%s: nothing could check it - no reference "
                            "decoder covers it and no oracle read it"
                            % (group, name))

    print("bmpsuite %s" % args.suite)
    print("  %d files; %d refused as intended" % (len(files),
                                                  rejected_as_expected))
    print("  %d checked against a decoder written from the specification"
          % checked_by_reference)
    print("  %d corroborated by %s" % (checked_by_oracle,
                                       ", ".join(sorted(available)) or "nothing"))
    if failures:
        print("\n%d problems:" % len(failures))
        for line in failures:
            print("  " + line)
        return 1
    print("  no disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
