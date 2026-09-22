# Ghoti.io Image Library

Raster image decoding/encoding, metadata, transformations, and colour handling for multi-image formats.

## Overview

The `image` library provides:

- Core types and stream abstraction for image I/O
- **Configurable allocator** — Pluggable malloc/free/realloc (`GIMG_Allocator`) for embedding and custom memory management; all owned allocations use the allocator (default is stdlib when NULL is passed)
- Multi-image container model (documents, items, frames/pages)
- Pixel formats, colour representation, and metadata (common + raw preservation)
- **Colour is carried, not converted.** An ICC profile is read, preserved,
  reported and written back; a gamut a BMP states as endpoints comes out as a
  PNG's `cHRM`; a JPEG gets a synthesized profile because APP2 is the only
  place it can name a colour space at all. What the library does **not** have
  is a colour engine: no pixel is ever transformed from one space into
  another, and `gimg_ops_resize()`'s linear-light mode works because the
  caller asserts sRGB, not because the library read a profile. A CMYK JPEG
  converts to RGB by the naive ink model or not at all
- Codec framework and format support: PNG (including APNG), JPEG, BMP and GIF. TIFF, WebP and the rest are not written yet and have no page under [Format and specification references](@ref format_references); a page is written with its codec rather than ahead of it
- Transformations and image operations — orientation, pixel format and bit depth conversion, colour reduction, and geometry: cropping, resizing with a choice of resampling filter, and compositing

## Dependencies

- [`ghoti.io-cutil`](https://github.com/coreyp1/cutil) and
  [`ghoti.io-compress`](https://github.com/coreyp1/compress), both **required**
  - the shared library carries a `NEEDED` entry for each
- Google Test for the unit tests, Python 3 with Pillow for the output
  verification `make test` runs

**pkg-config is the only way this library finds its dependencies.** There is
deliberately no sibling-checkout fallback: a second resolution path that only
in-tree builds exercise is one that silently rots. A dependency pkg-config
cannot find is a hard error naming the fix.

## Building

With the dependencies already installed somewhere pkg-config can see them:

```bash
make
```

Building the whole suite into a local prefix, which is what the development
tree does:

```bash
./bootstrap.sh                       # from the workspace root
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/image PREFIX="$PWD/.local"
```

## Testing

```bash
make test
```

`make test` runs the unit tests and then verifies the PNG, JPEG, BMP and GIF
this library writes by reading them back with outside decoders, checks the
structure of the bytes themselves, and compares the resampler against Pillow.
`make test-asan` adds AddressSanitizer and UndefinedBehaviorSanitizer;
`make test-tsan` runs the concurrency test under ThreadSanitizer;
`make coverage` reports line coverage. `make help` lists the rest.

## Installation

```bash
sudo make install                    # into /usr/local
make install PREFIX=/some/prefix     # or wherever
```

## Usage

See the examples directory for usage examples.

## Documentation

- [Modules](@ref modules) - Detailed documentation for library modules
- [Examples](@ref examples) - Example programs demonstrating library usage
- [Function Index](@ref functions_index) - Complete API reference
- [Format and specification references](@ref format_references) - One page per format: the specification implemented, the parts covered, deviations, and tested scope
  - [PNG and APNG](@ref format_png), [JPEG](@ref format_jpeg), [BMP](@ref format_bmp), [GIF](@ref format_gif)
  - [Adding a format](@ref format_adding) - the checklist and page template for a new codec

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


## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
