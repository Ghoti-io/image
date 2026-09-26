@page format_tiff TIFF

# TIFF

The TIFF codec reads a baseline subset of TIFF 6.0 and writes nothing yet.
This page says which parts, how each claim was checked, and - at greater
length than the other format pages, because the absences outnumber the
presences today - what is deliberately not here. Back to
\ref image_format_references "Format and specification references".

## Normative references

- **Specification:** [TIFF Revision 6.0, final, 3 June 1992](https://web.archive.org/web/20210108172930/https://www.adobe.io/content/dam/udp/en/open/standards/tiff/TIFF6.pdf),
  Adobe Systems. Sections referenced below by their numbers in that document.
- **BigTIFF** is a different format with its own version number (43 where TIFF
  writes 42). It is recognised only so that it can be refused by name.

Codec-owned allocations use the codec allocator, as
\ref image_development "Development" requires; the document and its rasters
use the document allocator.

## Parts implemented

**Structure (sections 2 and 3).** Both byte orders. The header, the IFD chain,
and directory entries of every type the specification defines, with a value
read from the entry itself when it fits in four bytes and from the offset
those bytes hold when it does not. Every IFD becomes one document item.

**Tags read (sections 8 and 15).** ImageWidth, ImageLength, BitsPerSample,
Compression, PhotometricInterpretation, StripOffsets, SamplesPerPixel,
RowsPerStrip, StripByteCounts, XResolution, YResolution, PlanarConfiguration,
ResolutionUnit, ColorMap, TileWidth, TileLength, TileOffsets, TileByteCounts,
ExtraSamples, SampleFormat. Every default the specification states is applied,
so a tag's absence means what section 8 says it means rather than zero.

**Storage layout.** Strips and tiles, both assembled into one raster. A tile's
stored data is the full tile size even where the tile overhangs the right or
bottom edge; the padding is read and dropped.

**Colour.** PhotometricInterpretation 0 (WhiteIsZero), 1 (BlackIsZero), 2
(RGB) and 3 (Palette), at eight bits per sample. Grayscale comes back as
GRAY8 and everything else as RGBA8, which is what the BMP and GIF decoders
also hand back. WhiteIsZero is complemented on the way out. A ColorMap is
expanded with the rounding PNG 13.12 states, which is what this library uses
for every other depth conversion.

**Alpha.** ExtraSamples 1 (associated) is divided back out, because this
library's RGBA8 is unassociated; ExtraSamples 2 (unassociated) passes through.

**Resolution.** XResolution and YResolution reach `GIMG_Meta_Common` as DPI
when ResolutionUnit is 2 (inches). Centimetres and "no unit" are not
converted, because that field carries dots per inch and nothing else.

## Save

There is no TIFF writer. `GIMG_CAP_WRITE` is not set, so `gimg_doc_save()`
refuses the format rather than half-writing one, and the cross-codec sweeps
that look for a save target leave TIFF out rather than reporting its absence
as a stream of failures.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Byte order | "II" and "MM" | - |
| Version | 42 | 43 (BigTIFF) &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| IFD chain | Any number up to 4096, refusing a chain that does not advance | A chain that points backwards or at itself &rarr; `GIMG_ERR_CORRUPT`; `max_frame_count` caps it lower |
| Compression | 1 (none) | 2, 3, 4 (CCITT), 5 (LZW), 6/7 (JPEG), 8 (Deflate), 32773 (PackBits) &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| Photometric | 0, 1, 2, 3 | 4 (mask), 5 (CMYK), 6 (YCbCr) and the rest &rarr; `GIMG_ERR_UNSUPPORTED` |
| BitsPerSample | 8 | 1, 2, 4, 16, 32 &rarr; `GIMG_ERR_UNSUPPORTED`; samples that differ from each other are refused as that, separately |
| SamplesPerPixel | 1 for grayscale and palette, 3 or 4 for RGB | Anything else &rarr; `GIMG_ERR_UNSUPPORTED` |
| PlanarConfiguration | 1 (interleaved) | 2 (separate planes) &rarr; `GIMG_ERR_UNSUPPORTED` |
| SampleFormat | 1 (unsigned integer) | 2 (signed), 3 (float) &rarr; `GIMG_ERR_UNSUPPORTED` |
| Geometry | Strips and tiles | A block list whose length disagrees with the geometry &rarr; `GIMG_ERR_CORRUPT`; a block outside the file &rarr; `GIMG_ERR_CORRUPT` |
| Source | A stream that knows its length | A non-seekable stream &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| Limits | `max_decoded_pixels`, `max_frame_count` | `max_chunk_size` has no analogue; a TIFF has no chunk structure |

## Where this codec differs from the reference implementations

No outside decoder has been run against it yet, so this table has one entry
and it is a warning rather than a measurement.

| Case | This codec | Elsewhere |
|---|---|---|
| A ColorMap storing 8-bit values in the low byte instead of scaling to 16 bits | Read as the specification says: a 16-bit value, so 255 decodes as almost black | libtiff guesses, by checking whether every entry is under 256, and rereads such a map as 8-bit. A file written by such a writer will look wrong here |

That divergence is written down rather than fixed because fixing it means
adopting a heuristic, and a heuristic adopted without a corpus to measure it
against is a guess with a comment. It is the first thing to settle when an
oracle is wired up.

## Tested scope

- **Fixtures** in `tests/data/tiff/`, generated by
  `tests/data/tiff/generate.py` rather than vendored, so the properties
  asserted about them are pinned to something other than this library.
- **Three properties that need no reference decoder**, in
  `tests/codec/tiff/test_tiff_decode.cpp`:
  the same picture written in both byte orders decodes identically; the same
  picture cut into tiles and into strips decodes identically, with the tiles
  deliberately not fitting a whole number of times across; and both are
  checked against a gradient recomputed in the test from the formula the
  generator used, so a transposed or mirrored decode fails rather than merely
  agreeing with its sibling.
- **The cross-codec sweeps** cover TIFF with no per-format code, because they
  take their population from the codec registry: truncation at every cut point
  of every fixture, the refusal-reason sweep, the conversion matrix, and the
  item-role sweep.
- **No external oracle yet.** Nothing outside this library has read these
  files and agreed. A round trip is not available either, there being no
  writer, so the evidence here is "the decoder agrees with the specification
  as the generator spells it" and nothing stronger. That is the largest gap on
  this page.

## Not implemented

Listed so the absences are visible rather than discovered:

- **Every compression method.** LZW, PackBits, Deflate, CCITT G3/G4 and JPEG.
- **Bit depths other than 8**, including the 1-bit bilevel images that are
  most of the TIFFs in the world.
- **PlanarConfiguration 2**, CMYK, YCbCr, and transparency masks.
- **A writer.**
- **Pyramids and SubIFDs.** `GIMG_ITEM_LEVEL` exists for them and nothing
  sets it yet; a reduced-resolution subfile currently loads as another page.
- **Region decode.** A TIFF too large to hold is refused through
  `max_decoded_pixels` rather than decoded in pieces. The reasoning is in the
  workspace note on the decode model: that needs a lazy load contract, which
  is a document-model change for every codec rather than a TIFF feature.
- **Exif and XMP**, which a TIFF carries in tags this codec does not read.
