#!/usr/bin/env python3
"""
Decode the first 4 blocks of scan 1 (AC initial band 1-5) and optionally the first 6
blocks of scan 2 (AC initial band 6-63) from a progressive JPEG, and print bitstream
(byte_off, bit_off) after each block. Compare to decoder TRACE_JPEG_AC_INITIAL_BITSTREAM
to find desync.

Supports any progressive JPEG (e.g. progressive_sample.jpg 16x16 4:2:0). Uses same
bitstream rules and AC-initial decode (including EOBRUN per T.81 Annex G) as the
decoder and compare_blocks_after_scans.py.

Usage:
  python3 trace_scan1_bitstream_positions.py <file.jpg> [--scan2]
  # Compare to:
  TRACE_JPEG_AC_INITIAL_BITSTREAM=1 dump_jpeg_raster <file.jpg> 2>&1 | grep TRACE_JPEG_AC_INITIAL_BITSTREAM

Output: SCRIPT_AC_INITIAL_BITSTREAM scanN Y0/.../Cr0 byte_off=X bit_off=Y
"""
import sys
from typing import Dict, List, Optional, Tuple

# Reuse Bitstream and helpers from compare_blocks_after_scans (same bitstream rules).
from compare_blocks_after_scans import (
    Bitstream,
    build_canonical_table,
    extract_scan_data,
    jpeg_extend,
    marker_has_no_length,
)

MARKER_SOS = 0xDA
MARKER_DHT = 0xC4
MARKER_EOI = 0xD9
MARKER_RST_LO, MARKER_RST_HI = 0xD0, 0xD7


def parse_scans_and_dht(data: bytearray) -> Optional[Tuple[List[tuple], List[dict]]]:
    """Return (dht_list, scans). dht_list = [(pos, tc, th, bits, vals), ...].
    scans[i] = {sos_start, ss, se, ah, al, ta_list, scan_data, scan_data_end}."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    dht_list: List[tuple] = []
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
        if marker == MARKER_DHT and payload_size >= 17:
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
            ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append({
                "sos_start": pos,
                "ss": ss,
                "se": se,
                "ah": ah,
                "al": al,
                "ta_list": ta_list,
                "scan_data": scan_data,
                "scan_data_end": next_i,
            })
            i = next_i
            continue
    return (dht_list, scans)


def decode_one_ac_initial_block(
    bs: Bitstream,
    tbl: dict,
    ss: int,
    se: int,
    al: int,
    eobrun: List[int],
) -> bool:
    """Decode one block's AC initial band [ss,se]. eobrun is [value] (mutable).
    Return True on success, False on underflow."""
    k = ss
    while k <= se:
        if eobrun[0] > 0:
            eobrun[0] -= 1
            return True
        sym = bs.huff_decode(tbl)
        if sym < 0:
            return False
        if sym == 0:
            return True  # EOB
        run = sym >> 4
        size = sym & 0x0F
        if size == 0:
            if run == 15:
                k += 16
                if k > se:
                    return True
                continue
            if 1 <= run <= 14:
                eobrun[0] = 1 << run
                if run > 0:
                    rbits = bs.read_bits(run)
                    if rbits < 0:
                        return False
                    eobrun[0] += rbits
                return True
            return False
        k += run
        if k > se:
            if size > 0:
                bs.read_bits(size)
            return True
        if size > 0:
            ac_val = bs.read_bits(size)
            if ac_val < 0:
                return False
            ac_val = jpeg_extend(ac_val, size)
        k += 1
    return True


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: python3 trace_scan1_bitstream_positions.py <file.jpg> [--scan2]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    do_scan2 = "--scan2" in sys.argv
    with open(path, "rb") as f:
        data = bytearray(f.read())
    result = parse_scans_and_dht(data)
    if not result or len(result[1]) < 2:
        print("Need at least 2 scans (DC + AC initial)", file=sys.stderr)
        return 1
    dht_list, scans = result
    sc0, sc1 = scans[0], scans[1]
    if sc0["ss"] != 0 or sc0["se"] != 0:
        print("Scan 0 must be DC", file=sys.stderr)
        return 1
    if sc1["ah"] != 0 or sc1["ss"] < 1:
        print("Scan 1 must be AC initial (Ah=0, Ss>=1)", file=sys.stderr)
        return 1
    ss, se, al = sc1["ss"], sc1["se"], sc1["al"]
    ta1 = sc1["ta_list"][0]

    def last_dht_before(before_pos: int, tc: int, th: int) -> Optional[dict]:
        cand = None
        best_pos = -1
        for p, tcc, thh, bits, vals in dht_list:
            if p < before_pos and p > best_pos and tcc == tc and thh == th:
                best_pos = p
                cand = {"bits": bits, "vals": vals}
        return cand

    def first_dht_after(after_pos: int, tc: int, th: int) -> Optional[dict]:
        for p, tcc, thh, bits, vals in dht_list:
            if p >= after_pos and tcc == tc and thh == th:
                return {"bits": bits, "vals": vals}
        return None

    ac1 = first_dht_after(sc0["scan_data_end"], 1, ta1)
    if not ac1:
        ac1 = last_dht_before(sc1["sos_start"], 1, ta1)
    if not ac1:
        print("No AC table for scan 1 (Ta=%d)" % ta1, file=sys.stderr)
        return 1
    tbl = build_canonical_table(ac1["bits"], ac1["vals"])
    bs = Bitstream(sc1["scan_data"])
    eobrun = [0]
    names_scan1 = ["Y0", "Y1", "Y2", "Y3"]
    for block_id in range(4):
        ok = decode_one_ac_initial_block(bs, tbl, ss, se, al, eobrun)
        if not ok:
            print("SCRIPT_AC_INITIAL_BITSTREAM scan1 %s UNDERFLOW" % names_scan1[block_id], file=sys.stderr)
            return 1
        by, bo = bs.bit_position()
        print("SCRIPT_AC_INITIAL_BITSTREAM scan1 %s byte_off=%d bit_off=%d" % (names_scan1[block_id], by, bo))

    if not do_scan2 or len(scans) < 3:
        return 0
    sc2 = scans[2]
    if sc2["ah"] != 0 or sc2["ss"] < 1:
        return 0
    ss2, se2, al2 = sc2["ss"], sc2["se"], sc2["al"]
    ta2 = sc2["ta_list"][0]
    ac2 = first_dht_after(sc1["scan_data_end"], 1, ta2)
    if not ac2:
        ac2 = last_dht_before(sc2["sos_start"], 1, ta2)
    if not ac2:
        return 0
    n_syms2 = len(ac2["vals"])
    print("SCRIPT_SCAN2_AC_TABLE n_syms=%d" % n_syms2, file=sys.stderr)
    tbl2 = build_canonical_table(ac2["bits"], ac2["vals"])
    bs2 = Bitstream(sc2["scan_data"])
    eobrun2 = [0]
    names_scan2 = ["Y0", "Y1", "Y2", "Y3", "Cb0", "Cr0"]
    for block_id in range(6):
        ok = decode_one_ac_initial_block(bs2, tbl2, ss2, se2, al2, eobrun2)
        if not ok:
            print("SCRIPT_AC_INITIAL_BITSTREAM scan2 %s UNDERFLOW" % names_scan2[block_id], file=sys.stderr)
            return 1
        by, bo = bs2.bit_position()
        print("SCRIPT_AC_INITIAL_BITSTREAM scan2 %s byte_off=%d bit_off=%d" % (names_scan2[block_id], by, bo))
    return 0


if __name__ == "__main__":
    sys.exit(main())
