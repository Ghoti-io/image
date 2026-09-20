#!/usr/bin/env python3
"""
Generate oracle raw pixel files for decoder tests.

Oracle flow: (1) JPEG on disk (e.g. from generate.py / Pillow), (2) Oracle
decodes JPEG, writes .raw to disk. Our decoder test then: (1) our decoder
reads same JPEG, (2) decodes to raw, (3) compares to .raw.

.raw format: 1 byte mode (0 = L, 1 = RGB, 2 = CMYK), 4 bytes width (LE),
4 bytes height (LE), then raw pixels (L: w*h, RGB: w*h*3, CMYK: w*h*4).

- L and RGB: Pillow decodes and writes .raw.
- CMYK: libjpeg (dump_jpeg_pixels_ref -o) writes .raw so decoder tests match
  libjpeg. Requires jpeg-oracle-tools to be built in DIR.
- CMYK also gets a second file, `<base>.cmyk2rgb.raw`, holding Pillow's RGB
  rendering of the same image (mode 1). That is the oracle for
  gimg_ops_convert_pixel_format's CMYK-to-RGBA conversion: this library has no
  colour engine, so what it can be held to is agreeing with the naive
  conversion everything else without one performs. Pillow's is libjpeg's CMYK
  handling plus its own ink arithmetic, and the two agree exactly on every
  pixel of every CMYK fixture here.

Usage:
  python3 tests/data/jpeg/generate_jpeg_oracle_raws.py [DIR]
  DIR defaults to tests/data/jpeg (fixture directory).

Requires: Pillow (pip install Pillow). For CMYK: build with make jpeg-oracle-tools.
"""
import os
import struct
import subprocess
import sys

try:
    from PIL import Image
except ImportError:
    Image = None

MODE_L = 0
MODE_RGB = 1
MODE_CMYK = 2


def repo_root() -> str:
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        if os.path.isdir(os.path.join(d, ".git")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return d


def main() -> int:
    if len(sys.argv) > 1:
        dirpath = os.path.abspath(sys.argv[1])
    else:
        root = repo_root()
        dirpath = os.path.join(root, "tests", "data", "jpeg")
    if not os.path.isdir(dirpath):
        print(f"Not a directory: {dirpath}", file=sys.stderr)
        return 1
    if Image is None:
        print("Pillow is required. Install with: pip install Pillow", file=sys.stderr)
        return 1
    written = 0
    for name in sorted(os.listdir(dirpath)):
        if not (name.lower().endswith(".jpg") or name.lower().endswith(".jpeg")):
            continue
        path = os.path.join(dirpath, name)
        if not os.path.isfile(path):
            continue
        try:
            with Image.open(path) as im:
                im.load()
        except Exception as e:
            print(f"{name}: skip (Pillow could not decode: {e})", file=sys.stderr)
            continue
        w, h = im.size
        if w <= 0 or h <= 0:
            continue
        base, _ = os.path.splitext(name)
        raw_path = os.path.join(dirpath, base + ".raw")

        if im.mode == "CMYK":
            # Pillow's RGB rendering of the same image, as the oracle for our
            # CMYK-to-RGBA conversion.  Written whether or not the libjpeg
            # tool below is available, because it needs nothing but Pillow.
            rgb_path = os.path.join(dirpath, base + ".cmyk2rgb.raw")
            with open(rgb_path, "wb") as f:
                f.write(struct.pack("<BII", MODE_RGB, w, h))
                f.write(im.convert("RGB").tobytes())
            written += 1

            # Use libjpeg (dump_jpeg_pixels_ref) so decoder tests match libjpeg.
            ref_exe = os.path.join(dirpath, "dump_jpeg_pixels_ref")
            if os.path.isfile(ref_exe):
                try:
                    subprocess.run(
                        [ref_exe, "-o", raw_path, path],
                        check=True,
                        capture_output=True,
                    )
                    written += 1
                except (subprocess.CalledProcessError, OSError) as e:
                    print(f"{name}: skip (dump_jpeg_pixels_ref failed: {e})", file=sys.stderr)
            else:
                exe_win = ref_exe + ".exe"
                if os.path.isfile(exe_win):
                    try:
                        subprocess.run(
                            [exe_win, "-o", raw_path, path],
                            check=True,
                            capture_output=True,
                        )
                        written += 1
                    except (subprocess.CalledProcessError, OSError) as e:
                        print(f"{name}: skip (dump_jpeg_pixels_ref failed: {e})", file=sys.stderr)
                else:
                    print(f"{name}: skip (CMYK needs libjpeg .raw; run make jpeg-oracle-tools)", file=sys.stderr)
            continue

        if im.mode == "L":
            mode_byte = MODE_L
            pixels = im.tobytes()
        elif im.mode in ("RGB", "RGBA"):
            rgb = im.convert("RGB")
            mode_byte = MODE_RGB
            pixels = rgb.tobytes()
        else:
            rgb = im.convert("RGB")
            mode_byte = MODE_RGB
            pixels = rgb.tobytes()
        with open(raw_path, "wb") as f:
            f.write(struct.pack("<BII", mode_byte, w, h))
            f.write(pixels)
        written += 1
    print(f"Wrote {written} .raw oracle files to {dirpath}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
