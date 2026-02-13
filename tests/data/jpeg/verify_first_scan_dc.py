#!/usr/bin/env python3
"""
Verify first progressive scan DC decode against the bitstream.

Parses the JPEG file to (1) collect DHT DC tables before first SOS,
(2) parse first SOS (components, Td, Al), (3) extract scan 0 data using
the same logic as the loader (T.81 B.2.4: entropy until next SOS/EOI,
including 0xFF stuffing and inter-scan marker segments), (4) build
canonical Huffman decode tables, (5) simulate the bitstream (MSB-first,
skip 0xFF markers/stuffing per T.81 B.2.2), (6) decode the first MCU's
DC coefficients with IJG extend and (diff << Al).

Outputs lines in the same format as TRACE_JPEG_DC_SYMBOLS so they can
be compared to decoder stderr when run with TRACE_JPEG_DC_SYMBOLS=1.

Usage:
  python3 verify_first_scan_dc.py <file.jpg>
  # Compare to: decode with TRACE_JPEG_DC_SYMBOLS=1 and diff the lines.
"""

import sys
from typing import List, Optional, Tuple

# Marker bytes (after 0xFF)
MARKER_SOI = 0xD8
MARKER_EOI = 0xD9
MARKER_DHT = 0xC4
MARKER_DRI = 0xDD
MARKER_SOS = 0xDA
MARKER_RST_LO, MARKER_RST_HI = 0xD0, 0xD7


def marker_has_no_length(m: int) -> bool:
    if m == MARKER_SOI or m == MARKER_EOI:
        return True
    if MARKER_RST_LO <= m <= MARKER_RST_HI:
        return True
    return False


def parse_sof_payload(payload: bytearray) -> Optional[dict]:
    """SOF payload: P(1), Y(2), X(2), Nf(1), then per component Ci(1), HiVi(1), Tqi(1)."""
    if len(payload) < 8:
        return None
    nf = payload[5]
    if len(payload) < 6 + nf * 3:
        return None
    comps = []
    for c in range(nf):
        ci = payload[6 + c * 3]
        hv = payload[7 + c * 3]
        tq = payload[8 + c * 3]
        comps.append({
            "id": ci,
            "h": hv >> 4,
            "v": hv & 0x0F,
            "tq": tq,
        })
    return {
        "precision": payload[0],
        "height": (payload[1] << 8) | payload[2],
        "width": (payload[3] << 8) | payload[4],
        "nf": nf,
        "comps": comps,
    }


def build_canonical_dc_table(bits: List[int], vals: List[int]) -> dict:
    """Build decode table: for each len 1..16, min_code, max_code, base_index, values slice."""
    code = 0
    base = 0
    min_code = [0] * 17
    max_code = [0] * 17
    base_index = [0] * 17
    values = list(vals)
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
            base_index[length] = 0
        code = (code + count) << 1
    return {
        "min_code": min_code,
        "max_code": max_code,
        "base_index": base_index,
        "values": values,
    }


def jpeg_extend(val: int, n: int) -> int:
    """Match decoder (jpeg_entropy.c) and IJG/libjpeg: V < 2^(n-1) -> negative (V - (2^n - 1)); else V."""
    if n == 0:
        return 0
    half = 1 << (n - 1)
    if val < half:
        return val - ((1 << n) - 1)
    return val


class Bitstream:
    """MSB-first bitstream over scan data; skips 0xFF markers/stuffing like the C decoder."""

    def __init__(self, data: bytearray):
        self.data = data
        self.byte_off = 0
        self.bit_off = 0
        self.size = len(data)

    def skip_marker_at_ff(self) -> bool:
        """If at 0xFF and next is a marker (not 0x00/0xFF), skip and return True."""
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
        """After having consumed a 0xFF byte: skip stuffing (0x00), RST byte, or full segment."""
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
        """Return 0 or 1, or -1 on underflow."""
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
        """Read n bits MSB-first; return value or -1 on underflow."""
        v = 0
        for _ in range(n):
            b = self.read_bit()
            if b < 0:
                return -1
            v = (v << 1) | b
        return v

    def huff_decode(self, tbl: dict) -> int:
        """Decode one symbol; return symbol or -1."""
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


def extract_scan0_data_after_sos(data: bytearray, scan0_start: int, sos_payload_len: int) -> bytearray:
    """
    From file bytes, extract the first scan's data exactly as the loader does.
    scan0_start = index of first byte of SOS segment (0xFF). SOS segment is
    2 (0xFF 0xDA) + 2 (length) + (4 + 2*Ns) payload. So scan data starts at
    scan0_start + 2 + 2 + (4 + 2*Ns) = scan0_start + 8 + 2*Ns.
    We don't have Ns here; caller passes sos_payload_len = 4 + 2*Ns (payload only).
    So data start = scan0_start + 2 + 2 + sos_payload_len = scan0_start + 4 + sos_payload_len.
    """
    i = scan0_start + 2 + 2 + sos_payload_len  # past 0xFF, marker, length, payload
    out = bytearray()
    while i < len(data):
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
            break
        if marker_has_no_length(m):
            out.append(0xFF)
            break
        # Segment with length (DHT, etc.): per T.81 B.2.4 the 0xFF is the start
        # of the marker, not entropy; do not append. End scan data here.
        break
    return out


