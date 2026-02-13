#!/usr/bin/env python3
"""
Verify first AC refinement scan's first block against the bitstream.

Finds the first scan with Ah!=0 and (Ss!=0 or Se!=0) (AC refinement), extracts
its scan data and the 17-symbol DHT (Tc=1, 17 values), gets the coefficient
block after the previous scans (from decoder with GIMG_JPEG_PROGRESSIVE_MAX_SCANS=
scan_idx), decodes the refinement bits (run of zeros + one bit per non-zero),
and prints TRACE_JPEG_AC_REFINE lines for comparison with decoder stderr.

Usage:
  python3 verify_ac_refine_first_block.py <file.jpg> [path-to-dump_jpeg_raster]
  python3 verify_ac_refine_first_block.py --scan 1 <file.jpg> [path-to-dump_jpeg_raster]

  --scan N: verify the (N+1)th AC refinement scan (default 0 = first).

Requires decoder to get block state after previous scans (DUMP_JPEG_COEF_AFTER_SCAN=1,
GIMG_JPEG_PROGRESSIVE_MAX_SCANS=scan_idx). If decoder path not given, uses
dump_jpeg_raster from same dir or env.
"""
import os
import re
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

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


def extract_scan_data(data: bytearray, start: int) -> Tuple[bytearray, int]:
    """Extract scan data from start until next SOS/EOI; do not append 0xFF for DHT."""
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
        # DHT or other length marker: do not append 0xFF.
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


def get_block_after_scans(decoder: str, jpeg_path: str, max_scans: int, env: dict) -> List[int]:
    """Run decoder with MAX_SCANS=max_scans, parse last DUMP_JPEG_COEF_AFTER_SCAN block (zigzag 64)."""
    e = env.copy()
    e["DUMP_JPEG_COEF_AFTER_SCAN"] = "1"
    e["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = str(max_scans)
    r = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=e,
    )
    if r.returncode != 0:
        raise SystemExit(f"Decoder failed: {r.stderr.decode() or r.returncode}")
    stderr = (r.stderr or b"").decode("utf-8", errors="replace")
    blocks = re.findall(r"DUMP_JPEG_COEF_AFTER_SCAN scan\d+ block:\s*((?:-?\d+\s+)+)", stderr)
    if not blocks:
        raise SystemExit("Decoder produced no DUMP_JPEG_COEF_AFTER_SCAN block")
    vals = [int(x) for x in blocks[-1].split()]
    if len(vals) != 64:
        raise SystemExit(f"Block has {len(vals)} values, expected 64")
    return vals


def main(jpeg_path: str, decoder: Optional[str] = None, refine_scan_index: int = 0) -> int:
    with open(jpeg_path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:
        print("Not a JPEG", file=sys.stderr)
        return 1

    ac_refine_by_th: Dict[int, dict] = {}
    ac_by_th: Dict[int, dict] = {}
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
                    if num_syms == 17:
                        ac_refine_by_th[th] = {"bits": bits, "vals": vals}
                    else:
                        ac_by_th[th] = {"bits": bits, "vals": vals}
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
            ta_list = [data[payload_start + 2 + c * 2] & 0x0F for c in range(ns)]
            scan_data_start = payload_start + payload_size
            scan_data, next_i = extract_scan_data(data, scan_data_start)
            scans.append({
                "scan_idx": len(scans),
                "ss": ss,
                "se": se,
                "ah": ah,
                "ta_list": ta_list,
                "scan_data": scan_data,
                "ac_refine_tables": dict(ac_refine_by_th),
                "ac_tables": dict(ac_by_th),
            })
            i = next_i
            continue
        continue

    ac_refine_indices = [
        idx for idx, sc in enumerate(scans)
        if sc["ah"] != 0 and (sc["ss"] != 0 or sc["se"] != 0)
    ]
    if not ac_refine_indices:
        print("No AC refinement scan found", file=sys.stderr)
        return 1
    if refine_scan_index >= len(ac_refine_indices):
        print(f"Only {len(ac_refine_indices)} AC refinement scan(s)", file=sys.stderr)
        return 1
    first_ac_refine = ac_refine_indices[refine_scan_index]

    sc = scans[first_ac_refine]
    ss = sc["ss"]
    se = sc["se"]
    ah = sc["ah"]
    bitpos = ah - 1 if 1 <= ah <= 16 else 0
    ta = sc["ta_list"][0]
    if ta in sc["ac_refine_tables"]:
        tbl_src = sc["ac_refine_tables"][ta]
    elif ta in sc["ac_tables"]:
        tbl_src = sc["ac_tables"][ta]
    else:
        print(f"Missing AC table Th={ta} for first component", file=sys.stderr)
        return 1

    if decoder is None:
        decoder = os.path.join(os.path.dirname(os.path.abspath(jpeg_path)), "dump_jpeg_raster")
        if not os.path.isfile(decoder):
            decoder = os.environ.get("GIMG_TEST_DUMP_JPEG_RASTER", "dump_jpeg_raster")
    env = os.environ.copy()
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    block = get_block_after_scans(decoder, os.path.abspath(jpeg_path), first_ac_refine, env)

    tbl = build_canonical_table(tbl_src["bits"], tbl_src["vals"])
    bs = Bitstream(sc["scan_data"])
    k = ss
    while k <= se:
        sym = bs.huff_decode(tbl)
        if sym < 0:
            print(f"Underflow at k={k}", file=sys.stderr)
            return 1
        if sym == 0:
            print("TRACE_JPEG_AC_REFINE EOB")
            break
        run = sym >> 4
        while run > 0 and k <= se:
            if block[k] == 0:
                run -= 1
            k += 1
        if k > se:
            print("Run overrun", file=sys.stderr)
            return 1
        bit = bs.read_bit()
        if bit < 0:
            print(f"Bit underflow at k={k}", file=sys.stderr)
            return 1
        print(f"TRACE_JPEG_AC_REFINE k={k} bit={bit} (bitpos={bitpos})")
        k += 1
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print("Usage: verify_ac_refine_first_block.py [--scan N] <file.jpg> [dump_jpeg_raster]", file=sys.stderr)
        sys.exit(1)
    refine_scan_index = 0
    if args[0] == "--scan":
        if len(args) < 4:
            print("Usage: verify_ac_refine_first_block.py --scan N <file.jpg> [dump_jpeg_raster]", file=sys.stderr)
            sys.exit(1)
        refine_scan_index = int(args[1])
        jpeg_path = os.path.abspath(args[2])
        decoder = args[3] if len(args) >= 4 else None
    else:
        jpeg_path = os.path.abspath(args[0])
        decoder = args[1] if len(args) >= 2 else None
    sys.exit(main(jpeg_path, decoder, refine_scan_index) or 0)
