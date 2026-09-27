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

**Pyramids.** A reduced-resolution copy is a `GIMG_ITEM_LEVEL` of the page it
belongs to rather than a second picture, in both of the format's spellings:
`NewSubfileType` bit 0 on a directory in the main chain, and a `SubIFDs` entry
(330) hanging off the full-size page (Technical Note 1). A caller counting the
pictures in a document must not count a smaller copy of one of them, and the
role is how the model says so.

**Tags read (sections 8 and 15).** ImageWidth, ImageLength, BitsPerSample,
Compression, PhotometricInterpretation, StripOffsets, SamplesPerPixel,
RowsPerStrip, StripByteCounts, XResolution, YResolution, PlanarConfiguration,
ResolutionUnit, ColorMap, TileWidth, TileLength, TileOffsets, TileByteCounts,
ExtraSamples, SampleFormat. Every default the specification states is applied,
so a tag's absence means what section 8 says it means rather than zero.

**Storage layout.** Strips and tiles, both assembled into one raster. A tile's
stored data is the full tile size even where the tile overhangs the right or
bottom edge; the padding is read and dropped.

**Metadata.** The ICC profile (34675) reaches the raster's colour info and is
written back unchanged - a TIFF is what professional colour work is stored in,
and a profile dropped in passing makes a file's colours mean something else.
ImageDescription (270) becomes the document's description, XMP (700) is kept
whole for the round trip, and Orientation (274) is applied rather than
carried. The Exif sub-IFD (34665) is not read: it is an offset to another
directory, and preserving one by copying bytes would preserve offsets that no
longer point anywhere.

**YCbCr (section 21).** Any subsampling up to 4x4, converted to RGB on the
way out. The multipliers are computed from the file's own `YCbCrCoefficients`
rather than borrowed from JPEG's rounded constants, and the arithmetic is the
scaled-integer form every implementation uses - which is what makes it agree
with libtiff sample for sample rather than by one on the green channel. See
the deviations table.

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

**Every item becomes a page.** TIFF is the only format this library writes
that holds several pictures as pictures rather than as frames of an animation
or as a thumbnail, so a document of several items goes out whole - none of
the "item 0 and a documented silence" that BMP and JPEG need.

Written: grayscale at 8 and 16 bits, RGB and RGBA at 8 and 16, and CMYK at 8
and 16, always interleaved (PlanarConfiguration 1) and always in strips of
about eight kilobytes. `ExtraSamples` says *unassociated* for an RGBA raster,
because that is what this library's RGBA means and because libtiff's guess
for a fourth sample it was not told about is "unspecified" rather than alpha.
Resolution is written when the document carries DPI.

Four options, all in @ref api_options "API options": `tiff_compression`
(none, PackBits, LZW or Deflate), `tiff_predictor` (horizontal differencing),
`tiff_big_endian`, and `tiff_rows_per_strip`. The default is an uncompressed
little-endian file, which is the format's own default.

The file is laid out in one pass over a plan rather than written and patched.
Everything in a TIFF is found by absolute offset, so a writer either computes
its offsets before it writes a byte or seeks back to fix them up - and seeking
back is not available, because an output stream here is append-only.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|---|---|---|
| Byte order | "II" and "MM" | - |
| Version | 42 | 43 (BigTIFF) &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| IFD chain | Any number up to 4096, refusing a chain that does not advance | A chain that points backwards or at itself &rarr; `GIMG_ERR_CORRUPT`; `max_frame_count` caps it lower |
| Compression (read) | 1 (none), 2 (CCITT modified Huffman), 3 (Group 3, one- and two-dimensional), 4 (Group 4), 5 (LZW, including the pre-1993 bit-reversed spelling), 6 (old-style JPEG, both shapes), 7 (JPEG), 8 and 32946 (Deflate), 32773 (PackBits), 32809 (ThunderScan) | 34676/34677 (LogLuv) and anything else &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| JPEG-in-TIFF | Compression 7 with JPEGTables (347), decoded by this library's own JPEG codec through T.81 B.4's abbreviated format - which is what the tag pair *is*. Compression 6 too: the 1992 tags, whose tables are bare arrays at file offsets and whose strips hold entropy data with no frame header, so the frame is assembled from the TIFF tags | 8-bit only, PlanarConfiguration 1 only, and JPEGProc other than baseline &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| ThunderScan | Compression 32809, four-bit greyscale | Any other depth or sample count &rarr; `GIMG_ERR_CORRUPT`, named |
| FillOrder | 1 and 2, read by the CCITT decoder | Ignored for every other compression, which is defined on whole bytes; libtiff does the same |
| T4Options / T6Options | Two-dimensional coding, end-of-line codes present or absent, fill bits | Uncompressed mode (bit 1 of either) &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| Compression (write) | none, PackBits, LZW, Deflate, with or without Predictor 2 | - |
| Predictor | 1 and 2, at 8 and 16 bits | 3 (floating point) and any other value &rarr; `GIMG_ERR_UNSUPPORTED` |
| Photometric | 0, 1, 2, 3, 5 (separated, read as CMYK), 6 (YCbCr, any subsampling up to 4x4) | 4 (mask), 32844/32845 (LogLuv) and the rest &rarr; `GIMG_ERR_UNSUPPORTED` |
| BitsPerSample | 1, 2, 4, 8 and 16 | 6, 10, 12, 14, 24, 32 &rarr; `GIMG_ERR_UNSUPPORTED`; samples that differ from each other are refused as that, separately |
| SamplesPerPixel | 1 for grayscale and palette, 3 or 4 for RGB, 4 for separated | Anything else &rarr; `GIMG_ERR_UNSUPPORTED` |
| PlanarConfiguration | 1 and 2 | Any other value &rarr; `GIMG_ERR_CORRUPT`; the writer always writes 1 |
| SampleFormat | 1 (unsigned integer) | 2 (signed), 3 (float) &rarr; `GIMG_ERR_UNSUPPORTED` |
| Geometry | Strips and tiles | A block list whose length disagrees with the geometry &rarr; `GIMG_ERR_CORRUPT`; a block outside the file &rarr; `GIMG_ERR_CORRUPT` |
| Source | A stream that knows its length | A non-seekable stream &rarr; `GIMG_ERR_UNSUPPORTED`, named |
| Limits | `max_decoded_pixels`, `max_frame_count` | `max_chunk_size` has no analogue; a TIFF has no chunk structure |

