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
way, bicubic for version 0 and bilinear for version 1. Version 3
keeps the integer sample. A split partition, a golden or altref
selection, a sign-bias flip, and a simple loop-filter edge are
checked the same way.

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

def renorm(range_value: int) -> tuple[int, int]:
    """Encoder range is one less than the RFC 6386 section 7 range.

    Shift the span (range + 1) until it is at least 128. The new range
    is that span minus one. This is the same step the library encoder
    uses, so a partition written here is one the RFC reader accepts.
    """
    span = range_value + 1
    shift = 0
    while span < 128:
        span <<= 1
        shift += 1
    return span - 1, shift


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
            self.range, shift = renorm(self.range)
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
            self.range, shift = renorm(self.range)
            self.value = s32(self.value << shift)
            self.nb_bits += shift
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


def finish_inter(bw: BoolWriter, version: int) -> bytes:
    part0 = bw.finish()
    tag = 0x11 | ((version & 7) << 1) | (len(part0) << 5)
    header = bytes((tag & 0xff, (tag >> 8) & 0xff, (tag >> 16) & 0xff))
    return header + part0 + b"\x00\x00"


def write_inter_header(bw: BoolWriter, filter_type: int = 0,
        filter_level: int = 0, sharpness: int = 0, refresh_gf: int = 1,
        refresh_arf: int = 1, refresh_last: int = 1, bias_gf: int = 0,
        bias_arf: int = 0) -> None:
    bw.uniform(0)  # segmentation off
    bw.literal(filter_type, 1)
    bw.literal(filter_level, 6)
    bw.literal(sharpness, 3)
    bw.uniform(0)  # no loop-filter deltas
    bw.literal(0, 2)  # one coefficient partition
    bw.literal(0, 7)  # quantizer index; skip ignores it
    for _ in range(5):
        bw.uniform(0)
    bw.uniform(refresh_gf)
    bw.uniform(refresh_arf)
    if not refresh_gf:
        bw.literal(0, 2)  # copy nothing onto golden
    if not refresh_arf:
        bw.literal(0, 2)
    bw.uniform(bias_gf)
    bw.uniform(bias_arf)
    bw.uniform(1)  # keep token probabilities
    bw.uniform(refresh_last)
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


def write_skip_inter(bw: BoolWriter, ref: str) -> None:
    bw.uniform(1)  # skip coefficients
    bw.uniform(1)  # inter
    if ref == "last":
        bw.uniform(0)
    elif ref == "golden":
        bw.uniform(1)
        bw.uniform(0)
    elif ref == "alt":
        bw.uniform(1)
        bw.uniform(1)
    else:
        raise ValueError(ref)


def write_zero_mode(bw: BoolWriter, cnt0: int) -> None:
    bw.put(0, MODE_CTX[cnt0][0])


def write_new_mode(bw: BoolWriter, cnt0: int, row_q: int, col_q: int) -> None:
    bw.put(1, MODE_CTX[cnt0][0])
    bw.put(1, MODE_CTX[0][1])
    bw.put(1, MODE_CTX[0][2])
    bw.put(0, MODE_CTX[0][3])
    write_component(bw, row_q, MV_DEFAULT[0])
    write_component(bw, col_q, MV_DEFAULT[1])


def inter_payload(mb_count: int, row_q: int, col_q: int, version: int = 0,
        filter_type: int = 0, filter_level: int = 0) -> bytes:
    """One-partition interframe. Every macroblock is skipped.

    row_q and col_q are quarter-pel luma displacements. Both zero
    selects the zero-motion mode. Anything else is a new vector.
    The census of an all-zero neighborhood, including the border, is
    five.
    """
    bw = BoolWriter()
    write_inter_header(
        bw, filter_type=filter_type, filter_level=filter_level)
    zero = row_q == 0 and col_q == 0
    for _ in range(mb_count):
        write_skip_inter(bw, "last")
        if zero:
            write_zero_mode(bw, 5)
        else:
            write_new_mode(bw, 5, row_q, col_q)
    return finish_inter(bw, version)


