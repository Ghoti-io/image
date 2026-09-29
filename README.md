# Ghoti.io Image

Raster images in C, as a multi-image document with metadata, colour
information, and a small set of operations.

## Formats

This is what the library implements.

- PNG, including APNG.
- JPEG, BMP and GIF.
- ICO and CUR (one codec; DIB and PNG payloads, AND-mask alpha, hotspots).
- WebP Phase A: RIFF container, `VP8X` canvas, chunk inventory, and
  `ICCP`/`EXIF`/`XMP ` carriage. Picture decode and encode are not yet
  implemented; the format page lists what remains.
- TIFF. It reads every bit depth from 1 to 32, both byte orders, strips and
  tiles, grayscale, palette, RGB, RGBA, separated and YCbCr, separate and
  contiguous planes, every compression the format defines except LogLuv -
  PackBits, LZW, Deflate, CCITT Group 3 and 4, JPEG in both spellings,
  ThunderScan - with multi-page documents and pyramids. It writes 8- and
  16-bit grayscale, RGB, RGBA and CMYK, uncompressed or with PackBits, LZW or
  Deflate. What is absent is listed on the format's own page rather than left
  to be discovered.

Later WebP phases (VP8L/VP8 decode, animation items, lossless encode) follow
the format page's "Not implemented" list. A page for a format is written with
its codec.

## Before you call it

- Colour is carried until you ask. An ICC profile is read, kept, reported and written back. Load and save never remap samples. `gimg_ops_transform_color()` is the explicit CMM step (via libs/color), in the same shape as Pillow ImageCms or WIC's colour transform.
- `gimg_ops_resize()` can work in linear light using the raster's stated transfer via libs/color. An unstated transfer is still the caller's assertion of sRGB.
- A memory stream borrows its bytes. They stay alive for the life of the stream.
- Load and save take a limits struct. `NULL` options are the defaults.
- `NULL` for an allocator is cutil's default.

## Examples

```c
#include <ghoti.io/image/image.h>
#include <stdio.h>

int dimensions(const void * bytes, size_t length) {
  GIMG_Stream * stream = NULL;
  GIMG_Doc * doc = NULL;
  GIMG_Raster * raster = NULL;

  if (gimg_stream_create_memory(bytes, length, &stream) != GIMG_OK) {
    return 1;
  }
  if (gimg_doc_load(stream, NULL, NULL, &doc) != GIMG_OK) {
    gimg_stream_destroy(stream);
    return 1;
  }

  GIMG_Item * item = gimg_doc_item(doc, 0);
  if (gimg_item_ensure_decoded(item, NULL) == GIMG_OK) {
    raster = gimg_item_raster(item);
    printf("%u x %u\n", gimg_raster_width(raster), gimg_raster_height(raster));
  }

  gimg_doc_destroy(doc);
  gimg_stream_destroy(stream);
  return 0;
}
```

It prints the width and height of the first item, in pixels.
`gimg_doc_save()` writes a document back through a stream.
`examples/apng_white_block.c` builds an animation from scratch.

## Compile and link

Once the library is installed, pkg-config carries the include path, the
library, and its dependencies:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-image-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-image-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil) and
[compress](https://github.com/Ghoti-io/compress) must already be installed
where pkg-config can see them. A dependency it cannot find is a hard error
naming the fix. Google Test builds the unit tests. Python 3 with Pillow is
what `make test` uses to read this library's output back.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout, which installs cutil and compress first:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/image test PREFIX="$PWD/.local"
```

`make test` is the suite. `make help` lists the rest, including
`make test-asan` and `make test-valgrind`.

| Target | What it does |
| --- | --- |
| `make coverage` | Line coverage, per file |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `gimg_` / `GIMG_`, under `<ghoti.io/image/...>`.
`<ghoti.io/image/image.h>` is the umbrella.

- **`stream.h`** — a byte stream over memory. Codecs read and write through it.
- **`codec.h`** — `gimg_probe()`, `gimg_doc_load()`, `gimg_doc_save()`, and `gimg_item_decode()`. The format is recognised from the bytes.
- **`doc.h`** — a document of items (frames or pages), loop count, frame delay, dispose and blend.
- **`raster.h`** — width, height, stride, pixel format, and the pixel buffer.
- **`color.h`** — includes `ghoti.io/color/color.h`. Colour information a file stated (`GCOL_Color_Info`: profile, chromaticities, transfer) is reported and preserved on load/save; sample remapping is `gimg_ops_transform_color` in `ops.h`.
- **`meta.h`** — common metadata, plus the raw chunks a format carried so a round trip can put them back.
- **`ops.h`** — orientation, pixel-format and bit-depth conversion, colour transform, colour reduction, crop, resize, and composite.
- **`allocator.h`** — `GIMG_Allocator`, which is cutil's `GCU_Allocator`.

[Formats](#formats) is what is implemented.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

Both are found through pkg-config, and the installed `.pc` file names them,
so a program that links `ghoti.io-image-0` links these too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator and the overflow-checked size arithmetic.
- [ghoti.io-compress](https://github.com/Ghoti-io/compress) — deflate and CRC-32 for PNG.

## Documentation

| Page | What it settles |
| --- | --- |
| \ref image_format_references "formats/" | One page per format: the specification, what is covered, where this library differs |
| \ref image_modules "modules/" | The API |
| \ref image_examples "examples.md" | The example programs |

`make docs` builds the manual those pages feed.

## Status

PNG, APNG, JPEG, BMP and GIF load and save.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
