# JPEG test data

Reference JPEG files for Phase 2 codec tests.

## Intended contents

| File | Description |
|------|-------------|
| Baseline (grayscale) | Small 8×8 or 16×16 grayscale baseline JPEG |
| Baseline (YCbCr) | Small RGB/YCbCr baseline JPEG |
| Progressive | Progressive DCT JPEG (for 2.2 tests) |
| With EXIF/ICC | JPEG with APP1 EXIF and/or APP2 ICC (for 2.1.3, 2.3.2) |
| CMYK sample | 4-component CMYK JPEG (for 2.4.1) |

## Generating test files

You can create minimal reference files with ImageMagick or Python (PIL/Pillow), for example:

```bash
# 8×8 grayscale
convert -size 8x8 xc:gray -quality 85 baseline_8x8_gray.jpg

# With EXIF orientation
exiftool -Orientation=6 -n baseline_8x8_gray.jpg
```

Or use `tests/codec/jpeg/test_jpeg_load.cpp` which builds a minimal in-memory baseline JPEG (SOI, SOF0, DQT, DHT, SOS, EOI) for parse/load tests without external files.

## Golden and fuzz

- **Golden decode tests:** Compare pixel hash and metadata after load/decode to reference decoder output when available.
- **Fuzz:** Corpus and harness under `tests/fuzz/`; JPEG parser/decoder should return `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT` on invalid input without crashing.
