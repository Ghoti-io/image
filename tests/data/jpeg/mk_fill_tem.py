"""Build JPEGs exercising T.81 B.1.1.2 fill bytes and the B.1.1.3 TEM marker.

B.1.1.2: "any marker may optionally be preceded by any number of fill bytes,
which are bytes assigned code X'FF'".  B.1.1.3 Table B.1 lists TEM (X'FF01')
among the markers that stand alone, with no length field and no payload.

No encoder emits either, so these are assembled from an ordinary baseline file.
The picture is unchanged in every one of them - that is the point: each must
decode to exactly what the file decodes to without the padding.
"""
import struct, sys

SOF = {0xC0, 0xC1, 0xC2, 0xC3}

def segments(d):
    i = 2
    while i < len(d) - 1:
        m = d[i + 1]
        if m == 0xD9:
            yield m, d[i:i + 2]
            return
        if m == 0xDA:                      # SOS: entropy data to end of file
            yield m, d[i:]
            return
        ln = struct.unpack('>H', d[i + 2:i + 4])[0]
        yield m, d[i:i + 2 + ln]
        i += 2 + ln

def build(src, kind):
    d = open(src, 'rb').read()
    out = bytearray(d[:2])
    first = True
    for m, seg in segments(d):
        if kind == 'first' and first:
            out += b'\xFF\xFF'
        if kind == 'sof' and m in SOF:
            out += b'\xFF\xFF\xFF'
        if kind == 'one' and m in SOF:
            out += b'\xFF'
        if kind == 'sos' and m == 0xDA:
            out += b'\xFF\xFF'
        if kind == 'tem' and m in SOF:
            out += b'\xFF\x01'             # TEM, standalone
        first = False
        out += seg
    if kind == 'eoi':                      # fill immediately before EOI
        assert bytes(out[-2:]) == b'\xFF\xD9'
        out = out[:-2] + bytearray(b'\xFF\xFF\xFF\xD9')
    return bytes(out)

if __name__ == '__main__':
    src = sys.argv[1]
    for kind, name in (('sof', 'marker_fill_before_sof'),
                       ('one', 'marker_fill_single_byte'),
                       ('first', 'marker_fill_first_segment'),
                       ('sos', 'marker_fill_before_sos'),
                       ('eoi', 'marker_fill_before_eoi'),
                       ('tem', 'marker_tem')):
        out = sys.argv[2].rstrip('/') + '/' + name + '.jpg'
        open(out, 'wb').write(build(src, kind))
        print(out)
