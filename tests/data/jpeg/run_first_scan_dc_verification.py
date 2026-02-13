#!/usr/bin/env python3
"""
Run first-scan DC verification: Python script vs decoder TRACE.

Runs verify_first_scan_dc.py on the fixture and the decoder with
TRACE_JPEG_DC_SYMBOLS=1, then compares the two outputs.
Exits 0 if identical, 1 if mismatch, 2 if setup failed (e.g. decoder not set).

Usage:
  python3 run_first_scan_dc_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]

If decoder path is not provided, exits 2 (caller may skip the test).
"""
import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print(
            "Usage: python3 run_first_scan_dc_verification.py <path-to.jpeg> [path-to-dump_jpeg_raster]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    verify_script = os.path.join(script_dir, "verify_first_scan_dc.py")
    decoder = sys.argv[2] if len(sys.argv) >= 3 else None

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(verify_script):
        print(f"Script not found: {verify_script}", file=sys.stderr)
        return 2
    if decoder is None:
        return 2
    if decoder == "dump_jpeg_raster" and not os.path.isfile(decoder):
        pass
    elif not os.path.isfile(decoder):
        print(f"Decoder not found: {decoder}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["TRACE_JPEG_DC_SYMBOLS"] = "1"
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    r = subprocess.run(
        [sys.executable, verify_script, jpeg_path],
        capture_output=True,
        text=True,
        timeout=10,
        cwd=script_dir,
    )
    if r.returncode != 0:
        print(
            f"verify_first_scan_dc.py failed: {r.stderr or r.returncode}",
            file=sys.stderr,
        )
        return 2
    script_lines = [
        line.strip()
        for line in r.stdout.splitlines()
        if "TRACE_JPEG_DC_SYMBOLS" in line
    ]

    r2 = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=env,
    )
    stderr = (r2.stderr or b"").decode("utf-8", errors="replace")
    decoder_lines = [
        line.strip()
        for line in stderr.splitlines()
        if "TRACE_JPEG_DC_SYMBOLS" in line
    ]

    if len(script_lines) != len(decoder_lines):
        print(
            f"Line count mismatch: script {len(script_lines)}, decoder {len(decoder_lines)}",
            file=sys.stderr,
        )
        return 1
    for a, b in zip(script_lines, decoder_lines):
        if a != b:
            print(f"Mismatch:\n  script:  {a}\n  decoder: {b}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
