# PNG/APNG fuzz harness

LibFuzzer harness for PNG/APNG load and decode. Ensures the parser and decoder do not crash on random or truncated input and return appropriate errors (e.g. `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, `GIMG_ERR_LIMIT`).

## Build

From the image library root, with clang available:

```bash
make fuzz-png
```

This builds `build/<build-dir>/apps/fuzz_png_load` with `-fsanitize=fuzzer`.

## Corpus

Seed the corpus with valid PNGs so the fuzzer can mutate them. Copy reference files:

```bash
mkdir -p tests/fuzz/corpus
cp tests/data/png/png_1x1_gray.png tests/data/png/png_1x1_rgba.png tests/fuzz/corpus/
```

Or use the full set from `tests/data/png/` (after running `python3 tests/data/png/generate.py`).

## Run

Run in CI or periodically:

```bash
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_png_load tests/fuzz/corpus
```

Without a corpus directory, the fuzzer runs with no seeds (slower to find coverage). Use `-max_total_time=N` to limit run time.