def split_top_payload() -> bytes:
    """One macroblock, split top/bottom. Top is a new vector, bottom is zero."""
    bw = BoolWriter()
    write_inter_header(bw)
    write_skip_inter(bw, "last")
    bw.put(1, MODE_CTX[5][0])
    bw.put(1, MODE_CTX[0][1])
    bw.put(1, MODE_CTX[0][2])
    bw.put(1, MODE_CTX[0][3])  # split
    bw.put(1, 110)
    bw.put(1, 111)
    bw.put(0, 150)  # top_bottom
    bw.put(1, 208)
    bw.put(1, 1)
    bw.put(1, 1)  # new 4x4; both neighbors are outside the frame
    write_component(bw, 0, MV_DEFAULT[0])
    write_component(bw, 8, MV_DEFAULT[1])
    bw.put(1, 106)
    bw.put(1, 145)
    bw.put(0, 1)  # zero 4x4; left is zero, above is the new vector
    return finish_inter(bw, 0)


def sign_bias_payload() -> bytes:
    """Two macroblocks. The right one takes the left vector, negated.

    Golden's sign bias differs from last, so the left macroblock's
    vector flips when it is the right macroblock's nearest neighbor.
    """
    bw = BoolWriter()
    write_inter_header(bw, bias_gf=1)
    write_skip_inter(bw, "last")
    write_new_mode(bw, 5, 0, 8)
    write_skip_inter(bw, "golden")
    bw.put(1, MODE_CTX[3][0])
    bw.put(0, MODE_CTX[2][1])  # nearest
    return finish_inter(bw, 0)


def preserve_refs_payloads(ref: str) -> tuple[bytes, bytes]:
    """Move, leave golden and altref on the keyframe, then copy one back."""
    moved = BoolWriter()
    write_inter_header(moved, refresh_gf=0, refresh_arf=0, refresh_last=1)
    write_skip_inter(moved, "last")
    write_new_mode(moved, 5, 0, 8)
    copied = BoolWriter()
    write_inter_header(copied)
    write_skip_inter(copied, ref)
    write_zero_mode(copied, 5)
    return finish_inter(moved, 0), finish_inter(copied, 0)


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


def animated(width: int, height: int, payloads: list[bytes]) -> bytes:
    vp8x = bytes((0x02, 0, 0, 0)) + (width - 1).to_bytes(3, "little") + (
        height - 1).to_bytes(3, "little")
    anim = b"\x00\x00\x00\x00\x00\x00"
    frames = b"".join(anmf(width, height, payload) for payload in payloads)
    body = b"WEBP" + chunk(b"VP8X", vp8x) + chunk(b"ANIM", anim) + frames
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
        bicubic: bool, fullpel: bool = False) -> bytes:
    if fullpel:
        mv_r &= ~7
        mv_c &= ~7
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


def planes_of(yuv: bytes, width: int, height: int) -> tuple[bytes, bytes, bytes, int, int]:
    cw = (width + 1) // 2
    ch = (height + 1) // 2
    y_n = width * height
    c_n = cw * ch
    return yuv[:y_n], yuv[y_n:y_n + c_n], yuv[y_n + c_n:y_n + 2 * c_n], cw, ch


def rgba_of(y_plane: bytes, u_plane: bytes, v_plane: bytes, width: int,
        height: int, cw: int, ch: int) -> bytes:
    out = bytearray()
    for y in range(height):
        for x in range(width):
            out += yuv_rgba(
                y_plane[y * width + x], fancy(u_plane, cw, ch, x, y),
                fancy(v_plane, cw, ch, x, y))
    return bytes(out)


def stitch_rows(top: bytes, bottom: bytes, width: int, split: int) -> bytes:
    return top[:split * width] + bottom[split * width:]


def stitch_cols(left: bytes, right: bytes, width: int, split: int,
        height: int) -> bytes:
    out = bytearray()
    for y in range(height):
        row = y * width
        out += left[row:row + split]
        out += right[row + split:row + width]
    return bytes(out)


