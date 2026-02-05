@page api_options API Options and Types

# API Options and Types

This page documents the main option structures and enumerations used by the codec load, save, and decode APIs. Header-level documentation is generated from the source; here we summarize behavior and cross-references.

## Probe and codec dispatch

- **GIMG_Probe_Result** — Filled by `gimg_probe()`. Contains `format_name` (e.g. `"png"`) and `confidence` (0–100). Used to select the codec for `gimg_doc_load()` or to report format detection.
- **GIMG_Codec** — Opaque codec descriptor. Create stubs with `gimg_codec_create_stub()` or `gimg_codec_create_stub_with_allocator()`, register with `gimg_codec_register()`. Look up by name with `gimg_codec_by_name()`.

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
| **GIMG_META_STRIP_GPS**    | Like PRESERVE_ALL except eXIf is omitted and text chunks with GPS-related keywords are omitted. |
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
- **GIMG_Diagnostics** — List of **GIMG_Diagnostic** items (codec name, offset, chunk/tag id, severity, recommended action). Filled when provided to load/save/decode.
