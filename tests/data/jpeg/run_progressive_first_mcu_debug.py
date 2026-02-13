#!/usr/bin/env python3
"""
Run the decoder once with all progressive first-MCU debug outputs enabled,
so you get in a single run:
  - TRACE_JPEG_AC_SYMBOLS for scan 1 and 4 (Y AC 1-5 and Y AC 6-63) block 0..5
  - DUMP_JPEG_COEF_AFTER_SCAN (hash + optional block) after each scan
  - TRACE_JPEG_AC_REFINE and DUMP_JPEG_AC_REFINE_BLOCK at start of refinement scans

Usage:
  python3 run_progressive_first_mcu_debug.py <file.jpg> [decoder] [out_stderr.txt]
  If out_stderr.txt is given, decoder stderr is written there; otherwise to stdout.

Example (6-scan truncated file, decoder in path):
  LD_LIBRARY_PATH=... python3 run_progressive_first_mcu_debug.py \\
    tests/data/jpeg/progressive_sample_6scan.jpg build/linux/release/apps/dump_jpeg_raster debug.log
"""
import os
import subprocess
import sys


def main() -> int:
    argv = list(sys.argv[1:])
    if len(argv) < 1:
        print("Usage: run_progressive_first_mcu_debug.py <file.jpg> [decoder] [out_stderr.txt]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(argv[0])
    decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    stderr_path = argv[2] if len(argv) >= 3 else None
    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["TRACE_JPEG_AC_SYMBOLS"] = "1"
    env["TRACE_JPEG_AC_SYMBOLS_SCAN"] = "1,4"   # Y AC 1-5 and Y AC 6-63
    env["TRACE_JPEG_AC_REFINE"] = "1"
    env["DUMP_JPEG_AC_REFINE_BLOCK"] = "1"
    env["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = "6"  # through first refinement for 6-scan file

    r = subprocess.run(
        [decoder, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env,
    )
    stderr = r.stderr.decode("utf-8", errors="replace")
    if stderr_path:
        with open(stderr_path, "w") as f:
            f.write(stderr)
        print("Wrote stderr to", stderr_path, file=sys.stderr)
    else:
        print(stderr, end="")
    return 0 if r.returncode == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
