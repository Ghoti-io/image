# JPEG test data

Reference JPEG files for Phase 2 codec and golden tests.

**Fixture generation:** Run `python3 tests/data/jpeg/generate.py` from the repo root. Pillow is required for generation; `piexif` is optional for EXIF orientation. See `documentation/development.md` (Prerequisites).

**Decode oracle:** The `Decode*PillowOracle` and `.raw`-comparison tests compare **actual pixels** (raster) to the oracle’s .raw output, not hashes, so failures report the first differing pixel (e.g. `First pixel diff at (x,y): ours=(...) oracle=(...)`). By default they use **Python (Pillow)** via `decode_oracle_pillow.py` and `encode_oracle_pillow.py` in this directory—no libjpeg required. Require Pillow: `pip install Pillow`. If the Python scripts are missing or fail, the tests try the **libjpeg-based** oracle built by `make jpeg-oracle-tools` from **`tests/tools/jpeg-oracle/`**, when `GIMG_JPEG_ORACLE_DIR` points at its `build/` dir. The image library does not link to libjpeg; the C oracle is optional for instrumented debugging (e.g. bit-exact match with libjpeg).

**Encode verification:** After unit tests, `make test` runs `verify_jpeg_output.py`, which uses PIL to verify that JPEGs written to `tests/out/jpeg/` are valid (SOI, structure, optional dimension/SOF expectations). JPEG is lossy; we do not compare encode output to pre-encode source. Run verification only: `make test-verify-jpeg`.

## Oracle tools

The libjpeg oracle tools are small C programs we wrote; they link system libjpeg,
which the image library itself does not. Sources are tracked in
**`tests/tools/jpeg-oracle/`**; `make jpeg-oracle-tools` builds them into
`tests/tools/jpeg-oracle/build/` and prints the `GIMG_JPEG_ORACLE_DIR` to export.
Building them needs the libjpeg **headers** (Debian/Ubuntu: `libjpeg-dev`); the
runtime library alone is not enough. They are optional: every test that reaches
for an oracle tries Pillow first and skips if neither is available.

| Tool | State |
| --- | --- |
| `dump_jpeg_pixels_ref` | source present; built by `make jpeg-oracle-tools`; verified |
| `dump_jpeg_coef_ref` | source present; built by `make jpeg-oracle-tools` |
| `dump_jpeg_raw_first_mcu` | **source lost** |
| `encode_libjpeg_baseline_scan` | **source lost** |
| `encode_libjpeg_baseline_rgb` | **source lost** |
| `encode_libjpeg_1x1_rgb` | **source lost** |

**Verified** means regenerated against what is tracked: of the 54 `.raw`
fixtures with a JPEG beside them, 45 come back byte-identical and 9 are
refused by libjpeg itself (three at twelve-bit precision, six with more
components than it will decode, both covered by their own oracles below).
Running `generate_jpeg_oracle_raws.py` over the whole fixture directory
reproduces 60 tracked `.raw` files byte for byte and differs on none.

Scripts below that drive a `*_debug` tool need both a lost source and an
instrumented libjpeg built by hand; they are kept as a record of how the
progressive decoder was debugged, and cannot be run as written.

### The decoder half of these scripts is gone too

Most of the comparison scripts drive our decoder through environment
variables - `DUMP_JPEG_COEF_AFTER_SCAN`, `GIMG_JPEG_TRACE_ALL`,
`DUMP_JPEG_COMPONENTS` and the rest. **Twenty-one of those no longer exist in
the library.** They were removed from the decoder at some point and nothing
updated the scripts or this file, so a script whose oracle is present still
compares against nothing: our side emits no lines at all. The three trace
variables the decoder does still read - `GIMG_JPEG_TRACE_BASELINE_BIT_POS`,
`GIMG_JPEG_TRACE_ENTROPY`, `GIMG_JPEG_TRACE_FIRST_CB` - are named by no
script and no document here.

So fixing where these scripts look for their oracle, which was worth doing
because the lookup was wrong in its own right, does not make them runnable.
Reviving one means restoring the decoder instrumentation it was written
against.

**As of 2026-09-22 that instrumentation is not merely unreachable - it is
gone.** The compile-time macros behind it (`GIMG_JPEG_TRACE_ALL`,
`GIMG_JPEG_DUMP_AC_INITIAL_FULL`, `GIMG_JPEG_DUMP_AC_REFINE_FULL` and sixteen
others) and the 785 lines they guarded were deleted from `jpeg_block.c`,
`jpeg_entropy.c`, `jpeg_bitstream.c` and `jpeg_encode.c`. Nothing could turn
them on in any case: the four progressive block decoders took their trace
settings as parameters and the single call site of each passed `0` and `-1` as
literals, so no `-D` and no environment variable reached them. Reviving one of
these scripts now means writing the decoder side again, against a decoder that
is bit-identical to libjpeg on every progressive fixture here - which is the
reason the instrumentation had stopped being worth its weight.

### What does still run: compare_progressive_pixels.py

It compares pixels rather than internals, so it needs no decoder
instrumentation - only `dump_jpeg_pixels_ref` and `dump_jpeg_raster`. Across
all 22 progressive fixtures here, our decoder is **bit-identical to libjpeg
on 14**, including every complete file, both arithmetic-coded ones, the
restart-interval one, and every grayscale truncation.

The eight that differ are `progressive_sample_2scan` through `_9scan`:
colour files deliberately cut short mid-progression. The difference is 4 of
255 at two through five scans, 2 at six through nine, and **zero at ten,
where the scan sequence is complete** - so the two decoders converge exactly
as the refinement bits arrive, and differ only in how they render a
progression that stops early. No test uses those eight fixtures. This is
recorded as a measurement, not diagnosed: what a decoder should show for
coefficients whose refinement never arrived is a quality choice, and ours is
exact wherever the file is whole.

Compiled copies of all six were once committed under `tests/data/jpeg/`. They were
untracked: they are build output, they were checked in without the sources that
build them, and four of the six had no source in the repo at any commit. Scripts
in this directory that drive a lost tool are kept as a record of how the
progressive decoder was debugged, but cannot be run until someone rewrites it.

**Decoder oracle (.raw):** To test that our decoder matches an external oracle: (1) Oracle decodes a fixture JPEG and writes raw pixels to a `.raw` file. (2) Our decoder reads the same JPEG and decodes. (3) The test compares our decode to the `.raw` byte-for-byte. Generate oracle `.raw` files with `make jpeg-oracle-tools` (then set `GIMG_JPEG_ORACLE_DIR` as that target prints), then run `python3 tests/data/jpeg/generate_jpeg_oracle_raws.py`. CMYK fixtures require the oracle.

Grayscale and RGB fixtures use Pillow to produce `.raw`; CMYK fixtures use libjpeg (`dump_jpeg_pixels_ref -o <base>.raw <file.jpg>`). `.raw` format: 1 byte mode (0=L, 1=RGB, 2=CMYK), 4 bytes width LE, 4 height LE, then pixels. Tests such as `DecodeBaselineGrayOracleRaw` and `GoldenCmyk` load the corresponding `.raw` and compare with zero tolerance for a clear picture of any difference. **Direct libjpeg comparison:** `DecodeJpegWithIccVsLibjpeg` runs `dump_jpeg_pixels_ref -o <path> jpeg_with_icc.jpg` at test time and compares our decoder output to that `.raw` byte-for-byte, so you can compare what libjpeg decodes vs what we decode on the same file.

## Generated fixtures (feature matrix)

Each combination below has a sample `.jpg`; generate matching `.raw` oracles with `generate_jpeg_oracle_raws.py` so decode tests can compare byte-for-byte.

| File | Description |
|------|-------------|
| `baseline_8x8_gray.jpg` | 8×8 grayscale baseline JPEG |
| `baseline_16x16_ycbcr.jpg` | 16×16 RGB baseline (stored as YCbCr) |
| `baseline_9x9_gray.jpg` | 9×9 grayscale (edge dimensions) |
| `baseline_8x16_gray.jpg` | 8×16 grayscale (edge dimensions) |
| `baseline_8x8_gray_q50.jpg` | 8×8 gray quality 50 |
| `baseline_8x8_gray_q100.jpg` | 8×8 gray quality 100 |
| `progressive_sample.jpg` | 16×16 progressive DCT JPEG |
| `progressive_32x32.jpg` | 32×32 progressive (multi-MCU) |
| `progressive_8x8_gray.jpg` | 8×8 grayscale progressive |
| **`baseline_640x480_gray.jpg`** | **640×480 grayscale baseline (non-trivial size)** |
| **`baseline_640x480_ycbcr.jpg`** | **640×480 RGB baseline (non-trivial size)** |
| **`progressive_640x480_ycbcr.jpg`** | **640×480 progressive RGB** |
| `jpeg_exif_orientation.jpg` | 8×8 gray with APP1 EXIF Orientation=6 (90° CW); requires `pip install piexif` when generating |
| `jpeg_with_icc.jpg` | 8×8 RGB with APP2 ICC profile (minimal) |
| `cmyk_sample.jpg` | 8×8 CMYK baseline JPEG |
| `baseline_gray12.jpg` | 16×16 grayscale 12-bit (SOF1); from encoder test `SaveGray12ThenLoadDecode` (optional; if absent, `Decode12BitFixture` skips) |

