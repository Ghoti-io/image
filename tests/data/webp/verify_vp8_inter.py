#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Image.
"""Check VP8 interframe prediction against a container keyframe.

`cwebp` and `dwebp` run in the pinned oracle image. The interframe
itself is written here: the container tools refuse an interframe
payload, so they cannot be the pixel oracle for that frame. A skipped
zero-motion macroblock must reproduce `dwebp` of the keyframe. An
integer-pel motion vector must match the same YUV shifted with the
edge repeated, then this library's 9-3-3-1 upsample and BT.601
conversion. A fractional vector checks the section 18.3 taps the same
way.

Usage:  python3 tests/data/webp/verify_vp8_inter.py
        GIMG_WEBP_DUMP points at dump_webp_raster (or pass --dump PATH).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))

# Bool-coder renormalization, the same tables the library encoder uses
# so a partition written here is one the RFC reader accepts.
K_NORM = [
    7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 0,
]
K_NEW_RANGE = [
    127, 127, 191, 127, 159, 191, 223, 127, 143, 159, 175, 191, 207, 223, 239,
    127, 135, 143, 151, 159, 167, 175, 183, 191, 199, 207, 215, 223, 231, 239,
    247, 127, 131, 135, 139, 143, 147, 151, 155, 159, 163, 167, 171, 175, 179,
    183, 187, 191, 195, 199, 203, 207, 211, 215, 219, 223, 227, 231, 235, 239,
    243, 247, 251, 127, 129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149,
    151, 153, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179,
    181, 183, 185, 187, 189, 191, 193, 195, 197, 199, 201, 203, 205, 207, 209,
    211, 213, 215, 217, 219, 221, 223, 225, 227, 229, 231, 233, 235, 237, 239,
    241, 243, 245, 247, 249, 251, 253, 127,
]

def s32(value: int) -> int:
    """int32 wrap, matching the library bool writer's value register."""
    value &= 0xffffffff
    if value >= 0x80000000:
        return value - 0x100000000
    return value


MV_UPDATE = [
    [237, 246, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
     250, 250, 252, 254, 254],
    [231, 243, 245, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
     251, 251, 254, 254, 254],
]
MV_DEFAULT = [
    [162, 128, 225, 146, 172, 147, 214, 39, 156, 128, 129, 132, 75, 145,
     178, 206, 239, 254, 254],
    [164, 128, 204, 170, 119, 235, 140, 230, 228, 128, 130, 130, 74, 148,
     180, 203, 236, 254, 254],
]
SMALL_TREE = [2, 8, 4, 6, 0, -1, -2, -3, 10, 12, -4, -5, -6, -7]
MV_REF_TREE = [-7, 2, -5, 4, -6, 6, -8, -9]
MODE_CTX = [
    [7, 1, 1, 143],
    [14, 18, 14, 107],
    [135, 64, 57, 68],
    [60, 56, 128, 65],
    [159, 134, 128, 34],
    [234, 188, 128, 28],
]
BILINEAR = [
    [0, 0, 128, 0, 0, 0],
    [0, 0, 112, 16, 0, 0],
    [0, 0, 96, 32, 0, 0],
    [0, 0, 80, 48, 0, 0],
    [0, 0, 64, 64, 0, 0],
    [0, 0, 48, 80, 0, 0],
    [0, 0, 32, 96, 0, 0],
    [0, 0, 16, 112, 0, 0],
]
BICUBIC = [
    [0, 0, 128, 0, 0, 0],
    [0, -6, 123, 12, -1, 0],
    [2, -11, 108, 36, -8, 1],
    [0, -9, 93, 50, -6, 0],
    [3, -16, 77, 77, -16, 3],
    [0, -6, 50, 93, -9, 0],
    [1, -8, 36, 108, -11, 2],
    [0, -1, 12, 123, -6, 0],
]


