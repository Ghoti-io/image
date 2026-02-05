# PNG test data

Reference PNG files for decode tests. Generate them with:

```bash
# From repo root
python3 tests/data/png/generate.py
```

Generated files:

| File | Description |
|------|-------------|
| `png_1x1_gray.png` | 1×1 grayscale 8-bit (black) |
| `png_1x1_palette.png` | 1×1 palette + tRNS (R=0x11 G=0x22 B=0x33 A=0x80) |
| `png_16bit_gray.png` | 1×1 grayscale 16-bit (value 0x1234) |
| `png_1x1_rgba.png` | 1×1 RGBA 8-bit (R=0x11 G=0x22 B=0x33 A=0x80) |
| `png_16bit_rgba.png` | 1×1 RGBA 16-bit (R=0x1234 G=0x5678 B=0x9ABC A=0xDEF0) |
| `png_srgb.png` | 1×1 gray + sRGB chunk (rendering intent 0) |
| `png_exif.png` | 1×1 gray + eXIf chunk (minimal payload) |
| `png_iccp.png` | 1×1 gray + iCCP chunk (tiny zlib-compressed profile) |

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
