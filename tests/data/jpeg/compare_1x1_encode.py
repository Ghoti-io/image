#!/usr/bin/env python3
"""
Compare our encoder's 1×1 baseline RGB output to libjpeg's with the same settings.

Same image as SaveRgb1x1ThenLoadDecode: 1×1 RGB (128, 128, 128).
Settings: baseline, quality 85, 4:4:4.

Steps:
  1. Run JpegEncode.SaveRgb1x1ThenLoadDecode with GIMG_JPEG_DUMP_SCAN_BASELINE set
     → produces our scan bytes (and baseline_1x1.jpg)
  2. Run encode_libjpeg_1x1_rgb → libjpeg scan bytes
  3. Compare scan bytes byte-by-byte; report first difference and context

Run from image/: python3 tests/data/jpeg/compare_1x1_encode.py [--work-dir ...]
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
        description="Compare our 1×1 baseline RGB encode to libjpeg (same settings)"
    )
    ap.add_argument(
        "--work-dir",
        default=None,
        help="Directory for scan dumps (default: tests/out/jpeg)",
    )
    ap.add_argument("--test-exe", default="", help="Path to testJpeg_encode")
    ap.add_argument("--encode-ref", default="", help="Path to encode_libjpeg_1x1_rgb")
    args = ap.parse_args()

    work = args.work_dir or os.path.join(IMAGE_DIR, "tests", "out", "jpeg")
    os.makedirs(work, exist_ok=True)

    encode_ref = args.encode_ref or os.path.join(ORACLE_DIR, "encode_libjpeg_1x1_rgb")
    if not os.path.isfile(encode_ref):
        encode_ref = os.path.join(ORACLE_DIR, "encode_libjpeg_1x1_rgb.exe")
    if not os.path.isfile(encode_ref):
        print(f"ERROR: encode_libjpeg_1x1_rgb not found at {encode_ref}", file=sys.stderr)
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
    if not test_exe or not os.path.isfile(test_exe):
        print("ERROR: testJpeg_encode not found. Run from image/ and build with make test.", file=sys.stderr)
        return 1

    app_dir = os.path.dirname(test_exe)
    env = os.environ.copy()
    env["GIMG_IMAGE_ROOT"] = IMAGE_DIR
    if app_dir:
        env["LD_LIBRARY_PATH"] = app_dir + os.pathsep + env.get("LD_LIBRARY_PATH", "")

    ours_scan = os.path.join(work, "encoder_1x1_ours_scan.bin")
    ref_scan = os.path.join(work, "encoder_1x1_libjpeg_scan.bin")
    oracle_jpeg = os.path.join(work, "libjpeg_encoded_1x1.jpg")

    # 1) Our encoder: run SaveRgb1x1ThenLoadDecode with scan dump
    env_dump = {**env, "GIMG_JPEG_DUMP_SCAN_BASELINE": ours_scan}
    r = subprocess.run(
        [test_exe, "--gtest_filter=JpegEncode.SaveRgb1x1ThenLoadDecode"],
        cwd=IMAGE_DIR,
        env=env_dump,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if r.returncode != 0:
        print("SaveRgb1x1ThenLoadDecode exit code:", r.returncode, file=sys.stderr)
        if r.stderr:
            print(r.stderr, file=sys.stderr)
    if not os.path.isfile(ours_scan):
        print("Our encoder did not produce scan dump. Set GIMG_JPEG_DUMP_SCAN_BASELINE.", file=sys.stderr)
        return 1

    # 2) Libjpeg oracle: same 1×1 RGB (128,128,128), Q85, 4:4:4; save full JPEG to disk
    subprocess.run(
        [encode_ref, ref_scan, oracle_jpeg],
        cwd=SCRIPT_DIR,
        capture_output=True,
        text=True,
        timeout=10,
    )
    if not os.path.isfile(ref_scan):
        print("encode_libjpeg_1x1_rgb did not produce ref scan.", file=sys.stderr)
        return 1

    # 3) Compare scan bytes
    with open(ours_scan, "rb") as f:
        ours_bytes = f.read()
    with open(ref_scan, "rb") as f:
        ref_bytes = f.read()

    print(f"Ours:  {len(ours_bytes)} bytes")
    print(f"Ref:   {len(ref_bytes)} bytes")

    n = min(len(ours_bytes), len(ref_bytes))
    match = ours_bytes == ref_bytes
    if not match:
        for i in range(n):
            if ours_bytes[i] != ref_bytes[i]:
                print(f"First difference at scan byte {i}: ours=0x{ours_bytes[i]:02x} ref=0x{ref_bytes[i]:02x}")
                start = max(0, i - 8)
                end = min(len(ours_bytes), len(ref_bytes), i + 9)
                print("  ours:", " ".join(f"{ours_bytes[j]:02x}" for j in range(start, end)))
                print("  ref :", " ".join(f"{ref_bytes[j]:02x}" for j in range(start, end)))
                # Bit-level hint
                x, y = ours_bytes[i], ref_bytes[i]
                if x != y:
                    diff = x ^ y
                    bits = [b for b in range(8) if (diff >> b) & 1]
                    print(f"  differing bits (0=LSB): {bits}")
                break
        else:
            print(f"Length mismatch: first {n} bytes match")
    else:
        print("Scan bytes match.")

    return 0 if match else 1


if __name__ == "__main__":
    sys.exit(main())
