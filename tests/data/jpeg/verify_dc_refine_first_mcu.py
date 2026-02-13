#!/usr/bin/env python3
"""
Verify first DC refinement scan's first MCU (one bit per block) against the bitstream.

Finds the first scan with Ss=0, Se=0, Ah!=0 (DC refinement), extracts its scan data,
decodes the first MCU's 6 refinement bits (Y0..Y3, Cb0, Cr0), and prints
TRACE_JPEG_DC_REFINE lines for comparison with decoder stderr (TRACE_JPEG_DC_REFINE=1).

Usage:
  python3 verify_dc_refine_first_mcu.py <file.jpg>
"""
import sys
from typing import List, Optional, Tuple

MARKER_SOS = 0xDA
MARKER_EOI = 0xD9
MARKER_DHT = 0xC4
MARKER_RST_LO, MARKER_RST_HI = 0xD0, 0xD7


def marker_has_no_length(m: int) -> bool:
    if m in (0xD8, 0xD9):
        return True
    if MARKER_RST_LO <= m <= MARKER_RST_HI:
        return True
    return False


def skip_scan_data(data: bytearray, start: int) -> int:
    """Return index of the 0xFF that starts the next marker (SOS/EOI/DHT/...)."""
    i = start
    while i < len(data):
        if i + 1 >= len(data):
            return len(data)
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m == 0x00:
            i += 2
            continue
        if MARKER_RST_LO <= m <= MARKER_RST_HI:
            i += 2
            continue
        return i
    return len(data)


def extract_scan_data_from(
    data: bytearray, start: int
) -> Tuple[bytearray, int]:
    """
    Extract scan data starting at `start` until we hit 0xFF followed by
    SOS or EOI. Include 0xFF and DHT segments. Return (scan_data, next_offset).
    """
    out = bytearray()
    i = start
    while i < len(data):
        if i + 1 > len(data):
            break
        b = data[i]
        i += 1
        if b != 0xFF:
            out.append(b)
            continue
        if i >= len(data):
            out.append(0xFF)
            break
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
            out.append(0xFF)
            return (out, i - 1)  # 0xFF is at i-1; next iteration will see it
        if marker_has_no_length(m):
            out.append(0xFF)
            return (out, i - 1)
        out.append(0xFF)
        i += 1
        if i + 2 > len(data):
            break
        seg_len = (data[i] << 8) | data[i + 1]
        out.append(data[i])
        out.append(data[i + 1])
        i += 2
        if seg_len >= 2 and i + (seg_len - 2) <= len(data):
            for _ in range(seg_len - 2):
                out.append(data[i])
                i += 1
        else:
            break
    return (out, i)


class Bitstream:
    def __init__(self, data: bytearray):
        self.data = data
        self.byte_off = 0
        self.bit_off = 0
        self.size = len(data)

    def skip_marker_at_ff(self) -> bool:
        if self.byte_off >= self.size or self.data[self.byte_off] != 0xFF:
            return False
        if self.byte_off + 1 >= self.size:
            return False
        m = self.data[self.byte_off + 1]
        if m == 0x00 or m == 0xFF:
            return False
        self.byte_off += 2
        if marker_has_no_length(m):
            return True
        if self.byte_off + 2 > self.size:
            self.byte_off -= 2
            return False
        seg_len = (self.data[self.byte_off] << 8) | self.data[self.byte_off + 1]
        if seg_len < 2 or self.byte_off + seg_len > self.size:
            self.byte_off -= 2
            return False
        self.byte_off += seg_len
        return True

    def read_bit(self) -> int:
        while self.byte_off < self.size and self.data[self.byte_off] == 0xFF:
            if self.skip_marker_at_ff():
                continue
            break
        if self.byte_off >= self.size:
            return -1
        b = self.data[self.byte_off]
        bit = (b >> (7 - self.bit_off)) & 1
        self.bit_off += 1
        if self.bit_off == 8:
            self.bit_off = 0
            self.byte_off += 1
        return bit


def parse_sof(data: bytearray) -> Optional[dict]:
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        i += 2
        if m == 0xD9:
            break
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        if m in (0xC0, 0xC1, 0xC2) and length >= 8:
            nf = data[i + 5]
            if length < 6 + nf * 3:
                i += length - 2
                continue
            comps = []
            for c in range(nf):
                comps.append({
                    "h": data[i + 7 + c * 3] >> 4,
                    "v": data[i + 7 + c * 3] & 0x0F,
                })
            return {"nf": nf, "comps": comps}
        i += length - 2
    return None


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        print("Not a JPEG", file=sys.stderr)
        return 1
    sof = parse_sof(data)
    if not sof:
        print("No SOF", file=sys.stderr)
        return 1
    nf = sof["nf"]
    comp_names = ["Y", "Cb", "Cr"]
    # Find first DC refinement scan (Ss=0, Se=0, Ah!=0)
    i = 2
    scan_idx = 0
    dc_refine_scan_idx: Optional[int] = None
    dc_refine_data_start: Optional[int] = None
    dc_refine_ns: int = 0
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        i += 2
        if m == 0xD9:
            break
        if m == 0x00 or (0xD0 <= m <= 0xD7):
            continue
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        if m == MARKER_SOS and length >= 6:
            ns = data[i]
            payload_len = 4 + ns * 2  # Ns + comps + Ss + Se + AhAl
            if length < 2 + payload_len:
                i += length - 2
                scan_idx += 1
                continue
            ss = data[i + 1 + 2 * ns]
            se = data[i + 2 + 2 * ns]
            ah_al = data[i + 3 + 2 * ns]
            ah = ah_al >> 4
            is_dc = (ss == 0 and se == 0)
            if is_dc and ah != 0:
                dc_refine_scan_idx = scan_idx
                dc_refine_data_start = i + payload_len
                dc_refine_ns = ns
                break
            i = skip_scan_data(data, i + payload_len)
            scan_idx += 1
            continue
        # Other marker (DHT, DQT, etc.): skip payload
        if length >= 2:
            i += length - 2
    if dc_refine_scan_idx is None or dc_refine_data_start is None:
        print("No DC refinement scan found", file=sys.stderr)
        return 1
    scan_data, _ = extract_scan_data_from(data, dc_refine_data_start)
    bs = Bitstream(scan_data)
    # First MCU: Ns components, each with h*v blocks, one bit per block
    block_names: List[str] = []
    for s in range(dc_refine_ns):
        h = sof["comps"][s]["h"]
        v = sof["comps"][s]["v"]
        name = comp_names[s] if s < 3 else f"C{s}"
        for by in range(v):
            for bx in range(h):
                block_names.append(f"{name}{by * h + bx}")
    for blk_name in block_names:
        bit = bs.read_bit()
        if bit < 0:
            print(f"Underflow at {blk_name}", file=sys.stderr)
            return 1
        print(f"TRACE_JPEG_DC_REFINE scan{dc_refine_scan_idx} {blk_name} bit={bit}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: verify_dc_refine_first_mcu.py <file.jpg>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1]) or 0)
