#!/usr/bin/env python3
"""
Create a libjpeg-generated progressive 8×8 grayscale JPEG for encoder comparison.

Use case: If our decoder matches libjpeg on a file encoded by cjpeg (libjpeg),
but not on the Pillow-generated progressive_8x8_gray.jpg, the 26/64 mismatch
is likely due to Pillow's progressive encoding convention (DHT placement or
refinement order) differing from libjpeg. See task doc "Encoder comparison".

Requires: cjpeg (libjpeg-turbo-progs or libjpeg-progs), e.g.:
  sudo apt install libjpeg-turbo-progs   # or libjpeg-progs

Steps:
  1. Write an 8×8 grayscale PPM (P5) to progressive_8x8_libjpeg.ppm.
  2. Run: cjpeg -progressive -grayscale -quality 85 -outfile progressive_8x8_libjpeg.jpg progressive_8x8_libjpeg.ppm
  3. Run: python3 verify_script_vs_libjpeg.py progressive_8x8_libjpeg.jpg [dump_jpeg_coef_ref [dump_jpeg_raster]]

Exits: 0 if cjpeg produced a valid 8×8 progressive and verify was run; 1 if cjpeg
  failed or not found; 2 if verify failed (script/decoder disagree with libjpeg).
"""
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR


def main() -> int:
    ppm_path = os.path.join(SCRIPT_DIR, "progressive_8x8_libjpeg.ppm")
    jpeg_path = os.path.join(SCRIPT_DIR, "progressive_8x8_libjpeg.jpg")

    # 8×8 grayscale PPM (P5): magic, size, maxval, then 64 raw bytes
    # Use a simple pattern so the image is not uniform (affects coefficient structure)
    pixels = bytearray(64)
    for y in range(8):
        for x in range(8):
            pixels[y * 8 + x] = (x * 16 + y * 32) % 256
    with open(ppm_path, "wb") as f:
        f.write(b"P5\n8 8\n255\n")
        f.write(pixels)
    print(f"Wrote {ppm_path}")

    cjpeg = os.environ.get("CJPEG", "cjpeg")
    try:
        r = subprocess.run(
            [
                cjpeg,
                "-progressive",
                "-grayscale",
                "-quality",
                "85",
                "-outfile",
                jpeg_path,
                ppm_path,
            ],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except FileNotFoundError:
        print(
            "cjpeg not found. Install libjpeg-turbo-progs (e.g. sudo apt install libjpeg-turbo-progs).",
            file=sys.stderr,
        )
        return 1
    if r.returncode != 0:
        print(f"cjpeg failed: {r.stderr or r.stdout}", file=sys.stderr)
        return 1
    print(f"Wrote {jpeg_path}")

    ref_tool = os.path.join(ORACLE_DIR, "dump_jpeg_coef_ref")
    if not os.path.isfile(ref_tool):
        print(f"Reference tool not found: {ref_tool}", file=sys.stderr)
        return 2
    decoder = os.environ.get(
        "GIMG_TEST_DUMP_JPEG_RASTER",
        os.path.join(SCRIPT_DIR, "..", "..", "..", "build", "linux", "release", "apps", "dump_jpeg_raster"),
    )
    if not os.path.isfile(decoder):
        decoder = os.path.join(SCRIPT_DIR, "dump_jpeg_raster")  # same dir as ref
    env = os.environ.copy()
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    # Prefer full script vs libjpeg (needs 5+ scans). Else decoder vs libjpeg first-block.
    cmd = [sys.executable, os.path.join(SCRIPT_DIR, "verify_script_vs_libjpeg.py"), jpeg_path, ref_tool]
    if decoder and os.path.isfile(decoder):
        cmd.append(decoder)
    r2 = subprocess.run(cmd, cwd=SCRIPT_DIR, env=env, capture_output=True, text=True, timeout=15)
    if r2.returncode == 0:
        print("verify_script_vs_libjpeg: script and libjpeg match (64/64).")
        return 0
    if "5+ scans" in (r2.stderr or "") or "could not build reference" in (r2.stderr or "").lower():
        # Fewer than 5 scans: compare decoder vs libjpeg only (compare_first_block_natural).
        if not decoder or not os.path.isfile(decoder):
            print("Decoder not found; cannot fall back to compare_first_block_natural.", file=sys.stderr)
            return 2
        print("Fixture has <5 scans; comparing decoder vs libjpeg first block only.")
        cmd2 = [sys.executable, os.path.join(SCRIPT_DIR, "compare_first_block_natural.py"), jpeg_path, decoder, ref_tool]
        r3 = subprocess.run(cmd2, cwd=SCRIPT_DIR, env=env, capture_output=False, timeout=15)
        return 0 if r3.returncode == 0 else 2
    print(r2.stderr or r2.stdout, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
