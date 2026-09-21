@page api_options API Options and Types

# API Options and Types

This page documents the main option structures and enumerations used by the codec load, save, and decode APIs. Header-level documentation is generated from the source; here we summarize behavior and cross-references.

## Probe and codec dispatch

- **GIMG_Probe_Result** — Filled by `gimg_probe()`. Contains `format_name` (e.g. `"png"`) and `confidence` (0–100). Used to select the codec for `gimg_doc_load()` or to report format detection. **Lifetime:** `format_name` is valid only until the next call that mutates the codec registry (e.g. `gimg_codec_register()`). Do not store the pointer long-term; copy the string if you need to keep it.
- **GIMG_Codec** — Opaque codec descriptor. Create stubs with `gimg_codec_create_stub()` or `gimg_codec_create_stub_with_allocator()`, register with `gimg_codec_register()`. Look up by name with `gimg_codec_by_name()`.

@section api_options_codec_capabilities Codec capabilities (GIMG_CAP_*)

Codec capability bits (see `ghoti.io/image/codec.h`) form a bitmask returned by `gimg_codec_capabilities()`. They indicate what a codec supports so callers can check before using load/save/decode or format-specific options.

| Bit | Meaning |
|-----|---------|
| **GIMG_CAP_READ** | Codec can load documents (probe + load). |
| **GIMG_CAP_WRITE** | Codec can save documents. |
| **GIMG_CAP_ANIMATION** | Format supports multiple frames (e.g. APNG); document may have multiple items with frame timing. |
| **GIMG_CAP_PALETTE** | Codec supports palette/indexed color. |
| **GIMG_CAP_ICC** | Codec supports ICC profile (e.g. iCCP in PNG, APP2 in JPEG, `PROFILE_EMBEDDED` in a BMP V5 header). |
| **GIMG_CAP_16BPC** | Codec supports 16-bit-per-channel samples. |
| **GIMG_CAP_CMYK** | Codec supports four ink channels (`GIMG_PIXEL_CMYK8`). |

What the three registered codecs declare:

| Codec | READ | WRITE | ANIMATION | PALETTE | ICC | 16BPC | CMYK |
|---|---|---|---|---|---|---|---|
| PNG | yes | yes | yes (APNG) | yes | yes | yes | no |
| JPEG | yes | yes | no | no | yes | yes | yes |
| BMP | yes | yes | no | yes | yes | no | no |

BMP's absences are the format's, not the codec's: a BMP holds one image, its
samples are a byte at most - a deeper raster is restated at 8 bits rather than
refused - and its writer reports `GIMG_ERR_UNSUPPORTED` for a CMYK raster
rather than reinterpreting four ink channels as colour. JPEG has no palette:
T.81 describes none.

## Load options

**GIMG_Load_Options** (see `ghoti.io/image/codec.h`):

| Field        | Description |
|-------------|-------------|
| `limits`    | Pointer to **GIMG_Limits**; `NULL` = use defaults (no limits). Enforced during load and decode (chunk size, decoded pixels, frame count). |
| `strictness`| **GIMG_Strictness** — how to handle recoverable issues (see below). |
| `jpeg_tables` | For JPEG: tables to install before reading an abbreviated stream (T.81 B.4). Ignored by other codecs. |
| `bmp_rgb32_alpha` | For BMP: what the undefined fourth byte of a 32-bit `BI_RGB` pixel means. `GIMG_BMP_RGB32_ALPHA_IGNORE` (0, default) decodes such an image opaque; `GIMG_BMP_RGB32_ALPHA_HEURISTIC` reads the byte as alpha when any pixel sets it. A file that *declares* its alpha is unaffected either way. Ignored by other codecs. |
| `icc_resolver` | Called when a file names an ICC profile rather than carrying one - BMP's `PROFILE_LINKED` is the case. The library never opens the path itself; it hands the path over and takes bytes back, so the decision sits with the code that knows where the image came from. Return `GIMG_OK` with the bytes to attach them, or anything else to leave the image untagged, which is not an error. The bytes are copied before the call returns. Unset (the default) means no profile is resolved and the path is reported as `GIMG_Color_Info.icc_linked_path`. |
| `icc_resolver_user` | Passed to `icc_resolver` untouched. |
| `_reserved` | Reserved; set to zero. |

Used by `gimg_doc_load()`.

## Save options

**GIMG_Save_Options** (see `ghoti.io/image/codec.h`):

Every field below is read and honoured. The one-line summaries here are a
reference; `codec.h` carries the full reasoning for each, and the format pages
say what a given option costs in a real file.

