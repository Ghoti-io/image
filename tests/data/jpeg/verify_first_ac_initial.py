#!/usr/bin/env python3
"""
Verify first AC-initial scan's first block against the bitstream.

Finds the first scan with Ah=0 and (Ss!=0 or Se!=0), extracts its scan data
and DHT AC table (Ta from SOS), decodes the first block's band [Ss,Se] with
the same bitstream/extend as the decoder, and prints TRACE_JPEG_AC_SYMBOLS
lines for comparison with decoder stderr (TRACE_JPEG_AC_SYMBOLS=1).

Usage:
  python3 verify_first_ac_initial.py <file.jpg>
"""
import os
import sys
from typing import List, Optional, Tuple

MARKER_SOS = 0xDA
MARKER_EOI = 0xD9
MARKER_DHT = 0xC4
MARKER_RST_LO, MARKER_RST_HI = 0xD0, 0xD7


def marker_has_no_length(m: int) -> bool:
    if m == 0xD8 or m == 0xD9:
        return True
    if MARKER_RST_LO <= m <= MARKER_RST_HI:
        return True
    return False


def build_canonical_table(bits: List[int], vals: List[int]) -> dict:
    code = 0
    base = 0
    min_code = [0] * 17
    max_code = [0] * 17
    base_index = [0] * 17
    for length in range(1, 17):
        count = bits[length - 1]
        min_code[length] = code
        if count > 0:
            max_code[length] = code + count - 1
            base_index[length] = base
            base += count
        else:
            # No symbols at this length: make range invalid (min > max) so we never match.
            min_code[length] = 1
            max_code[length] = 0
        code = (code + count) << 1
    return {"min_code": min_code, "max_code": max_code, "base_index": base_index, "values": vals}


def jpeg_extend(val: int, n: int) -> int:
    """Match decoder (jpeg_entropy.c) and IJG: V < 2^(n-1) -> negative (V - (2^n - 1)); else V."""
    if n == 0:
        return 0
    half = 1 << (n - 1)
    if val < half:
        return val - ((1 << n) - 1)
    return val


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

    def skip_after_ff(self) -> None:
        while self.byte_off < self.size:
            m = self.data[self.byte_off]
            if m == 0x00:
                self.byte_off += 1
                continue
            if m == 0xFF:
                break
            if MARKER_RST_LO <= m <= MARKER_RST_HI:
                self.byte_off += 1
                continue
            if self.byte_off + 2 > self.size:
                break
            self.byte_off += 1
            seg_len = (self.data[self.byte_off] << 8) | self.data[self.byte_off + 1]
            self.byte_off += 2
            if seg_len >= 2 and self.byte_off + (seg_len - 2) <= self.size:
                self.byte_off += seg_len - 2
            else:
                break

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
            if self.byte_off > 0 and self.byte_off <= self.size and self.data[self.byte_off - 1] == 0xFF:
                self.skip_after_ff()
        return bit

    def read_bits(self, n: int) -> int:
        v = 0
        for _ in range(n):
            b = self.read_bit()
            if b < 0:
                return -1
            v = (v << 1) | b
        return v

    def huff_decode(self, tbl: dict) -> int:
        code = 0
        for length in range(1, 17):
            b = self.read_bit()
            if b < 0:
                return -1
            code = (code << 1) | b
            if code >= tbl["min_code"][length] and code <= tbl["max_code"][length]:
                idx = tbl["base_index"][length] + (code - tbl["min_code"][length])
                return tbl["values"][idx]
        return -1


def extract_scan_data(data: bytearray, start: int) -> Tuple[bytearray, int]:
    """Extract scan data from start until next SOS/EOI; return (data, next_i)."""
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
            out.append(0xFF)
            return (out, i - 1)
        if marker_has_no_length(m):
            out.append(0xFF)
            return (out, i - 1)
        # Marker with length (DHT, etc.): per T.81 B.2.4 the 0xFF is the start
        # of the marker, not entropy; do not append. Match loader (jpeg_load.c).
        return (out, i - 1)
    return (out, len(data))


