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
- **GIMG_META_STRIP_GPS:** Emit ancillary as PRESERVE_ALL except: the eXIf chunk is omitted (so no GPS or other Exif data is written). tEXt, zTXt, and iTXt chunks whose keyword is "GPS", "GPS " (with trailing space), or "EXIF:GPS" (case-insensitive) are omitted. Other ancillary is preserved.
- **GIMG_META_NORMALIZE_EXIF:** Currently eXIf and other ancillary are passed through unchanged. Full normalization (orientation applied/cleared, duplicate Exif tags removed) may be implemented when Exif parsing is available.
- **GIMG_META_KEEP_RAW_ONLY:** Emit only ancillary chunks that are *not* known semantic metadata. Omitted: iCCP, sRGB, gAMA, cHRM, eXIf, tEXt, zTXt, iTXt. Emitted: any other ancillary chunk type (e.g. unknown or private chunks) in read order. eXIf from doc meta_raw is not written. PLTE/tRNS are emitted when required for the image.
- **GIMG_META_KEEP_COMMON_ONLY:** Emit only metadata that maps to common metadata. Exactly one color chunk is written from the decoded raster’s `GIMG_Color_Info`: sRGB (if transfer/primaries indicate sRGB), or gAMA (if transfer is gamma with a positive value), or iCCP (if an ICC profile is attached). No eXIf, no text chunks, no other ancillary. PLTE/tRNS are emitted when required for the image.

---

*Other formats (JPEG, GIF, TIFF, etc.) will be added as those codecs are implemented.*