| Field | Description |
|-------|-------------|
| `metadata_policy` | **GIMG_Meta_Policy** — which metadata to write (see @ref api_options_meta_policy). |
| `interlaced` | For PNG: `0` = non-interlaced (default), `1` = Adam7 interlaced. |
| `quality` | For JPEG: `1`–`100` (100 = finest). `0` = unspecified, codec default (85). Ignored by a lossless frame, which reconstructs exactly. Ignored by other codecs. |
| `exif_thumbnail_format` | IFD1 thumbnail compression: `0` = default (6), or `1`, `6`, `7`. |
| `exif_thumbnail_quality` | Thumbnail JPEG quality `1`–`100` when the format is 6 or 7; `0` = default (85). |

**JPEG.** Ignored by other codecs.

| Field | Description |
|-------|-------------|
| `jpeg_chroma_subsampling` | `GIMG_JPEG_CHROMA_420` (default), `GIMG_JPEG_CHROMA_422`, `GIMG_JPEG_CHROMA_444`. Ignored by a lossless or hierarchical frame, and by a raw CMYK frame, which has no chrominance. |
| `jpeg_fdct_method` | `GIMG_JPEG_FDCT_LOEFFLER` (0, default) = libjpeg-compatible Loeffler integer DCT, for an exact match against it; `GIMG_JPEG_FDCT_REF` (1) = float reference implementation. |
| `jpeg_quant_method` | `GIMG_JPEG_QUANT_RECIP` (0, default) = reciprocal-based, matching libjpeg; `GIMG_JPEG_QUANT_DIV` (1) = integer division. |
| `jpeg_progressive` | `0` = baseline (default), `1` = progressive (Annex G). |
| `jpeg_progressive_config` | When `jpeg_progressive` is 1: `NULL` or `scan_count` 0 = default progression (DC + AC scan(s)); otherwise a **GIMG_JPEG_Progressive_Config** giving a custom scan script. Ignored for baseline. |
| `jpeg_restart_interval` | Restart interval in MCUs; `0` = none. Non-zero writes a DRI segment and injects RST0–RST7 every N MCUs. In a non-interleaved scan an MCU is a single block (A.2.3), so that is what N counts. |
| `jpeg_precision` | Output precision. `0` = derive from the raster; `8` or `12` = write at that precision. Table B.2 allows no other value in a DCT frame, so `16` returns `GIMG_ERR_UNSUPPORTED` and a 16-bit raster is written at 12-bit when this is `0`. A differing raster depth is converted by the library (`gimg_ops_convert_bit_depth`). |
| `jpeg_arithmetic` | `1` writes arithmetic entropy coding (Annex D) — SOF9/SOF10, a DAC segment and no DHT — instead of Huffman. Both are normative; arithmetic is a few per cent smaller and understood by far fewer decoders, so Huffman stays the default. |
| `jpeg_lossless_predictor` | `0` (default) writes a DCT frame; `1`–`7` write a lossless frame (Annex H, SOF3) with that predictor from Table H.1. Reconstruction is exact, so `quality` and `jpeg_chroma_subsampling` have no meaning; colour is stored as RGB because the YCbCr conversion is not reversible. Precision follows the raster (8, 12 or 16). |
| `jpeg_hierarchical_levels` | `0` (default) writes one frame; *n* writes a hierarchical sequence (Annex J) with *n* resolution doublings. Sampling is 4:4:4 throughout and the raster must be 8-bit. Combines with `jpeg_arithmetic`. |
| `jpeg_non_interleaved` | `1` writes a sequential frame as one non-interleaved scan per component (A.2.3) rather than one interleaved scan (A.2.2). Same blocks, same picture, different order — a decoder wanting only luminance can stop after the first scan. Refused with `jpeg_progressive`, a lossless frame, or `jpeg_hierarchical_levels`. |
| `jpeg_cmyk_transform` | Adobe APP14 transform for a four-component raster. `0` (default) writes CMYK unchanged, so a CMYK JPEG survives a load and save; `2` writes YCCK. Only 0 and 2 are accepted; anything else returns `GIMG_ERR_UNSUPPORTED`. Ignored unless the raster is `GIMG_PIXEL_CMYK8`. |
| `jpeg_abbreviated` | `0` (default) writes a complete file. `1` writes the frame with its tables left out (B.4), read back by passing `GIMG_Load_Options.jpeg_tables`. `2` writes the tables alone, with no frame — the raster is read only for its shape. Refused with `jpeg_hierarchical_levels` and with a lossless frame. |

**PNG.** Ignored by other codecs.

