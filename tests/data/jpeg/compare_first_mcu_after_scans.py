#!/usr/bin/env python3
"""
Compare our decoder's first-MCU coefficients to libjpeg after N scans by using
truncated JPEG files. Creates files that contain only the first N scans (through
end of scan N-1 entropy data + EOI), then runs our decoder with
GIMG_JPEG_PROGRESSIVE_MAX_SCANS=N and ref (full decode on the truncated file).
Thus "ref final" = state after N scans. Reports diff count per N to find the
first scan where we diverge.

T.81 B.2.4: scan entropy data runs until the next 0xFF that starts a marker;
we truncate at that 0xFF and append EOI (0xFF 0xD9).

Usage:
  LD_LIBRARY_PATH=build/linux/release/apps python3 compare_first_mcu_after_scans.py <file.jpg> [decoder] [ref_tool]
  python3 compare_first_mcu_after_scans.py --create-only <file.jpg> [output_dir]

  --create-only: only write truncated files (progressive_sample_2scan.jpg etc.), do not run compare.
Exit 0 if all compared scans match; 1 if any diff; 2 on setup error.
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oracle_path import BUILD_HINT, find_oracle
from typing import Dict, List, Optional, Tuple

MARKER_SOS = 0xDA
MARKER_EOI = 0xD9
MARKER_DHT = 0xC4
MARKER_SOF0, MARKER_SOF1, MARKER_SOF2 = 0xC0, 0xC1, 0xC2
MARKER_RST_LO, MARKER_RST_HI = 0xD0, 0xD7


def marker_has_no_length(m: int) -> bool:
    if m in (0xD8, 0xD9):
        return True
    if MARKER_RST_LO <= m <= MARKER_RST_HI:
        return True
    return False


def extract_scan_data(data: bytearray, start: int) -> Tuple[bytearray, int]:
    """Return (scan_bytes, next_i) where next_i is index of 0xFF starting next marker (T.81 B.2.4)."""
    i = start
    out = bytearray()
    while i < len(data):
        b = data[i]
        i += 1
        if b != 0xFF:
            out.append(b)
            continue
        if i >= len(data):
            out.append(0xFF)
            return (out, i)
        m = data[i]
        if m == 0x00:
            i += 1
            out.append(0xFF)
            out.append(0x00)
            continue
        if MARKER_RST_LO <= m <= MARKER_RST_HI:
            i += 1
            out.append(0xFF)
            out.append(m)
            continue
        if m == MARKER_SOS or m == MARKER_EOI:
            return (out, i - 1)
        if marker_has_no_length(m):
            return (out, i - 1)
        return (out, i - 1)
    return (out, len(data))


def get_scan_boundaries(data: bytearray) -> Optional[List[int]]:
    """Return list of scan_data_end indices (0xFF of next marker) for each SOS, or None."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    ends: List[int] = []
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker == MARKER_EOI:
            break
        if marker == 0x00 or (MARKER_RST_LO <= marker <= MARKER_RST_HI):
            continue
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        payload_start = i
        payload_size = length - 2
        i += payload_size
        if marker == MARKER_SOS and payload_size >= 6:
            scan_data_start = payload_start + payload_size
            _, next_i = extract_scan_data(data, scan_data_start)
            ends.append(next_i)
            i = next_i
            continue
        continue
    return ends if ends else None


def parse_our_lines(stderr: str) -> Dict[Tuple[int, int], List[int]]:
    out: Dict[Tuple[int, int], List[int]] = {}
    for m in re.finditer(r"OUR_COMP(\d+)_BLOCK(\d+) (.+)", stderr):
        c, b = int(m.group(1)), int(m.group(2))
        vals = [int(x) for x in m.group(3).split()]
        if len(vals) == 64:
            out[(c, b)] = vals
    return out


def parse_ref_lines(stdout: str) -> Dict[Tuple[int, int], List[int]]:
    out: Dict[Tuple[int, int], List[int]] = {}
    for line in stdout.splitlines():
        m = re.match(r"REF_COMP(\d+)_BLOCK(\d+) (.+)", line)
        if m:
            c, b = int(m.group(1)), int(m.group(2))
            vals = [int(x) for x in m.group(3).split()]
            if len(vals) == 64:
                out[(c, b)] = vals
    return out


