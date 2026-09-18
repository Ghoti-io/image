#!/usr/bin/env python3
"""Generator for the wide-frame JPEG fixtures.

T.81 B.2.2 lets a frame carry from 1 to 255 components and B.2.3 caps one scan
at 4, so a frame wider than four is legal and has to be written as several
non-interleaved scans (A.2.3).  Nothing in the fixture set had one.

libjpeg cannot be used directly as the oracle here: its decoder matches a
scan's Cs against only the first MAX_COMPS_IN_SCAN components of the frame
(jdmarker.c get_sos: "ci < cinfo->num_components && ci < MAX_COMPS_IN_SCAN"),
so it refuses any file whose scan names the fifth component or later, however
well formed.  That is a limit of that implementation, not of the standard.

So the fixture is assembled from files libjpeg did write.  A frame of N
components, all 1x1, written as N single-component scans is - block for block,
and with the DC predictor reset at every SOS - exactly N grayscale JPEGs
sharing one quantization table and one set of Huffman tables.  This script
writes those N grayscale files with cjpeg, checks that their tables really are
identical, splices their scans into one N-component frame, and keeps libjpeg's
own decode of each grayscale file as the expected plane.  Both ends of the
comparison are therefore libjpeg's.

Usage:
  mk_wide.py <path-to-cjpeg> <path-to-djpeg>

Writes wide_<N>comp.jpg and wide_<N>comp.raw into the working directory.
The .raw format is: byte 0 = 4 (N channels, 8-bit), byte 1 = N, 4 bytes width
LE, 4 bytes height LE, then w * h * N samples.

Copyright 2026 by Corey Pennycuff
"""
import struct
import subprocess
import sys
import tempfile
import os

W, H = 33, 17
QUALITY = 90


def sample_at(x, y, c):
    """Different in every component, so a mix-up between them shows."""
    return (x * (7 + c * 13) + y * (3 + c * 29) + c * 41) & 0xFF


def write_pgm(path, c):
    body = bytes(sample_at(x, y, c) for y in range(H) for x in range(W))
    with open(path, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (W, H))
        f.write(body)


def read_pgm(path):
    with open(path, "rb") as f:
        d = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i : i + 1].isspace():
            i += 1
        if d[i : i + 1] == b"#":
            while d[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not d[j : j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    i += 1
    assert parts[0] == b"P5", parts[0]
    return int(parts[1]), int(parts[2]), d[i:]


def segments(jpg):
    """Yield (marker, payload, entropy) walking a baseline JPEG."""
    assert jpg[0:2] == b"\xff\xd8"
    i = 2
    while i < len(jpg):
        assert jpg[i] == 0xFF, "desync at %d" % i
        m = jpg[i + 1]
        if m == 0xD9:
            return
        length = (jpg[i + 2] << 8) | jpg[i + 3]
        payload = jpg[i + 4 : i + 2 + length]
        i += 2 + length
        if m == 0xDA:
            j = i
            while j + 1 < len(jpg):
                if jpg[j] == 0xFF and jpg[j + 1] != 0 and not (
                    0xD0 <= jpg[j + 1] <= 0xD7
                ):
                    break
                j += 1
            yield m, payload, jpg[i:j]
            i = j
        else:
            yield m, payload, b""


def build(n, cjpeg, djpeg, tmp):
    dqt, dht, scans, planes = None, [], [], []
    for c in range(n):
        pgm = os.path.join(tmp, "g%d.pgm" % c)
        jpg = os.path.join(tmp, "g%d.jpg" % c)
        out = os.path.join(tmp, "g%d.out.pgm" % c)
        write_pgm(pgm, c)
        subprocess.run(
            [cjpeg, "-grayscale", "-quality", str(QUALITY), "-outfile", jpg, pgm],
            check=True,
        )
        subprocess.run([djpeg, "-pnm", "-outfile", out, jpg], check=True)
        w, h, body = read_pgm(out)
        assert (w, h) == (W, H), (w, h)
        planes.append(body)
        with open(jpg, "rb") as f:
            data = f.read()
        this_dqt, this_dht, scan = None, [], None
        for m, payload, entropy in segments(data):
            if m == 0xDB:
                assert this_dqt is None, "more than one DQT"
                this_dqt = payload
            elif m == 0xC4:
                this_dht.append(payload)
            elif m == 0xDA:
                assert payload[0] == 1, "grayscale scan must name one component"
                # Td and Ta must be 0 for the spliced frame to name one table
                # set for every component.
                assert payload[2] == 0x00, "unexpected table selectors"
                scan = entropy
        assert this_dqt is not None and scan is not None
        if dqt is None:
            dqt, dht = this_dqt, this_dht
        else:
            assert this_dqt == dqt, "components must share one DQT"
            assert this_dht == dht, "components must share one DHT set"
        scans.append(scan)

    out = bytearray(b"\xff\xd8")
    out += b"\xff\xdb" + struct.pack(">H", len(dqt) + 2) + dqt
    for t in dht:
        out += b"\xff\xc4" + struct.pack(">H", len(t) + 2) + t
    sof = bytes([8]) + struct.pack(">HH", H, W) + bytes([n])
    for c in range(n):
        sof += bytes([c + 1, 0x11, 0x00])  # Ci, Hi|Vi = 1x1, Tq = 0
    out += b"\xff\xc0" + struct.pack(">H", len(sof) + 2) + sof
    for c in range(n):
        sos = bytes([1, c + 1, 0x00, 0, 63, 0])
        out += b"\xff\xda" + struct.pack(">H", len(sos) + 2) + sos
        out += scans[c]
    out += b"\xff\xd9"

    raw = bytearray([4, n])
    raw += struct.pack("<II", W, H)
    for y in range(H):
        for x in range(W):
            for c in range(n):
                raw.append(planes[c][y * W + x])
    return bytes(out), bytes(raw)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cjpeg, djpeg = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as tmp:
        for n in (2, 5, 8, 10, 32, 255):
            jpg, raw = build(n, cjpeg, djpeg, tmp)
            with open("wide_%dcomp.jpg" % n, "wb") as f:
                f.write(jpg)
            with open("wide_%dcomp.raw" % n, "wb") as f:
                f.write(raw)
            print("wide_%dcomp: %d bytes, %d components" % (n, len(jpg), n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
