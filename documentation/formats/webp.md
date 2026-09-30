@page format_webp WebP

# WebP

Phases A–G of the WebP codec: the RIFF/`WEBP` container, `VP8X` canvas
geometry, chunk inventory (including bitstream chunks nested in `ANMF`),
carriage of `ICCP` / `EXIF` / `XMP `, **VP8L lossless picture decode**,
**ALPH plane decode**, **ANIM/ANMF animation** (frame items with offset,
duration, dispose and blend; decode returns the composited canvas),
**VP8L lossless encode**, and a **stub VP8 lossy encode** (not competitive;
see Save and `make webp-rd`). See \ref image_format_references "Formats"
and `notes/image/webp-plan.md`.

Claims below are checked. The structure gate is `webpinfo` (committed
fixtures under `tests/data/webp/` plus outside corpora from
`tools/oracle/fetch.sh webp-refs`). Pixel identity for those same trees —
stills vs `dwebp -pam` (with this library's EXIF orientation apply),
animations vs `anim_dump -pam` — is `verify_webp_pixels.py`. Committed
fixture PAMs remain the unit-test goldens. Lossless saves are left in
`tests/out/webp/` and checked by `verify_webp_output.py`: `dwebp -pam`
must match the source pixels, our decoder must match `dwebp`, and the
file's size and encode time are printed beside `cwebp -lossless -exact`.
A VP8 keyframe is decoded, coefficient partitions, segmentation, and
the loop filter included, and matches `dwebp`. Chroma is upsampled
with the 9-3-3-1 kernel. A nonzero frame scale is left at the coded
size. An interframe predicts from the last, golden, and altref
pictures of the same sequence; a still interframe has no reference and
is corrupt. Lossy stub saves are
accepted by `dwebp`; rate/distortion vs `cwebp` is reported by `make
webp-rd`. The reference is the pinned `libwebp` 1.5.0 image in
`tools/oracle/containers/IMAGES` (`deb13-8`). This library does not
contain libwebp's source.

## Normative references

- **WebP Container Specification** (Google). Not an ISO/ITU standard; retrieved
  alongside the Phase A implementation (2026-09-29). Where it is silent,
  libwebp's `webpinfo` behaviour is treated as the practical reference for
  chunk listing.
- **VP8** (RFC 6386, *VP8 Data Format and Decoding Guide*). Keyframes and
  interframes. A still image's interframe is corrupt: it has no reference
  picture. An animation keeps the last, golden, and altref pictures across
  frames. Reconstruction follows that RFC. Keyframe pixels match `dwebp`,
  coefficient partitions and segmentation included. The 9-3-3-1 chroma
  upsample is the display step `dwebp` applies; the RFC leaves that
  conversion unspecified. The container tools refuse an interframe
  animation, so that path is checked by shifting `dwebp -yuv` of the
  keyframe.
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
- **VP8 decode** to `GIMG_PIXEL_RGBA8`: boolean decoder, segment / filter /
  quant / probability headers, intra prediction, inter prediction, the
  coefficient partitions of section 9.5, IDCT/WHT, the section 15 loop
  filter, the 9-3-3-1 chroma upsample, and the fixed-point YUV→RGB
  conversion `dwebp` uses. A nonzero frame-scale field is ignored and the
  coded size is emitted. An animation keeps the last, golden, and altref
  pictures across its frames, so an interframe predicts from earlier frames
  of that decode. Dispose and blend still follow the ANMF flags. When an
  `ALPH` chunk is present, its plane replaces the opaque alpha channel
  sample-wise. A still interframe is corrupt. A version above 3 is
  unsupported.
- **Animation** (`ANIM` / `ANMF`): one `GIMG_ITEM_FRAME` per ANMF (or a single
  `IMAGE` when there is only one frame), with duration in ms/1000, dispose
  `NONE`/`BACKGROUND`, and blend `SOURCE`/`OVER`. Frame offsets stay in
  codec-private state. Decode of item *i* returns the full VP8X canvas after
  compositing frames `0…i`, matching `anim_dump`. `ANIM` loop count and
  background colour are carried on the document. `max_frame_count` caps ANMFs.
- **VP8L encode** of a still image (first document item): Huffman over
  literals, subtract-green when `webp_effort` ≥ 1, optional `ICCP` / `EXIF` /
  `XMP ` via `VP8X`. See Save.
- **VP8 lossy stub encode** when `webp_lossless = GIMG_WEBP_COMPRESS_LOSSY`:
  Intra16 DC, single quantizer, opaque only. Not competitive with `cwebp`;
  see Save and `make webp-rd`.

## YUV→RGB (a match, not a derivation)

VP8 produces YUV 4:2:0. Turning that into RGB is a choice of matrix, range and
rounding. This codec **matches libwebp 1.5.0's fixed-point conversion and
fancy upsampler** exactly — the same path `dwebp` uses — rather than deriving
a floating-point conversion that would disagree by ±1 forever. Say so on every
oracle comparison: identity with `dwebp -pam` is the gate, not "close to BT.601".

## Save

Default is lossless (`VP8L`). `gimg_doc_save(..., "webp", ...)` writes a
simple RIFF/`WEBP`/`VP8L` file, or an extended file with `VP8X` when `ICCP` /
`EXIF` / `XMP ` are preserved. Options: `webp_effort` (0–9; ≥1 applies
subtract-green for lossless when the literal histogram shrinks, ≥2 also
applies one spatial predictor when that histogram shrinks and then LZ77,
≥3 adds a cross-colour transform when the finished bitstream is shorter,
≥4 also tries a palette of at most 256 colours and keeps it when the file
is smaller; the main image also keeps a Huffman image when several tree
groups encode smaller than one; coarse Q ladder for lossy), `webp_exact`
(preserve RGB under full transparency, lossless only), and `webp_lossless`
(`GIMG_WEBP_COMPRESS_LOSSLESS` by default, or `GIMG_WEBP_COMPRESS_LOSSY` for
the stub VP8 encoder). Multi-frame documents are written as a still of the
first item (animation encode is not implemented). Non-8-bit sources are
refused. Lossy with any non-opaque alpha is refused until ALPH encode exists.

**Lossless.** Round-trip through this library's decoder is identity. Outside
consumers (`dwebp`, Pillow, ImageMagick) decode the bytes to the same pixels.
On the 32×32 `lossless_gradient` fixture at effort 2 and effort 4 this
encoder wrote **48** bytes against `cwebp -lossless -exact` at **60** bytes
(measured 2026-09-30 in `deb13-8`; `dwebp -pam` matched the fixture). The
stream is one predictor, the same shape as that `cwebp` file. No
cross-colour grid shortened it, so effort 4 stays at 48. Effort 1, literals
with subtract-green only when the histogram shrinks, is **1950** bytes
(2160 when subtract-green was unconditional and the image used one Huffman
group). That fixture has more than 256 colours, so the palette pass does
not apply. On `lossless_checker` effort 4 writes **46** bytes (62 without
the palette).

**Lossy (stub).** Intra16 DC only, single quantizer, Y2/UV DC residuals —
deliberately not competitive with `cwebp`. Accepted by `dwebp` and by this
decoder. Rate and distortion vs a fast `cwebp` baseline are reported by
`make webp-rd` (PNG corpus from `tools/oracle/fetch.sh webp-rd`; axes:
bytes and PSNR-RGB over opaque pixels vs `cwebp -q 75 -m 0`). The target
exits non-zero only if encode/decode/measure plumbing breaks; it does not
yet fail on worse PSNR or size.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|------------------------|
| Container | RIFF/`WEBP`, `VP8X`, chunk walk | truncated header, RIFF size past EOF, chunk size past end → `GIMG_ERR_CORRUPT` |
| Canvas | from `VP8X` or VP8/VP8L peek | unknown size → `GIMG_ERR_CORRUPT`; over `max_decoded_pixels` → `GIMG_ERR_LIMIT` |
| Metadata | `ICCP`/`EXIF`/`XMP ` on the document | — |
| VP8L decode | byte-identical to `dwebp -pam` | corrupt bitstream → `GIMG_ERR_CORRUPT` |
| ALPH plane | byte-identical to `dwebp` alpha | corrupt → `GIMG_ERR_CORRUPT` |
| VP8 decode | keyframe and, in one sequence, interframe → RGBA, optional ALPH merge, 9-3-3-1 chroma; a keyframe matches `dwebp` | a still interframe → `GIMG_ERR_CORRUPT`; version above 3 → `GIMG_ERR_UNSUPPORTED` |
| Animation | ANMF → `FRAME` items; composite matches `anim_dump -pam` | rectangle past canvas / no bitstream → `GIMG_ERR_CORRUPT`; over `max_frame_count` → `GIMG_ERR_LIMIT` |
| Encode | VP8L still (default) or stub VP8 lossy of the first item; optional ICCP/EXIF/XMP; lossless round-trip identity | non-8-bit → `GIMG_ERR_UNSUPPORTED`; lossy + non-opaque alpha → `UNSUPPORTED`; animation encode not implemented (first frame only) |

## Where this codec differs from libwebp

| Case | This codec | Elsewhere |
|---|---|---|
| VP8L / ALPH samples | match `dwebp -pam` | same |
| VP8 bitstream | keyframe and interframe; a keyframe matches `dwebp` | `dwebp` and `anim_dump` refuse an interframe animation |
| Anim composite | match `anim_dump -pam` | same |
| Lossless save | round-trip identity; accepted by `dwebp`; effort ≥ 2 uses one predictor and LZ77 when the residual histogram shrinks; effort ≥ 3 adds cross-colour when the file is shorter; effort ≥ 4 keeps a palette when it is smaller; a Huffman image is kept when it is smaller | gradient effort 4 is 48 bytes, `cwebp -lossless -exact` is 60; see Save |
| Lossy save | stub VP8 accepted by `dwebp` and by our decoder; `make webp-rd` vs `cwebp -q 75 -m 0` | not competitive; quality bar not armed |
| Dispose to background | clears the frame rect to transparent (libwebp) | same; ANIM bgcolor is reported, not painted on dispose |
| VP8L / ALPH oracle count | one reference (libwebp) | wrappers around the same code are not additional oracles |
| VP8 oracle count | libwebp is the RGB gate; FFmpeg/libvpx are independent for YUV | three readings for the bitstream |
| VP8 implementation | not in this repository | `dwebp` in the oracle image |

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
- VP8 keyframe features vs `dwebp` (`verify_vp8_intra.py`): intra modes,
  the loop filter (including sharpness and the simple filter), and
  segmentation. Coefficient partitions and the frame-scale file are in
  the pixel gate, which decodes at the coded size.
- VP8 interframes (`verify_vp8_inter.py`). A skipped zero-motion frame
  matches `dwebp` of the keyframe. An integer or fractional motion
  vector matches that keyframe's YUV shifted with the edge repeated,
  then the 9-3-3-1 upsample: bicubic on version 0, bilinear on version
  1, and the integer sample on version 3. The same shift checks a
  top/bottom split, a golden and an altref kept from the keyframe, a
  sign-bias flip of the nearest vector, and a simple loop-filter edge
  between two macroblocks. `cwebp` and `dwebp` run in the oracle
  image. The animation is written under `tests/out/` because the
  container tools refuse an interframe payload.
- Lossless save: round-trip identity through this decoder; outside acceptance
  by `dwebp`, Pillow and ImageMagick; size vs `cwebp -lossless -exact` on
  `lossless_gradient` published on the Save section (48 vs 60 bytes at
  effort 2 and 4). Effort 4 is no larger than effort 2, which is smaller
  than effort 1, on that fixture.
- Lossy stub save: unit tests for decodable round-trip and alpha refusal;
  `make webp-rd` vs `cwebp -q 75 -m 0` on `third_party/webp-rd/`
  (`tools/oracle/fetch.sh webp-rd`).
- Unit tests: load, VP8L/VP8 decode, ALPH plane match, filter round trip,
  anim geometry/dispose/blend, lossless save round-trip, lossy stub
  decodable / alpha refused.
- Fuzz: `fuzz_webp_load` with seeds from the fixture set.

## Not implemented

- Animation encode (multi-frame `ANIM`/`ANMF` write). A multi-item document
  is saved as a still of the first item.
- Competitive lossy encode (mode search, trellis, segments, SNS, multi-pass)
  and lossy ALPH. The stub exists; `make webp-rd` measures it; the quality
  bar is not armed (`notes/image/webp-plan.md` §6).

---

Back to \ref image_format_references "Formats".