def main(path: str, dump_scan0_bytes: bool = False) -> int:
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < 2 or data[0] != 0xFF or data[1] != MARKER_SOI:
        print("Not a JPEG (no SOI)", file=sys.stderr)
        return 1

    i = 2
    dc_tables_by_th: dict = {}  # Th -> {bits, vals} then we build canonical
    sof: Optional[dict] = None
    first_sos_offset: Optional[int] = None
    sos_ns: int = 0
    sos_comp_td: List[int] = []
    sos_al: int = 0
    sos_payload_len: int = 0
    restart_interval: int = 0  # 0 = no restart

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

        if marker in (0xC0, 0xC1, 0xC2):  # SOF0/SOF1/SOF2
            if payload_size >= 8:
                sof = parse_sof_payload(data[payload_start:payload_start + payload_size])
            continue
        if marker == 0xDD:  # DRI
            if payload_size >= 2:
                restart_interval = (data[payload_start] << 8) | data[payload_start + 1]
            continue
        if marker == 0xC4:  # DHT
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
                if tc == 0:  # DC
                    dc_tables_by_th[th] = {"bits": bits, "vals": vals}
                p += 17 + num_syms
            continue
        if marker == MARKER_SOS:
            if first_sos_offset is None and payload_size >= 6:
                first_sos_offset = payload_start - 4  # start of 0xFF
                ns = data[payload_start]
                sos_ns = ns
                if 4 + ns * 2 > payload_size:
                    break
                sos_payload_len = 4 + ns * 2
                for c in range(ns):
                    td = (data[payload_start + 2 + c * 2] >> 4) & 0x0F
                    sos_comp_td.append(td)
                ah_al = data[payload_start + 3 + ns * 2]
                sos_al = ah_al & 0x0F
            break  # stop parsing after first SOS for our purpose
        # other markers: already advanced i
        continue

    if sof is None or first_sos_offset is None or sos_ns == 0:
        print("No SOF or first SOS found", file=sys.stderr)
        return 1

    scan0_data = extract_scan0_data_after_sos(data, first_sos_offset, sos_payload_len)
    if dump_scan0_bytes:
        print(f"SCAN0_BYTES {len(scan0_data)} {scan0_data.hex()}")
        return 0

    # Build canonical DC tables for Th we need
    dc_tbls = {}
    for th, raw in dc_tables_by_th.items():
        dc_tbls[th] = build_canonical_dc_table(raw["bits"], raw["vals"])

    for idx in range(sos_ns):
        td = sos_comp_td[idx]
        if td not in dc_tbls:
            print(f"Missing DC table Th={td} for scan component {idx}", file=sys.stderr)
            return 1

    bs = Bitstream(scan0_data)

    # MCU dimensions from SOF (same as decoder)
    width = sof["width"]
    height = sof["height"]
    nf = sof["nf"]
    max_h = max(sof["comps"][c]["h"] for c in range(nf))
    max_v = max(sof["comps"][c]["v"] for c in range(nf))
    mcu_per_row = (width + 8 * max_h - 1) // (8 * max_h)
    mcu_per_col = (height + 8 * max_v - 1) // (8 * max_v)
    num_mcus = mcu_per_row * mcu_per_col

    comp_names = ["Y", "Cb", "Cr"]
    dc_pred = [0] * nf
    for mcu_id in range(num_mcus):
        if restart_interval > 0 and mcu_id > 0 and (mcu_id % restart_interval) == 0:
            dc_pred = [0] * nf
        for s in range(sos_ns):
            cs = data[first_sos_offset + 4 + 1 + s * 2]
            comp_idx = None
            for c in range(nf):
                if sof["comps"][c]["id"] == cs:
                    comp_idx = c
                    break
            if comp_idx is None:
                print(f"Scan comp Cs={cs} not in SOF", file=sys.stderr)
                return 1
            h = sof["comps"][comp_idx]["h"]
            v = sof["comps"][comp_idx]["v"]
            td = sos_comp_td[s]
            tbl = dc_tbls[td]
            comp_name = comp_names[comp_idx] if comp_idx < 3 else f"C{comp_idx}"
            for by in range(v):
                for bx in range(h):
                    blk = by * h + bx
                    sym = bs.huff_decode(tbl)
                    if sym < 0:
                        print(f"Underflow at MCU {mcu_id} {comp_name}{blk}", file=sys.stderr)
                        return 1
                    nbits = sym
                    diff = 0
                    if nbits > 0:
                        diff = bs.read_bits(nbits)
                        if diff < 0:
                            print(f"Underflow reading {nbits} bits", file=sys.stderr)
                            return 1
                        diff = jpeg_extend(diff, nbits)
                    dc_pred[comp_idx] += diff << sos_al
                    dc = dc_pred[comp_idx]
                    # Format matches decoder: TRACE_JPEG_DC_SYMBOLS scanN mcu=N Y0 sym= ...
                    print(f"TRACE_JPEG_DC_SYMBOLS scan0 mcu={mcu_id} {comp_name}{blk} sym={sym} diff={diff} dc={dc}")

    return 0


if __name__ == "__main__":
    dump_scan0_bytes = False
    path = None
    for arg in sys.argv[1:]:
        if arg in ("--dump-scan0-bytes", "-d"):
            dump_scan0_bytes = True
        else:
            path = arg
    if not path:
        print("Usage: python3 verify_first_scan_dc.py [--dump-scan0-bytes] <file.jpg>", file=sys.stderr)
        sys.exit(1)
    sys.exit(main(path, dump_scan0_bytes) or 0)
