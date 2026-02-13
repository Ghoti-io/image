#!/usr/bin/env python3
"""
Verify DC-only progressive decode: expected pixels from DCs + DQT vs our decoder dump.

After the first scan (DC-only), each 8×8 block should be a constant value:
  pixel = clamp(((4*DC*quant[0] + 16) >> 5) + 128, 0, 255)
This matches the decoder's jpeg_dequantise + jpeg_idct_8x8_islow DC path.

Usage:
  python3 verify_dc_only_pipeline.py <file.jpg> [dump_dir] [path-to-dump_jpeg_raster]
  Runs decoder with GIMG_JPEG_PROGRESSIVE_MAX_SCANS=1 and DUMP_JPEG_COMPONENTS,
  parses DUMP_JPEG_DC from stderr for the 6 DCs, parses DQT from file for quant[0],
  computes expected Y/Cb/Cr, and compares to the dumped component files.
Exits 0 if match, 1 if mismatch, 2 on setup error.
"""
import os
import struct
import subprocess
import sys


def parse_sof(data: bytearray) -> dict | None:
    """Find first SOF (C0/C1/C2), return {width, height, nf, comps: [{id, h, v, tq}]}."""
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker == 0xD9:
            break
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        if marker in (0xC0, 0xC1, 0xC2) and length >= 8:
            # Payload: P(1), Y(2)=height, X(2)=width, Nf(1), then per comp Ci(1) HiVi(1) Tqi(1)
            nf = data[i + 5]
            if length < 6 + nf * 3:
                i += length - 2
                continue
            comps = []
            for c in range(nf):
                comps.append({
                    "id": data[i + 6 + c * 3],
                    "h": data[i + 7 + c * 3] >> 4,
                    "v": data[i + 7 + c * 3] & 0x0F,
                    "tq": data[i + 8 + c * 3],
                })
            return {
                "width": (data[i + 3] << 8) | data[i + 4],
                "height": (data[i + 1] << 8) | data[i + 2],
                "nf": nf,
                "comps": comps,
            }
        i += length - 2
    return None


def parse_dqt_tables(data: bytearray) -> dict[int, list[int]]:
    """Parse all DQT before first SOS. Return {tq: list of 64 values (zigzag order)}."""
    out = {}
    i = 2
    while i + 2 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker == 0xD9 or marker == 0xDA:  # EOI or SOS: stop before scan
            break
        if i + 2 > len(data):
            break
        length = (data[i] << 8) | data[i + 1]
        i += 2
        if marker == 0xDB and length >= 2 + 1 + 64:
            p = i
            while p + 1 + 64 <= i + length - 2:
                pq_tq = data[p]
                tq = pq_tq & 0x0F
                is_16 = (pq_tq >> 4) != 0
                n = 128 if is_16 else 64
                if p + 1 + n > len(data):
                    break
                if is_16:
                    vals = [
                        (data[p + 1 + j * 2] << 8) | data[p + 1 + j * 2 + 1]
                        for j in range(64)
                    ]
                else:
                    vals = list(data[p + 1 : p + 1 + 64])
                out[tq] = vals
                p += 1 + n
        i += length - 2
    return out


