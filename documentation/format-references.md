# Format and specification references

This document lists the external specifications (or parts thereof) that the image library implements for each format. Use these as the authoritative reference for chunk layout, ordering, and behavior.

## PNG

- **Specification:** ISO/IEC 15948:2004 (PNG — Portable Network Graphics), and the PNG Specification (Second Edition) at <https://www.w3.org/TR/PNG/>.
- **APNG:** APNG extension (animated PNG); see <https://wiki.mozilla.org/APNG_Specification> and common implementations.

### Parts implemented (Phase 1)

- **Chunk layout and CRC:** Signature (5.2), chunk structure — 4-byte length (big-endian), 4-byte type, payload, 4-byte CRC (5.3). CRC-32 over type+payload per PNG spec.
- **Chunk ordering:** Critical and ancillary chunk order per spec (5.4 and 11.2–11.3): IHDR first; PLTE (and tRNS if present) before IDAT for palette images; ancillary in allowed positions; IDAT contiguous; IEND last. On save, emit chunks in this order.
- **Critical chunks:** IHDR (11.2.1), PLTE (11.2.2), IDAT (11.2.4), IEND (11.2.5). For palette images, PLTE (and optional tRNS) must appear before IDAT.
- **Ancillary chunks:** tEXt, zTXt, iTXt, iCCP, sRGB, gAMA, cHRM, eXIf, and others as raw or typed per spec (11.3).
- **Filtering and interlace:** Filter types (9), Adam7 interlace (8).
- **APNG:** acTL, fcTL, fdAT; frame timing, dispose (None/Background/Previous), blend (Source/Over) per APNG spec.

---

*Other formats (JPEG, GIF, TIFF, etc.) will be added as those codecs are implemented.*