**Decode coverage:** `DecodeFixtureOraclesRaw` decodes every **baseline** (and EXIF/ICC/CMYK) fixture and compares to its `.raw`; progressive fixtures are covered by `Decode*PillowOracle` (libjpeg hash). `DecodeBaseline640x480Ycbcr` asserts successful decode and 640×480 dimensions for non-trivial size coverage (no Pillow .raw pixel comparison for that fixture).

**Progressive Pillow fixture (failing until implemented):** `DecodeProgressivePillowOracle` and `GoldenProgressive` in `testJpeg_load` **assert** that `gimg_item_decode` succeeds on Pillow-generated `progressive_sample.jpg`; they fail with "progressive decoder must decode Pillow fixture progressive_sample.jpg (feature not implemented)" until the progressive decoder supports that file. Other progressive tests (e.g. our own encoded progressive files) may still pass.

**12-bit decode fixtures:** 12-bit JPEG (SOF1 or SOF2, precision 12, DQT Pq=1) is produced by our encoder; encode tests write `baseline_gray12.jpg`, `baseline_rgb12.jpg`, `progressive_gray12.jpg`, `progressive_rgb12.jpg` to `tests/out/jpeg/`. To exercise the 12-bit decode path without running encode tests, copy e.g. `baseline_gray12.jpg` from `tests/out/jpeg/` to this directory. The test `Decode12BitFixture` loads it if present and asserts format and dimensions; if the file is absent, the test is skipped. Note that it checks dimensions and bit depth only — **no pixel comparison** — because Pillow's libjpeg is an 8-bit build and cannot open a 12-bit file at all. A real 12-bit oracle needs libjpeg built with `BITS_IN_JSAMPLE=12`, or IJG v10 built for 12-bit.

**12-bit status:** every quality from 1 to 100 round-trips through this library, and every one of them is byte-identical to what libjpeg-turbo's 12-bit build decodes from the same file. Quality 100 used to be refused at save time, on the grounds that the resulting file could not be read back; the fault was in the extended Huffman tables, not in the quantizer, and it is fixed.

The earlier failures had two causes, both since fixed: the FDCT overflowed at 12-bit sample range (its intermediates are sized for 8-bit), and the extended AC Huffman tables were over-subscribed - their bit counts needed 65615 codes of length 16 where only 65536 exist, so 79 symbols had no valid code and the decoder resolved them to something else. The tables now carry the 226 symbols T.81 F.1.2.2 actually defines at 12-bit (EOB, ZRL, and RRRR/SSSS for SSSS 1..14) and satisfy the Kraft inequality; the DC tables carry 16 (SSSS 0..15). They are generated by Huffman's algorithm with the 16-bit length limit of B.2.4.2, using the Annex K table as the frequency model so common symbols keep short codes.

**The 12-bit oracle:** Pillow's libjpeg is an 8-bit build and cannot open a 12-bit file at all, so for a long time nothing but this library had read our 12-bit output. That is no longer so: libjpeg-turbo 3.x built for 12-bit is the reference the `*_ljt_*` fixtures here were generated against, and comparing against it is what found four defects the 12-bit path had been carrying. See *The 12-bit oracle* below for how to build it. Round-trip and value checks (`Save12BitFlatFieldsKeepTheirValue`, `Save12BitRampKeepsItsContrast`) run beside it rather than in place of it.

**On 16-bit:** earlier revisions encoded and decoded "16-bit JPEG" as SOF2 with precision 16 and a 242-symbol AC table, and the documentation described this as being per T.81. **It is not.** T.81 Table B.2 gives every DCT-based frame a sample precision of 8 or 12; precision 2..16 belongs to lossless (SOF3) alone. There is no 16-bit DCT JPEG to be compatible with, which is why no tool encodes or decodes one and why IJG reports "Bogus Huffman table definition" on such files — IJG is right. The support has been removed: `jpeg_precision = 16` now returns `GIMG_ERR_UNSUPPORTED`, a 16-bit raster is written at 12-bit, and a frame declaring precision 16 is rejected on load (`Sof2Precision16Rejected`). If you want more than 8 bits per sample in a JPEG, 12-bit is the standard answer.

**manifest.json** — Written by `generate.py`; lists each fixture with `file`, `width`, `height`, `mode`, `progressive`, `quality`, `has_exif`, `has_icc`.

**Encoder reference (libjpeg “golden” for SaveRgbThenLoadDecode):** `baseline_rgb_reference.jpg` is the **libjpeg** encoding of the same 8×8 RGB image and settings as the test (R=x*32, G=y*32, B=128; baseline, Q85, 4:2:0). It cannot currently be regenerated: the tool that produced it, `encode_libjpeg_baseline_rgb`, has no source in the repo (see **Oracle tools** below).

**Generated by scripts (gitignored):** Running compare_progressive_bits.py or trace comparisons produces `trace_ours.txt` / `trace_ref.txt`. `create_libjpeg_progressive_fixture.py` produces `progressive_8x8_libjpeg.ppm` and `progressive_8x8_libjpeg.jpg`. These are listed in `.gitignore` and should not be committed.

## Systematic corpus

`generate_matrix.py <dir>` writes the cross-product rather than a hand-picked
list: sizes from 1x1 to 256x64 including 1xN, Nx1 and non-MCU-aligned shapes,
grayscale and RGB, 4:4:4 / 4:2:2 / 4:2:0, baseline and progressive, noise /
ramp / edges / flat content, qualities 10 / 50 / 90, and restart intervals of
1, 2, 3, 5, 8 and 17 - 1188 files.  `compare_matrix_to_libjpeg.py <dir>/*.jpg`
decodes each with our decoder and with Pillow and reports every file that is not
byte-identical.

Current result: **1182 of 1188 byte-exact, 0 failures to decode.**  The six that
differ are 17x1 4:2:2 noise, where scattered samples differ by at most 2 of 255;
the cause is not identified and the geometry is degenerate (one pixel tall).
Every other combination in the matrix is exact.

These files are not committed - they are generated on demand, and Pillow is the
oracle.  Run them when changing the decoder; the hand-picked fixtures in this
directory are too few and too uniform to catch geometry bugs on their own, which
is how a broken restart interval and a broken progressive scan order both
survived in tree.

## Scripts kept for verification