class BoolWriter:
    def __init__(self) -> None:
        self.range = 254
        self.value = 0
        self.nb_bits = -8
        self.run = 0
        self.buf = bytearray()

    def _flush(self) -> None:
        shift = 8 + self.nb_bits
        bits = self.value >> shift
        self.value = s32(self.value - (bits << shift))
        self.nb_bits -= 8
        if (bits & 0xff) == 0xff:
            self.run += 1
            return
        if bits & 0x100:
            if self.buf:
                self.buf[-1] = (self.buf[-1] + 1) & 0xff
        if self.run:
            fill = 0x00 if (bits & 0x100) else 0xff
            self.buf.extend([fill] * self.run)
            self.run = 0
        self.buf.append(bits & 0xff)

    def put(self, bit: int, prob: int) -> None:
        split = (self.range * prob) >> 8
        if bit:
            self.value = s32(self.value + split + 1)
            self.range -= split + 1
        else:
            self.range = split
        if self.range < 127:
            shift = K_NORM[self.range]
            self.range = K_NEW_RANGE[self.range]
            self.value = s32(self.value << shift)
            self.nb_bits += shift
            if self.nb_bits > 0:
                self._flush()

    def uniform(self, bit: int) -> None:
        split = self.range >> 1
        if bit:
            self.value = s32(self.value + split + 1)
            self.range -= split + 1
        else:
            self.range = split
        if self.range < 127:
            self.range = K_NEW_RANGE[self.range]
            self.value = s32(self.value << 1)
            self.nb_bits += 1
            if self.nb_bits > 0:
                self._flush()

    def literal(self, value: int, nbits: int) -> None:
        for shift in range(nbits - 1, -1, -1):
            self.uniform((value >> shift) & 1)

    def finish(self) -> bytes:
        self.literal(0, 9 - self.nb_bits)
        self.nb_bits = 0
        self._flush()
        return bytes(self.buf)


def tree_reaches(tree: list[int], node: int, value: int) -> bool:
    if node <= 0:
        return -node == value
    return tree_reaches(tree, tree[node], value) or tree_reaches(
        tree, tree[node + 1], value)


def write_tree(bw: BoolWriter, tree: list[int], probs: list[int],
        value: int) -> None:
    node = 0
    while True:
        left = tree[node]
        right = tree[node + 1]
        if left <= 0 and -left == value:
            bit = 0
        elif right <= 0 and -right == value:
            bit = 1
        elif left > 0 and tree_reaches(tree, left, value):
            bit = 0
        else:
            bit = 1
        bw.put(bit, probs[node >> 1])
        node = tree[node + bit]
        if node <= 0:
            return


def write_component(bw: BoolWriter, value: int, prob: list[int]) -> None:
    sign = 1 if value < 0 else 0
    mag = -value if value < 0 else value
    if mag >= 8:
        bw.put(1, prob[0])
        for i in range(3):
            bw.put((mag >> i) & 1, prob[9 + i])
        for i in range(9, 3, -1):
            bw.put((mag >> i) & 1, prob[9 + i])
        partial = 0
        for i in range(3):
            partial += ((mag >> i) & 1) << i
        for i in range(9, 3, -1):
            partial += ((mag >> i) & 1) << i
        if partial & 0xfff0:
            bw.put((mag >> 3) & 1, prob[12])
    else:
        bw.put(0, prob[0])
        write_tree(bw, SMALL_TREE, prob[2:9], mag)
    if mag:
        bw.put(sign, prob[1])


def coeff_update_probs() -> list[int]:
    path = os.path.join(ROOT, "src", "codec", "webp", "webp_vp8_proba.inc")
    text = open(path, encoding="ascii").read()
    start = text.index("static const uint8_t gimg_vp8_coeffs_update_proba")
    start = text.index("{", start)
    nums = []
    cur = ""
    for ch in text[start:]:
        if ch.isdigit():
            cur += ch
        elif cur:
            nums.append(int(cur))
            cur = ""
            if len(nums) == 4 * 8 * 3 * 11:
                return nums
    raise RuntimeError("update-prob table has %d entries" % len(nums))


