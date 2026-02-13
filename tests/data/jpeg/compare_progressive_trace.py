#!/usr/bin/env python3
"""
Compare our decoder's AC_INITIAL trace to libjpeg's (ref) AC_INITIAL trace.
Runs our decoder with TRACE_AC_COMPARE=1 and ref (dump_jpeg_coef_ref_debug)
with TRACE_REF_AC=1, extracts AC_INITIAL lines from both, diffs and reports
first discrepancy.

Requires:
- Our decoder (e.g. dump_jpeg_raster) built and on PATH or passed as arg.
- Ref binary dump_jpeg_coef_ref_debug linked against instrumented libjpeg-turbo:
  - Build in tree: cd image/third_party/libjpeg-turbo && mkdir -p build-debug && cd build-debug && cmake .. && make
  - Then from image: make jpeg-oracle-tools-debug-build (links build-debug/libjpeg.a; no install)
  - Or install to third_party/libjpeg-debug and make jpeg-oracle-tools-debug (needs LD_LIBRARY_PATH at run).

Usage:
  python3 compare_progressive_trace.py <file.jpg> [our_decoder] [ref_binary]
  python3 compare_progressive_trace.py --max-scans N <file.jpg> ...

Exit 0: AC_INITIAL lines match up to the last line (or both empty).
Exit 1: first mismatch or decoder/ref failed.
Exit 2: usage/setup error.
"""
import os
import re
import subprocess
import sys


def extract_ac_initial_lines(stderr_text: str) -> list:
    """Return list of lines that start with 'AC_INITIAL ' (canonical format)."""
    lines = []
    for line in stderr_text.splitlines():
        s = line.strip()
        if s.startswith("AC_INITIAL "):
            lines.append(s)
    return lines


def main() -> int:
    argv = list(sys.argv[1:])
    max_scans = 3  # scan 0, 1, 2 so we get first AC-initial (scan 1) and optionally more
    if "--max-scans" in argv:
        i = argv.index("--max-scans")
        if i + 1 >= len(argv):
            print("Usage: compare_progressive_trace.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
                  file=sys.stderr)
            return 2
        max_scans = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]

    if not argv:
        print("Usage: compare_progressive_trace.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
              file=sys.stderr)
        return 2

    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    our_decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_binary = argv[2] if len(argv) >= 3 else os.path.join(script_dir, "dump_jpeg_coef_ref_debug")

    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 2

    # Run our decoder with TRACE_AC_COMPARE so it emits AC_INITIAL lines
    env_ours = os.environ.copy()
    # If our decoder is in a build tree, set LD_LIBRARY_PATH so it finds libghoti.io-image-dev
    if os.path.isabs(our_decoder) or "/" in our_decoder:
        dec_dir = os.path.dirname(os.path.abspath(our_decoder))
        if "build" in dec_dir or "apps" in dec_dir:
            env_ours["LD_LIBRARY_PATH"] = dec_dir + (
                os.pathsep + env_ours.get("LD_LIBRARY_PATH", "")
            )
    env_ours["TRACE_AC_COMPARE"] = "1"
    env_ours["TRACE_JPEG_AC_SYMBOLS"] = "1"
    env_ours["TRACE_JPEG_AC_SYMBOLS_SCAN"] = "1,2,3,4,5,6,7,8,9,10"
    env_ours["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
    if max_scans > 1:
        env_ours["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"

    dec_out = subprocess.run(
        [our_decoder, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_ours,
    )
    ours_stderr = dec_out.stderr.decode("utf-8", errors="replace")
    ours_lines = extract_ac_initial_lines(ours_stderr)

    # Run ref (dump_jpeg_coef_ref_debug) with TRACE_REF_AC=1; ref uses libjpeg in libjpeg-debug
    env_ref = os.environ.copy()
    env_ref["TRACE_REF_AC"] = "1"
    libjpeg_lib = os.path.join(script_dir, "..", "..", "..", "third_party", "libjpeg-debug", "lib")
    if os.path.isdir(os.path.join(script_dir, "..", "..", "..", "third_party", "libjpeg-debug")):
        env_ref["LD_LIBRARY_PATH"] = os.path.abspath(libjpeg_lib) + (
            os.pathsep + env_ref.get("LD_LIBRARY_PATH", "")
        )

    ref_out = subprocess.run(
        [ref_binary, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_ref,
        cwd=script_dir,
    )
    if ref_out.returncode != 0:
        print("Ref (dump_jpeg_coef_ref_debug) failed:", ref_out.stderr.decode("utf-8", errors="replace")[:500],
              file=sys.stderr)
        return 1
    ref_stderr = ref_out.stderr.decode("utf-8", errors="replace")
    ref_lines = extract_ac_initial_lines(ref_stderr)

    # Compare line by line
    n = min(len(ours_lines), len(ref_lines))
    for i in range(n):
        if ours_lines[i] != ref_lines[i]:
            print(f"First mismatch at line {i + 1}:", file=sys.stderr)
            print(f"  Ours: {ours_lines[i]}", file=sys.stderr)
            print(f"  Ref:  {ref_lines[i]}", file=sys.stderr)
            return 1
    if len(ours_lines) != len(ref_lines):
        print(f"Line count: ours {len(ours_lines)}, ref {len(ref_lines)}", file=sys.stderr)
        if len(ours_lines) < len(ref_lines):
            print(f"  First ref-only line: {ref_lines[len(ours_lines)]}", file=sys.stderr)
        else:
            print(f"  First ours-only line: {ours_lines[len(ref_lines)]}", file=sys.stderr)
        return 1

    print(f"Match: {len(ours_lines)} AC_INITIAL lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