def expected_shift(yuv: bytes, width: int, height: int, row_q: int,
        col_q: int, bicubic: bool = True, fullpel: bool = False) -> bytes:
    y_plane, u_plane, v_plane, cw, ch = planes_of(yuv, width, height)
    mv_r = row_q * 2
    mv_c = col_q * 2
    pred_y = predict_plane(
        y_plane, width, height, mv_r, mv_c, bicubic, fullpel)
    cr = chroma_avg(mv_r)
    cc = chroma_avg(mv_c)
    if fullpel:
        cr &= ~7
        cc &= ~7
    pred_u = predict_plane(u_plane, cw, ch, cr, cc, bicubic, fullpel)
    pred_v = predict_plane(v_plane, cw, ch, cr, cc, bicubic, fullpel)
    return rgba_of(pred_y, pred_u, pred_v, width, height, cw, ch)


def clamp_signed(value: int) -> int:
    if value < -128:
        return -128
    if value > 127:
        return 127
    return value


def simple_vertical(plane: bytes, width: int, height: int, edge_x: int,
        limit: int) -> bytes:
    """Section 15.3 simple filter on one vertical luma edge."""
    out = bytearray(plane)
    for y in range(height):
        base = y * width + edge_x
        p1 = out[base - 2]
        p0 = out[base - 1]
        q0 = out[base]
        q1 = out[base + 1]
        if (abs(p0 - q0) * 2 + abs(p1 - q1) // 2) > limit:
            continue
        sp0 = p0 - 128
        sq0 = q0 - 128
        outer = clamp_signed((p1 - 128) - (q1 - 128))
        a = clamp_signed(outer + 3 * (sq0 - sp0))
        b = shr(clamp_signed(a + 3), 3)
        a = shr(clamp_signed(a + 4), 3)
        out[base - 1] = (clamp_signed(sp0 + b) + 128) & 255
        out[base] = (clamp_signed(sq0 - a) + 128) & 255
    return bytes(out)


def expected_simple_edge(yuv: bytes, width: int, height: int, edge_x: int,
        level: int) -> bytes:
    y_plane, u_plane, v_plane, cw, ch = planes_of(yuv, width, height)
    interior = level
    limit = ((level + 2) * 2) + interior
    y_plane = simple_vertical(y_plane, width, height, edge_x, limit)
    return rgba_of(y_plane, u_plane, v_plane, width, height, cw, ch)


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


def encode_key(oracle: str, work: str, name: str, width: int,
        height: int) -> tuple[bytes, str]:
    ppm_path = os.path.join(work, name + ".ppm")
    key_path = os.path.join(work, name + "_key.webp")
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
        raise RuntimeError(
            "%s: cwebp coded %dx%d" % (name, coded_w, coded_h))
    return key, key_path


def decode_frames(dump: str, work: str, name: str, anim: bytes,
        nframes: int) -> list[bytes]:
    anim_path = os.path.join(work, name + "_anim.webp")
    with open(anim_path, "wb") as handle:
        handle.write(anim)
    proc = subprocess.run(
        [dump, work, anim_path], check=False, capture_output=True, text=True)
    if proc.returncode != 0 or proc.stdout.count("\tok\t") < nframes:
        raise RuntimeError(
            "%s: our decoder refused it: %s"
            % (name, (proc.stderr or proc.stdout).strip()))
    frames = []
    for index in range(nframes):
        with open(os.path.join(work, "0.%d.rgba" % index), "rb") as handle:
            frames.append(handle.read())
    return frames


def key_pam(oracle: str, work: str, name: str, key_path: str) -> bytes:
    pam_path = os.path.join(work, name + ".pam")
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "dwebp", "-pam", "-o", pam_path, key_path,
    ])
    _, _, ref = pam_rgba(pam_path)
    return ref


