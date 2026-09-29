@page format_webp WebP

# WebP

Phases A–B of the WebP codec: the RIFF/`WEBP` container, `VP8X` canvas
geometry, chunk inventory (including bitstream chunks nested in `ANMF`),
carriage of `ICCP` / `EXIF` / `XMP `, and **VP8L lossless picture decode**.
Lossy VP8, separate `ALPH`, animation items, and encode are later phases;
see \ref image_format_references "Formats" and `notes/image/webp-plan.md`.

Claims below are checked. The structure gate is `webpinfo` and the lossless
pixel gate is `dwebp -pam`, both from the pinned `libwebp` 1.5.0 reference in
`tools/oracle/containers/IMAGES` (`deb13-8`).

## Normative references

- **WebP Container Specification** (Google). Not an ISO/ITU standard; retrieved
  alongside the Phase A implementation (2026-09-29). Where it is silent,
  libwebp's `webpinfo` behaviour is treated as the practical reference for
  chunk listing.
- **VP8** bitstream headers only (RFC 6386): key-frame start code and coded
  dimensions, used when a simple lossy file has no `VP8X`. Full VP8 decode is
  phase D.
- **VP8L** (Google's "WebP Lossless Bitstream Specification"). The prose is
  incomplete in places; **libwebp is the specification** wherever they
  disagree. Decode is required to be byte-identical to `dwebp`.

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
- **VP8L decode** to `GIMG_PIXEL_RGBA8`: prefix codes, LZ77, colour cache, and
  the four inverse transforms (predictor, cross-colour, subtract-green,
  colour-indexing). Top-level `VP8L` only (simple files and `VP8X`+`VP8L`
  with alpha carried in the VP8L stream).

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
| VP8L decode | byte-identical to `dwebp -pam` | corrupt bitstream → `GIMG_ERR_CORRUPT` |
| VP8 decode | — | `GIMG_ERR_UNSUPPORTED` (phase D) |
| ALPH plane | detected at load | not assembled (phase C); lossy+alpha decode refused |
| Animation items | chunks walked | frames not yet `GIMG_ITEM_FRAME` (phase E); decode refused |
| Encode | — | `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from libwebp

| Case | This codec | Elsewhere |
|---|---|---|
| VP8L pixels | match `dwebp -pam` | same |
| VP8 / ALPH / anim decode | refused until later phases | `dwebp` / `anim_dump` decode |
| Animation model | one `IMAGE` item; inventory only | `anim_dump` expands frames |
| VP8L oracle count | one reference (libwebp) | wrappers around the same code are not additional oracles |

## Tested scope

- Fixtures under `tests/data/webp/`: simple lossy/lossless, lossy+alpha,
  lossless+alpha, EXIF via `webpmux`, animation via `img2webp`, gradient and
  checkerboard lossless files that exercise predictor, cross-colour,
  subtract-green and palette transforms, plus truncated and oversized-RIFF
  corrupt cases. Generated with the pinned `cwebp` / `webpmux` / `img2webp`
  in the oracle image.
- Structure vs `webpinfo` (`verify_webp_structure.py`): fourcc, offset and
  length for every listed chunk.
- Pixels vs `dwebp -pam` (committed `.pam` sidecars): every lossless fixture.
- Unit tests: load, canvas, EXIF attach, corrupt refuse, VP8L decode match,
  lossy/anim/ALPH still unsupported for decode, save unsupported.
- Fuzz: `fuzz_webp_load` with seeds from the fixture set.

## Not implemented

- ALPH plane (phase C), VP8 decode (D), animation items (E), VP8L encode (F).
  Lossy encode is planned as a documented refusal, not a phase, until an RDO
  measurement plan exists.

---

Back to \ref image_format_references "Formats".
