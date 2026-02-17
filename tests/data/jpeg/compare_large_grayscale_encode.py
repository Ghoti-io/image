#!/usr/bin/env python3
"""
Compare our encoder's 640×480 baseline grayscale scan to the reference encoder.

Same image as Large640x480BaselineGrayscale: 640×480 GRAY8, pixel (x,y) = (x+y)&0xFF.
Settings: baseline, quality 85, no restart (T.81).

Steps:
  1. Run our encoder (Large640x480BaselineGrayscale) with GIMG_JPEG_DUMP_SCAN_BASELINE
     → produces our scan bytes (test will fail at decode; dump is written during save).
  2. Run encode_libjpeg_baseline_scan 640 480 85 → reference scan bytes.
  3. Compare scan bytes byte-by-byte; report first difference and approximate block index.

Run from image/:
  python3 tests/data/jpeg/compare_large_grayscale_encode.py [--work-dir ...]

Requires: make jpeg-oracle-tools, make test (testJpeg_encode).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
ORACLE_DIR = os.environ.get("GIMG_JPEG_ORACLE_DIR") or SCRIPT_DIR


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare our 640×480 baseline grayscale encode to reference (same image/settings)"
    )
    ap.add_argument(
        "--work-dir",
        default=None,
        help="Directory for scan dumps (default: tests/out/jpeg)",
    )
    ap.add_argument("--test-exe", default="", help="Path to testJpeg_encode")
    ap.add_argument("--encode-ref", default="", help="Path to encode_libjpeg_baseline_scan")
    ap.add_argument("--skip-ours", action="store_true", help="Skip running our encoder (use existing ours scan)")
    args = ap.parse_args()

    work = args.work_dir or os.path.join(IMAGE_DIR, "tests", "out", "jpeg")
    os.makedirs(work, exist_ok=True)

    encode_ref = args.encode_ref or os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_scan")
    if not os.path.isfile(encode_ref):
        encode_ref = os.path.join(ORACLE_DIR, "encode_libjpeg_baseline_scan.exe")
    if not os.path.isfile(encode_ref):
        print(f"ERROR: encode_libjpeg_baseline_scan not found at {encode_ref}", file=sys.stderr)
        return 1

    test_exe = args.test_exe
    if not test_exe:
        for base in [
            os.path.join(IMAGE_DIR, "build", "linux", "release", "apps"),
            os.path.join(IMAGE_DIR, "build", "mac", "release", "apps"),
            os.path.join(IMAGE_DIR, "build", "win32", "release", "apps"),
        ]:
            p = os.path.join(base, "testJpeg_encode")
            if os.path.isfile(p):
                test_exe = p
                break
            if os.path.isfile(p + ".exe"):
                test_exe = p + ".exe"
                break
    if (not args.skip_ours) and (not test_exe or not os.path.isfile(test_exe)):
        print("ERROR: testJpeg_encode not found. Run from image/ and build with make test.", file=sys.stderr)
        return 1

    ours_scan = os.path.join(work, "ours_640x480_scan.bin")
    ref_scan = os.path.join(work, "ref_640x480_scan.bin")

    # 1) Our encoder: run Large640x480BaselineGrayscale with scan dump (decode will fail; dump is from save).
    if not args.skip_ours:
        app_dir = os.path.dirname(test_exe)
        env = os.environ.copy()
        env["GIMG_IMAGE_ROOT"] = IMAGE_DIR
        env["GIMG_JPEG_DUMP_SCAN_BASELINE"] = ours_scan
        if app_dir:
            env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        r = subprocess.run(
            [test_exe, "--gtest_filter=JpegEncode.Large640x480BaselineGrayscale"],
            cwd=IMAGE_DIR,
            env=env,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if not os.path.isfile(ours_scan):
            print("Our encoder did not produce scan dump. Set GIMG_JPEG_DUMP_SCAN_BASELINE.", file=sys.stderr)
            return 1
        if r.returncode != 0:
            print("(Test failed at decode as expected; our scan dump was written during save.)", file=sys.stderr)

    # 2) Reference: same 640×480 grayscale, Q85, no restart.
    r = subprocess.run(
        [encode_ref, "640", "480", "85", ref_scan, "0"],
        cwd=SCRIPT_DIR,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if r.returncode != 0:
        print("Reference encoder failed:", r.stderr or r.stdout, file=sys.stderr)
        return 1
    if not os.path.isfile(ref_scan):
        print("Reference encoder did not produce scan.", file=sys.stderr)
        return 1

    # 3) Compare
    with open(ours_scan, "rb") as f:
        ours_bytes = f.read()
    with open(ref_scan, "rb") as f:
        ref_bytes = f.read()

    print(f"Our scan size:   {len(ours_bytes)} bytes")
    print(f"Reference scan: {len(ref_bytes)} bytes")
    if len(ours_bytes) != len(ref_bytes):
        print(f"Length delta:   {len(ours_bytes) - len(ref_bytes):+d} bytes")

    n = min(len(ours_bytes), len(ref_bytes))
    match = ours_bytes[:n] == ref_bytes[:n]
    if not match:
        for i in range(n):
            if ours_bytes[i] != ref_bytes[i]:
                print(f"\nFirst difference at scan byte {i}: ours=0x{ours_bytes[i]:02x} ref=0x{ref_bytes[i]:02x}")
                # Approximate block index: ~33 bits per block for grayscale Q85
                bits_before = i * 8
                approx_block = bits_before // 33 if bits_before else 0
                print(f"  (approx. bit position {bits_before}, ~block index {approx_block} of 4800)")
                start = max(0, i - 8)
                end = min(len(ours_bytes), len(ref_bytes), i + 9)
                print("  ours:", " ".join(f"{ours_bytes[j]:02x}" for j in range(start, end)))
                print("  ref :", " ".join(f"{ref_bytes[j]:02x}" for j in range(start, end)))
                break
    else:
        if len(ours_bytes) != len(ref_bytes):
            print(f"\nFirst {n} bytes match; length mismatch (ours longer by {len(ours_bytes) - len(ref_bytes)} bytes).")
        else:
            print("\nScan bytes match.")

    return 0 if match and len(ours_bytes) == len(ref_bytes) else 1


if __name__ == "__main__":
    sys.exit(main())