| Field | Description |
|-------|-------------|
| `png_filter` | Row filter (11.2.4). `GIMG_PNG_FILTER_ADAPTIVE` (0, default) chooses per row by the heuristic PNG 12.8 recommends; the other values force one filter on every row, which is mainly useful for testing that each of the five reconstructs. |
| `png_palette` | Whether the writer may build a palette for a raster that did not arrive with one (colour type 3). `GIMG_PNG_PALETTE_AUTO` (0, default) builds one when the image has at most 256 distinct colours and the palette form is smaller — a lossless choice, not quantization. `GIMG_PNG_PALETTE_NEVER` refuses to build one; a frame that *arrived* as a palette image is still written back as one either way. |

**BMP.** Ignored by other codecs.

| Field | Description |
|-------|-------------|
| `bmp_palette` | Whether the writer may store an image through a palette. `GIMG_BMP_PALETTE_AUTO` (0, default) writes an indexed bitmap when the image is fully opaque, has at most 256 distinct colours, and the indexed form is the smaller file — at the smallest depth that holds the indices, 1, 4 or 8 bits. Unlike PNG's, which form is smaller is arithmetic rather than a measurement, so both need not be written to find out. `GIMG_BMP_PALETTE_NEVER` always writes 24- or 32-bit colour. Transparency rules the palette out whatever the colour count: a BMP palette has no alpha. |
| `bmp_rle` | For BMP: `GIMG_BMP_RLE_NEVER` (0, default) writes plain rows. `GIMG_BMP_RLE_AUTO` writes `BI_RLE8` or `BI_RLE4` - whichever matches the depth chosen - when the encoded rows come out smaller than the plain ones, which is measured rather than assumed. RLE24 needs `bmp_allow_rle24` as well. Refused together with `bmp_top_down`. Ignored by other codecs. |
| `bmp_allow_2bit` | For BMP: 0 (default) uses 1, 4 or 8 bits for an indexed image. 1 also allows 2, which holds up to four colours. 2 bits per pixel is a Windows CE addition: Pillow refuses such a file outright, so it is never chosen unless the caller says they can read it back. Ignored by other codecs. |
| `bmp_allow_rle24` | For BMP: 0 (default) never writes the OS/2 RLE24 encoding. 1 allows a true-colour image to be written as an OS/2 bitmap with RLE24 when that is smaller. The header changes vocabulary with it - compression 4 means RLE24 in a `BITMAPCOREHEADER2` and `BI_JPEG` in a Windows header - so this is not a side effect but the whole of what the option means. Requires `bmp_rle` to be `GIMG_BMP_RLE_AUTO`. Ignored by other codecs. |
| `bmp_allow_huffman` | For BMP: 0 (default) never writes the OS/2 Huffman 1D encoding. 1 allows a two-colour image to be written that way when the encoded stream is smaller than the packed rows. Like RLE24 it lives only in an OS/2 header - compression 3 there is CCITT Group 3 coding, where a Windows reader sees `BI_BITFIELDS` - and of the decoders reachable here only bmplib reads one. Requires `bmp_rle` to be `GIMG_BMP_RLE_AUTO`. Ignored by other codecs. |
| `bmp_wrapper` | For BMP: `GIMG_BMP_WRAPPER_NONE` (0, default) writes pixels. `GIMG_BMP_WRAPPER_JPEG` and `GIMG_BMP_WRAPPER_PNG` write that format's whole stream as the pixel data, with `biCompression` saying which and `biBitCount` zero because the payload states its own depth. The payload is produced by this library's codec for that format at its defaults, so that codec's save options do not reach it. Most readers refuse these files; they were meant for spooling to printers, not interchange. Ignored by other codecs. |
| `bmp_top_down` | `0` (default) writes rows bottom-up with a positive `biHeight`, the layout every reader handles. `1` writes them top-down with a negative `biHeight`, legal from `BITMAPINFOHEADER` onwards and what a caller wants when something downstream reads the file as a memory-mapped framebuffer. Refused together with `GIMG_BMP_RLE_AUTO`: the format does not allow compression and top-down rows together. |
| `gif_interlace` | For GIF: `0` (default) writes rows top to bottom. `1` writes them in the four-pass interlaced order (89a 20), which lets a reader show a coarse version before the whole file arrives. It changes no pixel. Ignored by other codecs. |
| `gif_alpha_threshold` | For GIF: what to do with a pixel that is neither fully opaque nor fully transparent. GIF has one bit of transparency (89a 23), so an alpha of 128 cannot be stored, only decided about. `0` (default) refuses such a raster with `GIMG_ERR_UNSUPPORTED` rather than choosing silently; `1`–`255` is a threshold, alpha at or above it becoming opaque. Fully opaque and fully transparent pixels are unaffected. Ignored by other codecs. |
| `gif_loop_count` | For GIF: how many times a written animation repeats, as the NETSCAPE2.0 Application Extension every decoder reads for this. **An override, not the source** - the count normally comes from `gimg_doc_set_loop_count()`, so a loaded animation keeps what it declared. Non-zero wins; `0` (the default) means the caller did not ask and the document answers. Zero cannot mean "forever" here because it cannot also mean "not set"; say that on the document, which has a spelling for both. A document declaring no count gets no block at all, which browsers play once. Ignored by other codecs. |