def inter_payload(mb_count: int, row_q: int, col_q: int) -> bytes:
    """One-partition interframe. Every macroblock is skipped.

    row_q and col_q are quarter-pel luma displacements. Both zero
    selects the zero-motion mode. Anything else is a new vector.
    """
    bw = BoolWriter()
    bw.uniform(0)  # segmentation off
    bw.literal(0, 1)  # filter type
    bw.literal(0, 6)  # filter level
    bw.literal(0, 3)  # sharpness
    bw.uniform(0)  # no loop-filter deltas
    bw.literal(0, 2)  # one coefficient partition
    bw.literal(0, 7)  # quantizer index; skip ignores it
    for _ in range(5):
        bw.uniform(0)  # quantizer deltas absent
    bw.uniform(1)  # refresh golden
    bw.uniform(1)  # refresh altref
    bw.uniform(0)  # golden sign bias
    bw.uniform(0)  # altref sign bias
    bw.uniform(1)  # keep token probabilities
    bw.uniform(1)  # refresh last
    for prob in coeff_update_probs():
        bw.put(0, prob)
    bw.uniform(1)  # skip enabled
    bw.literal(128, 8)
    bw.literal(128, 8)  # prob_intra
    bw.literal(128, 8)  # prob_last
    bw.literal(128, 8)  # prob_gf
    bw.uniform(0)  # keep luma intra probs
    bw.uniform(0)  # keep chroma intra probs
    for row in MV_UPDATE:
        for prob in row:
            bw.put(0, prob)
    zero = row_q == 0 and col_q == 0
    # Every neighbor of a zero-motion inter macroblock, including the
    # border, is a zero vector, so the census is five and the zero-mode
    # branch probability is mode context [5][0].
    zero_prob = MODE_CTX[5][0]
    for _ in range(mb_count):
        bw.uniform(1)  # skip coefficients
        bw.uniform(1)  # inter
        bw.uniform(0)  # last frame
        if zero:
            bw.put(0, zero_prob)
        else:
            bw.put(1, MODE_CTX[5][0])
            bw.put(1, MODE_CTX[0][1])
            bw.put(1, MODE_CTX[0][2])
            bw.put(0, MODE_CTX[0][3])  # new, not split
            write_component(bw, row_q, MV_DEFAULT[0])
            write_component(bw, col_q, MV_DEFAULT[1])
    part0 = bw.finish()
    tag = 0x11 | (len(part0) << 5)  # inter, version 0, show
    header = bytes((tag & 0xff, (tag >> 8) & 0xff, (tag >> 16) & 0xff))
    return header + part0 + b"\x00\x00"


def chunk(tag: bytes, payload: bytes) -> bytes:
    out = tag + len(payload).to_bytes(4, "little") + payload
    if len(payload) & 1:
        out += b"\x00"
    return out


def vp8_payload(webp: bytes) -> bytes:
    off = 12
    while off + 8 <= len(webp):
        tag = webp[off:off + 4]
        size = int.from_bytes(webp[off + 4:off + 8], "little")
        payload = webp[off + 8:off + 8 + size]
        if tag == b"VP8 ":
            return payload
        off += 8 + size + (size & 1)
    raise RuntimeError("no VP8 chunk")


def vp8_size(payload: bytes) -> tuple[int, int]:
    width = int.from_bytes(payload[6:8], "little") & 0x3fff
    height = int.from_bytes(payload[8:10], "little") & 0x3fff
    return width, height


def anmf(width: int, height: int, payload: bytes) -> bytes:
    header = bytearray(16)
    header[6:9] = (width - 1).to_bytes(3, "little")
    header[9:12] = (height - 1).to_bytes(3, "little")
    header[12:15] = (100).to_bytes(3, "little")
    header[15] = 0x02  # do not blend
    body = bytes(header) + chunk(b"VP8 ", payload)
    return chunk(b"ANMF", body)