## Where this codec differs from libtiff

Measured against libtiff 4.7.0 in the pinned oracle image on 2026-09-26.
**Twelve of the fifteen fixtures are read by both, and on all twelve every
sample agrees.** The three differences below are the whole of the rest.

| Case | This codec | libtiff | Settled how |
|---|---|---|---|
| A ColorMap storing 8-bit values in a 16-bit field | Read as 8-bit when *every* entry is under 256 | The same guess | **Changed to match.** This page previously said the opposite, and said so as a warning rather than a measurement. Measured: given a map of 0..255 libtiff answers 255 where this codec answered 1 - not subtly wrong but nearly black. The guess is safe in the one direction that matters: a map that is genuinely 16-bit *and* has every entry under 256 describes an image whose brightest colour is 0.39% of full scale, so misreading it costs a picture that was already black |
| Alpha in the decoded raster | Unassociated, following PNG | **Associated.** Its own header names the table that does it: `UaToAa`, "Unassociated alpha to associated alpha conversion LUT" | Neither is wrong; they are different units. The comparison puts ours into libtiff's space by premultiplying, which is applied to every pixel of every file and is a change of units rather than a tolerance |
| The Orientation tag (274) | Applied at decode, as `gimg_item_decode` applies a JPEG's or a PNG's Exif orientation for every codec here. The written file therefore declares none: the pixels *are* the display image, and re-declaring it would have the next reader rotate them twice | Its RGBA reader **flips rather than transposes** for orientations 5 to 8. Measured on `tiff_4x4_metadata.tif`, whose first pixel comes back as the source's top-right where a 90° rotation puts its bottom-left | The comparison sweep counts such files instead of comparing them: two transforms, one of them wrong, is not two decoders. The orientation is checked against the specification in `test_tiff_decode.cpp` |
| YCbCr's green channel | The multipliers are computed from the file's `YCbCrCoefficients` at 16 fractional bits, which is what section 21's formula reduces to | The same | Agrees exactly. Worth recording because the first attempt did not: evaluating the formula in double precision and rounding at the end disagreed by one on 2 samples of 1,228,800 in `dscf0013.tif` and 94 of 325,000 in `ycbcr-cat.tif`, **every one of them green**, because green is the only channel whose multipliers are not exact in five decimal places. This library has met the same difference from the other side - its JPEG decoder uses libjpeg's five-place constants, and `tools/oracle/containers/IMAGES` records IJG v10 disagreeing with libjpeg-turbo for exactly that reason |
| A CCITT block whose bits decode to nothing | Refused, `GIMG_ERR_CORRUPT` | Read; it reports the error and hands back the rows it managed, which for a block that failed on its first row is a blank page | Deliberately not matched. A fax that produced no rows at all has not been decoded, and a blank page is the one wrong answer a caller cannot tell from a right one. A block that fails *part way* is treated as libtiff treats it: the rows that arrived are kept |
| Old-style JPEG in YCbCr | The JPEG's own colour conversion, as JFIF defines it, because the payload is a JPEG datastream written by a JPEG encoder | **Section 21's conversion, with ReferenceBlackWhite** - but only for compression 6. Measured with a minimal pair: rewriting that tag from the full-range default to CCIR 601's studio range changes libtiff's answer for a compression-6 file and does not change it at all for a compression-7 one | Deliberately not matched, and the asymmetry is libtiff's: it routes old-style JPEG through its generic YCbCr path and new-style through libjpeg. This codec agrees with libtiff exactly on every compression-7 file, YCbCr included. What stands in place of a comparison for the two old-style YCbCr files is three grayscale fixtures - the same JPEG cut three ways - where there is no colour model to disagree about and libtiff agrees with all three |
| A block that ends before its geometry does | The rows that arrived are kept and the rest stay as the raster was created | **The previous strip's rows.** libtiff decodes into a reused buffer and leaves what a short strip did not reach. Measured on `text.tif`, whose last strip declares 39 rows and encodes 36: libtiff's rows 357 and 358 come back byte-identical to its own rows 293 and 294 | Deliberately not matched. The sweep states the condition rather than the file name - every differing row is, on libtiff's side, an exact copy of an earlier row of its own output - so it stops applying if libtiff stops doing it |
| A file with no PhotometricInterpretation | Refused, `GIMG_ERR_CORRUPT`, named | Read; it supplies a default | Deliberately not matched. Section 8 gives that field no default, so a file without one has not said what its samples mean, and guessing is a worse answer than saying so |