**GIMG_JPEG_Progressive_Config** holds `scan_count` and `scans` (array of **GIMG_JPEG_Progressive_Scan**). Each scan has `Ss`, `Se` (spectral selection, 0–63), `Ah`, `Al` (successive approximation). Caller keeps the array valid for the duration of `gimg_doc_save()`. **When `jpeg_progressive_config` is NULL or `scan_count` is 0:** the encoder uses the default scan script (one DC scan Ss=0, Se=0 then one AC scan Ss=1..63, Ah=0, Al=0). **Custom script:** non-NULL with `scan_count` > 0 uses the given sequence of scans. Initial AC spectral bands (Ah=0, Ss≥1) must not overlap (T.81 Annex G); overlapping [Ss,Se] ranges are rejected with **GIMG_ERR_UNSUPPORTED**. Refinement passes (Ah>0) are supported: DC refinement (Ss=0, Se=0, Ah>0) and AC refinement (Ah>0 for band Ss..Se) with successive-approximation encoding and optional refinement DHT (Th=2).

Used by `gimg_doc_save()`.

@section api_options_meta_policy Metadata policy (GIMG_Meta_Policy)

Controls which ancillary metadata is written on save. Defined in `ghoti.io/image/meta.h`. No silent stripping: the policy is explicit.

| Value | Meaning |
|-------|--------|
| **GIMG_META_PRESERVE_ALL** | Emit all ancillary from the loaded document (and eXIf from doc meta_raw when doc was not loaded from PNG). |
| **GIMG_META_DROP_ALL**     | No ancillary; only signature, IHDR, PLTE/tRNS if palette, IDAT, IEND. |
| **GIMG_META_STRIP_GPS**    | Like PRESERVE_ALL except eXIf is parsed, GPS IFD (and tag 0x8825) are removed, and the remaining Exif is re-serialized and written as eXIf; text chunks with GPS-related keywords are omitted. |
| **GIMG_META_NORMALIZE_EXIF** | Pass through ancillary; Exif normalization (orientation, duplicates) may be applied when Exif parsing exists. |
| **GIMG_META_KEEP_RAW_ONLY**  | Emit only ancillary chunks that are *not* known semantic (no iCCP, sRGB, gAMA, cHRM, eXIf, tEXt, zTXt, iTXt). |
| **GIMG_META_KEEP_COMMON_ONLY** | Emit only metadata that maps to common metadata (e.g. one color chunk from raster color info; no eXIf or text). |

Format-specific behavior is described on the format's own page: \ref format_png "PNG and APNG" (save metadata policies), \ref format_jpeg "JPEG" (APP segment handling), \ref format_bmp "BMP" (which ignores the policy, having no metadata to write).

## Decode options

**GIMG_Decode_Options** (see `ghoti.io/image/codec.h`):

| Field     | Description |
|-----------|-------------|
| `limits`  | Pointer to **GIMG_Limits**; `NULL` = use defaults. Enforced during decode (e.g. max decoded pixels). |
| `jpeg_chroma_upsampling` | JPEG only: chroma upsampling for 4:2:0/4:2:2. **GIMG_JPEG_CHROMA_UPSAMPLE_DEFAULT** (0) = FANCY; **GIMG_JPEG_CHROMA_UPSAMPLE_FANCY** (1) = triangle filter; **GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE** (2) = box/replicate. SIMPLE deliberately does not live at zero, so that a zero-initialized struct and a NULL pointer select the same filter. Ignored for non-JPEG. |
| `jpeg_precision` | JPEG decode-to precision: `0` = use file precision (8→GRAY8/RGBA8; 12→GRAY16/RGB16, left-justified); `8`, `12`, or `16` = decode to that bit depth (library conversion when different from file). A *file* precision of 16 does not exist in T.81 and is rejected on load; this option is about the output raster. Ignored for non-JPEG. |
| `gif_background` | GIF only: what goes where no frame has drawn. **GIMG_GIF_BACKGROUND_TRANSPARENT** (0, default) leaves the logical screen empty and clears disposal method 2 to transparent, which is what browsers do; **GIMG_GIF_BACKGROUND_PAINT** paints the colour the Background Color Index names, which is what GIF89a 18 and 23 say. No effect on a file with no Global Color Table, where 89a 18 says the index is to be ignored. The colour is reported by `gimg_doc_background_color()` under either setting. See \ref format_gif "GIF" for why the default is the first, and for why the second still does not reproduce ImageMagick or Pillow. Ignored for non-GIF. |
| `_reserved` | Reserved; set to zero. |

