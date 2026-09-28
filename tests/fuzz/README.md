# PNG/APNG, JPEG, BMP, GIF and TIFF fuzz harnesses

**New codecs:** Add at least (1) a load (and decode) fuzz harness so that arbitrary or truncated input does not crash and returns appropriate errors; (2) if the codec supports save, a round-trip fuzz harness (load→save→load). PNG and JPEG are the reference; see `documentation/development.md` (Fuzzing) for the same requirement.

**fuzz_png_load**: LibFuzzer harness for PNG/APNG load and decode. Ensures the parser and decoder do not crash on random or truncated input and return appropriate errors (e.g. `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, `GIMG_ERR_LIMIT`).

**fuzz_png_encode**: Round-trip harness (load -> decode all items -> save -> load). Stress-tests the encoder; invalid input that fails load or save is ignored (no crash).

**fuzz_jpeg_load**: LibFuzzer harness for JPEG load and decode. Same contract: no crash on random or truncated input; return `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT` as appropriate. Covers baseline, progressive (SOF2), and 12/16-bit (SOF1/SOF2) when such files are in the corpus.

**fuzz_jpeg_encode**: Round-trip harness for JPEG (load -> decode all items -> save -> load). Stress-tests the JPEG encoder; invalid input that fails load or save is ignored (no crash).

**fuzz_bmp_load**: LibFuzzer harness for BMP load and decode. BMP is the parser here that most directly indexes a buffer from sizes the header supplied - the row stride from `biWidth` and `biBitCount`, a palette index against the entry count, an RLE run against a row - so it is the one that most needs the sanitizers pointed at it.

**fuzz_bmp_encode**: Round-trip harness for BMP (load -> decode -> save as BMP -> load). Note that `gimg_doc_load` dispatches on the bytes rather than on the harness's name, so this feeds the BMP *writer* rasters decoded from PNG and JPEG too.

**fuzz_gif_load** / **fuzz_gif_encode**, **fuzz_tiff_load** / **fuzz_tiff_encode**: The same pair for GIF and TIFF.

## Build and run

From the image library root, with clang available:

```bash
make fuzz-png                  # build fuzz_png_load
make fuzz-run-png_load         # run it for FUZZ_TIME seconds (default 60)
make fuzz FUZZ_TIME=300        # build and run every harness
```

These build `build/<build-dir>/apps/fuzz_*` with `-fsanitize=fuzzer,address,undefined`.

## Corpus

Each harness has its own directory under `tests/fuzz/corpus/<name>/`, matching the rest of the suite. Only hand-built `*.seed` files are tracked; what a campaign writes beside them is gitignored. After a campaign, `git status` in this library stays clean.

Seeds are small fixtures (and a few named regression inputs) copied in with a `.seed` suffix. Encode harnesses also carry seeds of other formats, because `gimg_doc_load` dispatches on the bytes and the writer under test should see rasters that arrived from somewhere else.

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

## Slow units are usually the sanitizers, not a defect

libFuzzer writes a `*-slow-unit-*` artifact when an input takes longer than a
second. Under ASan and UBSan that is roughly twenty to thirty times the
release cost, so an input that takes 40 ms in a release build trips it.

Every slow unit a thirty-minute six-harness run produced was a JPEG, and all
but one ran in 5 to 46 ms without the sanitizers. **Time one in a release
build before treating it as a complexity problem.**

The exception is worth knowing about: a 92-byte arithmetic **lossless** JPEG
whose SOF11 names 16385 by 219 - 3.59 megapixels, just under the harnesses'
`max_decoded_pixels` - takes about half a second to a second in release.
Arithmetic decoding is serial by construction and libjpeg's is slow too;
nothing here is quadratic. What bounds it is `max_decoded_pixels`, which is
why the harnesses set one (see `fuzz_limits.h`) and why a service should.
