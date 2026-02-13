#!/usr/bin/env python3
"""
Dump scan N bytes from a JPEG file using the same extraction logic as
compare_blocks_after_scans (T.81 B.2.4: entropy until next 0xFF+marker, no trailing 0xFF).

Output format matches decoder: SCANn_BYTES <len> <hex>

Usage:
  python3 dump_scan_bytes.py <file.jpg> [scan_index ...]
  # Default: dump scans 0, 1, 2. Compare to decoder with:
  DUMP_JPEG_SCAN0_BYTES=1 DUMP_JPEG_SCAN1_BYTES=1 DUMP_JPEG_SCAN2_BYTES=1 ./dump_jpeg_raster <file> 2>&1 | grep SCAN
"""
import sys

# Reuse exact extraction from compare_blocks_after_scans
script_dir = __file__ and __file__.rsplit("/", 1)[0] or "."
if script_dir not in sys.path:
    sys.path.insert(0, script_dir)

from compare_blocks_after_scans import (  # noqa: E402
    MARKER_SOS,
    MARKER_EOI,
    MARKER_DHT,
    MARKER_SOF0,
    MARKER_SOF1,
    MARKER_SOF2,
    MARKER_RST_LO,
    MARKER_RST_HI,
    marker_has_no_length,
    extract_scan_data,
)


def parse_and_dump_scan_bytes(data: bytearray, scan_indices: list) -> None:
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        print("Not a JPEG (no SOI)", file=sys.stderr)
        return
    scans = []
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        pos = i
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

        if marker in (MARKER_SOF0, MARKER_SOF1, MARKER_SOF2):
            continue
        if marker == MARKER_DHT:
            continue
        if marker == MARKER_SOS and payload_size >= 6:
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append(scan_data)
            i = next_i
            continue
        continue

    for idx in scan_indices:
        if idx < len(scans):
            b = scans[idx]
            hex_str = b.hex()
            if len(b) > 512:
                hex_str = b[:512].hex()
            print(f"SCAN{idx}_BYTES {len(b)} {hex_str}")
        else:
            print(f"SCAN{idx}_BYTES 0 (no such scan)", file=sys.stderr)


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: dump_scan_bytes.py <file.jpg> [scan_index ...]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        with open(path, "rb") as f:
            data = bytearray(f.read())
    except OSError as e:
        print(f"Open {path}: {e}", file=sys.stderr)
        return 2
    scan_indices = [0, 1, 2]
    if len(sys.argv) > 2:
        scan_indices = []
        for a in sys.argv[2:]:
            try:
                scan_indices.append(int(a))
            except ValueError:
                print(f"Invalid scan index: {a}", file=sys.stderr)
                return 2
    parse_and_dump_scan_bytes(data, scan_indices)
    return 0


if __name__ == "__main__":
    sys.exit(main())