The only other asymmetry is the obvious one: libtiff reads a few files this
codec does not, which is the to-do list rather than a divergence. The sweep
prints that list every run, and it shortens as the codec grows - CCITT came
off it, and what is left is in **Not implemented** below.

## Tested scope

- **libtiff 4.7.0**, pinned in `tools/oracle/containers/IMAGES` and reached
  through the container image, comparing every fixture sample for sample:
  `tests/codec/tiff/test_tiff_oracle.cpp`. It carries the sentinel the rest of
  this library's oracles carry, so a machine that cannot reach the reference
  fails rather than printing SKIPPED and exiting 0.

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
- **Three JPEG-in-TIFF fixtures cut from one grayscale JPEG**: the modern
  spelling with JPEGTables, the 1992 spelling with its tables as bare arrays,
  and the 1992 spelling carrying a whole datastream with the meaningless scan
  header a 1992 encoder wrote. Grayscale on purpose - it is the one shape
  where libtiff and this codec cannot disagree about a colour model, so the
  sweep can demand an exact match, and it gets one on all three.

- **Two ThunderScan fixtures**, the same picture compressed and stored, for
  the same reason: the sample set's only ThunderScan file is truncated, so it
  cannot be the whole evidence for that compression.

- **Fourteen CCITT files written by libtiff**, swept the same way, in the
  same test file. The sample set's only two fax files are both Group 3, both
  one-dimensional and both FillOrder 2, so Group 4, two-dimensional coding,
  byte-aligned rows, the other fill order and a strip boundary mid-page are
  *absent* from it - and a corpus that cannot vary an axis says nothing about
  that axis however green it is. `make oracle-tools` writes the fourteen with
  the pinned libtiff, against a raster chosen for the coding rather than for
  the picture: runs past 2560 that need two makeup codes, single-pixel
  alternation, edges that move one, two and three pixels a row, and blocks
  that end above where the row below ends. They also all have to decode to
  what the uncompressed copy of the same raster decodes to, which is the half
  of the question a comparison against libtiff cannot answer.

  This found the defect it was built to find: the two-dimensional vertical
  modes were off by one, `00001x` having been read as V(3) rather than V(2).
  Both sample-set files agreed before and after, because neither contains a
  single two-dimensional row.

- **Round trip**, every fixture written back out in five ways - stored,
  PackBits, LZW, Deflate with the predictor, and big-endian - read back by
  this library and by libtiff, and compared sample for sample against the
  raster that went in. The writer offers no CCITT, so that compression is
  checked in one direction only.

## Not implemented

Listed so the absences are visible rather than discovered:

- **LogLuv** (compressions 34676 and 34677, photometric 32844 and 32845),
  the only compression the sample set still names every run. It is a
  logarithmic encoding of CIE Lu'v', which decodes to floating-point
  luminance rather than to integer samples; this library has no float raster,
  and libtiff's eight-bit answer for one is a tone mapping rather than a
  decode. Both of those would have to be decided before it could be read, and
  neither is a TIFF question.
- **CCITT uncompressed mode**, the escape inside a Group 3 or Group 4 stream
  that switches to literal bits. It is refused where a file declares it in
  T4Options or T6Options rather than discovered mid-row: nothing is known to
  write it, and a decoder that mistook the escape for a mode code would
  produce plausible rubbish instead of stopping.
- **Writing CCITT.** The decoder reads all three compressions and the writer
  offers none of them. The reason is upstream of TIFF: the writer takes an
  eight- or sixteen-bit raster, nothing in this library produces a one-bit
  one, and CCITT codes nothing else. Writing it would mean thresholding a
  grey page on the caller's behalf, which is a lossy choice the caller should
  be making. A bilevel page written by this library is PackBits or Deflate,
  both of which compress a thresholded page perfectly well.
- **Transparency masks** (PhotometricInterpretation 4).
- **Bit depths of 6, 10, 12, 14, 24 and 32**, which libtiff's own RGBA reader
  also refuses.
- **Region decode.** A TIFF too large to hold is refused through
  `max_decoded_pixels` rather than decoded in pieces. The reasoning is in the
  workspace note on the decode model: that needs a lazy load contract, which
  is a document-model change for every codec rather than a TIFF feature.
- **Exif and XMP**, which a TIFF carries in tags this codec does not read.
