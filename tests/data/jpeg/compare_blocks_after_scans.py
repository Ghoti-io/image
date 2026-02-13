#!/usr/bin/env python3
"""
Build reference coefficient block after scans 2, 3, 4 from the bitstream (Python decode),
then compare to our decoder's DUMP_JPEG_COEF_AFTER_SCAN blocks to find the first scan
where we diverge.

Only supports 8x8 single-component (e.g. progressive_8x8_gray.jpg). Uses the same
bitstream rules as verify_first_scan_dc, verify_first_ac_initial, verify_second_ac_initial,
and verify_ac_refine_first_block.

Usage:
  python3 compare_blocks_after_scans.py <file.jpg> [path-to-dump_jpeg_raster]

Requires decoder for "our" blocks (DUMP_JPEG_COEF_AFTER_SCAN=1, GIMG_JPEG_PROGRESSIVE_MAX_SCANS=3,4,5).
"""
import os
import re
import subprocess
import sys
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


def build_canonical_dc_table(bits: List[int], vals: List[int]) -> dict:
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
            # T.81 B.2.4: 0xFF that starts the next marker is not scan entropy.
            return (out, i - 1)
        if marker_has_no_length(m):
            return (out, i - 1)
        # DHT or other length marker: do not append 0xFF to current scan.
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

    def _skip_after_ff(self) -> None:
        """T.81 B.2.2: after passing 0xFF, skip 0x00 (stuffing), RST, or length-based marker. Match decoder jpeg_bitstream_skip_after_ff."""
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
            self.byte_off += 1  # skip marker byte
            if self.byte_off + 2 > self.size:
                break
            seg_len = (self.data[self.byte_off] << 8) | self.data[self.byte_off + 1]
            if seg_len < 2 or self.byte_off + seg_len > self.size:
                self.byte_off -= 1
                break
            self.byte_off += seg_len

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
                self._skip_after_ff()
        return bit

    def read_bits(self, n: int) -> int:
        v = 0
        for _ in range(n):
            b = self.read_bit()
            if b < 0:
                return -1
            v = (v << 1) | b
        return v

    def bit_position(self) -> Tuple[int, int]:
        """Return (byte_off, bit_off) for diagnostic (e.g. where EOB was decoded)."""
        return (self.byte_off, self.bit_off)

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


def parse_sof(payload: bytes) -> Optional[dict]:
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
        comps.append({"id": ci, "h": hv >> 4, "v": hv & 0x0F, "tq": tq})
    return {
        "precision": payload[0],
        "height": (payload[1] << 8) | payload[2],
        "width": (payload[3] << 8) | payload[4],
        "nf": nf,
        "comps": comps,
    }


def parse_jpeg_dht_and_scans(
    data: bytearray,
) -> Optional[Tuple[List[Tuple[int, int, int, list, list]], List[dict]]]:
    """Parse markers only; return (dht_list, scans_info) or None.
    dht_list = [(pos, tc, th, bits, vals), ...]. scans_info[i] = {"sos_start", "ta_list", "scan_data"}."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    dht_list: List[Tuple[int, int, int, list, list]] = []
    scans_info: List[dict] = []
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
                dht_list.append((pos, tc, th, bits, vals))
                p += 17 + num_syms
            continue
        if marker == MARKER_SOS and payload_size >= 6:
            ns = data[payload_start]
            if 4 + ns * 2 <= payload_size:
                ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
                scan_data_start = payload_start + payload_size
                scan_data, next_i = extract_scan_data(data, scan_data_start)
                scans_info.append({"sos_start": pos, "ta_list": ta_list, "scan_data": scan_data})
                i = next_i
            continue
    return (dht_list, scans_info)


def get_scan_data_by_index(data: bytearray, scan_index: int) -> Optional[bytearray]:
    """Return raw scan data for the (scan_index+1)-th SOS (0-based), or None.
    Used by reverse_search to get scan 3 bytes without full decode."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    i = 2
    sos_count = -1
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
            sos_count += 1
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            if sos_count == scan_index:
                return scan_data
            i = next_i
            continue
    return None


