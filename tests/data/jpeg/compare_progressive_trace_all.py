#!/usr/bin/env python3
"""
Compare our decoder's full trace (GIMG_JPEG_TRACE_ALL=1) to libjpeg (ref).

Runs:
- Our decoder with GIMG_JPEG_TRACE_ALL=1; captures full stderr.
- Ref (dump_jpeg_coef_ref_debug) with TRACE_REF_AC=1 and
  DUMP_JPEG_COEF_AFTER_SCAN=1, DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1 so we get
  AC_INITIAL lines and first-MCU block dumps (REF_SCANn_COMPc_BLOCKb) after each scan.

Compares:
1. AC_INITIAL lines (canonical format) — line-by-line diff, same as compare_progressive_trace.py.
2. First-MCU block coefficients after each scan: our BLOCK_AFTER lines (last occurrence per
   block in first MCU for that scan) vs ref's REF_SCANn_COMPc_BLOCKb. Reports first mismatch.

Requires:
- Our decoder (e.g. dump_jpeg_raster) built and on PATH or passed as arg.
- Ref binary dump_jpeg_coef_ref_debug linked against instrumented libjpeg-turbo (see
  compare_progressive_trace.py or tests/data/jpeg/README.md).

Usage:
  python3 compare_progressive_trace_all.py <file.jpg> [our_decoder] [ref_binary]
  python3 compare_progressive_trace_all.py --max-scans N <file.jpg> ...

Exit 0: AC_INITIAL and block coeffs match.
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


# BLOCK_AFTER scan%u mcu=(%u,%u) comp=%u block=%zu byte_off=%zu bit_off=%u coeffs: v0 v1 ... v63
BLOCK_AFTER_RE = re.compile(
    r"BLOCK_AFTER scan(\d+) mcu=\((\d+),(\d+)\) comp=(\d+) block=(\d+) "
    r"byte_off=\d+ bit_off=\d+ coeffs:\s*(.*)"
)


def parse_block_after_line(line: str):
    """Parse BLOCK_AFTER line; return (scan, mcu_x, mcu_y, comp, block, coeffs) or None."""
    m = BLOCK_AFTER_RE.match(line.strip())
    if not m:
        return None
    scan, mcu_x, mcu_y, comp, block, coeff_str = m.groups()
    coeffs = [int(x) for x in coeff_str.split()]
    if len(coeffs) != 64:
        return None
    return (int(scan), int(mcu_x), int(mcu_y), int(comp), int(block), coeffs)


def collect_ours_blocks_by_scan(stderr_text: str) -> dict:
    """From our trace, collect first-MCU (mcu=(0,0)) BLOCK_AFTER per scan.
    Returns dict: scan_idx -> list of (comp, block, coeffs) in decode order."""
    by_scan = {}
    for line in stderr_text.splitlines():
        t = parse_block_after_line(line)
        if t is None:
            continue
        scan, mcu_x, mcu_y, comp, block, coeffs = t
        if mcu_x != 0 or mcu_y != 0:
            continue
        if scan not in by_scan:
            by_scan[scan] = []
        # Append; we get one per block decode so order is correct. Later scans
        # overwrite same (comp,block) for same scan if we had multiple MCUs - but
        # we only trace first MCU, so we get exactly one per (scan, comp, block).
        by_scan[scan].append((comp, block, coeffs))
    return by_scan


# REF_SCANn_COMPc_BLOCKb v0 v1 ... v63  (e.g. REF_SCAN0_COMP0_BLOCK0 1 2 3 ...)
REF_BLOCK_RE = re.compile(r"REF_SCAN(\d+)_COMP(\d+)_BLOCK(\d+)\s+(.*)")


def parse_ref_block_line(line: str):
    """Parse REF_SCANn_COMPc_BLOCKb line; return (scan, comp, block, coeffs) or None."""
    m = REF_BLOCK_RE.match(line.strip())
    if not m:
        return None
    scan, comp, block, coeff_str = m.groups()
    coeffs = [int(x) for x in coeff_str.split()]
    if len(coeffs) != 64:
        return None
    return (int(scan), int(comp), int(block), coeffs)


def collect_ref_blocks_by_scan(stderr_text: str) -> dict:
    """From ref trace, collect REF_SCANn_COMPc_BLOCKb per scan.
    Returns dict: scan_idx -> list of (comp, block, coeffs) in ref order."""
    by_scan = {}
    for line in stderr_text.splitlines():
        t = parse_ref_block_line(line)
        if t is None:
            continue
        scan, comp, block, coeffs = t
        if scan not in by_scan:
            by_scan[scan] = []
        by_scan[scan].append((comp, block, coeffs))
    return by_scan


def compare_blocks(ours_blocks: dict, ref_blocks: dict) -> str | None:
    """Compare block coefficients for scans we decoded (ours_blocks).
    Ref dumps all first-MCU blocks after each scan; we only trace blocks decoded
    in that scan. So for each (scan, comp, block) in ours, find matching ref and compare."""
    ref_by_key = {}
    for scan, lst in ref_blocks.items():
        for (c, b, coeffs) in lst:
            ref_by_key[(scan, c, b)] = coeffs
    for scan in sorted(ours_blocks.keys()):
        for (oc, ob, ocoeffs) in ours_blocks[scan]:
            key = (scan, oc, ob)
            if key not in ref_by_key:
                return f"Scan {scan} comp={oc} block={ob}: ref has no block"
            rcoeffs = ref_by_key[key]
            if ocoeffs != rcoeffs:
                for k in range(64):
                    if ocoeffs[k] != rcoeffs[k]:
                        return (
                            f"Scan {scan} comp={oc} block={ob} nat={k}: "
                            f"ours={ocoeffs[k]} ref={rcoeffs[k]}"
                        )
    return None


def main() -> int:
    argv = list(sys.argv[1:])
    max_scans = 10
    if "--max-scans" in argv:
        i = argv.index("--max-scans")
        if i + 1 >= len(argv):
            print(
                "Usage: compare_progressive_trace_all.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
                file=sys.stderr,
            )
            return 2
        max_scans = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]

    if not argv:
        print(
            "Usage: compare_progressive_trace_all.py [--max-scans N] <file.jpg> [our_decoder] [ref_binary]",
            file=sys.stderr,
        )
        return 2

    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    image_dir = os.path.dirname(os.path.dirname(os.path.dirname(script_dir)))
    our_decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_binary = argv[2] if len(argv) >= 3 else os.path.join(
        script_dir, "dump_jpeg_coef_ref_debug"
    )

    if not os.path.isfile(jpeg_path):
        print(f"Not a file: {jpeg_path}", file=sys.stderr)
        return 2

    # Run our decoder with full trace
    env_ours = os.environ.copy()
    if os.path.isabs(our_decoder) or "/" in our_decoder:
        dec_dir = os.path.dirname(os.path.abspath(our_decoder))
        if "build" in dec_dir or "apps" in dec_dir:
            env_ours["LD_LIBRARY_PATH"] = dec_dir + (
                os.pathsep + env_ours.get("LD_LIBRARY_PATH", "")
            )
    env_ours["GIMG_JPEG_TRACE_ALL"] = "1"
    env_ours["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
    # Emit canonical AC_INITIAL lines for comparison with ref (same as compare_progressive_trace.py)
    env_ours["TRACE_AC_COMPARE"] = "1"
    env_ours["TRACE_JPEG_AC_SYMBOLS"] = "1"
    env_ours["TRACE_JPEG_AC_SYMBOLS_SCAN"] = "1,2,3,4,5,6,7,8,9,10"

    dec_out = subprocess.run(
        [our_decoder, jpeg_path],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_ours,
    )
    ours_stderr = dec_out.stderr.decode("utf-8", errors="replace")

    # Run ref with TRACE_REF_AC, block dumps, and Huffman bit trace (for comparison with our HUFF_BIT/HUFF_MATCH)
    env_ref = os.environ.copy()
    env_ref["TRACE_REF_AC"] = "1"
    env_ref["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    env_ref["DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN"] = "1"
    env_ref["LIBJPEG_TRACE_HUFF_BITS"] = "1"
    libjpeg_debug = os.path.join(image_dir, "third_party", "libjpeg-debug")
    libjpeg_lib = os.path.join(libjpeg_debug, "lib")
    if os.path.isdir(libjpeg_debug):
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
        print(
            "Ref (dump_jpeg_coef_ref_debug) failed:",
            ref_out.stderr.decode("utf-8", errors="replace")[:500],
            file=sys.stderr,
        )
        return 1
    ref_stderr = ref_out.stderr.decode("utf-8", errors="replace")

    # 1) Compare AC_INITIAL lines (same as compare_progressive_trace.py).
    # We may limit scans (--max-scans), so ref can have more lines; we require
    # ref has at least ours and the first len(ours_ac) lines match.
    ours_ac = extract_ac_initial_lines(ours_stderr)
    ref_ac = extract_ac_initial_lines(ref_stderr)
    if len(ref_ac) < len(ours_ac):
        print(
            f"AC_INITIAL: ref has fewer lines ({len(ref_ac)}) than ours ({len(ours_ac)})",
            file=sys.stderr,
        )
        return 1
    for i in range(len(ours_ac)):
        if ours_ac[i] != ref_ac[i]:
            print(f"AC_INITIAL mismatch at line {i + 1}:", file=sys.stderr)
            print(f"  Ours: {ours_ac[i]}", file=sys.stderr)
            print(f"  Ref:  {ref_ac[i]}", file=sys.stderr)
            return 1

    # 2) Compare first-MCU block coefficients after each scan
    ours_blocks = collect_ours_blocks_by_scan(ours_stderr)
    ref_blocks = collect_ref_blocks_by_scan(ref_stderr)
    err = compare_blocks(ours_blocks, ref_blocks)
    if err is not None:
        print(f"Block coeff mismatch: {err}", file=sys.stderr)
        return 1

    print(
        f"Match: {len(ours_ac)} AC_INITIAL lines, "
        f"{sum(len(v) for v in ours_blocks.values())} blocks (first MCU per scan)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
