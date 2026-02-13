#!/usr/bin/env python3
"""
Trace AC-initial scan N's first block (band [Ss,Se]) from the bitstream.

Uses "last DHT before this scan" (T.81 B.2.4) for table selection to match the
decoder. Prints lines in TRACE_JPEG_AC_SYMBOLS format (block=0 run=X size=Y val=Z k=K
or EOB) so they can be diffed against decoder stderr with TRACE_JPEG_AC_SYMBOLS=1
and TRACE_JPEG_AC_SYMBOLS_SCAN=N.

Usage:
  python3 trace_scan_ac_initial_block.py --scan 5 <file.jpg>
  python3 trace_scan_ac_initial_block.py --scan 5 <file.jpg> 2>/dev/null | sort
"""
import sys
from typing import List, Tuple

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
            min_code[length] = 1
            max_code[length] = 0
        code = (code + count) << 1
    return {"min_code": min_code, "max_code": max_code, "base_index": base_index, "values": vals}


def jpeg_extend(val: int, n: int) -> int:
    if n == 0:
        return 0
    half = 1 << (n - 1)
    if val < half:
        return val - ((1 << n) - 1)
    return val


def extract_scan_data(data: bytearray, start: int) -> Tuple[bytearray, int]:
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


def main(path: str, scan_idx: int) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        print("Not a JPEG", file=sys.stderr)
        return 1

    dht_ac_list: List[Tuple[int, int, dict]] = []  # (position, th, {bits, vals})
    scans: List[dict] = []  # {sos_pos, ss, se, al, ta_list, scan_data}
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

        if marker == MARKER_DHT:
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
                if tc == 1:
                    dht_ac_list.append((pos, th, {"bits": bits, "vals": vals}))
                p += 17 + num_syms
            continue
        if marker == MARKER_SOS and payload_size >= 6:
            ns = data[payload_start]
            if 4 + ns * 2 > payload_size:
                continue
            ss = data[payload_start + 1 + ns * 2]
            se = data[payload_start + 2 + ns * 2]
            ah_al = data[payload_start + 3 + ns * 2]
            ah = ah_al >> 4
            al = ah_al & 0x0F
            is_ac_initial = (ss != 0 or se != 0) and (ah == 0)
            ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append({
                "sos_pos": pos,
                "ss": ss,
                "se": se,
                "al": al,
                "is_ac_initial": is_ac_initial,
                "ta_list": ta_list,
                "scan_data": scan_data,
            })
            i = next_i
            continue
        continue

    if scan_idx < 0 or scan_idx >= len(scans):
        print(f"Scan index {scan_idx} out of range (0..{len(scans)-1})", file=sys.stderr)
        return 1
    sc = scans[scan_idx]
    if not sc["is_ac_initial"]:
        print(f"Scan {scan_idx} is not AC-initial (Ah=0, Ss/Se)", file=sys.stderr)
        return 1
    ss = sc["ss"]
    se = sc["se"]
    al = sc["al"]
    ta = sc["ta_list"][0]
    # Last DHT before this scan (T.81 B.2.4): max position < sos_pos with th == Ta.
    best = None
    for pos, th, tbl in dht_ac_list:
        if pos < sc["sos_pos"] and th == ta:
            if best is None or pos > best[0]:
                best = (pos, th, tbl)
    if best is None:
        print(f"No AC table for scan {scan_idx} Th={ta}", file=sys.stderr)
        return 1
    _, _, ac_info = best
    tbl = build_canonical_table(ac_info["bits"], ac_info["vals"])
    bs = Bitstream(sc["scan_data"])
    k = ss
    while k <= se:
        sym = bs.huff_decode(tbl)
        if sym < 0:
            print(f"Underflow at k={k}", file=sys.stderr)
            return 1
        if sym == 0:
            print("TRACE_JPEG_AC_SYMBOLS block=0 EOB")
            return 0
        run = sym >> 4
        size = sym & 0x0F
        if size == 0:
            if run == 15:
                k += 16
                if k > se:
                    break
                continue
            # EOB run (r,0) r=1..14: consume r bits and stop (block 0 only).
            if 1 <= run <= 14:
                _ = bs.read_bits(run)
                if _ < 0:
                    return 1
            print("TRACE_JPEG_AC_SYMBOLS block=0 EOB")
            return 0
        k += run
        if k > se:
            if size > 0:
                bs.read_bits(size)
            break
        ac = bs.read_bits(size) if size > 0 else 0
        if ac < 0:
            return 1
        ac = jpeg_extend(ac, size)
        val = ac << al
        print(f"TRACE_JPEG_AC_SYMBOLS block=0 run={run} size={size} val={val} k={k}")
        k += 1
    return 0


if __name__ == "__main__":
    argv = sys.argv[1:]
    scan = 5
    if "--scan" in argv:
        idx = argv.index("--scan")
        if idx + 1 >= len(argv):
            print("Usage: trace_scan_ac_initial_block.py [--scan N] <file.jpg>", file=sys.stderr)
            sys.exit(2)
        scan = int(argv[idx + 1])
        argv = argv[:idx] + argv[idx + 2:]
    if len(argv) != 1:
        print("Usage: trace_scan_ac_initial_block.py [--scan N] <file.jpg>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(argv[0], scan) or 0)
