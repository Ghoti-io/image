@page modules Modules

# Modules

This section will contain detailed documentation for each module in the Ghoti.io Image library.

## Modules

- **Core** — Core types, invariants, error model (`image.h`, `core.h`).
- **Allocator** — Configurable allocator (`allocator.h`): `GIMG_ALLOCATOR` with malloc/free/realloc/calloc; used by raster, doc, stream, meta, codec, and ops. Pass NULL to use the default stdlib-backed allocator.
- **Stream** — Stream abstraction for I/O (`stream.h`).
- **Container** — Documents and items (`doc.h`).
- **Raster** — Raster image type and pixel formats (`raster.h`).
- **Color** — Color info and conversion stubs (`color.h`).
- **Meta** — Metadata common and raw (`meta.h`).
- **Ops** — Transformations and image operations (`ops.h`).
- **Codec** — Codec registry and probing (`codec.h`).

## Quick Links

- [Function Index](@ref functions_index) - Browse all library functions
- [Main Documentation](@ref mainpage) - Return to main page
