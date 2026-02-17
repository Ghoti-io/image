# JPEG test data

Reference JPEG files for Phase 2 codec and golden tests.

**Fixture generation:** Run `python3 tests/data/jpeg/generate.py` from the repo root. Pillow is required for generation; `piexif` is optional for EXIF orientation. See `documentation/development.md` (Prerequisites).

**Decode oracle:** The `Decode*PillowOracle` and `.raw`-comparison tests compare **actual pixels** (raster) to the oracle’s .raw output, not hashes, so failures report the first differing pixel (e.g. `First pixel diff at (x,y): ours=(...) oracle=(...)`). By default they use **Python (Pillow)** via `decode_oracle_pillow.py` and `encode_oracle_pillow.py` in this directory—no libjpeg required. Require Pillow: `pip install Pillow`. If the Python scripts are missing or fail, the tests try the **libjpeg-based** oracle in **`third_party/jpeg-oracle/`** when `GIMG_JPEG_ORACLE_DIR` points to its `build/` dir. The image library does not link to libjpeg; the C oracle is optional for instrumented debugging (e.g. bit-exact match with libjpeg).

**Encode verification:** After unit tests, `make test` runs `verify_jpeg_output.py`, which uses PIL to verify that JPEGs written to `tests/out/jpeg/` are valid (SOI, structure, optional dimension/SOF expectations). JPEG is lossy; we do not compare encode output to pre-encode source. Run verification only: `make test-verify-jpeg`.

**Decoder oracle (.raw):** To test that our decoder matches an external oracle: (1) Oracle decodes a fixture JPEG and writes raw pixels to a `.raw` file. (2) Our decoder reads the same JPEG and decodes. (3) The test compares our decode to the `.raw` byte-for-byte. Generate oracle `.raw` files with the tools from `third_party/jpeg-oracle` (build there, then set `GIMG_JPEG_ORACLE_DIR`), then run `python3 tests/data/jpeg/generate_jpeg_oracle_raws.py`. CMYK fixtures require the oracle.

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

**16-bit and 12-bit decode fixtures:** 16-bit JPEGs (SOF1/SOF2, precision 12 or 16, DQT Pq=1) are produced by our encoder; encode tests write `baseline_gray16.jpg`, `baseline_rgb16.jpg`, `progressive_gray16.jpg`, `progressive_rgb16.jpg` to `tests/out/jpeg/`. For decode-only tests that do not run the encoder, add at least one 16-bit fixture to this directory (e.g. copy from `tests/out/jpeg/` after running `JpegEncode.SaveGray16ThenLoadDecode` or `SaveGray16ProgressiveThenLoadDecode`) and document the source here. The verifier (`verify_jpeg_output.py`) accepts SOF2 with precision 16 and checks dimensions for these outputs; PIL may not open 16-bit files, in which case verification uses SOF parsing only. **12-bit:** Encode tests write `baseline_gray12.jpg`, `baseline_rgb12.jpg`, `progressive_gray12.jpg`, `progressive_rgb12.jpg` to `tests/out/jpeg/`. To exercise the 12-bit decode path without running encode tests, copy e.g. `baseline_gray12.jpg` from `tests/out/jpeg/` to this directory (after running `JpegEncode.SaveGray12ThenLoadDecode`). The test `Decode12BitFixture` loads `baseline_gray12.jpg` from here if present and asserts format and dimensions; if the file is absent, the test is skipped.

**16-bit validation note:** No known public corpus of externally produced 16-bit JPEG (SOF2 precision 16, extended DHT) exists; common tools (libjpeg, libjpeg-turbo, IJG) do not encode or decode 16-bit. Validation for 16-bit is therefore **round-trip and structure only** (our encode → our decode, plus SOF/DHT checks). Because external support for 16-bit JPEG is itself limited, **this library’s 16-bit encode/decode behavior is not externally validated.**