- **compare_coef_hash.py** — Compare our decoder’s final coefficient hash to `dump_jpeg_coef_ref`.
- **compare_progressive_pixels.py** — Compare decoded **pixels** (libjpeg ref vs our decoder). Reports first differing pixel (x,y), ref vs ours (R,G,B), and total differing count / max_abs_diff. Use `--verbose` to print the first row of ref and ours. Prefer this over hash-only comparison to see actual values and how far off we are. Usage: `python3 compare_progressive_pixels.py [--verbose] <file.jpg> [dump_jpeg_raster] [dump_jpeg_pixels_ref]`.
- **compare_progressive_bits.py** — Bit-level comparison for a given (scan, comp, block): HUFF_BIT + refinement + correction bits from our trace vs ref (when ref emits REF_HUFF_BIT / REF_AC_REFINE_*). Supports **scan 1** (AC initial) and **scan 2** (AC refinement) for a 3-scan file (DC=0, AC initial=1, refinement=2). Usage: create `progressive_refinement.jpg` by running the test `ProgressiveWithRefinementScanDecodeMatchesBaseline` once; then run our decoder with `GIMG_JPEG_TRACE_ALL=1` and capture stderr to `trace_ours.txt`; run ref with `LIBJPEG_TRACE_HUFF_BITS=1` and `LIBJPEG_DEBUG_AC_TABLE=1`, stderr to `trace_ref.txt`; then `python3 compare_progressive_bits.py --ours trace_ours.txt --ref trace_ref.txt --scan 1 --comp 0 --block 0` (AC initial) or `--scan 2 --block 0` (refinement). Ref sets its AC-initial trace start once per scan (in instrumented jdphuff) so REF_HUFF_BIT for block 0 is comparable. First divergence reported (e.g. AC initial bit index 1) indicates where our decoder diverges from libjpeg.
- **compare_progressive_trace.py** — Compare our AC_INITIAL trace to libjpeg's (ref) with `TRACE_REF_AC=1`.
- **compare_progressive_trace_all.py** — Compare full trace (`GIMG_JPEG_TRACE_ALL=1`) to libjpeg: AC_INITIAL lines and first-MCU block coefficients after each scan (ref needs `TRACE_REF_AC=1`, `DUMP_JPEG_COEF_AFTER_SCAN=1`, `DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1`). Requires `dump_jpeg_coef_ref_debug` (instrumented libjpeg).
- **compare_progressive_all_blocks.py** — Compare **all** coefficient blocks (ours vs ref) after each scan to find the first divergence. Ours: `GIMG_JPEG_TRACE_ALL=1`. Ref: `DUMP_JPEG_COEF_AFTER_SCAN=1` and `DUMP_JPEG_COEF_ALL_BLOCKS_AFTER_SCAN=1` (instrumented libjpeg in `third_party/libjpeg-turbo` must be built with the all-blocks dump in jdtrans.c). Usage: `python3 compare_progressive_all_blocks.py --ours trace_ours.txt --ref trace_ref.txt [--verbose]`.
- **compare_blocks_after_scans.py** — Per-scan block comparison (bitstream vs decoder).
- **compare_progressive_blocks.py** — First-MCU coefficient blocks (OUR vs REF) after each scan.
- **compare_first_mcu_after_scans.py**, **compare_first_mcu_coef.py** — First-MCU / coefficient comparison (truncated scans, bisect).
- **trace_scan_ac_initial_block.py**, **trace_scan1_bitstream_positions.py** — Bitstream position traces for AC-initial (used by run_scan_ac_initial_trace_compare.py).
- **compare_ref_ours_components.py** — Compare libjpeg ref Y/Cb/Cr dumps (DUMP_JPEG_COMPONENTS_REF) to our decoder dumps (DUMP_JPEG_COMPONENTS). Usage: `python3 compare_ref_ours_components.py <ref_dir> <ours_dir>`.
- **dump_jpeg_raw_first_mcu** — Decode first MCU with libjpeg `raw_data_out` and write ref_Y_block0.bin … ref_Cr_block0.bin (64 bytes each). **No source in the repo** (see **Oracle tools** below); the description is kept as a record of what it did. Usage was: `dump_jpeg_raw_first_mcu <file.jpg> [output_dir]`. Compare to our DUMP_JPEG_COMPONENTS Cr_block0.bin (bytes 256–319 = STORED) to confirm IDCT/component match.
- **compare_first_block_natural.py**, **compare_ycbcr_components.py** — Debug/regression.
- **verify_first_scan_dc.py**, **verify_dc_refine_first_mcu.py**, **verify_first_ac_initial.py**, **verify_second_ac_initial_first_block.py**, **verify_ac_refine_first_block.py** — Bitstream/decode checks.
- **run_*_verification.py** — Runners that compare script output to decoder trace.
- **verify_dc_only_pipeline.py**, **verify_script_vs_libjpeg.py** — Pipeline and script-vs-ref checks.
- **dump_scan_bytes.py** — Dump raw scan bytes (loader vs script).
- **create_libjpeg_progressive_fixture.py** — Optional: create progressive fixture with cjpeg.
- **sanity_check_libjpeg_baseline_progressive.py** — Sanity check: (1) libjpeg encodes same 16×16 image as baseline and progressive; (2) do those two decodes (libjpeg) produce the same raster? (3) does our decoder match libjpeg decode for each? Requires `encode_libjpeg_baseline_scan`, `cjpeg`, `dump_jpeg_pixels_ref`, `dump_jpeg_raster`. Usage: `python3 sanity_check_libjpeg_baseline_progressive.py [--work-dir /tmp/jpeg_sanity]`.
- **sanity_check_encoder_libjpeg.py** — Encoder sanity: (1) baseline — our scan bytes vs libjpeg (same 16×16 (x+y)&0xFF, Q85); run with `GIMG_JPEG_DUMP_SCAN_BASELINE`; (2) progressive — our 3-scan file vs cjpeg default (file sizes; structure differs). Run from **image/**: `python3 tests/data/jpeg/sanity_check_encoder_libjpeg.py [--work-dir ...]`.
- **encode_libjpeg_baseline_rgb.c** — Encode 8×8 RGB (R=x*32, G=y*32, B=128) with libjpeg baseline, Q85, 4:2:0; write scan 0 bytes and optionally full JPEG. Same image/settings as **SaveRgbThenLoadDecode**. **No source in the repo** (see **Oracle tools** below).
- **compare_save_rgb_encode.py** — Compare our 8×8 baseline RGB encode to libjpeg: runs SaveRgbThenLoadDecode with `GIMG_JPEG_DUMP_SCAN_BASELINE`, runs encode_libjpeg_baseline_rgb, then compares scan bytes (and optionally `--structure` for marker order). Run from **image/**: `python3 tests/data/jpeg/compare_save_rgb_encode.py [--structure]`.
- **compare_large_grayscale_encode.py** — Compare our 640×480 baseline grayscale encode to the reference (encode_libjpeg_baseline_scan, same image (x+y)&0xFF, Q85). Dump is written during save (test fails at decode). Run from **image/**: `python3 tests/data/jpeg/compare_large_grayscale_encode.py [--work-dir ...]`. Use `--skip-ours` to reuse an existing our-scan dump. Reports first differing byte and approximate block index. **Investigation (Feb 2025):** (1) First divergence was at scan byte 4; root cause was **RGB→YCbCr** using different integer coefficients (77/150/29 vs libjpeg’s FIX-scaled 19595/38470/7471). **Fix:** `jpeg_rgb_to_ycbcr` in jpeg_save.c now uses libjpeg’s CCIR 601-1 coefficients and rounding so block 0 and first 10 scan bytes match. (2) Divergence at byte 11 (block 1): **dummy blocks** — libjpeg uses zero blocks with DC = previous block’s DC for right/bottom edge blocks; we were replicating edge pixels. **Fix:** In `gimg_jpeg_progressive_fill_coef_buffer` and `_16bit` (jpeg_entropy.c), blocks with `blk_x >= comp_width_px || blk_y >= comp_height_px` are now emitted as all-zero with `block_zz[0] = last_dc[c]`. Scan bytes now match through byte 12. (3) Byte 13 = start of **block 4 (Cb)**. **Fixes applied:** (a) 4:2:0 chroma downsampling now uses libjpeg’s alternating bias (1,2,1,2 per column) in jpeg_save.c. (b) Cb/Cr block sampling uses full buffer dimensions (`comp_width_samp`/`comp_height_samp` from stride and 8×vc) so the 8×8 block is filled from the full 8×8 downsampled buffer, not 4×4 replicated. Cb block coefficients are closer; small residual difference (DC/AC) may be quant scaling or rounding. Segment order differs (we emit DHT before SOF0, libjpeg SOF0 before DHT — both valid per T.81).

Reference or third-party source trees used for debugging (e.g. a copy of libjpeg) belong in the repo root’s **third_party/** directory, which is gitignored. Oracle tools we wrote ourselves are not third-party source and do not go there: they live in **tests/tools/jpeg-oracle/**, which is tracked. Only the binaries they build are ignored. The decode oracle uses **system libjpeg** via the ref tools above, not anything in third_party. To debug with an instrumented libjpeg (e.g. TRACE_REF_AC, DUMP_JPEG_COEF_AFTER_SCAN): (1) **Link against build tree (no install):** from `image/third_party/libjpeg-turbo`, `mkdir -p build-debug && cd build-debug && cmake .. && make`; then from `image`, `make jpeg-oracle-tools-debug-build`. (2) **Or install then link:** same build, then `make install` (e.g. prefix `../../libjpeg-debug`) and from `image` run `make jpeg-oracle-tools-debug`. Our decoder: `DUMP_JPEG_QUANT_IDS=1` logs per-component quant table ID and first values (SOF Tqi); `DUMP_JPEG_COMPONENTS=<dir>` writes Y/Cb/Cr raw (see compare_ycbcr_components.py). **Ref component dump:** Build instrumented libjpeg (`make jpeg-oracle-tools-debug-build`), then run `dump_jpeg_pixels_ref_debug` with `DUMP_JPEG_COMPONENTS_REF=<dir>`; this writes `ref_Y.raw`, `ref_Cb.raw`, `ref_Cr.raw` (same format: 4b w LE, 4b h LE, raw). **compare_ref_ours_components.py** compares ref vs ours component dumps (e.g. `python3 compare_ref_ours_components.py /tmp/ref /tmp/ours`). Result for progressive_sample.jpg: Y/Cb/Cr component buffers **match** ref; any pixel hash difference is due to **chroma upsampling** (libjpeg default = fancy, ours = simple replication; both spec-valid). Ref: run with `TRACE_REF_AC=1`, `LIBJPEG_DEBUG_DHT=1`, `LIBJPEG_DEBUG_AC_TABLE=1`, `LIBJPEG_TRACE_HUFF_BITS=1`, or `LIBJPEG_DUMP_BLOCK0_BYTES=1` (dump actual bytes consumed for first AC refine block 0) as needed. With `LIBJPEG_TRACE_HUFF_BITS=1`, the ref emits `REF_HUFF_BIT` and `REF_HUFF_MATCH` lines (same idea as our `HUFF_BIT`/`HUFF_MATCH`) so you can compare bit-level Huffman decode; **compare_progressive_trace_all.py** sets this when running the ref. To compare our full decode trace to libjpeg, run our decoder with `GIMG_JPEG_TRACE_ALL=1` and use **compare_progressive_trace_all.py** against ref with the env vars above; the script compares AC_INITIAL lines and first-MCU block coefficients per scan.

**Comparing AC refinement decode step-by-step (bits/bytes/blocks):** The only reliable way to fix progressive decoder bugs is to dump bits, bytes, and block state at every step and compare with libjpeg. Use full-dump mode on both decoders, then diff the traces.

- **Our decoder (removed 2026-09-22 - see above; kept as a record of what the trace emitted):** `GIMG_JPEG_DUMP_AC_REFINE_FULL=1` dumped for every AC refinement block: `OUR_AC_REFINE_BLOCK_START` (scan, block, byte_off, bit_off, scan_size, nz_in_band, nz_at=...), then `OUR_AC_REFINE_HUFF`, `OUR_AC_REFINE_EOB_RUN` (for (r,0) path), `OUR_AC_REFINE_REFINEMENT_BIT`, `OUR_AC_REFINE_CORRECTION` (each with byte_off/bit_off), and `OUR_AC_REFINE_BLOCK_END`. Add `DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1` to get coefficient blocks after each scan (OUR_SCANn_COMPc_BLOCKb) so you can compare state before each refinement scan.
- **Ref (instrumented libjpeg):** `LIBJPEG_DUMP_AC_REFINE_FULL=1` and `LIBJPEG_TRACE_HUFF_BITS=1` so ref emits `REF_AC_REFINE_BLOCK_START` (block, byte_off, bit_off, nz_in_band, nz_at=...), `REF_HUFF_MATCH`, `REF_AC_REFINE_REFINEMENT_BIT`, `REF_AC_REFINE_CORRECTION_BIT`, and `REF_AC_REFINE_BLOCK_END` with byte_off/bit_off relative to scan start. Add `DUMP_JPEG_COEF_AFTER_SCAN=1` and `DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1` for REF_SCANn_COMPc_BLOCKb after each scan.

**Procedure:** From `image/`, build ref with `make jpeg-oracle-tools-debug-build`. Create the cjpeg progressive file: `python3 tests/data/jpeg/sanity_check_libjpeg_baseline_progressive.py --work-dir /tmp/jpeg_sanity`. Then:

```bash
# Ours (stderr only; stdout is raster)
LD_LIBRARY_PATH=build/linux/release/apps GIMG_JPEG_DUMP_AC_REFINE_FULL=1 DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1 \
  build/linux/release/apps/dump_jpeg_raster /tmp/jpeg_sanity/libjpeg_progressive_16.jpg 2> ours_ac_refine.txt

# Ref (stderr has REF_* and REF_SCAN*)
LIBJPEG_DUMP_AC_REFINE_FULL=1 LIBJPEG_TRACE_HUFF_BITS=1 DUMP_JPEG_COEF_AFTER_SCAN=1 DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1 \
  tests/data/jpeg/dump_jpeg_coef_ref_debug /tmp/jpeg_sanity/libjpeg_progressive_16.jpg 2> ref_ac_refine.txt
```

Compare: `diff -u <(grep -E '^OUR_AC_REFINE_|^OUR_SCAN' ours_ac_refine.txt) <(grep -E '^REF_AC_REFINE_|^REF_SCAN|^REF_HUFF_MATCH|^REF_COEF' ref_ac_refine.txt)` (or a small script that normalizes block indices if ref scan numbers differ). First divergence in byte_off/bit_off or nz_in_band/nz_at is the bug. If OUR_SCAN2 block 0 differs from REF_SCAN2 block 0, the bug is in an earlier scan (DC or AC initial).

When the entropy segment has only 2 bytes (e.g. scan 3 in the 16×16 cjpeg file: bytes at 249–250 then 0xFF 0xDA), our loader is correct (T.81 B.2.4); we underflow on block 1 and treat premature end as corrupt (we do not stuff zero bits). Libjpeg in `jpeg_fill_bit_buffer` sets `unread_marker` and stuffs zero bits (JWRN_HIT_MARKER), so its bytes_consumed can include the marker bytes. Use a file with sufficient entropy per scan to pass progressive tests.

**Dump analysis (16×16 cjpeg progressive):** AC-initial step-by-step matches ref (same decode order per T.81 Annex G). For AC refinement (scan 3), the test file has only **2 bytes** of entropy data for that scan (verified by scanning for SOS and next 0xFF in the file). Our decoder correctly assigns 2 bytes to scan 3, decodes block 0 (6 bits) then underflows on block 1 (remaining 10 bits are insufficient for (r,0) + EOBRUN + 3 correction bits, or we exhaust the buffer). Ref decodes all 4 blocks; ref’s byte_off is relative to its scan start and it may be reading from a single contiguous buffer rather than per-scan segments. Next step: confirm whether libjpeg’s data source is scan-bounded and, if so, compare bit consumption per block (HUFF + EOBRUN + correction bits) with ref to find any decode divergence.

**Comparing AC initial decode step-by-step (spec harmonization):** To find where our decoder diverges from libjpeg in scan 1 or 2 (e.g. block 1 ending up with 7 nonzero instead of 3), both decoders can emit a canonical step log so you can diff them line-by-line.

- **Our decoder (removed 2026-09-22 - see above; kept as a record of what the trace emitted):** `GIMG_JPEG_DUMP_AC_INITIAL_FULL=1` dumped `OUR_AC_INITIAL_STEP` for each logical step: `op=HUFF sym=0xNN`, `op=EOB`, `op=EOBRUN r=N eobrun=M`, `op=EOBRUN_SKIP eobrun=M`, `op=COEFF run=N size=N k=N val=N`, `op=ZRL`. Each line includes `scan=` (scan index), `block=` (global decode ordinal: 0,1,2,… across all AC-initial blocks), `byte=` and `bit=` (position after that step). T.81 Annex G is the authority; the trace is for comparison only.
- **Ref (instrumented libjpeg):** `LIBJPEG_DUMP_AC_INITIAL_FULL=1` dumps `REF_AC_INITIAL_STEP` with the same op values and the same `scan=` / `block=` / `byte=` / `bit=` semantics (byte/bit relative to start of that scan’s entropy data).

**Procedure (AC initial):** From `image/`, build ref with `make jpeg-oracle-tools-debug-build`. Create the cjpeg progressive file: `python3 tests/data/jpeg/sanity_check_libjpeg_baseline_progressive.py --work-dir /tmp/jpeg_sanity`. Then:

```bash
# Ours: AC initial steps only (first 16 blocks)
LD_LIBRARY_PATH=build/linux/release/apps:../compress/build/linux/release/apps GIMG_JPEG_DUMP_AC_INITIAL_FULL=1 build/linux/release/apps/dump_jpeg_raster /tmp/jpeg_sanity/libjpeg_progressive_16.jpg 2>&1 | grep '^OUR_AC_INITIAL_STEP' > ours_ac_initial.txt

# Ref: same (use LD_LIBRARY_PATH so ref tool finds instrumented libjpeg from jpeg-oracle-tools-debug-build)
LIBJPEG_DUMP_AC_INITIAL_FULL=1 tests/data/jpeg/dump_jpeg_coef_ref_debug /tmp/jpeg_sanity/libjpeg_progressive_16.jpg 2>&1 | grep '^REF_AC_INITIAL_STEP' > ref_ac_initial.txt

diff -u ours_ac_initial.txt ref_ac_initial.txt
```

First line that differs (op, sym, run, size, k, val, eobrun, or byte/bit) is the first divergence; fix our decoder to match the spec (and ref) at that step. **Normalized diff (ignore byte/bit):** `sed 's/ byte=[0-9]* bit=[0-9]*//' ours_ac_initial.txt > ours_norm.txt` and same for ref; then `diff -u ref_norm.txt ours_norm.txt`. The only difference should be the prefix (REF_ vs OUR_); if so, AC-initial decode is in sync and any progressive failure is in a later scan (e.g. AC refinement).