def decode_ref_blocks(
    data: bytearray,
    min_scans: int = 5,
) -> Optional[Tuple[List[int], ...]]:
    """Decode first block from bitstream; return (block_after_2, block_after_3, block_after_4[, block_after_5]) or None.
    When min_scans=3, only scans 0..2 are required and return is (block_after_2,)."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    sof = None
    dht_list: List[Tuple[int, int, int, list, list]] = []  # (pos, tc, th, bits, vals)
    scans: List[dict] = []
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

        if marker in (MARKER_SOF0, MARKER_SOF1, MARKER_SOF2) and payload_size >= 8:
            sof = parse_sof(data[payload_start : payload_start + payload_size])
            continue
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
                dht_list.append((pos, tc, th, bits, vals))
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
            # Td = DC table index (high nibble), Ta = AC table index (low nibble) per T.81 SOS
            td_list = [(data[payload_start + 2 + c * 2] >> 4) & 0x0F for c in range(ns)]
            ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append({
                "ss": ss, "se": se, "ah": ah, "al": al,
                "td_list": td_list, "ta_list": ta_list, "scan_data": scan_data,
                "sos_start": pos, "scan_data_end": next_i,
            })
            if os.environ.get("DEBUG_SOS_PARSE") == "1":
                sys.stderr.write("DEBUG_SOS_PARSE scan %d: Ss=%s Se=%s Ah=%s Al=%s\n" % (len(scans) - 1, ss, se, ah, al))
            i = next_i
            continue
        continue

    if sof is None or sof["nf"] != 1 or sof["width"] != 8 or sof["height"] != 8:
        return None
    if len(scans) < min_scans:
        return None

    first_sos_offset = None
    for j in range(len(data) - 1):
        if data[j] == 0xFF and data[j + 1] == MARKER_SOS:
            first_sos_offset = j
            break
    if first_sos_offset is None:
        return None

    dc_by_th: Dict[int, dict] = {}
    ac_by_th: Dict[int, dict] = {}
    for pos, tc, th, bits, vals in dht_list:
        if pos >= first_sos_offset:
            break
        if tc == 0:
            dc_by_th[th] = {"bits": bits, "vals": vals}
        else:
            ac_by_th[th] = {"bits": bits, "vals": vals}

    def first_dht_after(after_pos: int, tc: int, th: int) -> Optional[dict]:
        for p, tcc, thh, bits, vals in dht_list:
            if p >= after_pos and tcc == tc and thh == th:
                return {"bits": bits, "vals": vals}
        return None

    def last_dht_before(before_pos: int, tc: int, th: int) -> Optional[dict]:
        cand = None
        best_pos = -1
        for p, tcc, thh, bits, vals in dht_list:
            if p < before_pos and p > best_pos and tcc == tc and thh == th:
                best_pos = p
                cand = {"bits": bits, "vals": vals}
        return cand

    def first_dht_after_refine(after_pos: int, th: int) -> Optional[dict]:
        """AC refinement uses 17-symbol table (T.81 Table K.6). Prefer that when Ah!=0."""
        for p, tcc, thh, bits, vals in dht_list:
            if p >= after_pos and tcc == 1 and thh == th and len(vals) == 17:
                return {"bits": bits, "vals": vals}
        return None

    def last_dht_before_refine(before_pos: int, th: int) -> Optional[dict]:
        cand = None
        best_pos = -1
        for p, tcc, thh, bits, vals in dht_list:
            if p < before_pos and p > best_pos and tcc == 1 and thh == th and len(vals) == 17:
                best_pos = p
                cand = {"bits": bits, "vals": vals}
        return cand

    # T.81 Table K.6: default AC refinement (17 symbols). Same as jpeg_std_bits_ac_refine / jpeg_std_vals_ac_refine.
    STD_K6_AC_REFINE_BITS = [0, 0, 1, 0, 2, 14] + [0] * 10
    STD_K6_AC_REFINE_VALS = [
        0x00, 0x01, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
        0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0,
    ]

    block = [0] * 64

    # Scan 0: DC (use Td = DC table index from SOS)
    sc0 = scans[0]
    if sc0["ss"] != 0 or sc0["se"] != 0:
        return None
    td0 = sc0["td_list"][0]
    dc_tbl = dc_by_th.get(td0)
    if not dc_tbl:
        return None
    dc_tbl = build_canonical_dc_table(dc_tbl["bits"], dc_tbl["vals"])
    bs0 = Bitstream(sc0["scan_data"])
    sym = bs0.huff_decode(dc_tbl)
    if sym < 0:
        return None
    diff = 0
    if sym > 0:
        diff = bs0.read_bits(sym)
        if diff < 0:
            return None
        diff = jpeg_extend(diff, sym)
    al0 = sc0["al"]
    block[0] = diff << al0

    # Scan 1: AC 1-5
    sc1 = scans[1]
    if not (sc1["ss"] == 1 and sc1["se"] >= 5 and sc1["ah"] == 0):
        return None
    ta1 = sc1["ta_list"][0]
    ac1 = first_dht_after(scans[0]["scan_data_end"], 1, ta1)
    if not ac1:
        ac1 = last_dht_before(scans[1]["sos_start"], 1, ta1) or ac_by_th.get(ta1)
    if not ac1:
        return None
    tbl1 = build_canonical_table(ac1["bits"], ac1["vals"])
    bs1 = Bitstream(sc1["scan_data"])
    k = 1
    while k <= 5:
        sym = bs1.huff_decode(tbl1)
        if sym < 0:
            return None
        if sym == 0:
            if os.environ.get("TRACE_AC") == "1":
                sys.stdout.write("TRACE_JPEG_AC_SYMBOLS EOB\n")
            break
        run = sym >> 4
        size = sym & 0x0F
        # T.81: only (0,0)=EOB and (15,0)=ZRL have size 0. No (r,0) for r 1..14.
        if size == 0:
            if run != 15:
                return None  # Invalid (r,0)
            k += 16
            if k > 5:
                for i in range(k - 16, 6):
                    block[i] = 0
                break
            continue
        k += run
        if k > 5:
            if size > 0:
                bs1.read_bits(size)
            break
        ac_val = 0
        if size > 0:
            ac_val = bs1.read_bits(size)
            if ac_val < 0:
                return None
            ac_val = jpeg_extend(ac_val, size)
        val = ac_val << sc1["al"]
        block[k] = val
        if os.environ.get("TRACE_AC") == "1":
            sys.stdout.write("TRACE_JPEG_AC_SYMBOLS run=%d size=%d val=%d k=%d\n" % (run, size, val, k))
        k += 1

    # If only 2 scans (DC + AC 1-5), return block after scan 1 for 2-scan comparison.
    if len(scans) == 2 and min_scans <= 2:
        return (list(block),)

    # Scan 2: AC 6-63
    sc2 = scans[2]
    if not (sc2["ss"] == 6 and sc2["se"] == 63 and sc2["ah"] == 0):
        return None
    ta2 = sc2["ta_list"][0]
    # Use table in effect at start of scan 2 (last DHT before this SOS); matches loader snapshot.
    ac2 = last_dht_before(scans[2]["sos_start"], 1, ta2) or first_dht_after(
        scans[1]["scan_data_end"], 1, ta2
    )
    if not ac2:
        return None
    tbl2 = build_canonical_table(ac2["bits"], ac2["vals"])
    bs2 = Bitstream(sc2["scan_data"])
    k = 6
    nz_count_6_63 = 0
    while k <= 63:
        sym = bs2.huff_decode(tbl2)
        if sym < 0:
            return None
        if sym == 0:
            if os.environ.get("TRACE_AC") == "1":
                sys.stdout.write("TRACE_JPEG_AC_SYMBOLS EOB\n")
            if os.environ.get("TRACE_SCAN2_EOB") == "1":
                by, bo = bs2.bit_position()
                next3 = []
                for _ in range(3):
                    b = bs2.read_bit()
                    next3.append(b if b >= 0 else "?")
                sys.stdout.write(
                    "TRACE_SCAN2_EOB coeff_count_6_63=%d at byte=%d bit=%d next_3_bits=%s\n"
                    % (nz_count_6_63, by, bo, "".join(str(x) for x in next3))
                )
                sys.stdout.flush()
            break
        run = sym >> 4
        size = sym & 0x0F
        if size == 0:
            if run != 15:
                return None  # Invalid (r,0)
            k += 16
            if k > 63:
                for i in range(k - 16, 64):
                    block[i] = 0
                break
            continue
        k += run
        if k > 63:
            if size > 0:
                bs2.read_bits(size)
            break
        ac_val = 0
        if size > 0:
            ac_val = bs2.read_bits(size)
            if ac_val < 0:
                return None
            ac_val = jpeg_extend(ac_val, size)
        val = ac_val << sc2["al"]
        block[k] = val
        nz_count_6_63 += 1
        if os.environ.get("TRACE_AC") == "1":
            sys.stdout.write("TRACE_JPEG_AC_SYMBOLS run=%d size=%d val=%d k=%d\n" % (run, size, val, k))
        k += 1

    ref_after_2 = list(block)
    if len(scans) <= 3:
        return (ref_after_2,)

    def apply_refine(block_in: List[int], sc: dict) -> Optional[List[int]]:
        blk = list(block_in)
        ss, se = sc["ss"], sc["se"]
        # T.81 Annex G: refinement sets the bit at position Al (0-based). Reference uses Al.
        al = sc["al"]
        bitpos = al if 0 <= al <= 15 else 0
        ta = sc["ta_list"][0]
        after_prev = scans[scans.index(sc) - 1]["scan_data_end"]
        # Prefer 17-symbol AC refinement table (T.81 K.6) when present; else same AC table as initial.
        ref_tbl = first_dht_after_refine(after_prev, ta) or first_dht_after(after_prev, 1, ta)
        if not ref_tbl:
            ref_tbl = last_dht_before_refine(sc["sos_start"], ta) or last_dht_before(sc["sos_start"], 1, ta)
        # Diagnostic: try 10-symbol AC table for first AC refinement (scan 3) to test DHT hypothesis.
        if (
            os.environ.get("USE_AC_REFINE_10SYMBOL_SCAN3") == "1"
            and sc.get("ss") == 1
            and sc.get("se") == 63
            and sc.get("ah", 0) != 0
        ):
            best_10 = None
            best_p = -1
            for p, tcc, thh, bits, vals in dht_list:
                if tcc == 1 and thh == ta and len(vals) == 10 and p < sc["sos_start"] and p > best_p:
                    best_p = p
                    best_10 = {"bits": bits, "vals": vals}
            if best_10 is not None:
                ref_tbl = best_10
        # Diagnostic: try T.81 Table K.6 (default 17-symbol refinement) for first AC refinement.
        if (
            os.environ.get("USE_AC_REFINE_STD_K6_SCAN3") == "1"
            and sc.get("ss") == 1
            and sc.get("se") == 63
            and sc.get("ah", 0) != 0
        ):
            ref_tbl = {"bits": STD_K6_AC_REFINE_BITS, "vals": STD_K6_AC_REFINE_VALS}
        if not ref_tbl:
            if os.environ.get("DEBUG_REFINE") == "1":
                sys.stderr.write("apply_refine: no DHT for tc=1 th=%d\n" % ta)
            return None
        tbl = build_canonical_table(ref_tbl["bits"], ref_tbl["vals"])
        trace_scan3 = (
            os.environ.get("TRACE_REFINE_SCAN3") == "1"
            and sc.get("ss") == 1
            and sc.get("se") == 63
            and sc.get("ah", 0) != 0
        )
        debug_scan3_ss6 = (
            os.environ.get("DEBUG_SCAN3_SS6") == "1"
            and sc.get("ss") == 6
            and sc.get("se") == 63
            and sc.get("ah", 0) != 0
        )
        if trace_scan3:
            sys.stdout.write("Scan 3 (first AC refinement) DHT: %d symbols\n" % len(ref_tbl["vals"]))
        # T.81 Annex G / libjpeg: interleaved [run code][refinement bit]. For each
        # (run, size=1): read one bit for the new coeff, then advance (skip run
        # zeros; for each already-nonzero read one bit). Set new coeff at position we land on.
        bs = Bitstream(sc["scan_data"])
        k = ss
        _first_sym_logged = False
        _count_refine = os.environ.get("COUNT_REFINE") == "1"
        _n_sym = _n_corr = 0
        _trace_first_5 = os.environ.get("TRACE_FIRST_5_REFINE") == "1" and sc.get("ss") == 1 and sc.get("se") == 63
        while k <= se:
            sym = bs.huff_decode(tbl)
            if debug_scan3_ss6 and not _first_sym_logged and sym >= 0 and (sym >> 4) != 15:
                run0, size0 = sym >> 4, sym & 15
                sys.stderr.write("DEBUG_SCAN3_SS6 first sym run=%d size=%d k=%d blk[6]=%s\n" % (run0, size0, k, blk[6] if ss <= 6 <= se else "n/a"))
                _first_sym_logged = True
            if sym < 0:
                if os.environ.get("DEBUG_REFINE") == "1":
                    sys.stderr.write("apply_refine: huff_decode failed at k=%d (underflow)\n" % k)
                return None
            if sym == 0:
                if _count_refine:
                    sys.stderr.write("COUNT_REFINE %d (run,1) symbols %d correction bits\n" % (_n_sym, _n_corr))
                if trace_scan3:
                    sys.stdout.write("Scan 3 EOB\n")
                if os.environ.get("TRACE_AC") == "1":
                    sys.stdout.write("TRACE_JPEG_AC_REFINE EOB\n")
                break
            run = sym >> 4
            size = sym & 15
            run_decoded = run  # for trace before advance modifies run
            if size != 0:
                if _count_refine:
                    _n_sym += 1
                bit = bs.read_bit()
                if _trace_first_5 and _n_sym <= 5:
                    _corr_before = _n_corr
                if bit < 0:
                    if os.environ.get("DEBUG_REFINE") == "1":
                        sys.stderr.write("apply_refine: read_bit failed at k=%d\n" % k)
                    return None
                while run >= 0 and k <= se:
                    if os.environ.get("DEBUG_REFINE_FIRST") == "1" and run == 0 and k == ss and sym >= 0:
                        sys.stderr.write("DEBUG_REFINE_FIRST run=0 k=%d (ss=%d) blk[k]=%s\n" % (k, ss, blk[k]))
                    if blk[k] == 0:
                        run -= 1
                        if run < 0:
                            if debug_scan3_ss6 and k == 6:
                                sys.stderr.write("DEBUG_SCAN3_SS6 break at k=6 (zero), no correction bit read\n")
                            break
                    else:
                        if debug_scan3_ss6 and k == 6:
                            sys.stderr.write("DEBUG_SCAN3_SS6 reading correction bit for k=6 (already nz=%s)\n" % blk[k])
                        if _count_refine:
                            _n_corr += 1
                        rbit = bs.read_bit()
                        if rbit < 0:
                            if os.environ.get("DEBUG_REFINE") == "1":
                                sys.stderr.write("apply_refine: read_bit (already nz) failed at k=%d\n" % k)
                            return None
                        if trace_scan3:
                            sys.stdout.write("  k=%d (already nz) bit=%d\n" % (k, rbit & 1))
                        if os.environ.get("TRACE_AC") == "1":
                            sys.stdout.write("TRACE_JPEG_AC_REFINE k=%d (already nz) bit=%d\n" % (k, rbit & 1))
                        # Correction bit 1 = add 1 at Al; 0 = leave unchanged (T.81).
                        if rbit & 1:
                            c = blk[k]
                            mag = abs(c)
                            if (mag & (1 << bitpos)) == 0:
                                mag |= 1 << bitpos
                                blk[k] = -mag if c < 0 else mag
                    k += 1
                if k > se:
                    if os.environ.get("DEBUG_REFINE") == "1":
                        sys.stderr.write("apply_refine: k>se after advance k=%d se=%d\n" % (k, se))
                    return None
                if _trace_first_5 and _n_sym <= 5:
                    sys.stderr.write("TRACE_FIRST_5 sym%d run=%d size=1 k_land=%d corr_bits=%d\n" % (_n_sym, run_decoded, k, _n_corr - _corr_before))
                c = blk[k]
                if trace_scan3:
                    sys.stdout.write("  k=%d bit=%d was_zero=%s\n" % (k, bit & 1, (c == 0)))
                if os.environ.get("TRACE_AC") == "1":
                    sys.stdout.write("TRACE_JPEG_AC_REFINE k=%d bit=%d (bitpos=%d)\n" % (k, bit & 1, bitpos))
                # Newly nonzero: refinement bit is the sign (1 = +, 0 = −); magnitude at Al is 1.
                val = (1 << bitpos) if (bit & 1) else -(1 << bitpos)
                blk[k] = val
                k += 1
            else:
                if run != 15:
                    if os.environ.get("DEBUG_REFINE") == "1":
                        sys.stderr.write("apply_refine: invalid (run,0) run=%d\n" % run)
                    return None
                left = 16
                while left > 0 and k <= se:
                    if blk[k] == 0:
                        left -= 1
                    k += 1
        return blk

    ref_after_3 = apply_refine(ref_after_2, scans[3])
    if ref_after_3 is None:
        return None
    if len(scans) < 5:
        return (ref_after_2, ref_after_3, None, None)

    # Scan 4: DC (ss=0, se=0) or AC refinement
    sc4 = scans[4]
    if sc4["ss"] == 0 and sc4["se"] == 0:
        if sc4["ah"] != 0:
            # DC refinement: one raw bit per block (no Huffman).
            # Match libjpeg-turbo (jdphuff decode_mcu_DC_refine): block[0] |= (bit << Al).
            bs4 = Bitstream(sc4["scan_data"])
            bit = bs4.read_bit()
            if bit < 0:
                return None
            al4 = sc4["al"]
            ref_after_4 = list(ref_after_3)
            ref_after_4[0] = ref_after_3[0] | ((bit & 1) << al4)
            if os.environ.get("EXPECT_SCAN4_DC") == "1":
                sys.stdout.write("EXPECT_SCAN4_DC_REFINE dc_before=%d bit=%d dc_after=%d\n" % (ref_after_3[0], bit & 1, ref_after_4[0]))
                sys.stdout.flush()
        else:
            # DC initial (again): decode diff, add to predictor, block[0] = pred << al
            td4 = sc4["td_list"][0]
            dc_tbl = first_dht_after(scans[3]["scan_data_end"], 0, td4) or last_dht_before(sc4["sos_start"], 0, td4)
            if not dc_tbl:
                return None
            dc_tbl = build_canonical_dc_table(dc_tbl["bits"], dc_tbl["vals"])
            bs4 = Bitstream(sc4["scan_data"])
            sym = bs4.huff_decode(dc_tbl)
            if sym < 0:
                return None
            diff = 0
            if sym > 0:
                diff = bs4.read_bits(sym)
                if diff < 0:
                    return None
                diff = jpeg_extend(diff, sym)
            dc_pred = ref_after_3[0]  # predictor = previous block[0]
            al4 = sc4["al"]
            ref_after_4 = list(ref_after_3)
            new_dc = dc_pred + (diff << al4)
            ref_after_4[0] = new_dc
    else:
        ref_after_4 = apply_refine(ref_after_3, scans[4])
        if ref_after_4 is None:
            return None

    # Scan 5: AC refinement (for full-block comparison and pixel hash debugging)
    if len(scans) < 6:
        return (ref_after_2, ref_after_3, ref_after_4, None)
    ref_after_5 = apply_refine(ref_after_4, scans[5])
    if ref_after_5 is None:
        return (ref_after_2, ref_after_3, ref_after_4, None)
    return (ref_after_2, ref_after_3, ref_after_4, ref_after_5)


def get_our_blocks(decoder: str, jpeg_path: str, env: dict, through_scan5: bool = False) -> Optional[Tuple]:
    """Run decoder with MAX_SCANS=3,4,5[,6]; return (block_after_2, 3, 4[, 5])."""
    blocks_by_scan: Dict[int, List[int]] = {}
    max_list = (3, 4, 5, 6) if through_scan5 else (3, 4, 5)
    for max_scans in max_list:
        e = env.copy()
        e["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
        e["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
        r = subprocess.run(
            [decoder, jpeg_path],
            capture_output=True,
            timeout=10,
            env=e,
        )
        # Parse stderr even on failure (e.g. scan 5 underflow) so we still get blocks 0..4.
        stderr = (r.stderr or b"").decode("utf-8", errors="replace")
        for m in re.finditer(r"DUMP_JPEG_COEF_AFTER_SCAN scan(\d+) block:\s*((?:-?\d+\s+)+)", stderr):
            scan_num = int(m.group(1))
            vals = [int(x) for x in m.group(2).split()]
            if len(vals) == 64:
                blocks_by_scan[scan_num] = vals
    if 2 not in blocks_by_scan or 3 not in blocks_by_scan or 4 not in blocks_by_scan:
        return None
    if through_scan5 and 5 not in blocks_by_scan:
        return (blocks_by_scan[2], blocks_by_scan[3], blocks_by_scan[4], None)
    if through_scan5:
        return (blocks_by_scan[2], blocks_by_scan[3], blocks_by_scan[4], blocks_by_scan[5])
    return (blocks_by_scan[2], blocks_by_scan[3], blocks_by_scan[4])


def main() -> int:
    trace_ac_only = "--trace-ac" in sys.argv
    if trace_ac_only:
        sys.argv = [a for a in sys.argv if a != "--trace-ac"]
        os.environ["TRACE_AC"] = "1"
    if len(sys.argv) < 2:
        print("Usage: compare_blocks_after_scans.py <file.jpg> [path-to-dump_jpeg_raster]", file=sys.stderr)
        print("  --trace-ac  emit TRACE_JPEG_AC_SYMBOLS/TRACE_JPEG_AC_REFINE for first block then exit", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) >= 3:
        decoder = os.path.abspath(sys.argv[2])
    else:
        decoder = os.path.abspath(os.path.join(script_dir, "..", "..", "..", "build", "linux", "release", "apps", "dump_jpeg_raster"))
    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2
    if not os.path.isfile(decoder):
        print(f"Decoder not found: {decoder}", file=sys.stderr)
        return 2

    with open(jpeg_path, "rb") as f:
        data = bytearray(f.read())

    refs = decode_ref_blocks(data)
    if refs is None:
        print("Could not build reference blocks (need 8x8 single-component, 5+ scans)", file=sys.stderr)
        return 1
    if trace_ac_only:
        return 0
    ref_2, ref_3, ref_4 = refs[0], refs[1], refs[2]
    ref_5 = refs[3] if len(refs) > 3 else None

    env = os.environ.copy()
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]
    image_dir = os.path.dirname(os.path.dirname(os.path.dirname(script_dir)))
    if "LD_LIBRARY_PATH" not in env and os.path.isdir(os.path.join(image_dir, "build", "linux", "release", "apps")):
        env["LD_LIBRARY_PATH"] = os.path.join(image_dir, "build", "linux", "release", "apps")

    ours = get_our_blocks(decoder, jpeg_path, env, through_scan5=(ref_5 is not None))
    if ours is None:
        print("Could not get our decoder blocks (run with DUMP_JPEG_COEF_AFTER_SCAN)", file=sys.stderr)
        return 1
    our_2, our_3, our_4 = ours[0], ours[1], ours[2]
    our_5 = ours[3] if len(ours) > 3 else None

    def diff_count(a: List[int], b: List[int]) -> Tuple[int, List[Tuple[int, int, int]]]:
        diffs = [(i, a[i], b[i]) for i in range(64) if a[i] != b[i]]
        return (len(diffs), diffs)

    comparisons = [("scan 2", ref_2, our_2), ("scan 3", ref_3, our_3), ("scan 4", ref_4, our_4)]
    if ref_5 is not None and our_5 is not None:
        comparisons.append(("scan 5", ref_5, our_5))

    for name, ref_blk, our_blk in comparisons:
        n, diffs = diff_count(ref_blk, our_blk)
        status = "MATCH" if n == 0 else f"{n}/64 DIFFER"
        print(f"After {name}: {status}")
        if diffs and len(diffs) <= 20:
            for i, r, o in diffs:
                print(f"  [zigzag {i}] ref={r} ours={o}")
        elif diffs:
            for i, r, o in diffs[:12]:
                print(f"  [zigzag {i}] ref={r} ours={o}")
            print(f"  ... and {len(diffs) - 12} more")

    if ref_2 != our_2:
        print("\nFirst divergence: after scan 2 (AC 6-63 initial).")
        return 1
    if ref_3 != our_3:
        print("\nFirst divergence: after scan 3 (first AC refinement).")
        return 1
    if ref_4 != our_4:
        print("\nFirst divergence: after scan 4 (DC refinement).")
        return 1
    if ref_5 is not None and our_5 is not None and ref_5 != our_5:
        print("\nFirst divergence: after scan 5 (second AC refinement).")
        return 1
    through = "scan 5" if (ref_5 is not None and our_5 is not None) else "scan 4"
    print(f"\nAll blocks match through {through}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