Used by `gimg_item_decode()`.

## Limits (GIMG_Limits)

**GIMG_Limits** (see `ghoti.io/image/stream.h`) is used by load and decode to cap resource use. Initialize with `gimg_limits_default()`; any field set to `0` means “no limit”.

| Field                  | Use |
|------------------------|-----|
| `max_decoded_pixels`   | Decode: reject if width×height (or sum over passes/frames) exceeds this. |
| `max_memory`           | General memory cap (format-specific). |
| `max_metadata_size`    | Cap on metadata size (e.g. ancillary payload). |
| `max_frame_count`      | Max frames (e.g. APNG). |
| `max_chunk_size`       | Chunk parser: reject chunks larger than this (bomb protection). |
| `max_recursion`        | Max recursion depth (e.g. TIFF IFD). |

The compress library’s DEFLATE decoder may use a separate limit (e.g. `limits.max_output_bytes`) for decompression; the image library passes limits where applicable.

### The default is no limits, and that is a decision the caller makes

`gimg_limits_default()` sets every field to zero, and a `NULL` `limits` pointer
means the same thing. **An image is a header that names a size, so a file of a
hundred bytes can ask for as much memory as its fields allow.** Measured on
this library:

| File | Bytes | Raster it names | Peak RSS to refuse it |
|---|---|---|---|
| BMP naming 46340 &times; 46340 at 32 bpp | 118 | ~8.6 GB | 236 KB |
| JPEG with SOF0 naming 65535 &times; 65535, 3 components | 78 | ~17 GB | below measurement |
| PNG with IHDR naming 65535 &times; 65535 RGBA8 | 69 | ~17 GB | 1.9 MB |

None of these crashes: each allocation is checked and a failure comes back as
`GIMG_ERR_OOM`. A header naming more pixel data than the file actually holds
is refused as `GIMG_ERR_CORRUPT` **before** the allocation it names, whether
or not a limit is set - the BMP loader used to allocate first and discover the
truncation on the read, so the 118-byte row above really did allocate 8.6 GB.
What an unlimited default costs is therefore not safety from a malformed file
but **a bound on what a well-formed hostile one can make the process try to
allocate**: a file that genuinely carries 8.6 GB of pixels is not lying, and
only a limit will stop it.

This matches libpng and libjpeg, which have no built-in cap either, and it is
the right default for a library that does not know whether it is decoding a
thumbnail or a satellite image. It is the wrong setting for a service reading
files it did not produce. **Set `max_decoded_pixels` and `max_memory` to
whatever your largest legitimate input needs**, and the shape above becomes
`GIMG_ERR_LIMIT` before anything is allocated.

Limits set on the **load** are remembered and applied to a later **decode**
that carries none of its own, so setting them once at load is enough - and the
save paths, which re-decode their source item and have no options to pass on,
are covered by the same fallback.

## Strictness (GIMG_Strictness)

**GIMG_Strictness** (see `ghoti.io/image/core.h`) controls how recoverable format issues are handled:

| Value           | Behavior |
|-----------------|----------|
| **GIMG_STRICT** | Warnings treated as errors. |
| **GIMG_NORMAL** | Safe recoveries allowed; report warnings. |
| **GIMG_PERMISSIVE** | More heuristics; report warnings. |

> **Not honoured.** No codec reads `strictness`. The table above describes what
> the levels are *for*, not what setting one does today - which is nothing.
> Every codec behaves as GIMG_NORMAL describes whatever is set. Do not rely on
> GIMG_STRICT to reject a file: use the result code and **GIMG_Diagnostics**,
> which load and decode do fill. Note that the enum's zero value is
> GIMG_STRICT, so zero-initialized options already ask for the strictest
> setting and would change behaviour the day this is implemented.

> **GIMG_Save_Report.diagnostics is never set** either. No save path writes to
> it, so it holds whatever the caller left there. Saving reports through its
> result code alone; `bytes_written` is always filled.

## Save report

**GIMG_Save_Report** (see `ghoti.io/image/codec.h`): Optional output of `gimg_doc_save()`. Contains `bytes_written` and `diagnostics` (warnings, codec name, offset, chunk/tag when relevant).

## Result and diagnostics

- **GIMG_Result** — Result codes (e.g. `GIMG_OK`, `GIMG_ERR_FORMAT`, `GIMG_ERR_LIMIT`, `GIMG_ERR_CORRUPT`). See `ghoti.io/image/core.h`.
- **GIMG_Diagnostics** — List of **GIMG_Diagnostic** items (codec name, offset, chunk/tag id, severity, recommended action). Filled when provided to load/save/decode. **Lifecycle:** Call `gimg_diagnostics_init(d, allocator)` to set an optional allocator (NULL = default). Append uses this allocator for realloc. When done, call `gimg_diagnostics_clear(d)` to free the list and reset count/capacity, or `gimg_diagnostics_destroy(d)` to free and zero the whole struct. Callers must call clear or destroy to avoid leaks; the same allocator is used for growth and for release.

