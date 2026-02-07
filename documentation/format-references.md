@page format_references Format and Specification References

# Format and specification references

This document lists the external specifications (or parts thereof) that the image library implements for each format. Use these as the authoritative reference for chunk layout, ordering, and behavior.

## PNG

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

- **Specification:** ISO/IEC 10918-1 (ITU-T T.81) — Information technology – Digital compression and coding of continuous-tone still images: Requirements and guidelines.
- **Scope:** Baseline DCT (SOF0), progressive DCT (SOF2), lossless (SOF3), and hierarchical (SOF4–SOF7 as implemented); 8-, 12-, and 16-bit sample precision; **Huffman (DHT) and arithmetic (DAC, QM-coder) entropy coding**; COM and APP0–APP15 preservation; DRI/RST; JFIF thumbnail; chroma subsampling and progressive encode. See `image/tasks/image-phase-2-jpeg.md` for full task list; scope is updated as milestones 2.5–2.9 are completed.
- **Segment structure:** SOI (0xFF 0xD8), EOI (0xFF 0xD9); segments: 0xFF + marker + length (big-endian, length includes the 2 length bytes) + payload. Parsing enforces max segment size (bomb protection) and overflow-safe pixel count; returns `GIMG_ERR_LIMIT` when exceeded.
- **APP segments:** APP0 (JFIF), APP1 (EXIF, XMP), APP2 (ICC profile). EXIF is parsed via the shared Exif module; metadata common (orientation, DPI, etc.) is populated. Raw APP payloads are preserved for round-trip. Save policies (PRESERVE_ALL, DROP_ALL, STRIP_GPS, NORMALIZE_EXIF, KEEP_RAW_ONLY, KEEP_COMMON_ONLY) are honored.
- **Color:** Grayscale (1 component), YCbCr (4:2:0, 4:2:2, 4:4:4, 4:1:1), CMYK (4 components with tagging). Output is stored in `GIMG_RASTER` with appropriate color info.
- **Conformance and tested scope:** Implementation follows ISO 10918-1 baseline and progressive for 8-bit. SOS scan header is as in Annex B: after Ns and the component entries (Cs, Td|Ta per component), the segment ends with Ss (1 byte), Se (1 byte), and one byte with Ah in the high 4 bits and Al in the low 4 bits (successive approximation bit positions). Golden decode tests use reference files in `tests/data/jpeg/` (generated by `tests/data/jpeg/generate.py`); pixel hashes (FNV-1a 64-bit) and metadata are compared. Fuzz harness `fuzz_jpeg_load` ensures no crash on random or truncated input; errors returned as `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT`. No intentional deviations from the spec.

---

*Other formats (GIF, TIFF, etc.) will be added as those codecs are implemented.*
