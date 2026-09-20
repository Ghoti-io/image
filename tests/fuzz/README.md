# PNG/APNG, JPEG and BMP fuzz harnesses

**New codecs:** Add at least (1) a load (and decode) fuzz harness so that arbitrary or truncated input does not crash and returns appropriate errors; (2) if the codec supports save, a round-trip fuzz harness (load→save→load). PNG and JPEG are the reference; see `documentation/development.md` (Fuzzing) for the same requirement.

**fuzz_png_load**: LibFuzzer harness for PNG/APNG load and decode. Ensures the parser and decoder do not crash on random or truncated input and return appropriate errors (e.g. `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, `GIMG_ERR_LIMIT`).

**fuzz_png_encode**: Round-trip harness (load -> decode all items -> save -> load). Stress-tests the encoder; invalid input that fails load or save is ignored (no crash).

**fuzz_jpeg_load**: LibFuzzer harness for JPEG load and decode. Same contract: no crash on random or truncated input; return `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT` as appropriate. Covers baseline, progressive (SOF2), and 12/16-bit (SOF1/SOF2) when such files are in the corpus.

**fuzz_jpeg_encode**: Round-trip harness for JPEG (load -> decode all items -> save -> load). Stress-tests the JPEG encoder; invalid input that fails load or save is ignored (no crash).

**fuzz_bmp_load**: LibFuzzer harness for BMP load and decode. BMP is the parser here that most directly indexes a buffer from sizes the header supplied - the row stride from `biWidth` and `biBitCount`, a palette index against the entry count, an RLE run against a row - so it is the one that most needs the sanitizers pointed at it.

**fuzz_bmp_encode**: Round-trip harness for BMP (load -> decode -> save as BMP -> load). Note that `gimg_doc_load` dispatches on the bytes rather than on the harness's name, so this feeds the BMP *writer* rasters decoded from PNG and JPEG too.

## Build

From the image library root, with clang available:

```bash
make fuzz-png          # PNG load/decode only
make fuzz-png-encode   # PNG round-trip load/save/load
make fuzz-jpeg         # JPEG load/decode only
make fuzz-jpeg-encode  # JPEG round-trip load/save/load
make fuzz-bmp          # BMP load/decode only
make fuzz-bmp-encode   # BMP round-trip load/save/load
```

These build `build/<build-dir>/apps/fuzz_*` with `-fsanitize=fuzzer`.

## Corpus

Seed the corpus with valid images so the fuzzer can mutate them.

PNG:
```bash
mkdir -p tests/fuzz/corpus
cp tests/data/png/png_1x1_gray.png tests/data/png/png_1x1_rgba.png tests/fuzz/corpus/
```
Or use the full set from `tests/data/png/` (after running `python3 tests/data/png/generate.py`).

JPEG:
```bash
cp tests/data/jpeg/*.jpg tests/fuzz/corpus/
```

BMP: seed from `tests/data/bmp/` (after running `python3 tests/data/bmp/generate.py`), and from an unpacked copy of Jason Summers' bmpsuite, which carries header versions and malformations the hand-written fixtures do not reach:
```bash
cp tests/data/bmp/*.bmp tests/fuzz/corpus/
```
(After running `python3 tests/data/jpeg/generate.py`.) For broader coverage (Phase 2.2), include progressive and 12/16-bit JPEGs: copy from `tests/out/jpeg/` after running encode tests (e.g. `progressive_default.jpg`, `baseline_gray16.jpg`, `baseline_gray12.jpg`) so the fuzzer exercises multi-scan and extended-precision decode paths without crash.

## Run

Run in CI or periodically:

```bash
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_png_load tests/fuzz/corpus
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_png_encode tests/fuzz/corpus
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_jpeg_load tests/fuzz/corpus
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_jpeg_encode tests/fuzz/corpus
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_bmp_load tests/fuzz/corpus
LD_LIBRARY_PATH="build/linux/release/apps:../compress/build/linux/release/apps" \
  build/linux/release/apps/fuzz_bmp_encode tests/fuzz/corpus
```

Without a corpus directory, the fuzzer runs with no seeds (slower to find coverage). Use `-max_total_time=N` to limit run time.

## What the sanitizers cannot see

**A read of freed memory that happens inside `libghoti.io-compress` is
invisible to AddressSanitizer.** That library is linked from `PREFIX` as an
ordinary build; only this library's own objects are instrumented, and ASan
cannot poison or check an access made by code it did not compile. Its
interceptors cover `memcpy` and friends, so a clobber *through* one of those
is still caught - but a plain loop inside the dependency is not.

This is not hypothetical. The PNG writer once handed
`gimg_png_write_color_from_info` an `icc_bytes` pointing into a raster the
save had already destroyed. The profile was deflated by `gcomp_encode_buffer`,
which read the freed buffer from inside the dependency, so:

- every harness ran the input clean, including a fresh ASan build aimed
  straight at a file that reproduced it;
- the defect was visible only in the *output*, where the first sixteen bytes
  of the profile had become glibc's free-list pointer and the rest was intact.

The JPEG writer had the same defect on the same day and ASan found it in
seconds - because there the profile went through a `memcpy` in this library's
own code.

So: **anything handed to the compress library is unchecked ground.** Do not
read "the fuzzers are clean" as "the buffers we pass out are live". Where a
buffer crosses that boundary, own it. And check what came out: the round-trip
tests and `tests/data/*/verify_*_output.py` compare bytes, which is what
caught this one.

Building the compress dependency with the same sanitizers and linking that
build into the fuzz and ASan targets would close the gap. It is a cross-repo
change and has not been made.
