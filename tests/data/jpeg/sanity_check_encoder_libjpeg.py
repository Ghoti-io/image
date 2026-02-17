#!/usr/bin/env python3
"""
Sanity check: Does our *encoder* match libjpeg's encoder for baseline and progressive?

(1) Baseline: Same 16×16 grayscale image pixel(x,y) = (x+y)&0xFF, quality 85.
    - Libjpeg: encode_libjpeg_baseline_scan → scan bytes + full JPEG.
    - Ours: run test SaveGrayscaleThenLoadDecode with GIMG_JPEG_DUMP_SCAN_BASELINE
      to get our scan bytes. Compare our scan bytes to libjpeg's (byte-by-byte).

(2) Progressive: Same 16×16 image (x+y*16)&0xFF, quality 85.
    - Libjpeg: cjpeg -progressive (default scan script).
    - Ours: run test ProgressiveWithRefinementScanDecodeMatchesBaseline →
      writes progressive_refinement.jpg (3-scan: DC, AC initial, AC refinement).
    Scan structures differ (ours is custom 3-scan; cjpeg uses default multi-scan),
    so we compare file sizes and report structure difference.

Requires: encode_libjpeg_baseline_scan, cjpeg, testJpeg_encode (with LD_LIBRARY_PATH).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
# Image project root (tests/data/jpeg -> ../../..)
IMAGE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR
W = 16
H = 16
QUALITY = 85


def main() -> int:
    ap = argparse.ArgumentParser(description="Encoder sanity: our encoder vs libjpeg (baseline + progressive)")
    ap.add_argument("--work-dir", default=None, help="Work dir (default: tests/out/jpeg)")
    ap.add_argument("--test-exe", default="", help="Path to testJpeg_encode")
    ap.add_argument("--encode-ref", default="", help="Path to encode_libjpeg_baseline_scan")
    args = ap.parse_args()

    work = args.work_dir
    if not work:
        work = os.path.join(IMAGE_DIR, "tests", "out", "jpeg")
    os.makedirs(work, exist_ok=True)

    encode_ref = args.encode_ref or os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_scan")
    test_exe = args.test_exe
    if not test_exe:
        for base in [
            os.path.join(IMAGE_DIR, "build", "linux", "release", "apps"),
            os.path.join(SCRIPT_DIR, "..", "..", "..", "build", "linux", "release", "apps"),
        ]:
            p = os.path.join(os.path.normpath(base), "testJpeg_encode")
            if os.path.isfile(p):
                test_exe = p
                break
    if not os.path.isfile(encode_ref):
        print(f"ERROR: encode_libjpeg_baseline_scan not found at {encode_ref}", file=sys.stderr)
        return 1
    if not test_exe or not os.path.isfile(test_exe):
        print(f"ERROR: testJpeg_encode not found. Pass --test-exe or build from image/.", file=sys.stderr)
        return 1

    app_dir = os.path.dirname(test_exe)
    env = os.environ.copy()
    if app_dir:
        env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    # -------------------------------------------------------------------------
    # (1) Baseline: our encoder vs libjpeg (same image (x+y)&0xFF, 16×16, Q85)
    # -------------------------------------------------------------------------
    print("(1) Baseline encoder: our scan bytes vs libjpeg")
    ref_scan = os.path.join(work, "encoder_ref_scan.bin")
    ours_scan = os.path.join(work, "encoder_ours_scan.bin")
    lj_baseline_jpg = os.path.join(work, "encoder_lj_baseline_16.jpg")

    subprocess.run(
        [encode_ref, str(W), str(H), str(QUALITY), ref_scan, "0", lj_baseline_jpg],
        cwd=SCRIPT_DIR,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if not os.path.isfile(ref_scan):
        print("  encode_libjpeg_baseline_scan did not produce ref scan.", file=sys.stderr)
        return 1
    ref_size = os.path.getsize(ref_scan)
    print(f"  Libjpeg baseline scan: {ref_size} bytes")

    env_dump = {**env, "GIMG_JPEG_DUMP_SCAN_BASELINE": ours_scan}
    r = subprocess.run(
        [test_exe, "--gtest_filter=JpegEncode.SaveGrayscaleThenLoadDecode"],
        cwd=IMAGE_DIR,
        env=env_dump,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if not os.path.isfile(ours_scan):
        print("  Our encoder did not produce scan dump (set GIMG_JPEG_DUMP_SCAN_BASELINE).", file=sys.stderr)
        return 1
    ours_size = os.path.getsize(ours_scan)

    with open(ref_scan, "rb") as f:
        ref_bytes = f.read()
    with open(ours_scan, "rb") as f:
        ours_bytes = f.read()
    n = min(len(ref_bytes), len(ours_bytes))
    match = ref_bytes == ours_bytes
    if not match:
        for i in range(n):
            if ref_bytes[i] != ours_bytes[i]:
                print(f"  First difference at byte {i}: ours=0x{ours_bytes[i]:02x} ref=0x{ref_bytes[i]:02x}", file=sys.stderr)
                break
        else:
            print(f"  Length mismatch: ours={len(ours_bytes)} ref={len(ref_bytes)}", file=sys.stderr)
    else:
        print(f"  Match: {len(ours_bytes)} bytes (our baseline scan == libjpeg baseline scan)")
    baseline_ok = match
    print()

    # -------------------------------------------------------------------------
    # (2) Progressive: our encoder vs libjpeg (file sizes; scan structure differs)
    # -------------------------------------------------------------------------
    print("(2) Progressive encoder: our file vs libjpeg file (same image (x+y*16)&0xFF)")
    # Libjpeg progressive: need PPM (x+y*16) and cjpeg -progressive
    ppm_path = os.path.join(work, "encoder_prog_16.ppm")
    with open(ppm_path, "wb") as f:
        f.write(f"P5\n{W} {H}\n255\n".encode())
        for y in range(H):
            for x in range(W):
                f.write(bytes([(x + y * 16) & 0xFF]))
    lj_prog_jpg = os.path.join(work, "encoder_lj_progressive_16.jpg")
    cjpeg = os.environ.get("CJPEG", "cjpeg")
    r2 = subprocess.run(
        [cjpeg, "-progressive", "-grayscale", "-quality", str(QUALITY), "-outfile", lj_prog_jpg, ppm_path],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if r2.returncode != 0 or not os.path.isfile(lj_prog_jpg):
        print(f"  cjpeg failed: {r2.stderr or r2.stdout}", file=sys.stderr)
        lj_prog_size = None
    else:
        lj_prog_size = os.path.getsize(lj_prog_jpg)
        print(f"  Libjpeg progressive (cjpeg default): {lj_prog_size} bytes")

    # Our progressive: run refinement test (writes progressive_refinement.jpg to tests/out/jpeg)
    r3 = subprocess.run(
        [test_exe, "--gtest_filter=JpegEncode.ProgressiveWithRefinementScanDecodeMatchesBaseline"],
        cwd=IMAGE_DIR,
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )
    # Test writes to jpeg_output_dir() = tests/out/jpeg (relative to build or source)
    ours_prog_alt = os.path.join(IMAGE_DIR, "tests", "out", "jpeg", "progressive_refinement.jpg")
    ours_prog_path = os.path.join(work, "progressive_refinement.jpg")
    if os.path.isfile(ours_prog_alt):
        ours_prog_size = os.path.getsize(ours_prog_alt)
        print(f"  Our progressive (3-scan DC+AC init+refine): {ours_prog_size} bytes (from {ours_prog_alt})")
    elif os.path.isfile(ours_prog_path):
        ours_prog_size = os.path.getsize(ours_prog_path)
        print(f"  Our progressive: {ours_prog_size} bytes")
    else:
        print("  Our progressive file not found (test writes to tests/out/jpeg/progressive_refinement.jpg).", file=sys.stderr)
        ours_prog_size = None

    if lj_prog_size is not None and ours_prog_size is not None:
        print("  (Scan structure differs: ours = 3-scan DC / AC initial / AC refinement; cjpeg = default multi-scan.)")
    progressive_ok = ours_prog_size is not None
    print()

    if baseline_ok and progressive_ok:
        print("Encoder sanity: baseline scan bytes match libjpeg; progressive file produced.")
    elif not baseline_ok:
        print("Encoder sanity: baseline scan bytes do NOT match libjpeg.")
    return 0 if baseline_ok else 1


if __name__ == "__main__":
    sys.exit(main())
