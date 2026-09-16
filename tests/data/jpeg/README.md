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

**12-bit decode fixtures:** 12-bit JPEG (SOF1 or SOF2, precision 12, DQT Pq=1) is produced by our encoder; encode tests write `baseline_gray12.jpg`, `baseline_rgb12.jpg`, `progressive_gray12.jpg`, `progressive_rgb12.jpg` to `tests/out/jpeg/`. To exercise the 12-bit decode path without running encode tests, copy e.g. `baseline_gray12.jpg` from `tests/out/jpeg/` to this directory. The test `Decode12BitFixture` loads it if present and asserts format and dimensions; if the file is absent, the test is skipped. Note that it checks dimensions and bit depth only — **no pixel comparison** — because Pillow's libjpeg is an 8-bit build and cannot open a 12-bit file at all. A real 12-bit oracle needs libjpeg built with `BITS_IN_JSAMPLE=12`, or IJG v10 built for 12-bit.

**12-bit status:** every quality from 1 to 100 round-trips through this library, and every one of them is byte-identical to what libjpeg-turbo's 12-bit build decodes from the same file. Quality 100 used to be refused at save time, on the grounds that the resulting file could not be read back; the fault was in the extended Huffman tables, not in the quantiser, and it is fixed.

The earlier failures had two causes, both since fixed: the FDCT overflowed at 12-bit sample range (its intermediates are sized for 8-bit), and the extended AC Huffman tables were over-subscribed - their bit counts needed 65615 codes of length 16 where only 65536 exist, so 79 symbols had no valid code and the decoder resolved them to something else. The tables now carry the 226 symbols T.81 F.1.2.2 actually defines at 12-bit (EOB, ZRL, and RRRR/SSSS for SSSS 1..14) and satisfy the Kraft inequality; the DC tables carry 16 (SSSS 0..15). They are generated by Huffman's algorithm with the 16-bit length limit of B.2.4.2, using the Annex K table as the frequency model so common symbols keep short codes.

**Still outstanding for 12-bit:** no external oracle. Pillow's libjpeg is an 8-bit build and cannot open a 12-bit file at all, so nothing but this library has ever read our 12-bit output. A real oracle needs libjpeg built with `BITS_IN_JSAMPLE=12`, or IJG v10 built for 12-bit. Until then 12-bit is verified by round-trip and by value checks (`Save12BitFlatFieldsKeepTheirValue`, `Save12BitRampKeepsItsContrast`) rather than against an independent decoder.

**On 16-bit:** earlier revisions encoded and decoded "16-bit JPEG" as SOF2 with precision 16 and a 242-symbol AC table, and the documentation described this as being per T.81. **It is not.** T.81 Table B.2 gives every DCT-based frame a sample precision of 8 or 12; precision 2..16 belongs to lossless (SOF3) alone. There is no 16-bit DCT JPEG to be compatible with, which is why no tool encodes or decodes one and why IJG reports "Bogus Huffman table definition" on such files — IJG is right. The support has been removed: `jpeg_precision = 16` now returns `GIMG_ERR_UNSUPPORTED`, a 16-bit raster is written at 12-bit, and a frame declaring precision 16 is rejected on load (`Sof2Precision16Rejected`). If you want more than 8 bits per sample in a JPEG, 12-bit is the standard answer.

**manifest.json** — Written by `generate.py`; lists each fixture with `file`, `width`, `height`, `mode`, `progressive`, `quality`, `has_exif`, `has_icc`.

**Encoder reference (libjpeg “golden” for SaveRgbThenLoadDecode):** `baseline_rgb_reference.jpg` is the **libjpeg** encoding of the same 8×8 RGB image and settings as the test (R=x*32, G=y*32, B=128; baseline, Q85, 4:2:0). Regenerate from `third_party/jpeg-oracle`: `make -C third_party/jpeg-oracle baseline-rgb-reference IMAGE_ROOT=$(pwd)` (from the image repo root).

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
colour conversion that subtracted the level shift from Y and never added it
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

352 of 352 byte-exact as of this writing, across grey and colour, 4:4:4/4:2:2/
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
terms from the neighbouring blocks' DC values, so an incomplete progression
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
| `arith_rgb_64x64_420.jpg` | SOF9 colour, 4:2:0 |
| `arith_rgb_17x9_422_restart.jpg` | SOF9 colour, 4:2:2, restart interval 3, non-MCU-aligned |
| `arith_gray_1x1_empty_scan.jpg` | SOF9 with an entropy-coded segment of zero bytes |
| `arith_gray12_64x64.jpg` | SOF9 at P=12 |
| `arith_progressive_33x33_422.jpg` | SOF10 progressive, 4:2:2, non-MCU-aligned |
| `arith_progressive_restart_420.jpg` | SOF10 progressive, 4:2:0, restart intervals |
| `progressive_restart_420.jpg` | SOF2 progressive, 4:2:0, restart intervals (Huffman) |

The last three cover a hole the big corpus had: it contains 576 progressive
files and 576 with restart intervals, and not one file that has both.  Every
progressive image with a restart interval was undecodable, whichever entropy
coder it used, and nothing noticed.  Two independent causes:

- A restart marker is a hard resynchronisation point (B.2.1), so bits the
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
colour output - but it will refuse the file outright if the writer emits a JFIF
APP0, because JFIF declares three-component data to be YCbCr and libjpeg takes
that ahead of the Adobe marker.  A lossless frame stores RGB, so it carries the
Adobe marker and no JFIF.  And libjpeg's lossless decoder requires a restart
interval to be a whole number of MCU rows; ours rounds down to satisfy it.