def dc_to_pixel(dc: int, quant0: int) -> int:
    """Expected pixel for DC-only block (matches decoder ISLOW path)."""
    v = ((4 * dc * quant0 + 16) >> 5) + 128
    return max(0, min(255, v))


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: verify_dc_only_pipeline.py <file.jpg> [dump_dir] [decoder]", file=sys.stderr)
        return 2
    jpeg_path = os.path.abspath(sys.argv[1])
    script_dir = os.path.dirname(os.path.abspath(__file__))
    dump_dir = sys.argv[2] if len(sys.argv) >= 3 else None
    decoder = sys.argv[3] if len(sys.argv) >= 4 else "dump_jpeg_raster"
    if dump_dir is None:
        import tempfile
        dump_dir = tempfile.mkdtemp(prefix="jpeg_dc_verify_")
        cleanup = True
    else:
        os.makedirs(dump_dir, exist_ok=True)
        cleanup = False

    if not os.path.isfile(jpeg_path):
        print(f"File not found: {jpeg_path}", file=sys.stderr)
        return 2

    with open(jpeg_path, "rb") as f:
        data = bytearray(f.read())
    sof = parse_sof(data)
    if not sof:
        print("No SOF found", file=sys.stderr)
        return 2
    dqt = parse_dqt_tables(data)
    if not dqt:
        print("No DQT found", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["GIMG_JPEG_PROGRESSIVE_MAX_SCANS"] = "1"
    env["DUMP_JPEG_COMPONENTS"] = dump_dir
    env["DUMP_JPEG_DC"] = "1"
    if os.environ.get("LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    r = subprocess.run(
        [decoder, jpeg_path],
        capture_output=True,
        timeout=10,
        env=env,
    )
    if r.returncode != 0:
        print(f"Decoder failed: {r.stderr.decode(errors='replace')}", file=sys.stderr)
        return 2
    stderr = (r.stderr or b"").decode("utf-8", errors="replace")
    # Parse "DUMP_JPEG_DC scan0: Y0=... Y1=... Y2=... Y3=... Cb0=... Cr0=..."
    dc_y0 = dc_y1 = dc_y2 = dc_y3 = dc_cb = dc_cr = None
    for line in stderr.splitlines():
        if "DUMP_JPEG_DC scan0:" in line:
            # Y0=%d Y1=%d Y2=%d Y3=%d Cb0=%d Cr0=%d
            parts = line.split()
            for p in parts:
                if p.startswith("Y0="):
                    dc_y0 = int(p[3:])
                elif p.startswith("Y1="):
                    dc_y1 = int(p[3:])
                elif p.startswith("Y2="):
                    dc_y2 = int(p[3:])
                elif p.startswith("Y3="):
                    dc_y3 = int(p[3:])
                elif p.startswith("Cb0="):
                    dc_cb = int(p[4:])
                elif p.startswith("Cr0="):
                    dc_cr = int(p[4:])
            break
    if dc_y0 is None or dc_cb is None or dc_cr is None:
        print("Could not parse DUMP_JPEG_DC from decoder stderr", file=sys.stderr)
        return 2

    tq_y = sof["comps"][0]["tq"]
    tq_c = sof["comps"][1]["tq"]
    if tq_y not in dqt or tq_c not in dqt:
        print(f"Missing DQT for Tq={tq_y} or Tq={tq_c}", file=sys.stderr)
        return 2
    q0_y = dqt[tq_y][0]
    q0_c = dqt[tq_c][0]

    def read_raw(path: str) -> tuple[bytes, int, int]:
        with open(path, "rb") as f:
            raw = f.read()
        if len(raw) < 8:
            raise ValueError(f"{path}: too short")
        w, h = struct.unpack("<II", raw[:8])
        return raw[8:], w, h

    our_y, wy, hy = read_raw(os.path.join(dump_dir, "Y.raw"))
    our_cb, wcb, hcb = read_raw(os.path.join(dump_dir, "Cb.raw"))
    our_cr, wcr, hcr = read_raw(os.path.join(dump_dir, "Cr.raw"))

    # Expected: 4 Y blocks (2×2), 1 Cb, 1 Cr. Block (by,bx) for Y: 8×8 region.
    expect_y_blocks = [
        dc_to_pixel(dc_y0, q0_y),
        dc_to_pixel(dc_y1, q0_y),
        dc_to_pixel(dc_y2, q0_y),
        dc_to_pixel(dc_y3, q0_y),
    ]
    expect_cb = dc_to_pixel(dc_cb, q0_c)
    expect_cr = dc_to_pixel(dc_cr, q0_c)

    mismatches = []
    # Y: 16×16, blocks (0,0)=(0,0)-(7,7), (1,0)=(8,0)-(15,7), (0,1)=(0,8)-(7,15), (1,1)=(8,8)-(15,15)
    for by in range(2):
        for bx in range(2):
            idx = by * 2 + bx
            exp = expect_y_blocks[idx]
            for dy in range(8):
                for dx in range(8):
                    y = by * 8 + dy
                    x = bx * 8 + dx
                    got = our_y[y * wy + x]
                    if got != exp:
                        mismatches.append(f"Y ({x},{y}): got {got} expected {exp} (block {idx} DC={[dc_y0,dc_y1,dc_y2,dc_y3][idx]})")
    for y in range(hcb):
        for x in range(wcb):
            if our_cb[y * wcb + x] != expect_cb:
                mismatches.append(f"Cb ({x},{y}): got {our_cb[y*wcb+x]} expected {expect_cb}")
    for y in range(hcr):
        for x in range(wcr):
            if our_cr[y * wcr + x] != expect_cr:
                mismatches.append(f"Cr ({x},{y}): got {our_cr[y*wcr+x]} expected {expect_cr}")

    if cleanup:
        import shutil
        shutil.rmtree(dump_dir, ignore_errors=True)

    if mismatches:
        print("DC-only pipeline mismatch (expected from DC*quant[0] + IDCT formula):", file=sys.stderr)
        for m in mismatches[:20]:
            print(f"  {m}", file=sys.stderr)
        if len(mismatches) > 20:
            print(f"  ... and {len(mismatches) - 20} more", file=sys.stderr)
        return 1
    print("DC-only pipeline OK: decoder output matches expected from DCs + DQT.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
