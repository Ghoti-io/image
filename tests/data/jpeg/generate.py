#!/usr/bin/env python3
"""
Generate JPEG test data for decode and golden tests. Writes to the same
directory as this script (tests/data/jpeg/). Requires Pillow. For EXIF
orientation, piexif is used if available.

Run from repo root: python3 tests/data/jpeg/generate.py
Or from this dir: python3 generate.py
"""
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required. Install with: pip install Pillow")

# Optional: for EXIF orientation in jpeg_exif_orientation.jpg
try:
    import piexif
except ImportError:
    piexif = None


def write_jpeg(name: str, img: Image.Image, **save_kw) -> None:
    path = os.path.join(SCRIPT_DIR, name)
    kwargs = {"quality": 85, "format": "JPEG"}
    kwargs.update(save_kw)
    img.save(path, **kwargs)
    print("Wrote", path)


def main() -> None:
    os.makedirs(SCRIPT_DIR, exist_ok=True)

    # ---- Baseline 8×8 grayscale ----
    gray = Image.new("L", (8, 8), color=128)
    write_jpeg("baseline_8x8_gray.jpg", gray)

    # ---- Baseline 16×16 YCbCr (RGB saved as JPEG) ----
    rgb = Image.new("RGB", (16, 16), color=(0x11, 0x22, 0x33))
    write_jpeg("baseline_16x16_ycbcr.jpg", rgb)

    # ---- Progressive ----
    prog = Image.new("RGB", (16, 16), color=(0x80, 0x80, 0x80))
    write_jpeg("progressive_sample.jpg", prog, progressive=True)

    # ---- With EXIF orientation (tag 274 = 6, 90° CW) ----
    exif_img = Image.new("L", (8, 8), color=200)
    if piexif:
        zeroth_ifd = {piexif.ImageIFD.Orientation: 6}
        exif_dict = {"0th": zeroth_ifd, "Exif": {}, "GPS": {}, "1st": {}, "thumbnail": None}
        exif_bytes = piexif.dump(exif_dict)
        write_jpeg("jpeg_exif_orientation.jpg", exif_img, exif=exif_bytes)
    else:
        write_jpeg("jpeg_exif_orientation.jpg", exif_img)
        print("Note: install piexif for EXIF orientation in jpeg_exif_orientation.jpg: pip install piexif")

    # ---- With ICC (minimal profile: 128-byte header + tag table) ----
    # Minimal ICC v2 header (size, CMM, version, class, color space, etc.)
    minimal_icc = bytearray(128)
    minimal_icc[0:4] = (128 + 4 + 12).to_bytes(4, "big")  # profile size
    minimal_icc[4:8] = b"argl"  # CMM type
    minimal_icc[8:12] = (0x02000000).to_bytes(4, "big")  # version 2.0
    minimal_icc[12:16] = b"scnr"  # profile class (input)
    minimal_icc[16:20] = b"RGB "  # color space
    minimal_icc[20:24] = b"XYZ "  # PCS
    minimal_icc[36:40] = (0x00000000).to_bytes(4, "big")  # tag count = 0
    icc_img = Image.new("RGB", (8, 8), color=(100, 100, 100))
    write_jpeg("jpeg_with_icc.jpg", icc_img, icc_profile=bytes(minimal_icc))

    # ---- CMYK sample (8×8) ----
    cmyk = Image.new("CMYK", (8, 8), color=(0, 0, 0, 0))
    write_jpeg("cmyk_sample.jpg", cmyk)

    print("Done. Golden hashes: run testJpeg_load and update expected hashes in test_jpeg_load.cpp from failure output if needed.")


if __name__ == "__main__":
    main()