def main() -> int:
    argv = sys.argv[1:]
    create_only = False
    if argv and argv[0] == "--create-only":
        create_only = True
        argv = argv[1:]
    if len(argv) < 1:
        print(
            "Usage: python3 compare_first_mcu_after_scans.py [--create-only] <file.jpg> [decoder] [ref_tool]",
            file=sys.stderr,
        )
        return 2
    jpeg_path = os.path.abspath(argv[0])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    decoder = argv[1] if len(argv) >= 2 else "dump_jpeg_raster"
    ref_tool = find_oracle("dump_jpeg_coef_ref", argv[2] if len(argv) >= 3 else None)
    out_dir = os.path.dirname(jpeg_path)

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2

    with open(jpeg_path, "rb") as f:
        data = bytearray(f.read())

    boundaries = get_scan_boundaries(data)
    if not boundaries or len(boundaries) < 2:
        print("Could not parse scan boundaries or fewer than 2 scans", file=sys.stderr)
        return 2

    num_scans = len(boundaries)
    if create_only:
        for n in range(2, min(num_scans + 1, 11)):
            end_idx = boundaries[n - 1]
            truncated = bytes(data[:end_idx]) + b"\xff\xd9"
            out_path = os.path.join(out_dir, os.path.splitext(os.path.basename(jpeg_path))[0] + f"_{n}scan.jpg")
            with open(out_path, "wb") as f:
                f.write(truncated)
            print(f"Wrote {out_path} (through scan {n-1} data, {len(truncated)} bytes)")
        return 0

    if not os.path.isfile(decoder):
        print(f"Decoder not found: {decoder}", file=sys.stderr)
        return 2
    if ref_tool is None:
        print(f"Reference tool not found. {BUILD_HINT}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["DUMP_JPEG_COEF_FIRST_MCU"] = "1"
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
    env_ref = os.environ.copy()
    env_ref["DUMP_FIRST_MCU"] = "1"

    any_diff = False
    for n in range(2, min(num_scans + 1, 11)):
        end_idx = boundaries[n - 1]
        truncated = bytes(data[:end_idx]) + b"\xff\xd9"
        out_path = os.path.join(out_dir, os.path.splitext(os.path.basename(jpeg_path))[0] + f"_{n}scan.jpg")
        with open(out_path, "wb") as f:
            f.write(truncated)

        env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(n)
        r = subprocess.run(
            [decoder, out_path],
            capture_output=True,
            timeout=10,
            env=env,
        )
        our = parse_our_lines((r.stderr or b"").decode("utf-8", errors="replace"))
        if not our:
            print(f"  After {n} scans: our decoder produced no first-MCU dump", file=sys.stderr)
            any_diff = True
            continue

        r2 = subprocess.run(
            [ref_tool, out_path],
            capture_output=True,
            text=True,
            timeout=10,
            env=env_ref,
        )
        if r2.returncode != 0:
            print(f"  After {n} scans: ref failed ({r2.stderr or r2.returncode})", file=sys.stderr)
            any_diff = True
            continue
        ref = parse_ref_lines(r2.stdout)
        if not ref:
            print(f"  After {n} scans: ref produced no REF_COMP* lines", file=sys.stderr)
            any_diff = True
            continue

        if set(our) != set(ref):
            print(f"  After {n} scans: block set mismatch", file=sys.stderr)
            any_diff = True
            continue
        ndiff = 0
        for (c, b) in sorted(our):
            for i in range(64):
                if our[(c, b)][i] != ref[(c, b)][i]:
                    ndiff += 1
        if ndiff:
            print(f"  After {n} scans: {ndiff} coefficient(s) differ")
            any_diff = True
        else:
            print(f"  After {n} scans: match")

    return 1 if any_diff else 0


if __name__ == "__main__":
    sys.exit(main())
