#!/usr/bin/env python3
"""
Compare decoder TRACE_JPEG_AC_SYMBOLS (block=0) to script trace for one or more
AC-initial scans. Uses a single decoder run when --scans is used (e.g. --scans 1,4).

Usage:
  python3 run_scan_ac_initial_trace_compare.py [--scan N] <file.jpg> [decoder]
  python3 run_scan_ac_initial_trace_compare.py [--scans N,M,...] <file.jpg> [decoder]

Exit 0: script and decoder block=0 trace lines match for all requested scans.
Exit 1: mismatch or decoder failed.
Exit 2: usage/setup error.
"""
import os
import re
import subprocess
import sys


def extract_block0_trace_by_scan(stderr_lines: list, scans: list) -> dict:
    """TRACE block=0 lines before DUMP_JPEG_COEF_AFTER_SCAN scanK belong to scan K."""
    out = {s: [] for s in scans}
    buf = []
    for line in stderr_lines:
        m = re.match(r"DUMP_JPEG_COEF_AFTER_SCAN scan(\d+)", line)
        if m:
            k = int(m.group(1))
            if k in out:
                out[k] = list(buf)
            buf = []
            continue
        if "TRACE_JPEG_AC_SYMBOLS" in line and "block=0" in line:
            buf.append(line.strip())
    return out


def main() -> int:
    argv = list(sys.argv[1:])
    scans = [4]
    if "--scans" in argv:
        i = argv.index("--scans")
        if i + 1 >= len(argv):
            print("Usage: run_scan_ac_initial_trace_compare.py [--scan N | --scans N,M,...] <file.jpg> [decoder]", file=sys.stderr)
            return 2
        scans = [int(x) for x in argv[i + 1].split(",")]
        argv = argv[:i] + argv[i + 2:]
    elif "--scan" in argv:
        i = argv.index("--scan")
        if i + 1 >= len(argv):
            print("Usage: run_scan_ac_initial_trace_compare.py [--scan N | --scans N,M,...] <file.jpg> [decoder]", file=sys.stderr)
            return 2
        scans = [int(argv[i + 1])]
        argv = argv[:i] + argv[i + 2:]
    if not argv:
        print("Usage: run_scan_ac_initial_trace_compare.py [--scan N | --scans N,M,...] <file.jpg> [decoder]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(argv[0])
    decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 2
    script_dir = os.path.dirname(os.path.abspath(__file__))
    trace_script = os.path.join(script_dir, "trace_scan_ac_initial_block.py")

    max_scan = max(scans) + 2 if scans else 6
    env = os.environ.copy()
    env["TRACE_JPEG_AC_SYMBOLS"] = "1"
    env["TRACE_JPEG_AC_SYMBOLS_SCAN"] = ",".join(str(s) for s in scans)
    env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scan)
    if len(scans) > 1:
        env["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"  # need scan boundaries to split trace by scan
    dec_out = subprocess.run(
        [decoder, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env,
    )
    stderr_text = dec_out.stderr.decode("utf-8", errors="replace")
    stderr_lines = stderr_text.splitlines()
    decoder_by_scan = extract_block0_trace_by_scan(stderr_lines, scans) if len(scans) > 1 else None

    for scan in scans:
        script_out = subprocess.run(
            [sys.executable, trace_script, "--scan", str(scan), jpeg_path],
            capture_output=True,
            text=True,
            cwd=script_dir,
        )
        if script_out.returncode != 0:
            print(f"Script failed for scan {scan}:", script_out.stderr or script_out.stdout, file=sys.stderr)
            return 1
        script_lines = [s.strip() for s in script_out.stdout.strip().splitlines() if s.strip()]

        if len(scans) == 1:
            decoder_lines = []
            for line in stderr_lines:
                if "TRACE_JPEG_AC_SYMBOLS" in line and "block=0" in line:
                    decoder_lines.append(line.strip())
        else:
            decoder_lines = decoder_by_scan.get(scan, [])

        if dec_out.returncode != 0 and not decoder_lines and scan == scans[0]:
            print("Decoder failed and produced no block=0 trace", file=sys.stderr)
            return 1

        if script_lines != decoder_lines:
            print(f"Mismatch (script vs decoder) for scan {scan} block 0:", file=sys.stderr)
            print("Script:", file=sys.stderr)
            for L in script_lines:
                print(" ", L, file=sys.stderr)
            print("Decoder:", file=sys.stderr)
            for L in decoder_lines:
                print(" ", L, file=sys.stderr)
            return 1
        print(f"Match: scan {scan} block 0 {len(script_lines)} symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
