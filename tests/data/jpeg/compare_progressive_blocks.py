#!/usr/bin/env python3
"""
Compare first-MCU coefficient blocks (OUR_SCANn_COMPc_BLOCKb vs REF_SCANn_COMPc_BLOCKb)
after each scan. Runs our decoder and ref with DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1,
extracts block lines, and reports the first differing coefficient (scan, comp, block, nat).

Usage:
  python3 compare_progressive_blocks.py <file.jpg> [our_decoder] [ref_binary]
  python3 compare_progressive_blocks.py --max-scans N <file.jpg> ...

Exit 0: all block lines match.
Exit 1: first diff found or decoder/ref failed.
Exit 2: usage/setup error.
"""
import os
import re
import subprocess
import sys


def extract_block_lines(stderr_text: str, prefix: str) -> list:
    """Return list of lines that start with prefix (OUR_SCAN or REF_SCAN)."""
    lines = []
    for line in stderr_text.splitlines():
        s = line.strip()
        if s.startswith(prefix):
            lines.append(s)
    return lines


def parse_block_line(line: str) -> tuple:
    """Return (scan, comp, block, values) where values is list of 64 ints."""
    # "OUR_SCAN1_COMP0_BLOCK0 v0 v1 ... v63" or "REF_SCAN1_COMP0_BLOCK0 v0 ..."
    parts = line.split()
    if len(parts) < 1 + 64:
        return None
    header = parts[0]
    m = re.match(r"(?:OUR|REF)_SCAN(\d+)_COMP(\d+)_BLOCK(\d+)", header)
    if not m:
        return None
    scan, comp, block = int(m.group(1)), int(m.group(2)), int(m.group(3))
    try:
        values = [int(parts[i + 1]) for i in range(64)]
    except ValueError:
        return None
    return (scan, comp, block, values)


def main() -> int:
    argv = list(sys.argv[1:])
    max_scans = 2  # through scan 1 so we get scan 0 and 1 block dumps
    if "--max-scans" in argv:
        i = argv.index("--max-scans")
        if i + 1 >= len(argv):
            print("Usage: compare_progressive_blocks.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
                  file=sys.stderr)
            return 2
        max_scans = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]

    if not argv:
        print("Usage: compare_progressive_blocks.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
              file=sys.stderr)
        return 2

    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    our_decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_binary = argv[2] if len(argv) >= 3 else os.path.join(script_dir, "dump_jpeg_coef_ref_debug")

    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 2

    env_ours = os.environ.copy()
    env_ours["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    env_ours["DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN"] = "1"
    env_ours["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
    if os.path.isabs(our_decoder) or "/" in our_decoder:
        dec_dir = os.path.dirname(os.path.abspath(our_decoder))
        if "build" in dec_dir or "apps" in dec_dir:
            env_ours["LD_LIBRARY_PATH"] = dec_dir + (
                os.pathsep + env_ours.get("LD_LIBRARY_PATH", "")
            )

    dec_out = subprocess.run(
        [our_decoder, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_ours,
    )
    ours_stderr = dec_out.stderr.decode("utf-8", errors="replace")
    ours_lines = extract_block_lines(ours_stderr, "OUR_SCAN")

    env_ref = os.environ.copy()
    env_ref["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    env_ref["DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN"] = "1"

    ref_out = subprocess.run(
        [ref_binary, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_ref,
        cwd=script_dir,
    )
    if ref_out.returncode != 0:
        print("Ref failed:", ref_out.stderr.decode("utf-8", errors="replace")[:400],
              file=sys.stderr)
        return 1
    ref_stderr = ref_out.stderr.decode("utf-8", errors="replace")
    ref_lines = extract_block_lines(ref_stderr, "REF_SCAN")

    n = min(len(ours_lines), len(ref_lines))
    if n == 0:
        print("No block lines from one or both decoders", file=sys.stderr)
        return 1

    for i in range(n):
        oline, rline = ours_lines[i], ref_lines[i]
        op = parse_block_line(oline)
        rp = parse_block_line(rline)
        if op is None or rp is None:
            print(f"Line {i + 1}: parse failed", file=sys.stderr)
            return 1
        scan_o, comp_o, block_o, vals_o = op
        scan_r, comp_r, block_r, vals_r = rp
        if (scan_o, comp_o, block_o) != (scan_r, comp_r, block_r):
            print(f"Line {i + 1}: header mismatch ours SCAN{scan_o}_COMP{comp_o}_BLOCK{block_o} ref SCAN{scan_r}_COMP{comp_r}_BLOCK{block_r}",
                  file=sys.stderr)
            return 1
        for nat in range(64):
            if vals_o[nat] != vals_r[nat]:
                print(f"First diff: scan={scan_o} comp={comp_o} block={block_o} nat={nat} ours={vals_o[nat]} ref={vals_r[nat]}",
                      file=sys.stderr)
                return 1

    suffix = f" (ours {len(ours_lines)}, ref {len(ref_lines)})" if len(ours_lines) != len(ref_lines) else ""
    print(f"Match: {n} block lines{suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
