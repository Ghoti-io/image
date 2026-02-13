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
import json
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
    manifest: list[dict] = []

    # ---- Baseline 8×8 grayscale ----
    gray = Image.new("L", (8, 8), color=128)
    write_jpeg("baseline_8x8_gray.jpg", gray)
    manifest.append({
        "file": "baseline_8x8_gray.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": False, "has_icc": False,
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
    if piexif:
        zeroth_ifd = {piexif.ImageIFD.Orientation: 6}
        exif_dict = {"0th": zeroth_ifd, "Exif": {}, "GPS": {}, "1st": {}, "thumbnail": None}
        exif_bytes = piexif.dump(exif_dict)
        write_jpeg("jpeg_exif_orientation.jpg", exif_img, exif=exif_bytes)
        has_exif = True
    else:
        write_jpeg("jpeg_exif_orientation.jpg", exif_img)
        print("Note: install piexif for EXIF orientation in jpeg_exif_orientation.jpg: pip install piexif")
    manifest.append({
        "file": "jpeg_exif_orientation.jpg",
        "width": 8, "height": 8, "mode": "L",
        "progressive": False, "quality": 85, "has_exif": has_exif, "has_icc": False,
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
