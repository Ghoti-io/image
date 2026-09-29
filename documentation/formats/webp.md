@page format_webp WebP

# WebP

Phases A–F of the WebP codec: the RIFF/`WEBP` container, `VP8X` canvas
geometry, chunk inventory (including bitstream chunks nested in `ANMF`),
carriage of `ICCP` / `EXIF` / `XMP `, **VP8L lossless picture decode**,
**ALPH plane decode**, **VP8 lossy keyframe decode** (with optional ALPH
merged into RGBA), **ANIM/ANMF animation** (frame items with offset,
duration, dispose and blend; decode returns the composited canvas), and
**VP8L lossless encode** of still images. Lossy encode is refused; see
\ref image_format_references "Formats" and `notes/image/webp-plan.md`.

Claims below are checked. The structure gate is `webpinfo` (committed
fixtures under `tests/data/webp/` plus outside corpora from
`tools/oracle/fetch.sh webp-refs`). Pixel identity for those same trees —
stills vs `dwebp -pam` (with this library's EXIF orientation apply),
animations vs `anim_dump -pam` — is `verify_webp_pixels.py`. Committed
fixture PAMs remain the unit-test goldens. Lossless saves round-trip through
our decoder and are accepted by `dwebp`, Pillow and ImageMagick. All from
the pinned `libwebp` 1.5.0 reference in `tools/oracle/containers/IMAGES`
(`deb13-8`).

## Normative references

- **WebP Container Specification** (Google). Not an ISO/ITU standard; retrieved
  alongside the Phase A implementation (2026-09-29). Where it is silent,
  libwebp's `webpinfo` behaviour is treated as the practical reference for
  chunk listing.
- **VP8** (RFC 6386, *VP8 Data Format and Decoding Guide*). Key frames only —
  WebP still images never use inter prediction. The RFC's embedded reference C
  and libwebp 1.5.0's C decode paths are the practical specs; this codec ports
  the latter for byte identity with `dwebp`.
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
  `dwebp` leaves it off, so PAM alpha is the unfiltered plane.
- **VP8 keyframe decode** to `GIMG_PIXEL_RGBA8`: boolean decoder, segment /
  filter / quant / probability headers, intra prediction, IDCT/WHT, loop
  filter, fancy 4:2:0 upsample, and libwebp's fixed-point YUV→RGB. When an
  `ALPH` chunk is present, its plane replaces the opaque alpha channel
  sample-wise. Inter frames are refused.
- **Animation** (`ANIM` / `ANMF`): one `GIMG_ITEM_FRAME` per ANMF (or a single
  `IMAGE` when there is only one frame), with duration in ms/1000, dispose
  `NONE`/`BACKGROUND`, and blend `SOURCE`/`OVER`. Frame offsets stay in
  codec-private state. Decode of item *i* returns the full VP8X canvas after
  compositing frames `0…i`, matching `anim_dump`. `ANIM` loop count and
  background colour are carried on the document. `max_frame_count` caps ANMFs.
- **VP8L encode** of a still image (first document item): Huffman over
  literals, subtract-green when `webp_effort` ≥ 1, optional `ICCP` / `EXIF` /
  `XMP ` via `VP8X`. See Save.

## YUV→RGB (a match, not a derivation)

VP8 produces YUV 4:2:0. Turning that into RGB is a choice of matrix, range and
rounding. This codec **matches libwebp 1.5.0's fixed-point conversion and
fancy upsampler** exactly — the same path `dwebp` uses — rather than deriving
a floating-point conversion that would disagree by ±1 forever. Say so on every
oracle comparison: identity with `dwebp -pam` is the gate, not "close to BT.601".

## Save

Lossless only (`VP8L`). `gimg_doc_save(..., "webp", ...)` writes a simple
RIFF/`WEBP`/`VP8L` file, or an extended file with `VP8X` when `ICCP` / `EXIF` /
`XMP ` are preserved. Options: `webp_effort` (0–9; ≥1 applies subtract-green),
`webp_exact` (preserve RGB under full transparency), and `webp_lossless`
(`GIMG_WEBP_COMPRESS_LOSSLESS`, the default; `GIMG_WEBP_COMPRESS_LOSSY` →
`GIMG_ERR_UNSUPPORTED`). Multi-frame documents are written as a still of the first item (animation
encode is not implemented); `GIMG_WEBP_COMPRESS_LOSSY` and non-8-bit sources
are refused.

Round-trip through this library's decoder is identity. Outside consumers
(`dwebp`, Pillow, ImageMagick) decode the bytes to the same pixels. Size is
not competitive with `cwebp -lossless` when the latter uses prediction and
LZ77: on the 32×32 `lossless_gradient` fixture this encoder wrote **2160**
bytes against `cwebp -lossless -exact` at **60** bytes (measured 2026-09-29
in `deb13-8`). That gap is expected until LZ77 and spatial transforms land;
the gate for Phase F is correctness, not rate.

Lossy encode remains intentionally refused until there is a rate-distortion
measurement plan (`notes/image/webp-plan.md` §6).

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|------------------------|
| Container | RIFF/`WEBP`, `VP8X`, chunk walk | truncated header, RIFF size past EOF, chunk size past end → `GIMG_ERR_CORRUPT` |
| Canvas | from `VP8X` or VP8/VP8L peek | unknown size → `GIMG_ERR_CORRUPT`; over `max_decoded_pixels` → `GIMG_ERR_LIMIT` |
| Metadata | `ICCP`/`EXIF`/`XMP ` on the document | — |
| VP8L decode | byte-identical to `dwebp -pam` | corrupt bitstream → `GIMG_ERR_CORRUPT` |
| ALPH plane | byte-identical to `dwebp` alpha | corrupt → `GIMG_ERR_CORRUPT` |
| VP8 decode | keyframe → RGBA, optional ALPH merge; byte-identical to `dwebp -pam` | inter frame / corrupt → `GIMG_ERR_CORRUPT` / `UNSUPPORTED` |
| Animation | ANMF → `FRAME` items; composite matches `anim_dump -pam` | rectangle past canvas / no bitstream → `GIMG_ERR_CORRUPT`; over `max_frame_count` → `GIMG_ERR_LIMIT` |
| Encode | VP8L still of the first item; optional ICCP/EXIF/XMP; round-trip identity | lossy / non-8-bit → `GIMG_ERR_UNSUPPORTED`; animation encode not implemented (first frame only) |

## Where this codec differs from libwebp

| Case | This codec | Elsewhere |
|---|---|---|
| VP8 / VP8L / ALPH samples | match `dwebp -pam` | same |
| Anim composite | match `anim_dump -pam` | same |
| Lossless save | round-trip identity; accepted by `dwebp`/Pillow/ImageMagick | `cwebp -lossless` is much smaller (prediction + LZ77); see Save |
| Dispose to background | clears the frame rect to transparent (libwebp) | same; ANIM bgcolor is reported, not painted on dispose |
| VP8L / ALPH oracle count | one reference (libwebp) | wrappers around the same code are not additional oracles |
| VP8 oracle count | libwebp is the RGB gate; FFmpeg/libvpx are independent for YUV | three readings for the bitstream |
| VP8 implementation | adapted libwebp 1.5.0 C paths in `vp8ref/` (BSD; see `vp8ref/COPYING`) behind LGPL wrappers | same algorithm as `dwebp` by construction |

## Tested scope

- Fixtures under `tests/data/webp/`: simple lossy/lossless, lossy+alpha,
  lossless+alpha, EXIF via `webpmux`, animation via `img2webp` / `webpmux`
  (including a non-zero frame offset and dispose-to-background), gradient and
  checkerboard lossless files, ALPH method 0/1 fixtures from `cwebp`
  `-alpha_method` / `-alpha_filter`, plus crafted uncompressed ALPH round
  trips for every spatial filter, and truncated / oversized-RIFF corrupt
  cases.
- Outside-written corpora fetched by `tools/oracle/fetch.sh webp-refs` into
  `third_party/webp-refs/` at the commits `tools/oracle/VERSIONS` names:
  Google's `libwebp-test-data`, imazen `codec-corpus` (`webp-conformance/`),
  Pillow `Tests/images/*.webp`, image-rs `tests/images/webp/` (plus the
  regression panic fixture), and golang.org/x/image `testdata/*.webp`.
  None of these are committed. Structure and pixel gates cover every file
  libwebp accepts; files it refuses (intentional bad inputs in those trees)
  are skipped.
- Structure vs `webpinfo` (`verify_webp_structure.py`).
- Still and animation pixels vs `dwebp` / `anim_dump`
  (`verify_webp_pixels.py` via `dump_webp_raster`): committed fixtures and
  outside corpora together. EXIF orientation is applied on decode (as for
  every codec); the gate remaps `dwebp`'s bitstream pixels the same way.
  Colour under a fully transparent pixel is not compared (`dwebp` keeps YUV
  residue; `anim_dump` clears it).
- Committed fixture PAMs vs unit tests for lossless/lossy/ALPH/anim.
- Lossless save: round-trip identity through this decoder; outside acceptance
  by `dwebp`, Pillow and ImageMagick; size vs `cwebp -lossless -exact` on
  `lossless_gradient` published on the Save section (2160 vs 60 bytes).
- Unit tests: load, VP8L/VP8 decode, ALPH plane match, filter round trip,
  anim geometry/dispose/blend, save round-trip, lossy-save refusal.
- Fuzz: `fuzz_webp_load` with seeds from the fixture set.

## Not implemented

- Animation encode (multi-frame `ANIM`/`ANMF` write). A multi-item document
  is saved as a still of the first item.
- Lossy (`VP8 `) encode — refused until an RDO measurement plan exists
  (`notes/image/webp-plan.md` §6).
- LZ77 / predictor / cross-colour / palette search in the lossless encoder
  (literals + optional subtract-green only today).

---

Back to \ref image_format_references "Formats".
