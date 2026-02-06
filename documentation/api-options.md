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
| **GIMG_CAP_ICC** | Codec supports ICC profile (e.g. iCCP in PNG). |
| **GIMG_CAP_16BPC** | Codec supports 16-bit-per-channel samples. |

Example: PNG is registered with READ, WRITE, ANIMATION, PALETTE, ICC, and 16BPC set.

## Load options

**GIMG_Load_Options** (see `ghoti.io/image/codec.h`):

| Field        | Description |
|-------------|-------------|
| `limits`    | Pointer to **GIMG_Limits**; `NULL` = use defaults (no limits). Enforced during load and decode (chunk size, decoded pixels, frame count). |
| `strictness`| **GIMG_Strictness** — how to handle recoverable issues (see below). |
| `_reserved` | Reserved; set to zero. |

Used by `gimg_doc_load()`.

## Save options

**GIMG_Save_Options** (see `ghoti.io/image/codec.h`):

| Field              | Description |
|--------------------|-------------|
| `metadata_policy`  | **GIMG_Meta_Policy** — which metadata to write (see @ref api_options_meta_policy). |
| `interlaced`       | For PNG: `0` = non-interlaced (default), `1` = Adam7 interlaced. |
| `_reserved`        | Reserved; set to zero. |

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

Format-specific behavior (e.g. PNG chunk emission) is described in \ref format_references "Format and specification references" (PNG save metadata policies).

## Decode options

**GIMG_Decode_Options** (see `ghoti.io/image/codec.h`):

| Field     | Description |
|-----------|-------------|
| `limits`  | Pointer to **GIMG_Limits**; `NULL` = use defaults. Enforced during decode (e.g. max decoded pixels). |
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

## Strictness (GIMG_Strictness)

**GIMG_Strictness** (see `ghoti.io/image/core.h`) controls how recoverable format issues are handled:

| Value           | Behavior |
|-----------------|----------|
| **GIMG_STRICT** | Warnings treated as errors. |
| **GIMG_NORMAL** | Safe recoveries allowed; report warnings. |
| **GIMG_PERMISSIVE** | More heuristics; report warnings. |

## Save report

**GIMG_Save_Report** (see `ghoti.io/image/codec.h`): Optional output of `gimg_doc_save()`. Contains `bytes_written` and `diagnostics` (warnings, codec name, offset, chunk/tag when relevant).

## Result and diagnostics

- **GIMG_Result** — Result codes (e.g. `GIMG_OK`, `GIMG_ERR_FORMAT`, `GIMG_ERR_LIMIT`, `GIMG_ERR_CORRUPT`). See `ghoti.io/image/core.h`.
- **GIMG_Diagnostics** — List of **GIMG_Diagnostic** items (codec name, offset, chunk/tag id, severity, recommended action). Filled when provided to load/save/decode. **Lifecycle:** Call `gimg_diagnostics_init(d, allocator)` to set an optional allocator (NULL = default). Append uses this allocator for realloc. When done, call `gimg_diagnostics_clear(d)` to free the list and reset count/capacity, or `gimg_diagnostics_destroy(d)` to free and zero the whole struct. Callers must call clear or destroy to avoid leaks; the same allocator is used for growth and for release.

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

Codecs that support animation (GIMG_CAP_ANIMATION) set these on load and read them on save.