def animated(width: int, height: int, key: bytes, inter: bytes) -> bytes:
    vp8x = bytes((0x02, 0, 0, 0)) + (width - 1).to_bytes(3, "little") + (
        height - 1).to_bytes(3, "little")
    anim = b"\x00\x00\x00\x00\x00\x00"
    body = b"WEBP" + chunk(b"VP8X", vp8x) + chunk(b"ANIM", anim) + anmf(
        width, height, key) + anmf(width, height, inter)
    return b"RIFF" + len(body).to_bytes(4, "little") + body


def sample(plane: bytes, stride: int, w: int, h: int, x: int, y: int) -> int:
    if x < 0:
        x = 0
    if y < 0:
        y = 0
    if x >= w:
        x = w - 1
    if y >= h:
        y = h - 1
    return plane[y * stride + x]


def shr(value: int, bits: int) -> int:
    if value >= 0:
        return value >> bits
    return ~((~value) >> bits)


def clamp255(value: int) -> int:
    if value < 0:
        return 0
    if value > 255:
        return 255
    return value


def interp_at(fil: list[int], plane: bytes, stride: int, w: int, h: int,
        x: int, y: int, step_x: int, step_y: int) -> int:
    acc = 0
    for i in range(6):
        acc += sample(plane, stride, w, h, x + (i - 2) * step_x,
                      y + (i - 2) * step_y) * fil[i]
    return clamp255(shr(acc + 64, 7))


def predict_plane(src: bytes, w: int, h: int, mv_r: int, mv_c: int,
        bicubic: bool) -> bytes:
    full_r = shr(mv_r, 3)
    full_c = shr(mv_c, 3)
    vfrac = mv_r - (full_r << 3)
    hfrac = mv_c - (full_c << 3)
    out = bytearray(w * h)
    if hfrac == 0 and vfrac == 0:
        for y in range(h):
            for x in range(w):
                out[y * w + x] = sample(
                    src, w, w, h, x + full_c, y + full_r)
        return bytes(out)
    hfil = (BICUBIC if bicubic else BILINEAR)[hfrac]
    vfil = (BICUBIC if bicubic else BILINEAR)[vfrac]
    for y in range(h):
        for x in range(w):
            ox = x + full_c
            oy = y + full_r
            temp = [interp_at(hfil, src, w, w, h, ox, oy - 2 + row, 1, 0)
                    for row in range(9)]
            acc = 0
            for i in range(6):
                acc += temp[i] * vfil[i]
            out[y * w + x] = clamp255(shr(acc + 64, 7))
    return bytes(out)


def chroma_avg(value: int) -> int:
    s = value * 4
    if s >= 0:
        return (s + 4) >> 3
    return -(((-s) + 4) >> 3)


def fancy(plane: bytes, w: int, h: int, x: int, y: int) -> int:
    cx = x >> 1
    cy = y >> 1
    dx = 1 if (x & 1) else -1
    dy = 1 if (y & 1) else -1

    def at(px: int, py: int) -> int:
        if px < 0:
            px = 0
        if py < 0:
            py = 0
        if px >= w:
            px = w - 1
        if py >= h:
            py = h - 1
        return plane[py * w + px]

    a = at(cx, cy)
    b = at(cx + dx, cy)
    c = at(cx, cy + dy)
    d = at(cx + dx, cy + dy)
    return (9 * a + 3 * b + 3 * c + d + 8) >> 4


def mulhi(sample_v: int, coeff: int) -> int:
    return (sample_v * coeff) >> 8


def clip_fix6(value: int) -> int:
    if (value & ~16383) == 0:
        return value >> 6
    return 0 if value < 0 else 255