**IJG v10 (Independent JPEG Group reference):** Source lives in `image/third_party/jpeg-10` (obtain from https://ijg.org/files/jpegsrc.v10.tar.gz, extract into `third_party/`). Build with `make jpeg-ijg10-build` from `image/`; then `third_party/jpeg-10/djpeg` and `cjpeg` are available. IJG v10 supports decode **precision 8–12 only** (jdinput.c rejects precision &gt; 12) and rejects the **extended DHT** (242-symbol AC table) used for 16-bit, so it reports "Bogus Huffman table definition" on our 16-bit files. It is **not** a 16-bit oracle; it can still be used for 8-bit and (with a 12-bit build) 12-bit comparison when those fixtures exist.

**ISO/IEC 10918-7:2023 (ITU-T T.873) — JPEG 1 reference software:** Part 7 of JPEG 1 is the official standard that specifies reference software for T.81 (10918-1). The 2023 edition includes Reference Software A (v1.59) and B (v2.0.x), intended to implement the full spec (including decoder conformance per 10918-2). The standard is **not freely distributed**: the document (and any annexed source) is obtained by purchase from [ISO](https://www.iso.org/standard/85635.html) (CHF 63) or [ITU-T T.873](https://www.itu.int/rec/T-REC-T.873). jpeg.org’s JPEG 1 software page does not link to 10918-7 code. If you obtain the standard, you could build the reference decoder and use it as an authoritative 16-bit oracle (assuming the reference implements 16-bit decode); until then, we rely on round-trip and structure verification for 16-bit.

**manifest.json** — Written by `generate.py`; lists each fixture with `file`, `width`, `height`, `mode`, `progressive`, `quality`, `has_exif`, `has_icc`.

**Encoder reference (libjpeg “golden” for SaveRgbThenLoadDecode):** `baseline_rgb_reference.jpg` is the **libjpeg** encoding of the same 8×8 RGB image and settings as the test (R=x*32, G=y*32, B=128; baseline, Q85, 4:2:0). Regenerate from `third_party/jpeg-oracle`: `make -C third_party/jpeg-oracle baseline-rgb-reference IMAGE_ROOT=$(pwd)` (from the image repo root).

**Generated by scripts (gitignored):** Running compare_progressive_bits.py or trace comparisons produces `trace_ours.txt` / `trace_ref.txt`. `create_libjpeg_progressive_fixture.py` produces `progressive_8x8_libjpeg.ppm` and `progressive_8x8_libjpeg.jpg`. These are listed in `.gitignore` and should not be committed.

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
- **dump_jpeg_raw_first_mcu** — Decode first MCU with libjpeg `raw_data_out` and write ref_Y_block0.bin … ref_Cr_block0.bin (64 bytes each). Built from `third_party/jpeg-oracle`. Usage: `dump_jpeg_raw_first_mcu <file.jpg> [output_dir]`. Compare to our DUMP_JPEG_COMPONENTS Cr_block0.bin (bytes 256–319 = STORED) to confirm IDCT/component match.
- **compare_first_block_natural.py**, **compare_ycbcr_components.py** — Debug/regression.
- **verify_first_scan_dc.py**, **verify_dc_refine_first_mcu.py**, **verify_first_ac_initial.py**, **verify_second_ac_initial_first_block.py**, **verify_ac_refine_first_block.py** — Bitstream/decode checks.
- **run_*_verification.py** — Runners that compare script output to decoder trace.
- **verify_dc_only_pipeline.py**, **verify_script_vs_libjpeg.py** — Pipeline and script-vs-ref checks.
- **dump_scan_bytes.py** — Dump raw scan bytes (loader vs script).
- **create_libjpeg_progressive_fixture.py** — Optional: create progressive fixture with cjpeg.
- **sanity_check_libjpeg_baseline_progressive.py** — Sanity check: (1) libjpeg encodes same 16×16 image as baseline and progressive; (2) do those two decodes (libjpeg) produce the same raster? (3) does our decoder match libjpeg decode for each? Requires `encode_libjpeg_baseline_scan`, `cjpeg`, `dump_jpeg_pixels_ref`, `dump_jpeg_raster`. Usage: `python3 sanity_check_libjpeg_baseline_progressive.py [--work-dir /tmp/jpeg_sanity]`.
- **sanity_check_encoder_libjpeg.py** — Encoder sanity: (1) baseline — our scan bytes vs libjpeg (same 16×16 (x+y)&0xFF, Q85); run with `GIMG_JPEG_DUMP_SCAN_BASELINE`; (2) progressive — our 3-scan file vs cjpeg default (file sizes; structure differs). Run from **image/**: `python3 tests/data/jpeg/sanity_check_encoder_libjpeg.py [--work-dir ...]`.
- **encode_libjpeg_baseline_rgb.c** — Encode 8×8 RGB (R=x*32, G=y*32, B=128) with libjpeg baseline, Q85, 4:2:0; write scan 0 bytes and optionally full JPEG. Same image/settings as **SaveRgbThenLoadDecode**. Built by `make jpeg-oracle-tools`.
- **compare_save_rgb_encode.py** — Compare our 8×8 baseline RGB encode to libjpeg: runs SaveRgbThenLoadDecode with `GIMG_JPEG_DUMP_SCAN_BASELINE`, runs encode_libjpeg_baseline_rgb, then compares scan bytes (and optionally `--structure` for marker order). Run from **image/**: `python3 tests/data/jpeg/compare_save_rgb_encode.py [--structure]`.
- **compare_large_grayscale_encode.py** — Compare our 640×480 baseline grayscale encode to the reference (encode_libjpeg_baseline_scan, same image (x+y)&0xFF, Q85). Dump is written during save (test fails at decode). Run from **image/**: `python3 tests/data/jpeg/compare_large_grayscale_encode.py [--work-dir ...]`. Use `--skip-ours` to reuse an existing our-scan dump. Reports first differing byte and approximate block index. **Investigation (Feb 2025):** (1) First divergence was at scan byte 4; root cause was **RGB→YCbCr** using different integer coefficients (77/150/29 vs libjpeg’s FIX-scaled 19595/38470/7471). **Fix:** `jpeg_rgb_to_ycbcr` in jpeg_save.c now uses libjpeg’s CCIR 601-1 coefficients and rounding so block 0 and first 10 scan bytes match. (2) Divergence at byte 11 (block 1): **dummy blocks** — libjpeg uses zero blocks with DC = previous block’s DC for right/bottom edge blocks; we were replicating edge pixels. **Fix:** In `gimg_jpeg_progressive_fill_coef_buffer` and `_16bit` (jpeg_entropy.c), blocks with `blk_x >= comp_width_px || blk_y >= comp_height_px` are now emitted as all-zero with `block_zz[0] = last_dc[c]`. Scan bytes now match through byte 12. (3) Byte 13 = start of **block 4 (Cb)**. **Fixes applied:** (a) 4:2:0 chroma downsampling now uses libjpeg’s alternating bias (1,2,1,2 per column) in jpeg_save.c. (b) Cb/Cr block sampling uses full buffer dimensions (`comp_width_samp`/`comp_height_samp` from stride and 8×vc) so the 8×8 block is filled from the full 8×8 downsampled buffer, not 4×4 replicated. Cb block coefficients are closer; small residual difference (DC/AC) may be quant scaling or rounding. Segment order differs (we emit DHT before SOF0, libjpeg SOF0 before DHT — both valid per T.81).

Reference or third-party source trees used for debugging (e.g. a copy of libjpeg) belong in the repo root’s **third_party/** directory, which is gitignored. The decode oracle uses **system libjpeg** via the ref tools above, not anything in third_party. To debug with an instrumented libjpeg (e.g. TRACE_REF_AC, DUMP_JPEG_COEF_AFTER_SCAN): (1) **Link against build tree (no install):** from `image/third_party/libjpeg-turbo`, `mkdir -p build-debug && cd build-debug && cmake .. && make`; then from `image`, `make jpeg-oracle-tools-debug-build`. (2) **Or install then link:** same build, then `make install` (e.g. prefix `../../libjpeg-debug`) and from `image` run `make jpeg-oracle-tools-debug`. Our decoder: `DUMP_JPEG_QUANT_IDS=1` logs per-component quant table ID and first values (SOF Tqi); `DUMP_JPEG_COMPONENTS=<dir>` writes Y/Cb/Cr raw (see compare_ycbcr_components.py). **Ref component dump:** Build instrumented libjpeg (`make jpeg-oracle-tools-debug-build`), then run `dump_jpeg_pixels_ref_debug` with `DUMP_JPEG_COMPONENTS_REF=<dir>`; this writes `ref_Y.raw`, `ref_Cb.raw`, `ref_Cr.raw` (same format: 4b w LE, 4b h LE, raw). **compare_ref_ours_components.py** compares ref vs ours component dumps (e.g. `python3 compare_ref_ours_components.py /tmp/ref /tmp/ours`). Result for progressive_sample.jpg: Y/Cb/Cr component buffers **match** ref; any pixel hash difference is due to **chroma upsampling** (libjpeg default = fancy, ours = simple replication; both spec-valid). Ref: run with `TRACE_REF_AC=1`, `LIBJPEG_DEBUG_DHT=1`, `LIBJPEG_DEBUG_AC_TABLE=1`, `LIBJPEG_TRACE_HUFF_BITS=1`, or `LIBJPEG_DUMP_BLOCK0_BYTES=1` (dump actual bytes consumed for first AC refine block 0) as needed. With `LIBJPEG_TRACE_HUFF_BITS=1`, the ref emits `REF_HUFF_BIT` and `REF_HUFF_MATCH` lines (same idea as our `HUFF_BIT`/`HUFF_MATCH`) so you can compare bit-level Huffman decode; **compare_progressive_trace_all.py** sets this when running the ref. To compare our full decode trace to libjpeg, run our decoder with `GIMG_JPEG_TRACE_ALL=1` and use **compare_progressive_trace_all.py** against ref with the env vars above; the script compares AC_INITIAL lines and first-MCU block coefficients per scan.

**Comparing AC refinement decode step-by-step (bits/bytes/blocks):** The only reliable way to fix progressive decoder bugs is to dump bits, bytes, and block state at every step and compare with libjpeg. Use full-dump mode on both decoders, then diff the traces.

- **Our decoder:** `GIMG_JPEG_DUMP_AC_REFINE_FULL=1` dumps for every AC refinement block: `OUR_AC_REFINE_BLOCK_START` (scan, block, byte_off, bit_off, scan_size, nz_in_band, nz_at=...), then `OUR_AC_REFINE_HUFF`, `OUR_AC_REFINE_EOB_RUN` (for (r,0) path), `OUR_AC_REFINE_REFINEMENT_BIT`, `OUR_AC_REFINE_CORRECTION` (each with byte_off/bit_off), and `OUR_AC_REFINE_BLOCK_END`. Add `DUMP_JPEG_COEF_BLOCKS_AFTER_SCAN=1` to get coefficient blocks after each scan (OUR_SCANn_COMPc_BLOCKb) so you can compare state before each refinement scan.
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

- **Our decoder:** `GIMG_JPEG_DUMP_AC_INITIAL_FULL=1` dumps `OUR_AC_INITIAL_STEP` for each logical step: `op=HUFF sym=0xNN`, `op=EOB`, `op=EOBRUN r=N eobrun=M`, `op=EOBRUN_SKIP eobrun=M`, `op=COEFF run=N size=N k=N val=N`, `op=ZRL`. Each line includes `scan=` (scan index), `block=` (global decode ordinal: 0,1,2,… across all AC-initial blocks), `byte=` and `bit=` (position after that step). T.81 Annex G is the authority; the trace is for comparison only.
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
