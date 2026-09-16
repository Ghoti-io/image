"""Rewrite a single-scan JPEG to use DNL (T.81 B.2.5).

B.2.2 lets a frame header carry Y = 0, in which case "the number of lines shall
be defined by the DNL marker segment" that follows the first scan.  Nothing
writes such a file, so this makes one: zero the SOF's Y, and insert
FFDC 0004 <NL> immediately after the first entropy-coded segment.
"""
import struct, sys

SOF = {0xC0,0xC1,0xC2,0xC3,0xC5,0xC6,0xC7,0xC9,0xCA,0xCB,0xCD,0xCE,0xCF}

def build(src, dst):
    d = bytearray(open(src,'rb').read())
    i, nl, out = 2, None, bytearray(d[:2])
    while i < len(d)-1:
        assert d[i] == 0xFF, hex(d[i])
        m = d[i+1]
        if m == 0xD9:
            out += d[i:i+2]; i += 2; continue
        ln = struct.unpack('>H', d[i+2:i+4])[0]
        seg = bytearray(d[i:i+2+ln])
        if m in SOF:
            nl = struct.unpack('>H', seg[5:7])[0]
            seg[5] = 0; seg[6] = 0          # Y = 0: see DNL
        out += seg
        i += 2+ln
        if m == 0xDA:                        # entropy-coded segment follows
            j = i
            while j < len(d)-1:
                if d[j] == 0xFF and d[j+1] != 0 and not (0xD0 <= d[j+1] <= 0xD7):
                    break
                j += 1
            out += d[i:j]
            out += b'\xFF\xDC' + struct.pack('>HH', 4, nl)   # DNL
            i = j
    open(dst,'wb').write(bytes(out))
    return nl

for a, b in zip(sys.argv[1::2], sys.argv[2::2]):
    print(b, 'NL =', build(a,b))