def yuv_rgba(y: int, u: int, v: int) -> bytes:
    y_term = mulhi(y, 19077)
    r = clip_fix6(y_term + mulhi(v, 26149) - 14234)
    g = clip_fix6(y_term - mulhi(u, 6419) - mulhi(v, 13320) + 8708)
    b = clip_fix6(y_term + mulhi(u, 33050) - 17685)
    return bytes((r, g, b, 255))


def expected_shift(yuv: bytes, width: int, height: int, row_q: int,
        col_q: int) -> bytes:
    cw = (width + 1) // 2
    ch = (height + 1) // 2
    y_n = width * height
    c_n = cw * ch
    y_plane = yuv[:y_n]
    u_plane = yuv[y_n:y_n + c_n]
    v_plane = yuv[y_n + c_n:y_n + 2 * c_n]
    mv_r = row_q * 2
    mv_c = col_q * 2
    pred_y = predict_plane(y_plane, width, height, mv_r, mv_c, True)
    cr = chroma_avg(mv_r)
    cc = chroma_avg(mv_c)
    pred_u = predict_plane(u_plane, cw, ch, cr, cc, True)
    pred_v = predict_plane(v_plane, cw, ch, cr, cc, True)
    out = bytearray()
    for y in range(height):
        for x in range(width):
            uu = fancy(pred_u, cw, ch, x, y)
            vv = fancy(pred_v, cw, ch, x, y)
            out += yuv_rgba(pred_y[y * width + x], uu, vv)
    return bytes(out)


def read_i420(path: str, width: int, height: int) -> bytes:
    with open(path, "rb") as handle:
        data = handle.read()
    cw = (width + 1) // 2
    ch = (height + 1) // 2
    need = width * height + 2 * cw * ch
    if len(data) != need:
        raise RuntimeError("yuv is %d bytes, expected %d" % (len(data), need))
    return data


def pam_rgba(path: str) -> tuple[int, int, bytes]:
    with open(path, "rb") as handle:
        data = handle.read()
    marker = b"ENDHDR\n"
    idx = data.find(marker)
    if idx < 0:
        raise RuntimeError("no ENDHDR in %s" % path)
    width = None
    height = None
    for line in data[:idx].decode("ascii", errors="replace").splitlines():
        if line.startswith("WIDTH "):
            width = int(line.split()[1])
        elif line.startswith("HEIGHT "):
            height = int(line.split()[1])
    if width is None or height is None:
        raise RuntimeError("PAM missing size in %s" % path)
    return width, height, data[idx + len(marker):]


def first_diff(got: bytes, want: bytes, width: int) -> str | None:
    if len(got) != len(want):
        return "decoded %d bytes, expected %d" % (len(got), len(want))
    for off in range(0, len(got), 4):
        if got[off:off + 4] == want[off:off + 4]:
            continue
        pixel = off // 4
        a = got[off:off + 4]
        b = want[off:off + 4]
        return (
            "pixel (%d,%d) is rgba(%d,%d,%d,%d), expected rgba(%d,%d,%d,%d)"
            % (pixel % width, pixel // width, a[0], a[1], a[2], a[3],
               b[0], b[1], b[2], b[3]))
    return None


def run(argv: list[str]) -> None:
    proc = subprocess.run(argv, check=False, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            "%s failed (%d): %s"
            % (os.path.basename(argv[0]), proc.returncode,
               (proc.stderr or proc.stdout).strip()))


def find_dump(explicit: str | None) -> str:
    if explicit and os.path.isfile(explicit):
        return os.path.abspath(explicit)
    env = os.environ.get("GIMG_WEBP_DUMP")
    if env and os.path.isfile(env):
        return os.path.abspath(env)
    for cand in (
            os.path.join(ROOT, "build", "linux", "release", "apps",
                         "dump_webp_raster"),
            os.path.join(ROOT, "build", "linux", "debug", "apps",
                         "dump_webp_raster"),
    ):
        if os.path.isfile(cand):
            return cand
    raise FileNotFoundError("dump_webp_raster not found")