def key_yuv(oracle: str, work: str, name: str, key_path: str, width: int,
        height: int) -> bytes:
    yuv_path = os.path.join(work, name + ".yuv")
    run([
        oracle, "--scratch", work, "libwebp", "--",
        "dwebp", "-yuv", "-o", yuv_path, key_path,
    ])
    return read_i420(yuv_path, width, height)


def check_case(dump: str, oracle: str, work: str, name: str, width: int,
        height: int, row_q: int, col_q: int, version: int = 0,
        bicubic: bool = True, fullpel: bool = False) -> str | None:
    key, key_path = encode_key(oracle, work, name, width, height)
    mb_count = ((width + 15) // 16) * ((height + 15) // 16)
    inter = inter_payload(mb_count, row_q, col_q, version)
    frames = decode_frames(
        dump, work, name, animated(width, height, [key, inter]), 2)
    ref = key_pam(oracle, work, name, key_path)
    err = first_diff(frames[0], ref, width)
    if err:
        return "%s frame 0: %s" % (name, err)
    if (row_q == 0 and col_q == 0) or fullpel and row_q == 0 and col_q == 1:
        err = first_diff(frames[1], ref, width)
        if err:
            return "%s integer sample: %s" % (name, err)
        return None
    yuv = key_yuv(oracle, work, name, key_path, width, height)
    want = expected_shift(
        yuv, width, height, row_q, col_q, bicubic, fullpel)
    err = first_diff(frames[1], want, width)
    if err:
        return "%s motion (%d,%d): %s" % (name, col_q, row_q, err)
    return None


def check_split(dump: str, oracle: str, work: str) -> str | None:
    name, width, height = "split_16", 16, 16
    key, key_path = encode_key(oracle, work, name, width, height)
    frames = decode_frames(
        dump, work, name,
        animated(width, height, [key, split_top_payload()]), 2)
    ref = key_pam(oracle, work, name, key_path)
    err = first_diff(frames[0], ref, width)
    if err:
        return "split_16 frame 0: %s" % err
    yuv = key_yuv(oracle, work, name, key_path, width, height)
    y_plane, u_plane, v_plane, cw, ch = planes_of(yuv, width, height)
    top_y = predict_plane(y_plane, width, height, 0, 16, True)
    bot_y = predict_plane(y_plane, width, height, 0, 0, True)
    top_u = predict_plane(u_plane, cw, ch, 0, 8, True)
    bot_u = predict_plane(u_plane, cw, ch, 0, 0, True)
    top_v = predict_plane(v_plane, cw, ch, 0, 8, True)
    bot_v = predict_plane(v_plane, cw, ch, 0, 0, True)
    want = rgba_of(
        stitch_rows(top_y, bot_y, width, 8),
        stitch_rows(top_u, bot_u, cw, 4),
        stitch_rows(top_v, bot_v, cw, 4),
        width, height, cw, ch)
    return first_diff(frames[1], want, width)


def check_sign(dump: str, oracle: str, work: str) -> str | None:
    name, width, height = "sign_32", 32, 16
    key, key_path = encode_key(oracle, work, name, width, height)
    frames = decode_frames(
        dump, work, name,
        animated(width, height, [key, sign_bias_payload()]), 2)
    ref = key_pam(oracle, work, name, key_path)
    err = first_diff(frames[0], ref, width)
    if err:
        return "sign_32 frame 0: %s" % err
    yuv = key_yuv(oracle, work, name, key_path, width, height)
    y_plane, u_plane, v_plane, cw, ch = planes_of(yuv, width, height)
    left_y = predict_plane(y_plane, width, height, 0, 16, True)
    right_y = predict_plane(y_plane, width, height, 0, -16, True)
    left_u = predict_plane(u_plane, cw, ch, 0, 8, True)
    right_u = predict_plane(u_plane, cw, ch, 0, -8, True)
    left_v = predict_plane(v_plane, cw, ch, 0, 8, True)
    right_v = predict_plane(v_plane, cw, ch, 0, -8, True)
    want = rgba_of(
        stitch_cols(left_y, right_y, width, 16, height),
        stitch_cols(left_u, right_u, cw, 8, ch),
        stitch_cols(left_v, right_v, cw, 8, ch),
        width, height, cw, ch)
    return first_diff(frames[1], want, width)


def check_refs(dump: str, oracle: str, work: str, ref: str) -> str | None:
    name, width, height = ref + "_16", 16, 16
    key, key_path = encode_key(oracle, work, name, width, height)
    moved, copied = preserve_refs_payloads(ref)
    frames = decode_frames(
        dump, work, name, animated(width, height, [key, moved, copied]), 3)
    ref_pam = key_pam(oracle, work, name, key_path)
    err = first_diff(frames[0], ref_pam, width)
    if err:
        return "%s frame 0: %s" % (name, err)
    yuv = key_yuv(oracle, work, name, key_path, width, height)
    want = expected_shift(yuv, width, height, 0, 8)
    err = first_diff(frames[1], want, width)
    if err:
        return "%s moved: %s" % (name, err)
    err = first_diff(frames[2], ref_pam, width)
    if err:
        return "%s still holds the keyframe: %s" % (name, err)
    return None


def check_filter(dump: str, oracle: str, work: str) -> str | None:
    name, width, height, level = "simple_32", 32, 16, 20
    key, key_path = encode_key(oracle, work, name, width, height)
    inter = inter_payload(
        2, 0, 0, filter_type=1, filter_level=level)
    frames = decode_frames(
        dump, work, name, animated(width, height, [key, inter]), 2)
    ref = key_pam(oracle, work, name, key_path)
    err = first_diff(frames[0], ref, width)
    if err:
        return "simple_32 frame 0: %s" % err
    yuv = key_yuv(oracle, work, name, key_path, width, height)
    want = expected_simple_edge(yuv, width, height, 16, level)
    err = first_diff(frames[1], want, width)
    if err:
        return "simple_32 edge: %s" % err
    if want == ref:
        return "simple_32 edge: the filter left the keyframe unchanged"
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
        ("zero_32", 32, 32, 0, 0, 0, True, False),
        ("right_16", 16, 16, 0, 8, 0, True, False),
        ("up_left_16", 16, 16, -4, -4, 0, True, False),
        ("frac_16", 16, 16, 0, 1, 0, True, False),
        ("bilinear_16", 16, 16, 0, 1, 1, False, False),
        ("fullpel_int", 16, 16, 0, 8, 3, True, True),
        ("fullpel_frac", 16, 16, 0, 1, 3, True, True),
    )
    extra = (
        ("split_16", check_split),
        ("sign_32", check_sign),
        ("golden_16", lambda d, o, w: check_refs(d, o, w, "golden")),
        ("alt_16", lambda d, o, w: check_refs(d, o, w, "alt")),
        ("simple_32", check_filter),
    )
    failed = 0
    total = len(cases) + len(extra)
    for name, width, height, row_q, col_q, version, bicubic, fullpel in cases:
        try:
            err = check_case(
                dump, oracle, work, name, width, height, row_q, col_q,
                version, bicubic, fullpel)
        except (RuntimeError, OSError) as exc:
            print("  FAIL  %s  %s" % (name, exc), file=sys.stderr)
            failed += 1
            continue
        if err:
            print("  FAIL  %s  %s" % (name, err), file=sys.stderr)
            failed += 1
        else:
            print("  ok  %s" % name)
    for name, fn in extra:
        try:
            err = fn(dump, oracle, work)
        except (RuntimeError, OSError) as exc:
            print("  FAIL  %s  %s" % (name, exc), file=sys.stderr)
            failed += 1
            continue
        if err:
            print("  FAIL  %s  %s" % (name, err), file=sys.stderr)
            failed += 1
        else:
            print("  ok  %s" % name)
    print("%d ok, %d failed (%d files)" % (total - failed, failed, total))
    if failed:
        print("VP8 interframe verification FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
