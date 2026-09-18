@page modules Modules

# Modules

This section will contain detailed documentation for each module in the Ghoti.io Image library.

## Modules

- **Core** — Core types, invariants, error model (`image.h`, `core.h`).
- **Allocator** — Configurable allocator (`allocator.h`): `GIMG_Allocator` with malloc/free/realloc/calloc; used by raster, doc, stream, meta, codec, and ops. Pass NULL to use the default stdlib-backed allocator. The type is cutil's `GCU_Allocator`, so the same allocator serves every library in the suite.
- **Stream** — Stream abstraction for I/O (`stream.h`).
- **Container** — Documents and items (`doc.h`).
- **Raster** — Raster image type and pixel formats (`raster.h`).
- **Color** — Color info and conversion stubs (`color.h`).
- **Meta** — Metadata common and raw (`meta.h`).
- **Ops** — Transformations and image operations (`ops.h`).
- **Codec** — Codec registry and probing (`codec.h`).

## API options and types

Load, save, and decode options (e.g. **GIMG_Load_Options**, **GIMG_Save_Options**, **GIMG_Meta_Policy**, **GIMG_Limits**, **GIMG_Strictness**) are described in \ref api_options "API Options and Types". Format-specific behavior (e.g. PNG metadata policies) is on the format's own page - \ref format_png "PNG and APNG", \ref format_jpeg "JPEG", \ref format_bmp "BMP" - indexed by \ref format_references "Format and specification references".

## Quick Links

- [Function Index](@ref functions_index) - Browse all library functions
- [Main Documentation](@ref mainpage) - Return to main page