@section api_options_diagnostics_functions Diagnostics API (lifecycle)

| Function | Description |
|----------|-------------|
| `gimg_diagnostics_init(d, allocator)` | Set allocator for list growth and for clear/destroy; NULL = default. Safe to call on zero-initialized or already-initialized struct. |
| `gimg_diagnostics_append(d, ...)` | Append one diagnostic; grows list using d's allocator. Returns GIMG_OK or GIMG_ERR_OOM. |
| `gimg_diagnostics_clear(d)` | Free the list, set count/capacity to 0. Idempotent if empty. Allocator unchanged so append can be used again. |
| `gimg_diagnostics_destroy(d)` | Same as clear, then zero the whole struct (including allocator). Struct can be discarded or re-initialized with init. |

@section api_options_error_handling Error handling

When to return which result code, when to append diagnostics, and how output parameters behave on error.

### Result codes (GIMG_Result)

| Code | When to use |
|------|-------------|
| **GIMG_OK** | Operation succeeded. |
| **GIMG_ERR_IO** | Stream read/write/seek failed; underlying I/O error. |
| **GIMG_ERR_FORMAT** | Unrecognized format, invalid signature, or structurally invalid (e.g. invalid IHDR field combination). Use when the data is not the expected format or violates format structure. |
| **GIMG_ERR_UNSUPPORTED** | Format recognized but feature not supported (e.g. compression method, bit depth, or option). Use when the format is valid but the library cannot handle this variant. |
| **GIMG_ERR_LIMIT** | A configured limit was exceeded (e.g. max_chunk_size, max_decoded_pixels, max_frame_count). Use when the operation would exceed a safety or resource limit. |
| **GIMG_ERR_CORRUPT** | Data is corrupt or invalid for the format (e.g. CRC mismatch, truncated chunk, invalid DEFLATE, invalid filter). Use when the format is correct but the payload is damaged or inconsistent. |
| **GIMG_ERR_OOM** | Allocation failed. Use when malloc/realloc or allocator callback fails. |
| **GIMG_ERR_INTERNAL** | Internal library error (assertion, unexpected state). Use only for bugs; prefer a specific code (e.g. GIMG_ERR_CORRUPT) when the cause is input or limits. |

Guideline: Prefer the most specific code that fits (e.g. GIMG_ERR_CORRUPT for bad CRC, GIMG_ERR_LIMIT for pixel count overflow) so callers can handle errors appropriately.

### Diagnostics

- **When to append:** Append a diagnostic when the failure is recoverable or informative and the caller passed a non-NULL diagnostics pointer (e.g. chunk CRC error, limit exceeded, unsupported feature encountered). Load/save/decode may append zero or more items before returning an error.
- **Severity:** Use GIMG_DIAG_ERROR for conditions that cause the function to return an error; use GIMG_DIAG_WARNING for recoverable or advisory conditions when the function still returns GIMG_OK (e.g. under strictness NORMAL/PERMISSIVE).
- **recommended_action:** Optional string (e.g. "increase max_chunk_size") to suggest a remedy; stored by reference, may be NULL.

### Output parameters on error

On any error return, **output (out) parameters** (e.g. `GIMG_Doc ** out_doc`, `GIMG_Raster ** out_raster`) are left **unchanged** unless otherwise documented: the caller’s pointer is not written to, so any previous value remains. The implementation must not leak: if it allocated an object before failing later, it frees that object before returning. Callers should not rely on partial fills; on error they should treat all out parameters as unchanged and not use any partially filled state.

@section api_options_animation Animation (item frame API)

For multi-frame formats (e.g. APNG), each **GIMG_Item** carries frame timing and compositing hints. Defined in `ghoti.io/image/doc.h`:

- **Frame delay:** `gimg_item_frame_delay()` / `gimg_item_set_frame_delay()` — numerator and denominator (e.g. fcTL `delay_num`/`delay_den`). Delay in seconds = num/den; den 0 is treated as 100 when writing APNG.
- **Dispose:** `gimg_item_dispose_op()` / `gimg_item_set_dispose_op()` — **GIMG_Dispose_Op**: `GIMG_DISPOSE_NONE`, `GIMG_DISPOSE_BACKGROUND`, `GIMG_DISPOSE_PREVIOUS`. How to clear the frame region before the next frame.
- **Blend:** `gimg_item_blend_op()` / `gimg_item_set_blend_op()` — **GIMG_Blend_Op**: `GIMG_BLEND_SOURCE`, `GIMG_BLEND_OVER`. How to composite the frame over the canvas.

