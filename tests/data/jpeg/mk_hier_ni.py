"""Turn a single-frame JPEG into a one-frame hierarchical sequence.

T.81 B.3.1: a hierarchical sequence is DHP followed by the frames.  B.3.2: the
DHP segment "has the same parameters as a frame header" - the largest image
dimensions of the sequence - "except that Tq shall be zero".  A sequence of one
non-differential frame therefore decodes to exactly that frame (J.1.3 leaves a
first frame coded normally), which is what makes this a fixture with a known
answer: the answer is the file we started from.
"""
import struct, sys

SOF_SET = {0xC0,0xC1,0xC2,0xC3,0xC5,0xC6,0xC7,0xC9,0xCA,0xCB,0xCD,0xCE,0xCF}

def build(src, dst):
    d = open(src,'rb').read()
    i = 2
    out = bytearray(d[:2])
    while i < len(d)-1:
        assert d[i] == 0xFF, f"not at a marker at {i}"
        m = d[i+1]
        if m in SOF_SET:
            ln = struct.unpack('>H', d[i+2:i+4])[0]
            payload = bytearray(d[i+4:i+2+ln])
            nf = payload[5]
            dhp = bytearray(payload)
            for c in range(nf):
                dhp[8 + c*3] = 0        # Tq = 0 (B.3.2)
            out += b'\xFF\xDE' + struct.pack('>H', len(dhp)+2) + bytes(dhp)
            out += d[i:]                # SOF and everything after, unchanged
            open(dst,'wb').write(bytes(out))
            return True
        if m == 0xD9 or (0xD0 <= m <= 0xD7) or m == 0x01:
            out += d[i:i+2]; i += 2; continue
        ln = struct.unpack('>H', d[i+2:i+4])[0]
        out += d[i:i+2+ln]; i += 2+ln
    return False

for src, dst in zip(sys.argv[1::2], sys.argv[2::2]):
    print(dst, "ok" if build(src,dst) else "FAILED")
