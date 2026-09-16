# PNG test data

Reference PNG files for decode tests. Generate them with:

```bash
# From repo root (Pillow required for APNG 16-bit expected file: pip install Pillow)
python3 tests/data/png/generate.py
```

Generated files:

| File | Description |
|------|-------------|
| `png_1x1_gray.png` | 1×1 grayscale 8-bit (black) |
| `png_2x2_gray.png` | 2×2 grayscale 8-bit (for max_decoded_pixels limit test) |
| `png_1x1_palette.png` | 1×1 palette + tRNS (R=0x11 G=0x22 B=0x33 A=0x80) |
| `png_16bit_gray.png` | 1×1 grayscale 16-bit (value 0x1234) |
| `png_1x1_rgba.png` | 1×1 RGBA 8-bit (R=0x11 G=0x22 B=0x33 A=0x80) |
| `png_16bit_rgba.png` | 1×1 RGBA 16-bit (R=0x1234 G=0x5678 B=0x9ABC A=0xDEF0) |
| `png_srgb.png` | 1×1 gray + sRGB chunk (rendering intent 0) |
| `png_exif.png` | 1×1 gray + eXIf chunk (minimal payload) |
| `png_exif_orientation.png` | 1×1 gray + eXIf with Orientation tag 6 (90° CW); used for meta_common test |
| `png_iccp.png` | 1×1 gray + iCCP chunk (tiny zlib-compressed profile) |
| `png_apng_2frame.png` | 2-frame APNG: default image is first frame (1×1 gray 0, then 0x80); frame 0 delay 50/100, dispose NONE, blend SOURCE; frame 1 delay 25/100, dispose BACKGROUND, blend OVER |
| `png_apng_3frame.png` | 3-frame APNG: gray 0, 0x80, 0xC0; frame 0 delay 50/100 dispose NONE blend SOURCE; frame 1 delay 25/100 dispose BACKGROUND blend OVER; frame 2 delay 10/100 dispose PREVIOUS blend OVER |
| `png_apng_2frame_16bit_rgba.png` | 2-frame APNG 16-bit RGBA: frame 0 black opaque, frame 1 red 50% alpha with blend OVER (dispose NONE). Used to test 16-bit alpha compositing. |
| `png_apng_2frame_16bit_rgba_expected.bin` | Expected pixels (16 bytes) for the above; produced by `generate.py` from Pillow (8-bit scaled to 16-bit). Decode test allows ±257 per component for 8- vs 16-bit rounding. |

**APNG 16-bit blend expected:** The test `GoldenApng16bitRgbaBlend` compares decoder output to the expected file. `generate.py` uses Pillow as oracle (opens APNG, composites frames, reads first pixel); Pillow returns 8-bit so we scale to 16-bit. The test allows a per-component tolerance of 257 (one 8-bit step in 16-bit) to account for rounding.

**APNG (1.4):** Multi-frame reference files above are used by decode and encode tests. Tests also cover invalid/truncated APNG (no crash, error returned) and limits (`max_frame_count`, `max_chunk_size`).

Tests in `tests/codec/png/test_png_decode.cpp` load these files when built with `GIMG_TEST_DATA_PNG` (the Makefile sets this to the path of this directory).

## Fixtures for the sub-byte, integrity and Third Edition cases

| File(s) | What they are for |
|---------|-------------------|
| `png_gray{1,2,4}_{32x8,33x9}.png` and `_interlaced.png` | Grayscale at 1, 2 and 4 bits, each emitted twice from one sample array. Adam7 is a reordering, so the two members of a pair must decode alike - a property that needs no reference decoder. 33×9 additionally leaves a partly-used final byte in several passes. |
| `png_pal{1,2,4}_{32x8,33x9}.png` and `_interlaced.png` | The same for palette images. The only palette fixture before these was 8-bit, which is why a write overrun in the sub-byte packing survived so long. |
| `png_gray*.raw`, `png_pal*.raw` | The expected decoded pixels, computed in `generate.py` from the rescaling rule of PNG 13.12 - not captured from this library's own output. Used by the decode tests and by `verify_png_output.py`. |
| `png_zlib_ok.png` | Control: a correct zlib stream. Every file below differs from it only in the wrapper bytes, and all chunk CRCs are correct, so nothing else can account for rejecting them. |
| `png_zlib_bad_adler.png` | Adler-32 of the wrong bytes. DEFLATE alone accepts this stream; only the check RFC 1950 requires catches it. |
| `png_zlib_bad_header.png` | CMF/FLG that is not a multiple of 31, declaring a compression method other than the 8 PNG 10.3 allows. |
| `png_zlib_preset_dict.png` | FDICT set, which PNG forbids. The header still passes the multiple-of-31 test, so only the flag marks it - and a decoder ignoring it reads the DICTID as DEFLATE data. |
| `png_rgb_suggested_palette.png`, `png_rgba_suggested_palette.png` | PLTE on a truecolour image, which PNG 11.2.2 allows as a suggested palette. The palette deliberately does not contain the image's colours. |
| `png_rgb_no_palette.png` | The same image with no PLTE, so "the suggested palette was ignored" is checkable without a reference decoder. |
| `png_gray_forbidden_palette.png` | PLTE on colour type 0, which the spec forbids outright. Some decoders read it anyway; this one does not. |
| `png_gradient_64x64_rgb.png` | A smooth gradient, which is where row filtering pays most, so choosing per row can be shown to beat forcing any single filter. |
| `png_cicp_srgb.png` | cICP naming the sRGB pair beside a gAMA that disagrees, so the Third Edition's precedence is testable. |
| `png_cicp_bt2020_pq.png` | cICP naming BT.2020 primaries with the PQ transfer - legal, and beyond what `GIMG_Color_Info` can describe. A gAMA rides along so that "left unknown" is an assertion and not an accident. |
| `png_mdcv_clli.png` | The HDR mastering chunks, which are preserved and not interpreted. |

Every one of these was checked against Pillow and libpng before any test was
written on it: a fixture this library alone agrees with proves nothing.

**The published conformance suite** (PngSuite) is used as a development oracle
from a scratch directory and is deliberately **not** vendored here. These
fixtures cover the same ground in files this project generates.

**Encode test output:** When the PNG encode tests run (with `GIMG_TEST_DATA_PNG` set), they write encoded PNGs to `tests/out/png/`. That directory is in `.gitignore`. `make test` runs all unit tests and then verifies these PNGs with PIL (Pillow); Pillow is required (`pip install Pillow`). Verification ensures:

- Each file has a valid PNG signature and IHDR chunk.
- Python (PIL) can open and verify each file (`Image.open()` + `im.verify()`).
- Optional *expected features* per filename (see `verify_png_output.py` `EXPECTATIONS`): e.g. `interlaced_roundtrip.png` must have IHDR `interlace_method == 1`; `preserve_exif.png` must contain an `eXIf` chunk.

To verify outputs manually:

```bash
python3 tests/data/png/verify_png_output.py
# Or: python3 tests/data/png/verify_png_output.py /path/to/tests/out/png
```
