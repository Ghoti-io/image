@page format_references Format and Specification References

# Format and specification references

This document lists the external specifications (or parts thereof) that the image library implements for each format. Use these as the authoritative reference for chunk layout, ordering, and behavior.

## PNG

- **Allocator:** The PNG codec uses the document/codec allocator for all codec-owned allocations (including chunk reading when a temporary buffer is needed for large payloads).
- **Specification:** ISO/IEC 15948:2004 (PNG — Portable Network Graphics), and the PNG Specification (W3C Recommendation 10 Nov 2003) at <https://www.w3.org/TR/PNG/>.
- **Data representation (image layout, filtering, interlace):** <https://www.w3.org/TR/PNG-DataRep.html>
- **Filter algorithms (None, Sub, Up, Average, Paeth):** <https://www.w3.org/TR/PNG-Filters.html>
- **Interlaced data order (Adam7):** W3C §2.6 — <https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order>
- **APNG:** APNG extension (animated PNG); see <https://wiki.mozilla.org/APNG_Specification> and common implementations.

### Parts implemented

- **Chunk layout and CRC:** Signature (5.2), chunk structure — 4-byte length (big-endian), 4-byte type, payload, 4-byte CRC (5.3). CRC-32 over type+payload per PNG spec.
- **Chunk ordering:** Critical and ancillary chunk order per spec (5.4 and 11.2–11.3): IHDR first; PLTE (and tRNS if present) before IDAT for palette images; ancillary in allowed positions; IDAT contiguous; IEND last. On save, emit chunks in this order.
- **Critical chunks:** IHDR (11.2.1), PLTE (11.2.2), IDAT (11.2.4), IEND (11.2.5). For palette images, PLTE (and optional tRNS) must appear before IDAT.
- **Ancillary chunks:** tEXt, zTXt, iTXt, iCCP, sRGB, gAMA, cHRM, eXIf, and others as raw or typed per spec (11.3).
- **Color chunk policy:** PNG allows at most one of sRGB, iCCP, or gAMA+cHRM for color interpretation. If multiple are present, this implementation uses the first in priority order: **sRGB > iCCP > gAMA/cHRM**. The chosen chunk is applied to `GIMG_Color_Info`; iCCP bytes are stored for round-trip in ancillary and (when chosen) the decompressed profile is attached to the decoded raster.
- **Filtering and interlace:** Filter types (None, Sub, Up, Average, Paeth) per [PNG-Filters](https://www.w3.org/TR/PNG-Filters.html); Adam7 interlace (seven passes) per [Interlaced data order](https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order).
- **APNG:** acTL, fcTL, fdAT; frame timing, dispose (None/Background/Previous), blend (Source/Over) per APNG spec.

### PNG save metadata policies

On PNG save, `GIMG_Save_Options.metadata_policy` controls which ancillary chunks are written:

- **GIMG_META_PRESERVE_ALL:** Emit all ancillary chunks from the loaded document (and eXIf from doc meta_raw when the doc was not loaded from PNG). PLTE/tRNS are always emitted when required for the image.
- **GIMG_META_DROP_ALL:** Emit no ancillary metadata; only signature, IHDR, PLTE/tRNS (if palette), IDAT, IEND.
- **GIMG_META_STRIP_GPS:** Only actual GPS data is stripped. eXIf is parsed; the GPS IFD (and GPS-related tags) are removed; the remaining Exif is re-serialized and written as the eXIf chunk. Non-GPS Exif (orientation, datetime, etc.) is preserved. tEXt, zTXt, and iTXt chunks whose keyword is "GPS", "GPS " (with trailing space), or "EXIF:GPS" (case-insensitive) are omitted. Other ancillary is preserved.
- **GIMG_META_NORMALIZE_EXIF:** eXIf is normalized (e.g. orientation set to 1 / applied, duplicate tags removed) and written as a single eXIf chunk. Other ancillary is preserved. Implemented in Phase 1.5: Exif module sets orientation tag to 1 (normal) when present.
- **GIMG_META_KEEP_RAW_ONLY:** Emit only ancillary chunks that are *not* known semantic metadata. Omitted: iCCP, sRGB, gAMA, cHRM, eXIf, tEXt, zTXt, iTXt. Emitted: any other ancillary chunk type (e.g. unknown or private chunks) in read order. eXIf from doc meta_raw is not written. PLTE/tRNS are emitted when required for the image.
- **GIMG_META_KEEP_COMMON_ONLY:** Emit only metadata that maps to common metadata. Exactly one color chunk is written from the decoded raster’s `GIMG_Color_Info`: sRGB (if transfer/primaries indicate sRGB), or gAMA (if transfer is gamma with a positive value), or iCCP (if an ICC profile is attached). No eXIf, no text chunks, no other ancillary. PLTE/tRNS are emitted when required for the image.

### Conformance and tested scope

- **PNG:** Implementation follows PNG 1.2 (W3C Recommendation 10 Nov 2003) and ISO/IEC 15948:2004. Chunk layout, ordering, filtering, and interlace (Adam7) are as specified. No intentional deviations.
- **APNG:** Follows the APNG extension as implemented by Mozilla (<https://wiki.mozilla.org/APNG_Specification>). acTL, fcTL, fdAT chunk semantics; default image vs first frame; dispose (None/Background/Previous) and blend (Source/Over) are implemented per that specification. **16-bit RGBA blend (OVER):** Component-wise alpha blend is implemented per spec; 64-bit intermediates are used for composite math to avoid overflow.
- **Encode color types:** Save supports color_type 0 (grayscale), 2 (RGB), 3 (palette), 4 (grayscale+alpha), and 6 (RGBA). For color_type 2 and 4, the raster is RGBA; on round-trip (doc loaded from a PNG with ct 2 or 4) the same color_type is preserved. tRNS is emitted only for palette (3); we do not emit tRNS for color_type 0, 2, or 4 on save (transparency for 0/2 would require tRNS; type 4 carries alpha in samples).
- **Tested scope:** Decode and encode are tested with reference files in `tests/data/png/` (generated by `tests/data/png/generate.py`): all color types and bit depths (grayscale 8/16, RGB 8/16, grayscale+alpha 8/16, palette, RGBA 8/16), ancillary chunks (tEXt, zTXt, iTXt, iCCP, sRGB, gAMA, cHRM, eXIf), and multi-frame APNG. Golden-file decode tests compare canonical pixel hashes (FNV-1a 64-bit of decoded pixels) to stored values. Round-trip and encode tests verify save then re-load yields matching pixels.

---

## JPEG

- **Allocator:** The JPEG codec uses the document/codec allocator for all codec-owned allocations.
- **Specification:** ISO/IEC 10918-1 (ITU-T T.81) — Information technology – Digital compression and coding of continuous-tone still images: Requirements and guidelines.
- **Scope:** Baseline DCT (SOF0), extended sequential DCT (SOF1), progressive DCT (SOF2), and their arithmetic-coded counterparts SOF9 and SOF10; 8- and 12-bit sample precision; both entropy coders T.81 defines — Huffman (Annex F, DHT) and adaptive binary arithmetic (Annex D, DAC); COM and APP0–APP15 preservation; DRI/RST; JFIF thumbnail; chroma subsampling and progressive encode (including successive-approximation refinement scans, Ah>0).
- **Decode and encode are not symmetric yet.** Arithmetic coding is implemented on the decode side only: a SOF9 or SOF10 file decodes, and the encoder always writes Huffman.
- **Not implemented:** lossless (SOF3 and SOF11), differential and hierarchical frames (SOF5–SOF7, SOF13–SOF15) — rejected on load.
- **Segment structure:** SOI (0xFF 0xD8), EOI (0xFF 0xD9); segments: 0xFF + marker + length (big-endian, length includes the 2 length bytes) + payload. Parsing enforces max segment size (bomb protection) and overflow-safe pixel count; returns `GIMG_ERR_LIMIT` when exceeded.
- **SOF types and precision:** Supported frame markers: **SOF0** (0xC0, baseline DCT, 8-bit only), **SOF1** (0xC1, extended sequential DCT, 8- or 12-bit), **SOF2** (0xC2, progressive DCT, 8- or 12-bit). Invalid combinations (e.g. SOF0 with precision ≠ 8) return `GIMG_ERR_FORMAT` or `GIMG_ERR_UNSUPPORTED`. Other valid SOF markers per ISO 10918-1 are **SOF3** (0xC3, lossless), **SOF5** (0xC5), **SOF6** (0xC6), **SOF7** (0xC7), **SOF9** (0xC9), **SOF10** (0xCA), **SOF11** (0xCB), **SOF13**–**SOF15** (0xCD–0xCF); 0xC4 is DHT, 0xC8 reserved, 0xCC is DAC — not SOF. Unsupported SOF markers are not accepted. DQT may use 16-bit table entries (Pq=1) at 12-bit; decode uses int32_t dequant/IDCT and outputs GRAY16 or RGBA16. 12-bit samples are stored left-justified in 16-bit (sample<<4).
- **Segment order (load and save):** SOI first; then in any order before the first SOF: APP0–APP15 (JFIF, EXIF, XMP, ICC, COM, IPTC/Adobe, unknown), DQT, DHT, DRI. SOF (SOF0, SOF1, or SOF2) follows; then SOS + entropy-coded scan data. In progressive JPEG, further SOS segments (and their scan data) follow until EOI. DNL (Define Number of Lines, 0xFF 0xDC) may appear after the first scan; see below. RST0–RST7 (0xD0–0xD7) appear only within scan data, not as standalone segments. Unknown markers (e.g. other APPn or future extensions) are skipped and do not break parsing; their payload is discarded unless explicitly stored for round-trip (e.g. unknown APP segments are preserved in meta_raw).
- **DNL (Define Number of Lines, 0xFF 0xDC):** Optional. Segment length is 4 (2-byte length field + 2-byte payload). Payload is the number of lines (image height) as big-endian 16-bit. DNL may appear after the first scan (SOS + scan data). If SOF specified a non-zero height, DNL (when present) must match that height; otherwise the loader returns `GIMG_ERR_FORMAT`. If SOF specified height 0 (streaming case), the loader uses DNL to set the image height; if DNL is absent or height remains 0 after load, the loader returns `GIMG_ERR_FORMAT`. Save does not emit DNL; height is always written in SOF.
- **APP segments:** APP0 (JFIF), APP1 (EXIF, XMP), APP2 (ICC profile). EXIF is parsed via the shared Exif module; metadata common (orientation, DPI, etc.) is populated. Raw APP payloads are preserved for round-trip. Save policies (PRESERVE_ALL, DROP_ALL, STRIP_GPS, NORMALIZE_EXIF, KEEP_RAW_ONLY, KEEP_COMMON_ONLY) are honored.
- **Color:** Grayscale (1 component), YCbCr (4:2:0, 4:2:2, 4:4:4, 4:1:1), CMYK (4 components with tagging). Output is stored in `GIMG_RASTER` with appropriate color info.
- **Extended precision (12-bit):** T.81 Table B.2 gives a DCT-based frame a sample precision of 8 or 12; precision up to 16 exists only for lossless (SOF3). This codec therefore writes and accepts 8 and 12 only. **12-bit encode:** sequential at precision 12 emits SOF1 (extended sequential DCT), single scan, DQT Pq=1; progressive at precision 12 emits SOF2 with two scans (DC then AC), the same extended DHT and 12-bit coefficient range. Both **native 12-bit raster** (GRAY12/RGBA12, uint16_t 0..4095) and the **save option** (`jpeg_precision=12`, with library bit-depth conversion from an 8- or 16-bit raster) are supported; a 16-bit raster is written at 12-bit, and `jpeg_precision=16` returns `GIMG_ERR_UNSUPPORTED`. The extended DHT uses a 242-symbol AC table (162 + 80); note that T.81 F.1.2.2 only needs SSSS up to 14 at 12-bit (226 symbols), so this table is wider than the spec requires — it is not a spec requirement and is under review. **Known defect:** 12-bit encode at quality 88–90 and 94–99 produces a file this library's own decoder rejects as corrupt; 12-bit is not yet validated against an external decoder.
- **Decode capabilities (advertised via codec caps):** **Progressive decode:** SOF2 (progressive DCT), multi-scan (DC, AC initial, AC/DC refinement per T.81 Annex G); non-seekable streams supported. **12-bit decode:** SOF1 (extended sequential) and SOF2 with precision 12; DQT Pq=1 (16-bit quant table entries); output format GRAY16 or RGB16, 12-bit samples left-justified. **Limitations:** 12-bit decode supports grayscale and YCbCr only (no CMYK). The JPEG codec registers `GIMG_CAP_16BPC` to say that its output raster can be 16 bits per channel, not that it reads 16-bit JPEG — no such frame exists.
- **12-bit raster (encode input):** Native 12-bit formats **GIMG_PIXEL_GRAY12** and **GIMG_PIXEL_RGBA12** use uint16_t per sample, value 0..4095 (no left-shift in raster). Bit-depth conversion (8↔12↔16) is a first-class library API (`ghoti.io/image/bitdepth.h`, `gimg_ops_convert_bit_depth`); JPEG (and other codecs) use it when raster depth differs from codec precision.
- **Conformance and tested scope:** Implementation follows ISO 10918-1 baseline and progressive for 8-bit and 12-bit. SOS scan header is as in Annex B: after Ns and the component entries (Cs, Td|Ta per component), the segment ends with Ss (1 byte), Se (1 byte), and one byte with Ah in the high 4 bits and Al in the low 4 bits (successive approximation bit positions). Decode and encode support refinement scans (Ah>0): DC refinement (Ss=0, Se=0) and AC refinement with run-length then one bit per coefficient; a dedicated AC refinement DHT (Th=2) is emitted when needed. **Huffman tables (DHT):** Hard-coded tables in the encoder and DHT writer are spec-matching: 8-bit tables (DC/AC luminance and chrominance) match T.81 Annex K Tables K.3–K.6 (bit-count arrays and symbol order); cross-checked against libjpeg-turbo standard tables. Extended precision tables use 17-symbol DC (size 0..16) and 242-symbol AC (162 + 80 for size 11..15); T.81 F.1.2.2 needs only DC 0..15 and AC 1..14 at 12-bit, so these are wider than required and are under review; DHT payload length obeys B.2.4 (value bytes = sum of 16 bit counts). Decoder rejects DHTs where the number of value bytes does not equal that sum. Golden decode tests use reference files in `tests/data/jpeg/` (generated by `tests/data/jpeg/generate.py`); pixel hashes (FNV-1a 64-bit) and metadata are compared. **External oracle (Phase 2.1):** Decode correctness for Python-generated fixtures (baseline gray/YCbCr, progressive, EXIF, ICC, CMYK) is validated against Pillow via `pillow_decode_hash.py`; encode output in `tests/out/jpeg/` is verified by PIL (`verify_jpeg_output.py`). 12-bit baseline and progressive (grayscale and 3-component) round-trip encode/decode are tested, against this library only — there is no external 12-bit oracle configured. **Official reference software:** ISO/IEC 10918-7:2023 (ITU-T T.873) specifies the JPEG 1 reference software; it is not freely distributed (ISO/ITU purchase). See `tests/data/jpeg/README.md` for oracle options. Fuzz harness `fuzz_jpeg_load` ensures no crash on random or truncated input; errors returned as `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT`. **Known deviations:** progressive AC scans are written with Ns>1, which T.81 G.1.2.2 forbids; SOS parameters (Ss/Se/Ah/Al) are not validated on load.

### JPEG compliance checklist

Short reference for supported markers, precision, entropy, and limitations. Update when adding or restricting features.

| Area | Supported | Rejected / limitation |
|------|------------|-------------------------|
| **Frame (SOF)** | SOF0 (8-bit only), SOF1 (8/12-bit), SOF2 (8/12-bit), SOF9 (8/12-bit, arithmetic), SOF10 (8/12-bit, arithmetic) | SOF0 with P≠8 → GIMG_ERR_FORMAT; any SOF with P=16 → GIMG_ERR_UNSUPPORTED; SOF3 and SOF11 (lossless), SOF5–SOF7 and SOF13–SOF15 (differential/hierarchical) not supported |
| **DAC** | Parsed; conditioning applied per T.81 B.2.4.3, including between scans | Tc>1, Tb>3, DC L>U, or AC Kx outside 1..63 → GIMG_ERR_FORMAT |
| **Entropy** | Huffman (DHT) only | Arithmetic (DAC, QM-coder) not supported |
| **DHT** | T.81 B.2.4: TcTh + 16 bit counts + value bytes; value bytes = sum of counts | Payload length &lt; 17 or &lt; 17+sum(bits) → build fails / GIMG_ERR_FORMAT |
| **DRI/RST** | Restart interval, RST0–RST7 within scan data | — |
| **DNL** | After first scan only; must match SOF height if SOF height ≠ 0 | DNL before first scan → GIMG_ERR_FORMAT; DNL height ≠ SOF height → GIMG_ERR_FORMAT |
| **Precision** | 8 (baseline), 8/12 (SOF1), 8/12 (SOF2) — T.81 Table B.2 allows no other value in a DCT frame | SOF0 with 12-bit → GIMG_ERR_FORMAT; P=16 → GIMG_ERR_UNSUPPORTED (16-bit belongs to lossless SOF3 only) |
| **Color** | Grayscale, YCbCr (4:2:0, 4:2:2, 4:4:4, 4:1:1), CMYK (8-bit) | 12-bit decode: grayscale and YCbCr only (no CMYK) |
| **APP/COM** | APP0 (JFIF/JFXX), APP1 (EXIF/XMP), APP2 (ICC), APP13/14, COM, unknown APP round-trip | — |

### JPEG edge cases and test coverage

Short list of dimension/stream edge cases; update when adding tests. See `image/tasks/image-phase-2.3-jpeg-quality-maintainability.md` task 2.3.3.3.

| Edge case | Covered by |
|-----------|------------|
| **1×1** | SaveRgb1x1ThenLoadDecode (encode→load→decode round-trip) |
| **8×8** | make_minimal_jpeg (SOF0 8×8), baseline_8x8_gray fixtures, ProgressiveMinimalDimensions8x8And16x16, DecodeMinimalJpegReturnsError |
| **16×16** | baseline_16x16_ycbcr fixtures, ProgressiveMinimalDimensions8x8And16x16 |
| **Non-MCU width/height** | Supported; dimensions in SOF; decode uses padded MCUs; no dedicated “non-MCU” fixture (same as 8×8 / 16×16 for minimal) |
| **Restart interval 1** | Not explicitly tested; DRI=4 and 8 used (make_minimal_jpeg_with_dri, EncodeRestartIntervalThenLoadDecodeAndLibjpegOracle, ProgressiveWithRestartIntervalRoundTrip) |
| **Empty / minimal scan** | make_minimal_jpeg (minimal scan data), make_minimal_progressive_jpeg; DecodeMinimalJpegReturnsError (decode fails on empty scan) |
| **DNL-only height** | SOF height 0 with DNL setting height is documented in segment order; not in fixtures (SOF always has non-zero height in tests) |
| **DNL after first scan** | make_minimal_jpeg_with_dnl_after_scan (DNL matches SOF 8); LoadJpegWithDnlMismatchFails, LoadJpegDnlBeforeScanFails (negative) |
| **RST in scan** | make_minimal_jpeg_with_rst_in_scan (RST5), LoadJpegWithRstInScanSucceeds |

---

## Adding a new format

When adding a new format to this document, add a section with:

- **Parts implemented** — What chunks/segments, color models, bit depths, and options are implemented; any “first in priority” or preservation policies.
- **Conformance and tested scope** — Which spec (and version) is followed; tested scope (e.g. reference data under `tests/data/<format>/`, generators, golden/oracle verification).
- **Rejected / limitation** (if applicable) — Unsupported markers, precision, or features and the error returned (e.g. `GIMG_ERR_FORMAT`, `GIMG_ERR_UNSUPPORTED`).

Follow the structure of the PNG and JPEG sections above. For compliance checklists or edge-case tables, add subsections as needed (see JPEG compliance checklist and edge cases).

*Other formats (GIF, TIFF, etc.) will be added as those codecs are implemented.*