Alongside them, the **document** carries how many times the animation asks to
be played, because that is where both animated formats put it - GIF in a
NETSCAPE2.0 Application Extension, APNG in `acTL`'s `num_plays`:

- **Loop count:** `gimg_doc_loop_count()` / `gimg_doc_set_loop_count()` /
  `gimg_doc_clear_loop_count()`. Three states, not two. `gimg_doc_loop_count()`
  returns 1 when the document declares a count and writes it to the out-param,
  or 0 when it declares none, leaving the out-param untouched. A declared count
  of **0 means repeat forever**, which both formats agree on; "declares none"
  is a separate answer, and what to do about it is the player's policy. A GIF
  with no NETSCAPE2.0 block is shown once by every browser, and that convention
  is deliberately not applied here, so that a caller can tell a convention from
  a reading. `gimg_doc_copy()` carries the count; writing it back out is the
  caller's to do through the format's save option.

The document also carries two things about the screen a file is drawn on,
neither of which this library applies - both are reported so that a caller who
wants to honour them can:

- **Background colour:** `gimg_doc_background_color()` / `set` / `clear`.
  Reported as RGBA rather than as the index GIF states (89a 18) or the bKGD
  PNG states (11.3.4.1), because once a raster is decoded the palette an index
  referred to is gone. Filled by GIF, resolved through the Global Color Table,
  and by PNG from bKGD; JPEG and BMP have no field for one. **The alpha is part
  of the answer**: GIF cannot leave the field out, so an encoder says "nothing
  is behind this" by naming an entry its first frame marks transparent, and
  that arrives as the colour at alpha 0. **Nothing paints it** unless
  `gif_background` asks - a GIF decodes onto a transparent canvas, matching
  every viewer real files were authored against, and of the PNG decoders in
  common use only ImageMagick composites onto a bKGD at all. Setting or
  clearing it reaches the file: GIF repoints its index and adds a table entry
  if it must, PNG writes or removes the chunk, and a format that cannot state
  the colour asked for writes nothing rather than the nearest thing it could
  say.
- **Pixel aspect ratio:** `gimg_doc_pixel_aspect_ratio()` / `set` / `clear`,
  as a width-over-height ratio. This is "is a pixel square", which is not a
  physical density: GIF states it in its Pixel Aspect Ratio byte and PNG in
  `pHYs` when the unit specifier says "aspect ratio only", and a density from
  either arrives separately through `gimg_meta_common_dpi()`. A ratio of 1/1 is
  a real answer and is distinguished from the file having said nothing.

Codecs that support animation (GIMG_CAP_ANIMATION) set these on load and read them on save.

@section api_options_raster_formats_12bit Raster formats (12-bit)

**GIMG_PIXEL_GRAY12** and **GIMG_PIXEL_RGBA12** (see `ghoti.io/image/raster.h`): 12 bits per channel, stored as **uint16_t per sample** with value in **0..4095** (clamped; no left-shift in the raster). Used for codecs that support 12-bit precision (e.g. JPEG 12-bit). Conversion to/from 8- or 16-bit uses the library bit-depth API (`gimg_bitdepth_*`, `gimg_ops_convert_bit_depth`).

@section api_options_color_info Color info (GIMG_Color_Info)

**GIMG_Color_Info** (see `ghoti.io/image/color.h`) is attached to a raster and describes how to interpret color: primaries, transfer, rendering intent, optional ICC profile, and (for CMYK rasters) channel polarity.

| Field | Description |
|-------|-------------|
| `primaries` / `white_point` | **GIMG_Primaries** — sRGB, Adobe RGB, or unknown. `white_point` is **not read by any writer**: both named gamuts are D65, and an ICC profile states colorants already adapted to D50 regardless. |
| `transfer` | **GIMG_Transfer** — linear, sRGB, gamma, or unknown. |
| `gamma_value` | Used when `transfer` is **GIMG_TRANSFER_GAMMA**. |
| `intent` | **GIMG_Rendering_Intent** — written into an ICC profile's header and into a BMP V5 header. A value outside the four ICC names is written as perceptual. |
| `icc_bytes` / `icc_size` | Optional ICC profile; library does not take ownership. |
| `cmyk_polarity` | **GIMG_CMYK_Polarity** — interpretation of CMYK channel values. Only relevant when the raster format is a CMYK one (**GIMG_PIXEL_CMYK8**, **CMYK12**, **CMYK16**). |

### Where the model goes on save

`primaries` and `transfer` reach each format differently, because each format
offers something different to say them with.