def main(path: str) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        print("Not a JPEG", file=sys.stderr)
        return 1

    dc_by_th: dict = {}
    ac_by_th: dict = {}
    scans: List[dict] = []
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker == MARKER_EOI:
            break
        if marker == 0x00:
            continue
        if MARKER_RST_LO <= marker <= MARKER_RST_HI:
            continue
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        payload_start = i
        payload_size = length - 2
        i += payload_size

        if marker in (0xC0, 0xC1, 0xC2):
            continue
        if marker == 0xC4:
            p = payload_start
            end = payload_start + payload_size
            while p + 17 <= end:
                tc_th = data[p]
                th = tc_th & 0x0F
                tc = (tc_th >> 4) & 1
                bits = list(data[p + 1 : p + 17])
                num_syms = sum(bits)
                if p + 17 + num_syms > end:
                    break
                vals = list(data[p + 17 : p + 17 + num_syms])
                if tc == 0:
                    dc_by_th[th] = {"bits": bits, "vals": vals}
                else:
                    ac_by_th[th] = {"bits": bits, "vals": vals}
                p += 17 + num_syms
            continue
        if marker == MARKER_SOS:
            if payload_size < 6:
                continue
            ns = data[payload_start]
            if 4 + ns * 2 > payload_size:
                continue
            ss = data[payload_start + 1 + ns * 2]
            se = data[payload_start + 2 + ns * 2]
            ah_al = data[payload_start + 3 + ns * 2]
            ah = ah_al >> 4
            al = ah_al & 0x0F
            is_ac_initial = (ss != 0 or se != 0) and ah == 0
            ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append({
                "ns": ns,
                "ss": ss,
                "se": se,
                "ah": ah,
                "al": al,
                "is_ac_initial": is_ac_initial,
                "ta_list": ta_list,
                "scan_data": scan_data,
                "ac_tables": dict(ac_by_th),
            })
            i = next_i
            continue
        continue

    first_ac_initial = None
    for idx, sc in enumerate(scans):
        if sc["is_ac_initial"]:
            first_ac_initial = idx
            break
    if first_ac_initial is None:
        print("No AC-initial scan found", file=sys.stderr)
        return 1

    sc = scans[first_ac_initial]
    ss = sc["ss"]
    se = sc["se"]
    al = sc["al"]
    ta = sc["ta_list"][0]
    if ta not in sc["ac_tables"]:
        print(f"Missing AC table Th={ta} for first component", file=sys.stderr)
        return 1
    tbl = build_canonical_table(sc["ac_tables"][ta]["bits"], sc["ac_tables"][ta]["vals"])
    if os.environ.get("DUMP_JPEG_AC_TABLE") == "1":
        print(f"DUMP_JPEG_AC_TABLE scan{first_ac_initial} Ta={ta} num_values={len(tbl['values'])}", file=sys.stderr)
        for length in range(1, 17):
            mn = tbl["min_code"][length]
            mx = tbl["max_code"][length]
            count = (mx - mn + 1) if mx >= mn else 0
            base = tbl["base_index"][length]
            vals = tbl["values"]
            vals_slice = vals[base : base + count][:8]
            vals_str = " ".join(f"v[{i}]={v}" for i, v in enumerate(vals_slice))
            if count > 8:
                vals_str += " ..."
            print(f"  len{length:2d} min={mn} max={mx} base={base} {vals_str}", file=sys.stderr)
    bs = Bitstream(sc["scan_data"])

    k = ss
    while k <= se:
        sym = bs.huff_decode(tbl)
        if sym < 0:
            print("Underflow", file=sys.stderr)
            return 1
        if sym == 0:
            print("TRACE_JPEG_AC_SYMBOLS EOB")
            break
        run = sym >> 4
        size = sym & 0x0F
        k += run
        if k > se:
            if size > 0:
                bs.read_bits(size)
            break
        ac = 0
        if size > 0:
            ac = bs.read_bits(size)
            if ac < 0:
                return 1
            ac = jpeg_extend(ac, size)
        val = ac << al
        print(f"TRACE_JPEG_AC_SYMBOLS run={run} size={size} val={val} k={k}")
        k += 1
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python3 verify_first_ac_initial.py <file.jpg>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(sys.argv[1]) or 0)
