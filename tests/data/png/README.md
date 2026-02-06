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

**Encode test output:** When the PNG encode tests run (with `GIMG_TEST_DATA_PNG` set), they write encoded PNGs to `tests/out/png/`. That directory is in `.gitignore`. `make test` runs all unit tests and then verifies these PNGs with PIL (Pillow); Pillow is required (`pip install Pillow`). Verification ensures:

- Each file has a valid PNG signature and IHDR chunk.
- Python (PIL) can open and verify each file (`Image.open()` + `im.verify()`).
- Optional *expected features* per filename (see `verify_png_output.py` `EXPECTATIONS`): e.g. `interlaced_roundtrip.png` must have IHDR `interlace_method == 1`; `preserve_exif.png` must contain an `eXIf` chunk.

To verify outputs manually:

```bash
python3 tests/data/png/verify_png_output.py
# Or: python3 tests/data/png/verify_png_output.py /path/to/tests/out/png
```