| Format | With an ICC profile on the raster | With only `primaries` and `transfer` |
|--------|-----------------------------------|--------------------------------------|
| BMP | `BITMAPV5HEADER` with `PROFILE_EMBEDDED` | V4 header: endpoints and per-channel gamma |
| PNG | `iCCP` | `sRGB` when the transfer is exactly sRGB; otherwise `gAMA` for the curve, plus `cHRM` for a gamut a reader would not otherwise assume (Adobe RGB, not sRGB's own primaries) |
| JPEG | APP2 `ICC_PROFILE`, split across segments when needed | APP2 carrying an ICC profile **synthesized** to say it |

JPEG is the one that manufactures rather than repeats, because APP2 is the
only place the format can name a colour space — there is no `gAMA` or `cHRM`
equivalent. A profile the raster already carries is always written unchanged
in preference to a built one; a model missing either half is written as
nothing; and only a three-component frame gets one. See
\ref format_jpeg "JPEG"'s *Color on save*.

**GIMG_CMYK_Polarity** (see `ghoti.io/image/color.h`):

| Value | Meaning |
|-------|---------|
| **GIMG_CMYK_POLARITY_UNKNOWN** | Polarity not specified (default for newly created color info). |
| **GIMG_CMYK_POLARITY_INK** | 0 = full ink, 255 = no ink (Adobe / JPEG file convention). Set by the JPEG decoder for CMYK output. |
| **GIMG_CMYK_POLARITY_REFLECTION** | 0 = no ink, 255 = full ink (reflection; e.g. many design-tool APIs). |

Raster pixels are stored as raw values; `cmyk_polarity` tells consumers (e.g. display or CMYK→RGB conversion) whether to treat 0 as “no ink” or “full ink” when interpreting the channels. The JPEG decoder states it on every four-component frame, whatever the coding process.

`gimg_ops_convert_pixel_format` reads it to convert a CMYK raster to RGBA, which is the only route from a four-component JPEG into a PNG or a BMP — neither has CMYK. The conversion is the naive one (each ink an independent multiplicative filter over white) and is **not colorimetric**: this library has no colour engine, so what it offers is that conversion or none. It agrees with Pillow exactly on every pixel of every CMYK and YCCK fixture in `tests/data/jpeg`. A polarity of **GIMG_CMYK_POLARITY_UNKNOWN** is refused rather than guessed — the two readings are negatives of each other, and the wrong one gives a plausible but inverted picture. No writer performs the conversion on your behalf.

## `jpeg_arithmetic` (save)

Write the frame with the arithmetic entropy coder of ITU-T T.81 Annex D rather
than the Huffman coder of Annex F.  Both are normative parts of the standard and
both produce valid JPEG; the frame header says which (SOF9 rather than SOF0 or
SOF1), and the table specifications are DAC rather than DHT.

Arithmetic coding typically produces a smaller file for the same coefficients -
often noticeably so against the fixed tables of Annex K, which is what this
encoder uses for Huffman - at the cost of being understood by far fewer
decoders.  It is off by default for that reason.

Progressive and arithmetic are independent choices - T.81 Table B.1 has a
marker for each of the four combinations - and both work here, at 8- or 12-bit
precision, with or without restart intervals.

## `jpeg_lossless_predictor` (save)

Write a lossless frame (ITU-T T.81 Annex H, SOF3) rather than a DCT-based one.
0 (the default) writes a DCT frame; 1 to 7 select a predictor from Table H.1 —
1 is the sample to the left, 2 the one above, 3 the one above-left, and 4 to 7
combine them.  Which one compresses best depends on the image; 1 and 4 are the
usual choices.

The reconstruction is exact, so `quality` and `jpeg_chroma_subsampling` have no
meaning here and are ignored, and color is stored as RGB rather than YCbCr
because that conversion is not reversible.

Precision follows the raster: 8-bit rasters give P=8, 12-bit P=12 and 16-bit
P=16, all of which Table B.2 permits in a lossless frame.  This is the only way
a 16-bit raster survives a JPEG round trip unchanged — the DCT-based writers
narrow one to 12 bits, because a 16-bit DCT frame does not exist.

`jpeg_restart_interval` is honored but rounded down to a whole number of image
rows.  T.81 does not require a lossless restart interval to begin at the start
of a row, but an interval resets the prediction, and what "the first row of the
interval" means for an interval starting mid-row is not defined anywhere;
implementations resolve that by requiring alignment, and libjpeg refuses to
decode an unaligned one.

Setting this together with `jpeg_progressive` or `jpeg_arithmetic` returns
`GIMG_ERR_UNSUPPORTED`: progression belongs to the DCT-based processes, and
arithmetic lossless is SOF11, which is not implemented.
