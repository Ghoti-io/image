@page format_webp WebP

# WebP

Phases A–C of the WebP codec: the RIFF/`WEBP` container, `VP8X` canvas
geometry, chunk inventory (including bitstream chunks nested in `ANMF`),
carriage of `ICCP` / `EXIF` / `XMP `, **VP8L lossless picture decode**, and
**ALPH plane decode** (raw and VP8L-compressed, all four spatial filters).
Lossy VP8 colour, animation items, and encode are later phases; see
\ref image_format_references "Formats" and `notes/image/webp-plan.md`.

Claims below are checked. The structure gate is `webpinfo`; lossless pixels
and ALPH planes are gated by `dwebp -pam`, both from the pinned `libwebp`
1.5.0 reference in `tools/oracle/containers/IMAGES` (`deb13-8`).

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
- **ALPH** is specified only by libwebp's decoder: header byte, uncompressed
  or headerless-VP8L payload, spatial unfilters, optional level reduction.
  Alpha samples in the VP8L path ride in the green channel.

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
- **ALPH decode** to an owned `width × height` alpha plane: methods 0
  (uncompressed) and 1 (headerless VP8L), filters none / horizontal /
  vertical / gradient. Level-reduction dithering is not applied — default
  `dwebp` leaves it off, so PAM alpha is the unfiltered plane. Assembling
  ALPH with VP8 colour into a full raster waits on phase D.

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
| ALPH plane | byte-identical to `dwebp` alpha | corrupt → `GIMG_ERR_CORRUPT`; not yet merged with VP8 colour |
| VP8 decode | — | `GIMG_ERR_UNSUPPORTED` (phase D); lossy+alpha item decode refused |
| Animation items | chunks walked | frames not yet `GIMG_ITEM_FRAME` (phase E); decode refused |
| Encode | — | `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from libwebp

| Case | This codec | Elsewhere |
|---|---|---|
| VP8L / ALPH samples | match `dwebp -pam` | same |
| VP8 colour / anim decode | refused until later phases | `dwebp` / `anim_dump` decode |
| Animation model | one `IMAGE` item; inventory only | `anim_dump` expands frames |
| VP8L / ALPH oracle count | one reference (libwebp) | wrappers around the same code are not additional oracles |

## Tested scope

- Fixtures under `tests/data/webp/`: simple lossy/lossless, lossy+alpha,
  lossless+alpha, EXIF via `webpmux`, animation via `img2webp`, gradient and
  checkerboard lossless files, ALPH method 0/1 fixtures from `cwebp`
  `-alpha_method` / `-alpha_filter`, plus crafted uncompressed ALPH round
  trips for every spatial filter, and truncated / oversized-RIFF corrupt
  cases.
- Structure vs `webpinfo` (`verify_webp_structure.py`).
- Pixels vs `dwebp -pam` for every lossless fixture; ALPH plane vs PAM alpha
  for every ALPH-bearing fixture.
- Unit tests: load, VP8L decode, ALPH plane match, filter round trip,
  lossy/anim still unsupported for full decode, save unsupported.
- Fuzz: `fuzz_webp_load` with seeds from the fixture set.

## Not implemented

- VP8 decode and ALPH+VP8 raster assembly (phase D), animation items (E),
  VP8L encode (F). Lossy encode is a documented refusal until an RDO
  measurement plan exists.

---

Back to \ref image_format_references "Formats".
