#!/usr/bin/env python3
"""
Generate JPEG test data for decode and golden tests. Writes to the same
directory as this script (tests/data/jpeg/). Requires Pillow. For EXIF
orientation, piexif is used if available.

Run from repo root: python3 tests/data/jpeg/generate.py
Or from this dir: python3 generate.py

Writes manifest.json listing all fixtures with width, height, mode, progressive,
quality, has_exif, has_icc for tests and tooling.
"""
import io
import json
import os
import sys
# A fixture generator is pinned for the same reason a comparison oracle is:
# a fixture's bytes are part of what it means, and "whatever Pillow this
# machine has" is not a version anything records. `font` pins its fontTools
# the same way, and the fixtures
# under this directory are committed, so the version that wrote them outlives
# the machine that ran it.
#
# The one directory this script writes is declared read-write; the rest of the
# tree stays read-only, which is the same rule a comparison runs under and the
# reason a generator has to name its output rather than have it assumed.
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from oracle_reexec import inside_or_reexec  # noqa: E402

inside_or_reexec("pillow",
    scratch=[os.path.dirname(os.path.abspath(__file__))])


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required. Install with: pip install Pillow")

# piexif, which writes the EXIF orientation in jpeg_exif_orientation.jpg.
#
# Not optional any more, and the change matters. It used to be a try/except
# that left `piexif = None`, and the generator then wrote the fixture *without*
# an orientation tag and printed a note - so a machine without piexif produced
# a file named for a property it did not have, and every test that reads that
# orientation would be checking a fixture that never carried one. It is pinned
# in tools/oracle/containers/IMAGES and present in the image this script
# re-execs into, so a failure here is a broken image and not a missing
# convenience.
import piexif


def write_jpeg(name: str, img: Image.Image, **save_kw) -> None:
    path = os.path.join(SCRIPT_DIR, name)
    kwargs = {"quality": 85, "format": "JPEG"}
    kwargs.update(save_kw)
    img.save(path, **kwargs)
    print("Wrote", path)


def _patch_jfif_density(path: str, units: int, x: int, y: int) -> None:
    """Rewrite the APP0 JFIF density fields in place.

    JFIF 1.02: after the "JFIF\\0" identifier come a two-byte version, the
    units byte, then Xdensity and Ydensity as big-endian sixteen-bit values.
    """
    data = bytearray(open(path, "rb").read())
    at = data.find(b"JFIF\0")
    if at < 0:
        raise SystemExit("no APP0 JFIF in " + path)
    data[at + 7] = units
    data[at + 8:at + 10] = x.to_bytes(2, "big")
    data[at + 10:at + 12] = y.to_bytes(2, "big")
    open(path, "wb").write(bytes(data))
    print("Patched", path, "units=%d density=%d:%d" % (units, x, y))