## The 12-bit oracle

Nothing outside this library could read a 12-bit file we wrote, for a long
time: the libjpeg that Pillow links is an 8-bit build and refuses a P=12 frame
outright, so the 12-bit path had no external check of any kind.  Build one:

```sh
curl -LO https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.0.4/libjpeg-turbo-3.0.4.tar.gz
tar xzf libjpeg-turbo-3.0.4.tar.gz
cmake -S libjpeg-turbo-3.0.4 -B ljt-build -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_SHARED=OFF -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF
cmake --build ljt-build --target cjpeg-static djpeg-static jpeg-static
```

libjpeg-turbo 3.x carries 8-, 12- and 16-bit codecs in one library and picks by
the frame's precision, so `cjpeg-static -precision 12` writes a 12-bit file and
`djpeg-static -pnm` decodes one to a PNM with a maxval of 4095.  That is the
reference the 12-bit fixtures here were generated against, and comparing
against it is what found the defects the 12-bit path had been carrying: a
color conversion that subtracted the level shift from Y and never added it
back, a row stride computed in the wrong unit, a chroma filter chosen from
plane dimensions rather than sampling factors, and a naive float inverse DCT
where every other decoder uses the integer one.

Compare in the frame's own precision, not in the raster's: our decoder widens
12-bit samples to its 16-bit raster (see `gimg_bitdepth_12_to_16`), so narrow
the raster back with the exact inverse before comparing, or the widening rule
gets tested instead of the codec.

`generate_12bit_matrix.py` builds a corpus with it and
`compare_12bit_to_libjpeg.py` checks every file against it:

```sh
GIMG_CJPEG12=ljt-build/cjpeg-static python3 generate_12bit_matrix.py /tmp/m12
GIMG_DJPEG12=ljt-build/djpeg-static \
  python3 compare_12bit_to_libjpeg.py /tmp/m12/*.jpg
```

352 of 352 byte-exact as of this writing, across gray and color, 4:4:4/4:2:2/
4:2:0, baseline and progressive, qualities 25/75/95/100, and sizes including
1x1, 1xN, Nx1 and non-MCU-aligned shapes.

Fixtures generated this way:

| File | Content |
| ---- | ------- |
| `baseline_rgb12_444.jpg` | 16x16 flat (3000, 1000, 2000) at P=12, 4:4:4, q95 |
| `baseline_rgb12_422_16x1.jpg` | 16x1 gradient at P=12, 4:2:2, q90 - the single-row upsampling case |

## The one difference that is not a defect

Eight `progressive_sample_Nscan` fixtures decode a little differently from
libjpeg - at most 4 counts out of 255, on the truncated progressions but never
on the complete 10-scan one.  That is libjpeg's **block smoothing**
(`do_block_smoothing`, jdcoefct.c `decompress_smooth_data`): where a progressive
scan has not yet sent a coefficient, it estimates the missing low-frequency
terms from the neighboring blocks' DC values, so an incomplete progression
looks smooth rather than blocky.  It is an optional display refinement and
appears nowhere in T.81.

