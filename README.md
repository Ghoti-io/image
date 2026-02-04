# Ghoti.io Image Library

Raster image decoding/encoding, metadata, transformations, and color management for multi-image formats.

## Overview

The `image` library provides:

- Core types and stream abstraction for image I/O
- Multi-image container model (documents, items, frames/pages)
- Pixel formats, color representation, and metadata (common + raw preservation)
- Codec framework and format support (PNG, JPEG, GIF, TIFF, etc. per spec roadmap)
- Transformations and image operations

## Dependencies

- No external runtime dependencies beyond libc (optional: `cutil` when added)
- Google Test for unit tests

## Building

```bash
make
```

## Testing

```bash
make test
```

## Installation

```bash
sudo make install
```

## Usage

See the examples directory for usage examples.

## Documentation

- [Modules](@ref modules) - Detailed documentation for library modules
- [Examples](@ref examples) - Example programs demonstrating library usage
- [Function Index](@ref functions_index) - Complete API reference

## Macros and Utilities

The library provides cross-compiler macros in `include/ghoti.io/image/macros.h`:

- `GIMG_MAYBE_UNUSED(X)` - Mark unused function parameters
- `GIMG_DEPRECATED` - Mark deprecated functions
- `GIMG_API` - Mark functions for library export
- `GIMG_ARRAY_SIZE(a)` - Get compile-time array size
- `GIMG_BIT(x)` - Create bitmask with bit x set

Example:

```c
#include <ghoti.io/image/macros.h>

void my_function(int GIMG_MAYBE_UNUSED(param)) {
    // param is intentionally unused
}
```

## Spec and roadmap

See `tasks/ImageLibrarySpec.md` for the full feature specification and phase plan.