def main() -> None:
    os.makedirs(SCRIPT_DIR, exist_ok=True)
    manifest: list[dict] = []

    # ---- Baseline 8×8 grayscale ----
    gray = Image.new("L", (8, 8), color=128)
    write_jpeg("baseline_8x8_gray.jpg", gray)
    manifest.append({
        "file": "baseline_8x8_gray.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- JFIF density units 0: a pixel aspect ratio and no size ----
    # JFIF 1.02 gives the APP0 density fields a unit specifier.  Unit 1 is dots
    # per inch; unit 0 means the two numbers are the pixel's aspect ratio and
    # the file says nothing about physical size - the same statement PNG makes
    # with a unit 0 pHYs and GIF with its Pixel Aspect Ratio byte.
    #
    # Pillow cannot write that, so the segment is patched after the fact: the
    # density fields are 2 and 1, a pixel twice as wide as it is tall, which no
    # encoder emits by accident (1:1 is the boilerplate everything writes).
    aspect_src = Image.new("RGB", (16, 16), color=(0xC8, 0x3C, 0x28))
    write_jpeg("jfif_aspect_2_1.jpg", aspect_src)
    _patch_jfif_density(
        os.path.join(SCRIPT_DIR, "jfif_aspect_2_1.jpg"), units=0, x=2, y=1)
    manifest.append({
        "file": "jfif_aspect_2_1.jpg",
        "width": 16, "height": 16, "mode": "RGB",
        "progressive": False, "quality": 85, "has_exif": False,
        "has_icc": False,
    })

    # ---- Baseline 16×16 YCbCr (RGB saved as JPEG; Pillow writes 4:2:0) ----
    rgb = Image.new("RGB", (16, 16), color=(0x11, 0x22, 0x33))
    write_jpeg("baseline_16x16_ycbcr.jpg", rgb)
    manifest.append({
        "file": "baseline_16x16_ycbcr.jpg",
        "width": 16, "height": 16, "mode": "RGB",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- Edge dimensions: 9×9 gray, 8×16 gray ----
    gray_9x9 = Image.new("L", (9, 9), color=100)
    write_jpeg("baseline_9x9_gray.jpg", gray_9x9)
    manifest.append({
        "file": "baseline_9x9_gray.jpg",
        "width": 9, "height": 9, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })
    gray_8x16 = Image.new("L", (8, 16), color=64)
    write_jpeg("baseline_8x16_gray.jpg", gray_8x16)
    manifest.append({
        "file": "baseline_8x16_gray.jpg",
        "width": 8, "height": 16, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- Quality variants (8×8 gray at 50 and 100) ----
    write_jpeg("baseline_8x8_gray_q50.jpg", gray, quality=50)
    manifest.append({
        "file": "baseline_8x8_gray_q50.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 50, "has_exif": False, "has_icc": False,
    })
    write_jpeg("baseline_8x8_gray_q100.jpg", gray, quality=100)
    manifest.append({
        "file": "baseline_8x8_gray_q100.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 100, "has_exif": False, "has_icc": False,
    })

    # ---- Progressive (16×16 with variation so first AC scan has enough entropy) ----
    prog = Image.new("RGB", (16, 16), color=(0x80, 0x80, 0x80))
    px = prog.load()
    for y in range(16):
        for x in range(16):
            px[x, y] = ((x * 17) % 256, (y * 13) % 256, (x + y) % 256)
    write_jpeg("progressive_sample.jpg", prog, progressive=True)
    manifest.append({
        "file": "progressive_sample.jpg",
        "width": 16, "height": 16, "mode": "RGB",
        "progressive": True, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- Progressive 32×32 (4 MCUs for multi-MCU DC verification) ----
    prog32 = Image.new("RGB", (32, 32), color=(0x80, 0x80, 0x80))
    px32 = prog32.load()
    for y in range(32):
        for x in range(32):
            px32[x, y] = ((x * 11) % 256, (y * 7) % 256, (x * 3 + y) % 256)
    write_jpeg("progressive_32x32.jpg", prog32, progressive=True)
    manifest.append({
        "file": "progressive_32x32.jpg",
        "width": 32, "height": 32, "mode": "RGB",
        "progressive": True, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- Minimal progressive (8×8 grayscale, single component, fewer scans) ----
    prog_gray = Image.new("L", (8, 8), color=128)
    px_gray = prog_gray.load()
    for y in range(8):
        for x in range(8):
            px_gray[x, y] = (x * 32 + y * 16) % 256
    write_jpeg("progressive_8x8_gray.jpg", prog_gray, progressive=True)
    manifest.append({
        "file": "progressive_8x8_gray.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": True, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- Non-trivial size (640×480) for decode stress and MCU coverage ----
    # Baseline grayscale 640×480
    gray_640x480 = Image.new("L", (640, 480), color=128)
    px_640g = gray_640x480.load()
    for y in range(480):
        for x in range(640):
            px_640g[x, y] = (x + y * 3) % 256
    write_jpeg("baseline_640x480_gray.jpg", gray_640x480)
    manifest.append({
        "file": "baseline_640x480_gray.jpg",
        "width": 640, "height": 480, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })
    # Baseline YCbCr 640×480
    rgb_640x480 = Image.new("RGB", (640, 480), color=(0x40, 0x80, 0xc0))
    px_640r = rgb_640x480.load()
    for y in range(480):
        for x in range(640):
            px_640r[x, y] = ((x * 3) % 256, (y * 5) % 256, (x + y) % 256)
    write_jpeg("baseline_640x480_ycbcr.jpg", rgb_640x480)
    manifest.append({
        "file": "baseline_640x480_ycbcr.jpg",
        "width": 640, "height": 480, "mode": "RGB",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })
    # Progressive YCbCr 640×480
    write_jpeg("progressive_640x480_ycbcr.jpg", rgb_640x480, progressive=True)
    manifest.append({
        "file": "progressive_640x480_ycbcr.jpg",
        "width": 640, "height": 480, "mode": "RGB",
        "progressive": True, "quality": 85, "has_exif": False, "has_icc": False,
    })

    # ---- With EXIF orientation (tag 274 = 6, 90° CW) ----
    exif_img = Image.new("L", (8, 8), color=200)
    has_exif = False
    # No branch here any more: the fixture is named for its orientation tag,
    # so writing it without one is writing a different fixture under the same
    # name.
    zeroth_ifd = {piexif.ImageIFD.Orientation: 6}
    exif_dict = {"0th": zeroth_ifd, "Exif": {}, "GPS": {}, "1st": {}, "thumbnail": None}
    exif_bytes = piexif.dump(exif_dict)
    write_jpeg("jpeg_exif_orientation.jpg", exif_img, exif=exif_bytes)
    has_exif = True
    manifest.append({
        "file": "jpeg_exif_orientation.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": has_exif, "has_icc": False,
    })

    # ---- With an EXIF thumbnail ----
    #
    # A fixture of its own rather than a second property on the one above.
    # jpeg_exif_orientation.jpg is read by four tests, a committed .raw oracle
    # and a golden pixel hash; one fixture, one property keeps a failure
    # pointing at one thing. This one exists because
    # JpegEncode.RoundTripExifThumbnailPreserved had no thumbnail to preserve -
    # it loaded the orientation fixture, which carries none, and its only live
    # assertion was that the item count had not shrunk.
    #
    # The thumbnail is a gradient rather than a flat field so that "the
    # thumbnail survived" cannot be satisfied by a blank raster of the right
    # size, and the main image is a different gradient so that the two cannot
    # be confused for each other.
    thumb_img = Image.new("L", (4, 4))
    thumb_img.putdata([(x * 60 + y * 15) % 256 for y in range(4) for x in range(4)])
    thumb_buf = io.BytesIO()
    thumb_img.save(thumb_buf, format="JPEG", quality=85)
    thumb_bytes = thumb_buf.getvalue()
    thumb_main = Image.new("L", (8, 8))
    thumb_main.putdata([(x * 30 + y * 7) % 256 for y in range(8) for x in range(8)])
    # T.81 has nothing to say about this; the thumbnail lives in EXIF's IFD1,
    # and Compression 6 is what says it is a JPEG stream rather than raw
    # samples. The resolution tags are what piexif expects beside it.
    thumb_exif = {
        "0th": {},
        "Exif": {},
        "GPS": {},
        "1st": {
            piexif.ImageIFD.Compression: 6,
            piexif.ImageIFD.XResolution: (72, 1),
            piexif.ImageIFD.YResolution: (72, 1),
            piexif.ImageIFD.ResolutionUnit: 2,
        },
        "thumbnail": thumb_bytes,
    }
    write_jpeg("jpeg_exif_thumbnail.jpg", thumb_main,
               exif=piexif.dump(thumb_exif))
    manifest.append({
        "file": "jpeg_exif_thumbnail.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": True,
        "has_icc": False, "has_thumbnail": True,
    })

    # ---- With ICC (minimal profile: 128-byte header + tag table) ----
    minimal_icc = bytearray(128)
    minimal_icc[0:4] = (128 + 4 + 12).to_bytes(4, "big")
    minimal_icc[4:8] = b"argl"
    minimal_icc[8:12] = (0x02000000).to_bytes(4, "big")
    minimal_icc[12:16] = b"scnr"
    minimal_icc[16:20] = b"RGB "
    minimal_icc[20:24] = b"XYZ "
    minimal_icc[36:40] = (0x00000000).to_bytes(4, "big")
    icc_img = Image.new("RGB", (8, 8), color=(100, 100, 100))
    write_jpeg("jpeg_with_icc.jpg", icc_img, icc_profile=bytes(minimal_icc))
    manifest.append({
        "file": "jpeg_with_icc.jpg",
        "width": 8, "height": 8, "mode": "RGB",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": True,
    })

    # ---- With a real ICC profile ----
    #
    # jpeg_with_icc.jpg above keeps its stub: a 128-byte header with no tags
    # is the right fixture for "an opaque profile survives APP2 segmentation
    # and comes back byte for byte".  This one carries a profile littleCMS
    # parses and transforms with - the swap-red-and-green one, so a reader
    # that applies it differs visibly from one that merely carries it.
    # See tests/data/icc/generate.py.
    real_icc_path = os.path.join(
        os.path.dirname(SCRIPT_DIR), "icc", "swap_rg.icc")
    if not os.path.exists(real_icc_path):
        raise SystemExit(
            "missing %s; run tests/data/icc/generate.py first" % real_icc_path)
    with open(real_icc_path, "rb") as f:
        real_icc = f.read()
    real_img = Image.new("RGB", (8, 8), color=(200, 60, 40))
    write_jpeg("jpeg_icc_swap_rg.jpg", real_img, icc_profile=real_icc)
    manifest.append({
        "file": "jpeg_icc_swap_rg.jpg",
        "width": 8, "height": 8, "mode": "RGB",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": True,
    })

    # ---- CMYK sample (8×8) ----
    cmyk = Image.new("CMYK", (8, 8), color=(0, 0, 0, 0))
    write_jpeg("cmyk_sample.jpg", cmyk)
    manifest.append({
        "file": "cmyk_sample.jpg",
        "width": 8, "height": 8, "mode": "CMYK",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
    })

    manifest_path = os.path.join(SCRIPT_DIR, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print("Wrote", manifest_path)
    print("Done. Run generate_jpeg_oracle_raws.py for .raw oracles; run testJpeg_load for decode tests.")


if __name__ == "__main__":
    main()