This was a guess for a long time, and is now measured.  `djpeg -nosmooth` does
not test it - that flag is the fancy-upsampling switch - so the check has to
drive the library directly:

```c
jpeg_read_header(&cinfo, TRUE);
cinfo.do_block_smoothing = FALSE;    /* not reachable from djpeg's flags */
cinfo.do_fancy_upsampling = TRUE;
```

With smoothing off, all nine fixtures are byte-identical to our decoder, and
the complete 10-scan file is identical either way because there is nothing left
to estimate.  We do not implement it, deliberately: it changes samples the
standard says how to reconstruct.

## Arithmetic-coded fixtures

T.81 defines two entropy coders: the Huffman coding of Annex F and the adaptive
binary arithmetic coding of Annex D.  Both are normative, and a frame that uses
the second (SOF9 sequential, SOF10 progressive) is as much a JPEG as one that
uses the first.  It is rare only because the patents that once covered it - long
expired - kept it out of the early implementations everything else was built
from.

This library both reads and writes arithmetic-coded frames; the encoder is
selected with `GIMG_Save_Options.jpeg_arithmetic`.  A useful check that needs no
external oracle at all is to encode the same image both ways and compare what
comes back: both encoders are handed one coefficient buffer, so any difference
is a bug in one of them.  That is what found the Huffman encoder's zero-run
defect, which no decoder could have revealed.

libjpeg-turbo builds with arithmetic support by default, so the same oracle
serves here:

```sh
ljt-build/cjpeg-static -arithmetic -quality 75 -outfile out.jpg in.ppm
```

Decoding needs a small program rather than `djpeg`, because the fixtures are
compared against a decode with `do_block_smoothing` off, which djpeg does not
expose; see the section above.

| File | Content |
| ---- | ------- |
| `arith_gray_64x64.jpg` | SOF9 grayscale, quality 75 |
| `arith_rgb_64x64_420.jpg` | SOF9 color, 4:2:0 |
| `arith_rgb_17x9_422_restart.jpg` | SOF9 color, 4:2:2, restart interval 3, non-MCU-aligned |
| `arith_gray_1x1_empty_scan.jpg` | SOF9 with an entropy-coded segment of zero bytes |
| `arith_gray12_64x64.jpg` | SOF9 at P=12 |
| `arith_progressive_33x33_422.jpg` | SOF10 progressive, 4:2:2, non-MCU-aligned |
| `arith_progressive_restart_420.jpg` | SOF10 progressive, 4:2:0, restart intervals |
| `progressive_restart_420.jpg` | SOF2 progressive, 4:2:0, restart intervals (Huffman) |

The last three cover a hole the big corpus had: it contains 576 progressive
files and 576 with restart intervals, and not one file that has both.  Every
progressive image with a restart interval was undecodable, whichever entropy
coder it used, and nothing noticed.  Two independent causes:

- A restart marker is a hard resynchronization point (B.2.1), so bits the
  longest-match Huffman decode had read ahead of it have to be discarded.  They
  were not, so the first symbol of every interval after the first was decoded
  from bits belonging to the previous one.  Fixed-length reads push nothing
  back, which is why the DC scans survived this and the AC scans did not.
- DRI may appear between scans and change (B.2.4.4), and an encoder measuring
  its restart interval in MCU rows has to change it, because an interleaved
  scan and a single-component scan do not have the same number of MCUs in a
  row.  libjpeg writes a different DRI before nearly every scan of a subsampled
  progressive image; we kept the frame's first value and used it everywhere.

When generating a corpus, cross restart intervals with everything else rather
than testing them on their own.

The empty-scan fixture is worth keeping: D.2.9 has the decoder supply zero bytes
once it runs past the compressed data, so a frame whose every decision resolves
to the more probable symbol needs no bytes at all, and libjpeg writes none.  A
Huffman scan always has at least one.

## One comparator for every frame type

`compare_to_libjpeg_native.py` compares our decode against libjpeg at the
frame's own sample precision, for any frame this codec reads - baseline,
progressive, arithmetic, lossless, 8-, 12- or 16-bit.  It drives `ljdec.c`
(here) rather than `djpeg`, for two reasons: `djpeg` does not expose
`do_block_smoothing`, which libjpeg applies to progressive frames and we
deliberately do not implement, and above 8 bits libjpeg needs
`jpeg12_read_scanlines` or `jpeg16_read_scanlines` according to
`cinfo.data_precision` rather than the 8-bit entry point.

```sh
cc -O2 -o ljdec ljdec.c -I libjpeg-turbo-3.0.4 -I ljt-build ljt-build/libjpeg.a
GIMG_LJDEC=./ljdec python3 compare_to_libjpeg_native.py *.jpg
```

Note that it sets `GIMG_JPEG_FANCY_UPSAMPLE=1` for the dump tool.  That tool
defaults to the box filter and libjpeg defaults to the triangle one, so
comparing the two defaults reports a few counts of difference on every
subsampled file and says nothing about the codec.  It looks exactly like a real
defect, and it has been mistaken for one more than once in this codebase's
history.

## Lossless fixtures

T.81 Annex H is a predictive coding process, not a DCT one, and libjpeg-turbo
3.x can write it:

```sh
ljt-build/cjpeg-static -lossless <psv>[,<Pt>] -outfile out.jpg in.ppm
ljt-build/cjpeg-static -precision 16 -lossless 1 -outfile out.jpg in16.ppm
```

`psv` is the predictor selection value of Table H.1 (1–7) and `Pt` the point
transform.  Decoding these needs an oracle that reads at the frame's own
precision: `djpeg` will, but a program calling `jpeg_read_scanlines` will not -
above 8 bits libjpeg expects `jpeg12_read_scanlines` or `jpeg16_read_scanlines`
according to `cinfo.data_precision`.

| File | Content |
| ---- | ------- |
| `lossless_gray_psv1.jpg` | grayscale, predictor 1 (one-dimensional) |
| `lossless_rgb_psv4.jpg` | RGB, predictor 4 (two-dimensional) |
| `lossless_rgb_psv7_pt1.jpg` | RGB, predictor 7, point transform 1, 17x9 |
| `lossless_rgb_psv4_restart.jpg` | as psv4, with restart intervals |
| `lossless_gray16_psv1.jpg` | **16-bit** grayscale |
| `lossless_rgb12_psv4.jpg` | 12-bit RGB |

The restart fixture is the same image as `lossless_rgb_psv4.jpg` and must decode
to exactly the same samples.  It is worth having because a restart interval in a
lossless frame resets more than the entropy coder: the row an interval begins on
has no row above it that the interval may refer to, so that row is predicted
one-dimensionally, exactly as the first row of the image is.  Carrying the
prediction across the boundary decodes the first row of every interval after the
first as noise.

## Checking that "lossless" is true

The encoder side needs no golden values.  Encode a raster, decode it, and the
samples must be identical - not close.  `JpegEncode.LosslessRoundTripsExactly`
does that across all seven predictors, with and without restart intervals, for
GRAY8, RGBA8, GRAY12, RGBA12, GRAY16 and RGBA16.

Against libjpeg, the check is that `djpeg` recovers the original PNM byte for
byte:

```sh
ljt-build/djpeg-static -pnm -outfile back.pnm ours.jpg && cmp original.pnm back.pnm
```

Two things to know when doing that by hand.  `djpeg` needs no `-rgb` for our
color output - but it will refuse the file outright if the writer emits a JFIF
APP0, because JFIF declares three-component data to be YCbCr and libjpeg takes
that ahead of the Adobe marker.  A lossless frame stores RGB, so it carries the
Adobe marker and no JFIF.  And libjpeg's lossless decoder requires a restart
interval to be a whole number of MCU rows; ours rounds down to satisfy it.


## The lossless arithmetic oracle (SOF11)

libjpeg-turbo does not implement SOF11 - lossless coded with the arithmetic
coder of Annex D - so for a long time nothing here could check it.  The one
implementation that does is Thomas Richter's codec, the ISO group's own
reference for T.81:

```sh
git clone --depth 1 https://github.com/thorfdbg/libjpeg.git
cd libjpeg && ./configure && make final     # produces ./jpeg
```

It is used the way `cjpeg` and `djpeg` already are - a separate program run at
test time to produce reference files.  Nothing links against it and none of its
code is in this repository.

Two things to know before it will work:

- **It needs a patch to encode lossless at all.**  `Tables::InstallDefaultTables`
  builds quantization tables only for the lossy frame types, but `Scan` calls
  `Tables::QuantizationTableIndexOf` unconditionally to pick a DC table index,
  so every `-p` encode - including the one its own README documents - dies with
  "DQT marker missing, no quantization table defined".  Returning the index the
  surrounding logic would give when no tables are present fixes it:

  ```c++
  // codestream/tables.cpp, Tables::QuantizationTableIndexOf
  if (m_pQuant == NULL)
    return (separatechroma && component > 0) ? 1 : 0;   // was: JPG_THROW(...)
  ```

- **The predictor is fixed at 4.**  There is no command-line selection, so the
  other six are covered by round-trip and by libjpeg-turbo, which does let you
  choose (`cjpeg -lossless <psv>,<pt>`).

Encode and decode:

```sh
jpeg -p -a -c in.pgm out.jpg     # SOF11; -c keeps RGB out of YCbCr
jpeg -p -c    in.pgm out.jpg     # SOF3, the Huffman counterpart
jpeg -z 17 -p -a -c in.pgm out.jpg   # with a restart interval
jpeg out.jpg back.pgm            # decode
```

