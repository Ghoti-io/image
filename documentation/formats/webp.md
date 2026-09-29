@page format_webp WebP

# WebP

Phase A of the WebP codec: the RIFF/`WEBP` container, `VP8X` canvas geometry,
chunk inventory (including bitstream chunks nested in `ANMF`), and carriage of
`ICCP` / `EXIF` / `XMP `. Picture decode and encode are not implemented yet;
see \ref image_format_references "Formats".

Claims below are checked. The structure gate is `webpinfo` from the pinned
`libwebp` reference in `tools/oracle/containers/IMAGES`.

## Normative references

- **WebP Container Specification** (Google). Not an ISO/ITU standard; retrieved
  alongside the Phase A implementation (2026-09-29). Where it is silent,
  libwebp's `webpinfo` behaviour is treated as the practical reference for
  chunk listing.
- **VP8** bitstream headers only (RFC 6386): key-frame start code and coded
  dimensions, used when a simple lossy file has no `VP8X`.
- **VP8L** bitstream signature and dimension/alpha bits (Google's lossless
  bitstream specification), used when a simple lossless file has no `VP8X`.

Codec-owned allocations use the codec's allocator (default when NULL).

## Parts implemented

- RIFF form type `WEBP`, chunk walk with odd-size padding.
- `VP8X` feature flags and canvas width/height.
- Simple `VP8 ` / `VP8L` files: canvas from the bitstream header peek.
- Nested `ALPH` / `VP8 ` / `VP8L` inside `ANMF`, listed at absolute offsets the
  way `webpinfo` does.
- `ICCP`, `EXIF`, `XMP ` attached as `meta_raw` under format id `"webp"` with
  the chunk fourcc as the tag; EXIF orientation into `meta_common` when
  parseable.
- `max_decoded_pixels` applied to the canvas product at load.

## Save

Not implemented. `gimg_doc_save(..., "webp", ...)` returns
`GIMG_ERR_UNSUPPORTED`. Phase F will write lossless (`VP8L`) only; lossy
encode is intentionally refused until there is a rate-distortion measurement
plan (`notes/image/webp-plan.md` §6).

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|------------------------|
| Container | RIFF/`WEBP`, `VP8X`, chunk walk | truncated header, RIFF size past EOF, chunk size past end → `GIMG_ERR_CORRUPT` |
| Canvas | from `VP8X` or VP8/VP8L peek | unknown size → `GIMG_ERR_CORRUPT`; over `max_decoded_pixels` → `GIMG_ERR_LIMIT` |
| Metadata | `ICCP`/`EXIF`/`XMP ` on the document | — |
| Picture decode | — | `GIMG_ERR_UNSUPPORTED` (phases B/D) |
| Animation items | chunks walked | frames not yet `GIMG_ITEM_FRAME` (phase E) |
| Encode | — | `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from libwebp

| Case | This codec | Elsewhere |
|---|---|---|
| Decode | refused in Phase A | `dwebp` decodes |
| Animation model | one `IMAGE` item; inventory only | `anim_dump` expands frames |

## Tested scope

- Fixtures under `tests/data/webp/`: simple lossy/lossless, lossy+alpha,
  lossless+alpha, EXIF via `webpmux`, animation via `img2webp`, plus truncated
  and oversized-RIFF corrupt cases. Generated with the pinned `cwebp` /
  `webpmux` / `img2webp` in the oracle image.
- Structure vs `webpinfo` (`verify_webp_structure.py`): fourcc, offset and
  length for every listed chunk.
- Unit tests: load, canvas, EXIF attach, corrupt refuse, decode/save
  unsupported.
- Fuzz: `fuzz_webp_load` with seeds from the fixture set.

## Not implemented

- VP8L decode (phase B), ALPH plane (C), VP8 decode (D), animation items (E),
  VP8L encode (F). Lossy encode is planned as a documented refusal, not a
  phase, until an RDO measurement plan exists.

---

Back to \ref image_format_references "Formats".