def blocks_ppm(width: int, height: int) -> bytes:
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes(((x * 17 + 20) & 255, (y * 13 + 40) & 255,
                          ((x * 3) ^ (y * 5)) & 255))
        rows.append(bytes(row))
    return b"P6\n%d %d\n255\n" % (width, height) + b"".join(rows)


def check_case(dump: str, oracle: str, work: str, name: str, width: int,
        height: int, row_q: int, col_q: int) -> str | None:
    ppm_path = os.path.join(work, name + ".ppm")
    key_path = os.path.join(work, name + "_key.webp")
    yuv_path = os.path.join(work, name + ".yuv")
    pam_path = os.path.join(work, name + ".pam")
    anim_path = os.path.join(work, name + "_anim.webp")
    with open(ppm_path, "wb") as handle:
        handle.write(blocks_ppm(width, height))
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "cwebp", "-q", "40", "-m", "2", "-f", "0", "-segments", "1",
        "-sns", "0", "-o", key_path, ppm_path,
    ])
    key = vp8_payload(open(key_path, "rb").read())
    coded_w, coded_h = vp8_size(key)
    if (coded_w, coded_h) != (width, height):
        return "%s: cwebp coded %dx%d" % (name, coded_w, coded_h)
    mb_count = ((width + 15) // 16) * ((height + 15) // 16)
    inter = inter_payload(mb_count, row_q, col_q)
    with open(anim_path, "wb") as handle:
        handle.write(animated(width, height, key, inter))
    proc = subprocess.run(
        [dump, work, anim_path], check=False, capture_output=True, text=True)
    if proc.returncode != 0 or proc.stdout.count("\tok\t") < 2:
        return "%s: our decoder refused it: %s" % (
            name, (proc.stderr or proc.stdout).strip())
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "dwebp", "-pam", "-o", pam_path, key_path,
    ])
    _, _, ref = pam_rgba(pam_path)
    with open(os.path.join(work, "0.0.rgba"), "rb") as handle:
        got0 = handle.read()
    err = first_diff(got0, ref, width)
    if err:
        return "%s frame 0: %s" % (name, err)
    with open(os.path.join(work, "0.1.rgba"), "rb") as handle:
        got1 = handle.read()
    if row_q == 0 and col_q == 0:
        err = first_diff(got1, ref, width)
        if err:
            return "%s zero motion: %s" % (name, err)
        return None
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "dwebp", "-yuv", "-o", yuv_path, key_path,
    ])
    yuv = read_i420(yuv_path, width, height)
    want = expected_shift(yuv, width, height, row_q, col_q)
    err = first_diff(got1, want, width)
    if err:
        return "%s motion (%d,%d): %s" % (name, col_q, row_q, err)
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump", default=None)
    args = parser.parse_args(argv)
    try:
        dump = find_dump(args.dump)
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    oracle = os.path.join(ROOT, "tools", "oracle", "oracle-exec")
    work = os.path.join(ROOT, "tests", "out", "webp-inter")
    os.makedirs(work, exist_ok=True)
    cases = (
        ("zero_32", 32, 32, 0, 0),
        ("right_16", 16, 16, 0, 8),
        ("up_left_16", 16, 16, -4, -4),
        ("frac_16", 16, 16, 0, 1),
    )
    failed = 0
    for name, width, height, row_q, col_q in cases:
        try:
            err = check_case(
                dump, oracle, work, name, width, height, row_q, col_q)
        except (RuntimeError, OSError) as exc:
            print("  FAIL  %s  %s" % (name, exc), file=sys.stderr)
            failed += 1
            continue
        if err:
            print("  FAIL  %s  %s" % (name, err), file=sys.stderr)
            failed += 1
        else:
            print("  ok  %s" % name)
    print("%d ok, %d failed (%d files)" % (
        len(cases) - failed, failed, len(cases)))
    if failed:
        print("VP8 interframe verification FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