### What it settled

Lossless restart intervals are where the two reference codecs and a literal
reading of the spec come apart, and the fixtures `lossless_arith_restart.jpg`,
`lossless_arith_midrow.jpg` and `lossless_huff_midrow.jpg` exist to pin it down.

H.1.2.1 says both of these:

> The one-dimensional horizontal predictor (prediction sample Ra) is used for
> the first line of samples at the start of the scan and at the beginning of
> each restart interval.

> At the beginning of the first line and at the beginning of each restart
> interval the prediction value of 2^(P-1) is used.

Taken literally the second sentence resets the predictor at every interval,
wherever it falls.  Both reference codecs instead treat the predictor as **per
line**: a restart puts the coder back into first-line state - libjpeg does it by
calling `start_pass` again from `process_restart` in `jddiffct.c` - so it only
changes the prediction when the interval begins at a line boundary.  An interval
that starts mid-row predicts as though no restart had happened.

That distinction is invisible to libjpeg, which refuses a restart interval that
is not a whole number of MCU rows ("must be an integer multiple of the number of
MCUs in an MCU row"), and invisible to this library's own output, which snaps
the interval to whole rows for exactly that reason.  It took a mid-row interval
from the reference codec to expose it, and it was wrong here for both entropy
coders until then.

## Hierarchical fixtures (`hier_*.jpg`, `plain_*.jpg`)

T.81 Annex J.  A hierarchical JPEG is a sequence of frames: a DHP segment
(B.3.2) declares the size of the completed image, the first frame carries a
small version of it, and later frames either repeat it at a new resolution or
code the difference against what has been reconstructed so far.  An EXP segment
(B.3.3) before a frame doubles the reference first.

The ISO reference codec is the only implementation here that writes these, so
it produced every fixture and its expected decode.  The source images are
`hier_src_rgb.ppm` (17x9) and `hier_src_gray.pgm`, both committed beside the
fixtures; they are odd-sized on purpose, because doubling an odd dimension
overshoots by one and the surplus row and column have to be dropped (J.1.1.2).

```sh
J=/path/to/thorfdbg-libjpeg/jpeg
D=tests/data/jpeg

# frame sequence            source                  options
$J -y 2 -q 85 -h                $D/hier_src_rgb.ppm  $D/hier_rgb_2level.jpg
$J -y 2 -q 85 -a                $D/hier_src_rgb.ppm  $D/hier_rgb_2level_arith.jpg
$J -y 2 -q 85 -h                $D/hier_src_gray.pgm $D/hier_gray_2level.jpg
$J -y 2 -q 85 -a                $D/hier_src_gray.pgm $D/hier_gray_2level_arith.jpg
$J -y 2 -q 85 -h -s 1x1,2x2,2x2 $D/hier_src_rgb.ppm  $D/hier_rgb_420.jpg
$J -y 2 -q 85 -h -s 1x1,2x1,2x1 $D/hier_src_rgb.ppm  $D/hier_rgb_422.jpg
$J -y 1 -q 85 -h                $D/hier_src_gray.pgm $D/hier_gray_lossless.jpg
$J -y 1 -q 85 -a                $D/hier_src_gray.pgm $D/hier_gray_lossless_ar.jpg
$J -y 1 -q 85 -h                $D/hier_src_rgb.ppm  $D/hier_rgb_lossless.jpg
$J -y 0 -q 85 -h                $D/hier_src_gray.pgm $D/hier_gray_noexp.jpg
$J -y 0 -q 85 -a                $D/hier_src_gray.pgm $D/hier_gray_noexp_arith.jpg
$J -y 2 -q 85 -v -h             $D/hier_src_rgb.ppm  $D/hier_rgb_progressive.jpg
$J -y 2 -q 85 -v -a             $D/hier_src_rgb.ppm  $D/hier_rgb_prog_arith.jpg
$J -y 1 -q 85 -v -h             $D/hier_src_rgb.ppm  $D/hier_rgb_prog_lossless.jpg

# Controls: same encoder, same source, one frame.
$J -q 85 -h                     $D/hier_src_rgb.ppm  $D/plain_rgb_444.jpg
$J -q 85 -h -s 1x1,2x2,2x2      $D/hier_src_rgb.ppm  $D/plain_rgb_420.jpg
$J -q 85 -h -s 1x1,2x1,2x1      $D/hier_src_rgb.ppm  $D/plain_rgb_422.jpg
$J -q 85 -h                     $D/hier_src_gray.pgm $D/plain_gray.jpg
$J -q 85 -v -h                  $D/hier_src_rgb.ppm  $D/plain_rgb_progressive.jpg

# The expected decode of each, from the same codec.  It writes P5 for a
# single-component image and P6 for three, hence the two extensions.
for f in $D/hier_*.jpg $D/plain_*.jpg; do
  b=${f%.jpg}; $J $f $b.tmp
  case $(head -c2 $b.tmp) in P5) mv $b.tmp ${b}_ref.pgm;; *) mv $b.tmp ${b}_ref.ppm;; esac
done
```

`-h` (optimized Huffman tables) is not optional: without it the encoder refuses
every hierarchical Huffman combination with "Huffman table is unsuitable for
selected coding mode".  `-a` selects the arithmetic coder, and the two together
are not needed - `-a` implies its own conditioning.

What each `-y` produces:

| option | frames |
| --- | --- |
| `-y 2` | SOF1/SOF9 at half size, EXP(1,1), then differential SOF5/SOF13 |
| `-y 1` | SOF1/SOF9 at half size, EXP(1,1), then differential **lossless** SOF7/SOF15 |
| `-y 0` | SOF1/SOF9 at full size, EXP(0,0), then differential lossless SOF7/SOF15 |

Adding `-v` makes the DCT frames progressive, so `-y 2 -v` gives SOF2 then
SOF6 (or SOF10 then SOF14 with `-a`), and `-y 1 -v` gives SOF2 then a
differential lossless SOF7 - which is J's "the final differential frame for
each component may use a differential lossless process" in a DCT sequence.
Between the three `-y` values, `-a`, and `-v`, the fixtures reach all fourteen
SOFn codes of Table B.1.

Watch for silent truncation: `-y 2 -v` without `-h` writes 2048 bytes and then
fails with the Huffman-table error, leaving a file that is a valid prefix and
looks plausible until a decoder reaches the cut.

### Why the comparison has a tolerance

This codec's IDCT and chroma upsampler are not libjpeg's, and this library
matches libjpeg byte for byte.  A *single-frame* file from this encoder
therefore already needs a tolerance - 2 at 4:4:4, 8 at 4:2:0, 13 at 4:2:2 - and
that is what the `plain_*` controls in the test are for: the hierarchical
fixtures are held to the same numbers their single-frame counterparts need,
plus the one extra frame of rounding a pyramid adds.

### What it settled: the reference components are not clamped

J.2.1 has the differential components "added, modulo 2^16, to the upsampled
reference components".  The modulo is the clue: it would be pointless if the
intermediate were clipped to 0..2^P-1 first, and A.3.1's sample range is a rule
about *output*, not about the running reconstruction.

`hier_gray_noexp.jpg` is what proved it.  Its base frame undershoots to -9 at a
near-black pixel and the differential lossless frame corrects it by +13, for a
final value of 4 - which is unreachable if the -9 was flattened to 0 on the way.
Clipping between frames leaves that pixel at 13, nine too high, and it is the
only pixel in the whole fixture set that says so.

### Our own hierarchical output (`hier_ours_*.jpg`)

The other half of interoperability: files this library wrote, decoded by the
ISO reference codec.  A round trip through our own decoder cannot test this -
a private misreading of Annex J would round-trip perfectly - so the expected
output here is that codec's decode of our file, and the test compares our
decode against it.

`hier_enc_src.ppm` is the source, 129x77 so that every level of the pyramid
has an odd dimension to overshoot on.

```sh
J=/path/to/thorfdbg-libjpeg/jpeg
D=tests/data/jpeg

# Written by this library: see JpegEncode.HierarchicalRoundTrip for the API.
#   jpeg_hierarchical_levels = 1, 2, 3; jpeg_arithmetic = 0 or 1
#   hier_ours_l1.jpg  hier_ours_l2.jpg  hier_ours_l1_arith.jpg
#   hier_ours_l3_arith.jpg

for f in $D/hier_ours_*.jpg; do $J $f ${f%.jpg}_thor.ppm; done
```

Regenerating these means re-encoding with this library, which makes them a
weaker regression test than the fixtures above - they move when the encoder
moves.  What they pin down is the thing that matters and that nothing else
checks: that an independent Annex J decoder still reads the result.

### Lossless frames with sampling factors above one

`lossless_arith_h2v4.jpg` came out of the fuzzer; `lossless_huff_subsampled.jpg`
and `lossless_arith_subsampled.jpg` were produced to pin down the interleaved
case it implied:

```sh
J=/path/to/thorfdbg-libjpeg/jpeg
$J -p -c -s 1x1,2x2,2x2 tests/data/jpeg/hier_src_rgb.ppm \
    tests/data/jpeg/lossless_huff_subsampled.jpg
$J -p -a -c -s 2x2,1x1,1x1 tests/data/jpeg/hier_src_rgb.ppm \
    tests/data/jpeg/lossless_arith_subsampled.jpg
```

These are crash regressions, not correctness fixtures, and the difference
matters.  Nothing available writes a *correct* subsampled lossless JPEG:
libjpeg-turbo declines to subsample a lossless frame at all (it writes 1x1
whatever `-sample` says, which for a process whose point is exactness is the
defensible answer), and the reference codec's own decode of these files comes
back nothing like the source - its round trip through them is off by 215 out of
255.  So the test asserts only that they decode inside their buffers and at the
declared size.  What the decoder does with them follows A.2.2, A.2.3 and H.1.1
read directly; it is not checked against anything.

## Non-interleaved scans (`noninterleaved_*.jpg`, `lossless_noninterleaved*.jpg`)

T.81 A.2.3 lets a sequential or lossless frame be written as one scan per
component rather than as a single interleaved scan, and A.2.2 then puts the
data units in that component's own order, "left-to-right, top-to-bottom",
with the sampling factors playing no part in the walk.  libjpeg writes such a
file for any scan script that names components one at a time, so libjpeg-turbo
is both the producer and the oracle here, and the comparison is exact.

```sh
C=/path/to/libjpeg-turbo/cjpeg   # 3.0.4, built static
DJ=/path/to/libjpeg-turbo/djpeg
D=tests/data/jpeg

$C -scans $D/noninterleaved_scans.txt -sample 1x1,1x1,1x1 \
    -outfile $D/noninterleaved_444.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans.txt -sample 2x2,1x1,1x1 \
    -outfile $D/noninterleaved_420.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans.txt -sample 2x1,1x1,1x1 \
    -outfile $D/noninterleaved_422.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans.txt -arithmetic \
    -outfile $D/noninterleaved_arith.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans_mixed.txt \
    -outfile $D/noninterleaved_mixed.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans.txt -restart 1 \
    -outfile $D/noninterleaved_restart.jpg $D/hier_src_rgb.ppm
$C -scans $D/noninterleaved_scans.txt -precision 12 \
    -outfile $D/noninterleaved_12bit.jpg $D/hier_src_rgb.ppm
for f in $D/noninterleaved_*.jpg; do $DJ -pnm -outfile ${f%.jpg}_ref.ppm $f; done

# Lossless: no reference decode, because a lossless codec returns what went in
# and hier_src_rgb.ppm is therefore the expected output.
$C -lossless 4 -scans $D/lossless_noninterleaved_scans.txt \
    -outfile $D/lossless_noninterleaved.jpg $D/hier_src_rgb.ppm
$C -lossless 1 -scans $D/lossless_noninterleaved_scans_psv.txt \
    -outfile $D/lossless_noninterleaved_psv.jpg $D/hier_src_rgb.ppm
$C -lossless 4 -scans $D/lossless_noninterleaved_scans.txt -restart 1 \
    -outfile $D/lossless_noninterleaved_restart.jpg $D/hier_src_rgb.ppm
```

The scan scripts are committed beside the fixtures.  Note the trailing
semicolons: cjpeg refuses the file without them, with "Invalid scan entry
format" and no hint as to which part it disliked.

`lossless_noninterleaved_scans_psv.txt` gives each scan a different predictor -
1, 2 and 7 - because H.1 puts the predictor selection in the scan header, not
the frame header, and a decoder that reads it once per frame gets two of the
three components wrong.

### Our own non-interleaved output (`ni_ours_*.jpg`)

The other direction: A.2.3 files this library *wrote*, decoded by
libjpeg-turbo.  `jpeg_non_interleaved` writes one scan per component instead of
one interleaved scan, and these pin down that an independent decoder reads the
result.

```sh
DJ=/path/to/libjpeg-turbo/djpeg
D=tests/data/jpeg

# Written by this library at quality 85, jpeg_non_interleaved = 1:
#   ni_ours_444.jpg          4:4:4                      from hier_enc_src.ppm
#   ni_ours_420.jpg          4:2:0                      from hier_enc_src.ppm
#   ni_ours_422_restart.jpg  4:2:2, restart interval 3  from hier_enc_src.ppm
#   ni_ours_arith_420.jpg    4:2:0, arithmetic (SOF9)   from hier_enc_src.ppm
#   ni_ours_gray.jpg         grayscale                  from hier_src_gray.pgm

for f in $D/ni_ours_*.jpg; do $DJ -pnm -outfile ${f%.jpg}_turbo.ppm $f; done
# (ni_ours_gray_turbo.pgm for the grayscale one; djpeg picks P5 itself.)
```

The comparison is **exact**, unlike the hierarchical one.  Our IDCT is
libjpeg's islow and our chroma upsampler is its fancy one, so once both codecs
agree the file is well formed there is nothing left for them to disagree about.
The reference decode being the oracle rather than a round trip is what matters
here: a private misreading of A.2.3 would round-trip through our own decoder
perfectly, because the misunderstanding would be on both sides of it.

It caught one immediately.  The first version of the writer named Huffman table
1 for the chroma scans - copying the interleaved scan header, where that is
right - while the one-component encoder underneath had coded them with table 0,
because that is the only table it has.  libjpeg rejected the file outright:
`Corrupt JPEG data: bad Huffman code`.  B.2.3 lets any component select any
table, so the fix is to say 0 and mean it; the progressive writer's AC scans
had already met the same trap and say so in a comment there.

Being our own output, these move when the encoder moves, so they are a weaker
regression test than the fixtures above.  `JpegEncode.NonInterleavedAndInter‐
leavedDecodeToTheSamePixels` is the one that holds the encoder still: it writes
the same image both ways at test time and requires the two to decode to
identical pixels, at sizes where no dimension is a whole number of MCUs.

### A hierarchical frame coded as non-interleaved scans (`hier_noninterleaved_*.jpg`)

A.2.3 applies inside a sequence as well: Annex J changes the coding model, not
the scan arrangement, and nothing there forbids a frame being written one
component at a time.  Nothing available writes such a file - the ISO reference
codec has no option for it and libjpeg-turbo cannot write a sequence at all -
so these are assembled from files that were already validated.

```sh
# mk_hier_ni.py is committed next to the fixtures.  It is six lines:
# insert a DHP segment ahead of the SOF of a single-frame JPEG.  B.3.1 makes
# DHP-then-frames a hierarchical sequence; B.3.2 gives DHP "the same parameters
# as a frame header" with Tq zeroed.
python3 $D/mk_hier_ni.py \
    $D/ni_ours_444.jpg       $D/hier_noninterleaved_444.jpg \
    $D/ni_ours_420.jpg       $D/hier_noninterleaved_420.jpg \
    $D/ni_ours_arith_420.jpg $D/hier_noninterleaved_arith_420.jpg
```

A sequence of one non-differential frame decodes to exactly that frame (J.1.3
leaves a first frame coded normally), which is the whole point: the expected
answer is already committed, as the libjpeg-turbo decode of the plain file each
was built from, and the comparison is exact.

Two independent checks that these are real JPEGs rather than something this
decoder happens to like: libjpeg-turbo validated the frame and its scans before
the DHP was inserted, and the ISO reference codec reads all three afterwards as
hierarchical sequences.

The same trick supplies the lossless case, from the files libjpeg-turbo wrote
for the single-frame A.2.3 tests:

```sh
python3 $D/mk_hier_ni.py \
    $D/lossless_noninterleaved.jpg         $D/hier_lossless_noninterleaved.jpg \
    $D/lossless_noninterleaved_psv.jpg     $D/hier_lossless_noninterleaved_psv.jpg \
    $D/lossless_noninterleaved_restart.jpg $D/hier_lossless_noninterleaved_restart.jpg
```

Lossless, so the expected answer is `hier_src_rgb.ppm` itself, exactly.  The
`_psv` one is the useful one: it gives each scan a different predictor - 1, 2
and 7 - which is what says the predictor is a property of the scan and not of
the frame.

What none of them reach: a non-interleaved scan covers the component's own grid
rather than the MCU-padded one, and at 1x1 sampling those are the same size.
Every lossless file anything here can write is 1x1, because libjpeg-turbo
declines to subsample a lossless frame.  So that distinction follows A.2.2 read
directly, and is unverified - the same position as the subsampled lossless
fixtures above, and said out loud in the test for the same reason.

## Four-component frames (`cmyk_progressive.jpg`, `ycck_*.jpg`)

T.81 allows Nf up to 255 and says nothing about color; four components in
practice means CMYK, or YCCK when an Adobe APP14 says transform 2, and that
marker is the only thing in the file that tells them apart.

`cmyk_sample.jpg` covered four components in a baseline frame.  Nothing covered
them anywhere else, and the coefficient-buffer walk - progressive frames,
sequential frames written as several scans, and 12-bit frames - refused them.

```sh
D=tests/data/jpeg
python3 -c "
from PIL import Image
im = Image.open('$D/hier_enc_src.ppm').convert('CMYK')
im.save('$D/cmyk_progressive.jpg', quality=88, progressive=True)
"
# ycck_baseline.jpg and ycck_progressive.jpg are the CMYK files with the APP14
# transform byte set to 2 - see the note below.

# References, libjpeg as always for CMYK.  cmyk_ref.c is committed here; build
# it against libjpeg-turbo and it reproduces dump_jpeg_pixels_ref -o exactly
# (checked against the committed cmyk_sample.raw, byte for byte).
cc -O1 -o cmyk_ref $D/cmyk_ref.c -I<libjpeg-turbo-src> -I<build> <build>/libjpeg.a
for b in cmyk_progressive ycck_baseline ycck_progressive; do
  ./cmyk_ref $D/$b.raw $D/$b.jpg
done
```

`cmyk_progressive.jpg` is a real progressive CMYK file: 18 scans, a
four-component interleaved DC scan, successive approximation on both DC and AC,
and one AC scan per component as G.1.2.2 requires.

The two `ycck_` files are those images with the APP14 transform byte changed to
2.  Nothing available writes a genuine YCCK file, so the picture they decode to
is not meaningful - but the decode is well defined, and libjpeg produces the
same bytes we do.  That is what a branch that had never been exercised needs.
Disabling the YCCK conversion leaves `DecodeFixtureOraclesRaw` passing and only
these failing, which is how it was confirmed that `cmyk_sample.jpg` was not
reaching it.

The oracle is libjpeg and not Pillow because libjpeg does not invert Adobe CMYK
(jdcolor.c null_convert) and matching it was already the decision here.  Pillow
does invert, so a Pillow decode of any of these is the exact complement of the
`.raw` - worth knowing before concluding something is wrong.

## Every sampling factor (`sampling_s*.jpg`)

T.81 B.2.2 allows H_i and V_i from 1 to 4; A.2.3's limit of ten data units in
an MCU rules out the rest.  Everything here had only been tried at the three
combinations photographs actually use, and a sweep of the twelve libjpeg-turbo
will write found two faults - both wrong pixels, not refusals.

```sh
C=/path/to/libjpeg-turbo-3.0.4/cjpeg   # 3.0.4, built static
DJ=/path/to/libjpeg-turbo-3.0.4/djpeg
D=tests/data/jpeg

for hv in 1x2 4x1 2x4 3x1 1x4; do
  $C -quality 80 -sample $hv -outfile $D/sampling_s$hv.jpg $D/hier_enc_src.ppm
  $DJ -pnm -outfile $D/sampling_s$hv.ppm $D/sampling_s$hv.jpg
done
```

What they caught:

- **The box fallback scaled proportionally** - `x * cw / width` - where
  libjpeg's `int_upsample` replicates each sample `H_max/H_i` times.  The plane
  is `ceil(X x H_i / H_max)` wide, a little wider than `X / (H_max/H_i)`, and
  the proportional map spreads that surplus along the row while replication
  puts it all in the last group.  At a ratio of two the two maps agree exactly,
  which is why 4:2:0 and 4:2:2 never showed it.  At a ratio of four they
  disagree on half of every group of four pixels, by up to 47 out of 255.

- **4:4:0 had no fancy filter.**  Full width, half height: the mirror of 4:2:2,
  and the one that gets forgotten.  libjpeg 6b box-filters it too, so this was
  right once; libjpeg-turbo added `h1v2_fancy_upsample` and that is what a
  current decoder produces.  `cjpeg -sample 1x2` writes one directly, and
  libjpeg-turbo's own comment notes the other way in - losslessly transposing a
  4:2:2 file.  Note it has no `downsampled_width > 2` guard, unlike h2v1 and
  h2v2: their triangle runs horizontally and needs interior columns, this one
  runs vertically and does not care how wide the plane is.

All twelve combinations are byte-exact against libjpeg-turbo 3.0.4 in both
upsampling modes - 24 comparisons - which is what the sweep is for, even though
only five are committed as fixtures.

Ratios that do not divide (H_i = 3 against H_max = 4, say) keep the
proportional map: libjpeg refuses those frames outright (jdmaster.c,
`JERR_FRACT_SAMPLE_NOTIMPL`), so there is nothing to match and no oracle to
match it with.

## DNL: a height the frame header left at zero (`dnl_zero_height.jpg`)

T.81 B.2.2 lets a frame header carry Y = 0, and B.2.5 then has the DNL segment
after the first scan supply the number of lines.  That is how a JPEG gets
written by something that does not know the height until it has finished - a
scanner, a fax.  Nothing writes one now, so this is assembled.

```sh
C=/path/to/libjpeg-turbo/cjpeg
D=tests/data/jpeg

$C -quality 80 -outfile $D/dnl_stated_height.jpg $D/hier_enc_src.ppm
python3 $D/mk_dnl.py $D/dnl_stated_height.jpg $D/dnl_zero_height.jpg
# and the reference is libjpeg's decode of the file that still states its height
$DJ -pnm -outfile $D/dnl_zero_height.ppm $D/dnl_stated_height.jpg
```

The oracle needs both codecs, because neither can give one alone.
**libjpeg-turbo refuses the zero-height file** - `Empty JPEG image (DNL not
supported)` - so it cannot say what the pixels are.  But `dnl_stated_height.jpg`
is the same image with its height in the SOF and no DNL, which it reads
happily, and that decode is the committed reference.  **The ISO reference codec
does implement DNL**, reads the zero-height file, and agrees with our decode to
within 3 - its IDCT is not libjpeg's, the same tolerance the hierarchical
fixtures need.  So one codec vouches for the pixels and the other for the file.

The three DNL cases already covered here - a DNL agreeing with a stated height,
one contradicting it, one arriving before the first scan - are all about
rejecting or ignoring DNL.  This is the only one where it decides anything.

## Fill bytes and TEM (`marker_fill_*.jpg`, `marker_tem.jpg`)

T.81 B.1.1.2: "any marker may optionally be preceded by any number of fill
bytes, which are bytes assigned code X'FF'".  B.1.1.3 Table B.1 lists TEM
(X'FF01') among the markers that stand alone, carrying no length field.

No encoder here emits either, and no fixture had ever contained a pad byte, so
none of this was exercised and all six of these files were refused outright.
They are assembled from an ordinary baseline file by `mk_fill_tem.py`:

```sh
D=tests/data/jpeg
python3 $D/mk_fill_tem.py $D/baseline_8x8_gray.jpg $D
$DJ -pnm -outfile $D/marker_padding_ref.pgm $D/baseline_8x8_gray.jpg
```

Each is the same picture with padding added, so the expected answer is the
unpadded file's decode, and libjpeg accepts all six — that is what made them
worth building rather than guessing at.

Two faults, from opposite sides:

- **The marker reader** consumed exactly one byte after the first 0xFF and took
  whatever it found, so `FF FF C0` returned a marker of 0xFF.  One pad byte
  anywhere was enough to make a file unreadable.
- **The scan-data scanner** would have appended that second 0xFF to the entropy
  data.  Inside a scan a 0xFF is always followed by the 0x00 of byte stuffing
  (B.2.2), so `FF FF` there is padding ahead of a marker and never data;
  appending it hands the entropy decoder eight bits that were never coded.
  `marker_fill_before_eoi.jpg` is the one that reaches this path.

TEM has no length field, so reading a two-byte length after it swallows the
start of whatever follows.  libjpeg skips it.

## Four-component fixtures, read and written

`cmyk_ours_*.jpg` and `ycck_ours_*.jpg` are this encoder's own output, each
with libjpeg's decode of that exact file beside it as a `.raw`.  Both halves
matter: the committed bytes pin the encoder, and the `.raw` pins what those
bytes mean to another implementation.  `mk_cmyk.c` writes the other direction —
`ycck_ljt_420.jpg`, `ycck_ljt_422.jpg` and `cmyk_ljt_sub.jpg` are
libjpeg-turbo's own output, subsampled, which is the case that needs the
triangle filter of `jdsample.c` and had no fixture at all before.

```bash
cc -O1 -o mk_cmyk $D/mk_cmyk.c -I<libjpeg-turbo-src> -I<build> <build>/libjpeg.a
cd $D && ./mk_cmyk
for f in ycck_ljt_420 ycck_ljt_422 cmyk_ljt_sub; do ./cmyk_ref $f.raw $f.jpg; done
```

`mk_cmyk12.c` is the same idea at twelve bits, through libjpeg's separate
twelve-bit entry points (`jpeg12_*`).  Its `.raw` files carry 16-bit samples —
mode 3 of the oracle format — because twelve-bit values do not fit in bytes.

## Wide frames

T.81 B.2.2 allows Nf from 1 to 255.  libjpeg cannot read such a frame back:
`jdmarker.c` `get_sos` matches a scan's Cs against only the first
MAX_COMPS_IN_SCAN components of the frame, so it refuses any file whose scan
names the fifth or later, however well formed.  That is a limit of that
implementation, not of the standard, and it means the oracle has to be built
rather than run.

`mk_wide.py` builds it.  A frame of N components, all 1x1, written as N
single-component scans is — block for block, with the DC predictor reset at
every SOS — exactly N grayscale JPEGs sharing one quantization table and one
set of Huffman tables.  The script writes those N files with `cjpeg`, checks
that their tables really are identical, splices their scans into one
N-component frame, and keeps libjpeg's decode of each grayscale file as the
expected plane.  Both ends of the comparison are libjpeg's.

```bash
cd $D && python3 mk_wide.py <build>/cjpeg <build>/djpeg
```

The `.raw` files here use a header of their own (byte 0 = 4, byte 1 = the
channel count, then width and height as little-endian 32-bit), because the
channel count is not implied by a mode.

## The abbreviated formats of B.4

`mk_abbrev.c` writes a table-specification stream and an image with no tables,
from **one** libjpeg compress object: `jpeg_write_tables()` marks the tables as
sent and `jpeg_start_compress(..., FALSE)` then leaves them out.  Two objects
would each think its tables had never been written, and the "abbreviated" image
would quietly carry a full set — which is worth knowing, because the fixture
would then prove nothing.

```bash
cc -O1 -o mk_abbrev $D/mk_abbrev.c -I<libjpeg-turbo-src> -I<build> <build>/libjpeg.a
cd $D && ./mk_abbrev
```
