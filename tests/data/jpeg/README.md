# JPEG test data

Reference JPEG files for Phase 2 codec and golden tests. Generate them with:

```bash
# From repo root (Pillow required; piexif optional for EXIF orientation)
python3 tests/data/jpeg/generate.py
```

Generated files:

| File | Description |
|------|-------------|
| `baseline_8x8_gray.jpg` | 8×8 grayscale baseline JPEG |
| `baseline_16x16_ycbcr.jpg` | 16×16 RGB baseline (stored as YCbCr) |
| `progressive_sample.jpg` | 16×16 progressive DCT JPEG |
| `jpeg_exif_orientation.jpg` | 8×8 gray with APP1 EXIF Orientation=6 (90° CW); requires `pip install piexif` when generating |
| `jpeg_with_icc.jpg` | 8×8 RGB with APP2 ICC profile (minimal) |
| `cmyk_sample.jpg` | 8×8 CMYK baseline JPEG |

Tests in `tests/codec/jpeg/test_jpeg_load.cpp` load these files when built with `GIMG_TEST_DATA_JPEG` (the Makefile sets this to the path of this directory). Golden decode tests compare FNV-1a 64-bit pixel hashes to stored expected values.

## Golden and fuzz

- **Golden decode tests:** Load each reference file, decode, compute `raster_pixel_hash`; compare to expected hash and verify metadata (orientation, DPI, color info) where applicable.
- **Fuzz:** Corpus and harness under `tests/fuzz/`; JPEG parser/decoder should return `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT` on invalid input without crashing. Seed corpus with these JPEGs: `cp tests/data/jpeg/*.jpg tests/fuzz/corpus/`.
