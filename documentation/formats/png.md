@page format_png PNG and APNG

# PNG and APNG

What this codec implements of ISO/IEC 15948 and of the APNG extension, where
it is deliberately stricter than libpng, and how each claim was checked. The
library's format index is the
\ref image_format_references "format and specification references".

## Normative references

- **Specification:** ISO/IEC 15948:2004 (PNG — Portable Network Graphics), and the PNG Specification (W3C Recommendation 10 Nov 2003) at <https://www.w3.org/TR/PNG/>.
- **Data representation (image layout, filtering, interlace):** <https://www.w3.org/TR/PNG-DataRep.html>
- **Filter algorithms (None, Sub, Up, Average, Paeth):** <https://www.w3.org/TR/PNG-Filters.html>
- **Interlaced data order (Adam7):** W3C §2.6 — <https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order>
- **APNG:** APNG extension (animated PNG); see <https://wiki.mozilla.org/APNG_Specification> and common implementations.

The PNG codec uses the document/codec allocator for all codec-owned
allocations, including chunk reading when a temporary buffer is needed for a
large payload.

## Parts implemented

- **Chunk layout and CRC:** Signature (5.2), chunk structure — 4-byte length (big-endian), 4-byte type, payload, 4-byte CRC (5.3). CRC-32 over type+payload per PNG spec.
- **Chunk ordering:** Critical and ancillary chunk order per spec (5.6, Table 5.3): IHDR first; PLTE (and tRNS if present) before IDAT for palette images; IDAT contiguous; IEND last. On save, preserved ancillary chunks are written before PLTE, which is where Table 5.3 requires cHRM, gAMA, iCCP, sBIT, sRGB, cICP, mDCv and cLLi and where it permits pHYs, sPLT, eXIf and the text and time chunks - except bKGD and hIST, which Table 5.3 places *after* PLTE and which are therefore held back until it has been written.
- **Critical chunks:** IHDR (11.2.1), PLTE (11.2.2), IDAT (11.2.4), IEND (11.2.5). For palette images, PLTE (and optional tRNS) must appear before IDAT.
- **Ancillary chunks:** tEXt, zTXt, iTXt, iCCP, sRGB, gAMA, cHRM, eXIf, and others as raw or typed per spec (11.3).
- **Color chunk policy:** PNG allows at most one of sRGB, iCCP, or gAMA+cHRM for color interpretation. If multiple are present, this implementation uses the first in priority order: **sRGB > iCCP > gAMA/cHRM**. The chosen chunk is applied to `GCOL_Color_Info`; iCCP bytes are stored for round-trip in ancillary and (when chosen) the decompressed profile is attached to the decoded raster.
- **Filtering and interlace:** Filter types (None, Sub, Up, Average, Paeth) per [PNG-Filters](https://www.w3.org/TR/PNG-Filters.html); Adam7 interlace (seven passes) per [Interlaced data order](https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order).
- **pHYs** (11.3.4.3): a density in pixels per meter arrives as a DPI through `gimg_meta_common_dpi()`; unit specifier 0, which states a pixel aspect ratio and no physical size, arrives through `gimg_doc_pixel_aspect_ratio()`. That second case used to be dropped, which lost the only thing such a chunk says.
- **bKGD** (11.3.4.1): the colour a viewer is told to put behind the image, reported through `gimg_doc_background_color()` - the same accessor GIF's Background Color Index arrives through. See "The background colour" below; it is reported, never painted.
- **APNG:** acTL, fcTL, fdAT; frame timing, dispose (None/Background/Previous), blend (Source/Over) per APNG spec. acTL's `num_plays` is reported through `gimg_doc_loop_count()`, the same document-level accessor GIF's NETSCAPE2.0 count uses: 0 means forever, and a still PNG has no acTL and so declares nothing at all. Saving takes the count from the document, so a PNG round trip preserves it and a count carried in from a GIF is written too. It used to come from the loaded PNG's own state instead, which made `gimg_doc_set_loop_count()` a no-op that reported success on an APNG and wrote zero on anything else. Nothing falls back to that state now: the loader put the acTL's value in the document, so a document that declares none is one a caller cleared on purpose. Unlike GIF, APNG cannot say "no count" - acTL is what makes a PNG an APNG and always carries a `num_plays` - so a document declaring none is written as 0, the format's own word for forever.

## Save metadata policies

On PNG save, `GIMG_Save_Options.metadata_policy` controls which ancillary chunks are written:

- **GIMG_META_PRESERVE_ALL:** Emit all ancillary chunks from the loaded document (and eXIf from doc meta_raw when the doc was not loaded from PNG). PLTE/tRNS are always emitted when required for the image.
- **GIMG_META_DROP_ALL:** Emit no ancillary metadata; only signature, IHDR, PLTE/tRNS (if palette), IDAT, IEND.
- **GIMG_META_STRIP_GPS:** Only actual GPS data is stripped. eXIf is parsed; the GPS IFD (and GPS-related tags) are removed; the remaining Exif is re-serialized and written as the eXIf chunk. Non-GPS Exif (orientation, datetime, etc.) is preserved. tEXt, zTXt, and iTXt chunks whose keyword is "GPS", "GPS " (with trailing space), or "EXIF:GPS" (case-insensitive) are omitted. Other ancillary is preserved.
- **GIMG_META_NORMALIZE_EXIF:** eXIf is normalized (e.g. orientation set to 1 / applied, duplicate tags removed) and written as a single eXIf chunk. Other ancillary is preserved. Implemented in Phase 1.5: Exif module sets orientation tag to 1 (normal) when present.
- **GIMG_META_KEEP_RAW_ONLY:** Emit only ancillary chunks that are *not* known semantic metadata. Omitted: iCCP, sRGB, gAMA, cHRM, eXIf, tEXt, zTXt, iTXt. Emitted: any other ancillary chunk type (e.g. unknown or private chunks) in read order. eXIf from doc meta_raw is not written. PLTE/tRNS are emitted when required for the image.
- **GIMG_META_KEEP_COMMON_ONLY:** Emit only metadata that maps to common metadata. Exactly one color chunk is written from the decoded raster's `GCOL_Color_Info` (see **Color on save** below). No eXIf, no text chunks, no other ancillary. PLTE/tRNS are emitted when required for the image.

## Color on save

At most one color chunk is written, because PNG 11.3.3.3 does not want sRGB
and iCCP in the same file and gAMA is redundant beside either.

**The chunk the file came with wins.** A document loaded from a PNG that
carried sRGB, iCCP, gAMA, cHRM or cICP has that chunk preserved verbatim, and
nothing is added from the raster's `GCOL_Color_Info` on top of it - preserving
what was actually there and then restating it would put two color chunks in
one file.

**A document that brought no color chunk gets one from its raster.** That is
the case for anything that did not arrive as a PNG. It is chosen in this
order:

| Raster's `GCOL_Color_Info` | Written |
|---|---|
| `transfer` is sRGB | `sRGB`, with the rendering intent |
| a stated gamut that is not sRGB's, and no profile | `cHRM`, beside whichever of the next two applies |
| `transfer` is a gamma, or linear | `gAMA`; linear is a gamma of 1 |
| An ICC profile is attached | `iCCP`, deflated, keyword "ICC Profile" |
| None of the above | nothing |

`cHRM` (11.3.2.1) is the exception to "at most one": it states the **gamut**
and nothing about the curve, so it goes beside `gAMA` rather than instead of
it - the two are a pair. It is written only for a gamut a reader would not
otherwise assume. sRGB's primaries are what a PNG carrying no such chunk
means, so stating them costs 44 bytes and says nothing new; leaving Adobe
RGB's unstated loses them, and did - a BMP with a calibrated V4 header naming
Adobe RGB came out of a save as PNG carrying its gamma and not its gamut. It
is not written beside `iCCP`, where the profile is the more specific
statement.

On the way in, `cHRM` is read into `GCOL_Color_Info.gamut` exactly: the white
point and the three primaries as the file stated them, with no table in
between. `gcol_gamut_identify()` is what puts a name to the result, and a
gamut it has no name for is unlabelled rather than lost - the coordinates are
still there, and a save writes them back out. A `cHRM` of the
wrong length never reaches this: the loader enforces the fixed length of every
chunk whose shape the specification fixes, and refuses the file.

`gAMA` holds gamma x 100000 in four bytes and so cannot state a gamma above
about 42949. A BMP's V4 gamma is 16.16 fixed point and reaches 65535, and
converting one of those to `uint32_t` is undefined behaviour rather than a
large number - UBSan caught exactly that at 4.98588e+09 while fuzzing this
path. A gamma the chunk cannot hold goes unsaid.

The sRGB chunk takes the *transfer* actually saying sRGB, not the primaries.
Matching on the primaries alone was too loose: a BMP with a calibrated V4
header naming sRGB's primaries and a gamma of 2.2 is not an sRGB image, and
writing sRGB for it threw the gamma away and claimed a curve the file never
stated. gAMA says nothing about the primaries, which for an image whose
primaries are sRGB's costs nothing - those are what a PNG reader assumes when
no chunk says otherwise.

The profile the `iCCP` branch deflates is **owned by the writer**, not
borrowed from the raster. The raster is destroyed as soon as the image data is
deflated, and every chunk is written after that, so a borrowed pointer was a
read of freed memory - and because the deflating happens inside
`libghoti.io-compress`, which is not sanitizer-instrumented, no harness saw
it. The file came out with an `iCCP` of the right length whose first sixteen
bytes were glibc's free-list pointer. See `tests/fuzz/README.md`.

Until this was wired up the whole of the above was reachable only from
`GIMG_META_KEEP_COMMON_ONLY`, so under every ordinary policy a BMP carrying a
V5 embedded ICC profile came out as an untagged PNG and the profile was read
only to be dropped. `GIMG_META_DROP_ALL` and `GIMG_META_KEEP_RAW_ONLY` still
drop it, as they drop the rest.

## The background colour

bKGD (11.3.4.1) names a colour to present the image against. What its payload
means depends on the colour type in the IHDR beside it: one byte is a palette
index, two bytes a gray level, six bytes three 16-bit samples, and in every
case only the low `bit_depth` bits of a sample carry data.

**It is reported, never painted.** The spec says so itself - there is no
requirement that a viewer use it - and the decoders real files are authored
against mostly do not:

| | reads bKGD | composites onto it |
|---|---|---|
| here | yes, as RGBA | no |
| ImageMagick 7 | yes | **yes** - `-flatten` and `-alpha remove` both produce it |
| Pillow 11.1 | **no** - `im.info` has no such key at all | no |
| GdkPixbuf | no | no |
| Chromium | no - a transparent pixel stays `0,0,0,0` on a canvas | no |
| libpng | yes (`png_get_bKGD`) | only if the application asks (`png_set_background`) |

ImageMagick is the only one that acts on it, and it is the reason the value is
worth reporting: compositing over a reported colour is one line in a caller,
while un-compositing a background the decoder painted is not possible at all.

### Reading it

The value arrives as 8-bit RGBA, because by the time a caller has a decoded
raster the palette an index referred to is gone. Samples at other depths come
to eight bits by 13.12, the rule the pixels took, so the background matches an
image area painted the same value. The alpha is always 255: bKGD names a colour
to put *behind* the image, and a transparent one would say nothing.

A payload whose length is wrong for the colour type states nothing, rather than
a colour guessed from the bytes that are there - `png_gray8_bad_bkgd.png` has
three bytes where colour type 0 calls for two, and reading the first two would
give a plausible gray that never looked wrong.

### Writing it

The document owns the value, the same way it owns a unit 0 pHYs. The chunk the
file arrived with is dropped and the chunk written is made from whatever the
document now says - otherwise `gimg_doc_set_background_color()` would be a
no-op that reported success, with the accessor giving the new colour and the
file keeping the old one.

For a document nobody edited that re-make is the *same bytes*: the original
payload is put back verbatim when it still states what the document states and
the colour type has not changed. That matters because the document holds eight
bits a sample and a 16-bit file's bKGD holds sixteen - rebuilding it every time
would turn `1234 5678 9abc` into `1212 5656 9a9a` on a round trip that changed
nothing. `png_rgb16_bkgd.png` is the fixture that catches it.

Where the colour type cannot hold the colour asked for, **nothing is written**:

| colour type | can state | what a colour it cannot state does |
|---|---|---|
| 0, 4 (gray) | one gray level | refused unless the three samples already agree |
| 3 (palette) | an index into the PLTE being written | refused unless that palette holds the colour |
| 2, 6 (colour) | any colour | always writable |

An absent advisory chunk is a smaller lie than a wrong one, and a nearest
palette entry or a luminance would be a colour the caller never asked for and
could not tell apart from one they did. ImageMagick makes the other trade:
asked for a lime background on a grayscale PNG it promotes the image to
truecolour and warns that it cannot write the gray requested. Growing the
pixels to carry an advisory chunk is the wrong way round for a library - the
pixels are the payload.

Clearing the background writes no bKGD at all, which is the only way PNG has to
say nothing. It is written after PLTE, where Table 5.3 puts it and where the
index a palette image needs is finally known, and it is not written under
`GIMG_META_DROP_ALL`, `GIMG_META_KEEP_RAW_ONLY` (which passes the file's own
chunks through untouched) or `GIMG_META_KEEP_COMMON_ONLY` (a background is not
one of the fields `GIMG_Meta_Common` calls common).

A colour type change still retargets the chunk where it can - a 4-bit gray
background on an image promoted to colour type 6 becomes three equal 8-bit
samples - because the document's value and the file's agree there, and the
rewrite is the exact translation of what both say.

## Conformance

- **PNG:** Implementation follows PNG 1.2 (W3C Recommendation 10 Nov 2003) and ISO/IEC 15948:2004, with the color chunks the Third Edition adds. Chunk layout, ordering, filtering, and interlace (Adam7) are as specified. No intentional deviations.
- **APNG:** Follows the APNG extension as implemented by Mozilla (<https://wiki.mozilla.org/APNG_Specification>). acTL, fcTL, fdAT chunk semantics; default image vs first frame; dispose (None/Background/Previous) and blend (Source/Over) are implemented per that specification. **16-bit RGBA blend (OVER):** Component-wise alpha blend is implemented per spec; 64-bit intermediates are used for composite math to avoid overflow.
- **Color types and depths:** Every combination Table 11.1 allows is **read**: grayscale at 1, 2, 4, 8 and 16; truecolor at 8 and 16; palette at 1, 2, 4 and 8; grayscale+alpha and truecolor+alpha at 8 and 16. All of them are **written** as well, but the depths below 8 and the palette color type are reached by preserving what a frame arrived as, not by choosing them for a raster that came from somewhere else: a sub-byte frame goes back out at its own depth when every sample survives the rescaling of 13.12 in both directions, and at 8 bits otherwise, so nothing is rounded away silently. A raster with no PNG history is written at 8 or 16 bits in the color type its channels call for.
- **Transparency on save:** tRNS is written for palette images, and for color types 0 and 2 when the raster's alpha is something tRNS can express - every pixel wholly opaque or wholly transparent, one transparent color, and no opaque pixel wearing it (11.3.2.1). An image whose alpha needs more than that is written with an alpha channel (color type 4 or 6) instead of losing it.
- **Pixel aspect ratio:** a `pHYs` whose unit specifier is 0 states a ratio
  and no physical size (11.3.4.3). It is read into
  `gimg_doc_pixel_aspect_ratio()`, written back from it, and a ratio the caller
  sets or clears now beats the chunk the file arrived with - without that,
  setting one changed what the accessor reported and not what was written, and
  the old value came back on the next load. A unit 1 `pHYs` is a resolution and
  keeps its existing rule, where the file's own chunk wins; when a document has
  both, the resolution is written, because a size says everything a ratio does
  and more. The same property crosses to GIF and JPEG, which state it in their
  own ways - see \ref format_gif "GIF" for the shared model and for what other
  decoders do with it.
- **Palette on save:** A palette is built for a raster that did not arrive with one only where that is lossless - at 256 colors or fewer there is exactly one palette that reproduces the image, so nothing is being decided about the picture, only about how it is stored - and only where it is the smaller file, which is measured by encoding both forms rather than estimated. On images of the shape that benefits, a 48-color diagram and an 8-color icon with transparency, the saving is 59% and 78%; across the conformance suite, where most files are already palette or grayscale, two more files change form and no file gets larger.
- **Row filters:** All five of clause 9 are written, chosen per row by the heuristic 12.8 recommends (smallest sum of absolute filtered bytes read as signed). `GIMG_Save_Options.png_filter` pins one instead. Each Adam7 pass is filtered as its own image (9.2). On the published conformance suite the result is about 9% smaller than the suite's own files.
- **Integrity:** Both of PNG's checks are enforced. The CRC on every chunk (5.5), and the Adler-32 that ends every zlib stream (10.3, RFC 1950) - the second being the only one that catches data which still inflates but to the wrong bytes. The zlib header is validated too: compression method 8, window no larger than the format allows, and no preset dictionary, which PNG forbids and which would otherwise shift the DEFLATE data by four bytes.

## Compliance checklist

Short reference for chunks, depths, filters, and limitations. Update when adding or restricting features.

| Area | Supported | Rejected / limitation |
|------|------------|-------------------------|
| **IHDR (11.2.1)** | Every color type and bit depth combination of Table 11.1 is read; all are written too, though the sub-byte depths and color type 3 are reached by preserving a frame's own, not chosen for a raster with no PNG history (see Color types and depths above). Compression method 0, filter method 0, interlace 0 or 1 | Any other combination, a zero dimension, or an unknown method &rarr; `GIMG_ERR_FORMAT` |
| **PLTE (11.2.2)** | Required for color type 3; accepted and **ignored** for 2 and 6, where it is a suggested palette, and kept for round-trip. A palette is also **built** for a raster that arrived without one, when the image has no more than 256 distinct colors and the palette form is the smaller file - both forms are encoded and the smaller kept, since DEFLATE makes the arithmetic hard to predict. Indices are written at the smallest depth that holds them (1, 2, 4 or 8), and entries with alpha are placed first so tRNS can stop early (11.3.2.1) | Length not a multiple of 3, zero, or over 256 entries &rarr; `GIMG_ERR_FORMAT`; present for color type 0 or 4 &rarr; `GIMG_ERR_FORMAT` ("shall not appear"). More than 256 colors stays truecolor: reducing them would be color quantization, which is an image-processing decision and not a codec's - `gimg_ops_quantize()` does it on request (\ref module_palette "Palettes and colour reduction"), and the reduced raster then takes the palette path above with no option changed. Not attempted for an animation, whose frames must share one color type. `GIMG_Save_Options.png_palette` = `GIMG_PNG_PALETTE_NEVER` turns the building off; a frame that arrived as a palette is written back as one either way |
| **tRNS (11.3.2.1)** | Read for color types 0, 2 and 3. Written for 3, and for 0 and 2 when the alpha channel fits what the chunk can say | Must precede IDAT and follow PLTE; one only. Length is fixed by color type - 2 bytes for 0, 6 for 2, no more entries than PLTE for 3 - and any other length &rarr; `GIMG_ERR_FORMAT`. Present for color type 4 or 6 &rarr; `GIMG_ERR_FORMAT` ("shall not appear"): those carry alpha already, and keeping both would leave two sources of transparency and no rule for which wins |
| **IDAT (11.2.4)** | Contiguous, concatenated into one zlib stream; written split at 32768 bytes | Non-contiguous or absent &rarr; `GIMG_ERR_FORMAT` |
| **Filters (clause 9)** | All five, both directions: None, Sub, Up, Average, Paeth. Filtering is on bytes with bpp rounded up to one, so it works unchanged below 8 bits | A filter byte above 4 is not a type Table 9.1 defines, so the row cannot be reconstructed &rarr; `GIMG_ERR_CORRUPT`, on the interlaced path as well. libpng and Pillow both refuse such a file |
| **Interlace (clause 8)** | Adam7, read and written, at every bit depth. Below 8 bits samples are placed by bit: a pass row is not a byte-slice of an image row | — |
| **zlib (10.3)** | RFC 1950 wrapper checked on read: method, window, FDICT, and the trailing Adler-32 | Bad header &rarr; `GIMG_ERR_FORMAT`; failed inflate or Adler-32 mismatch &rarr; `GIMG_ERR_CORRUPT` |
| **Ancillary, interpreted** | tEXt, zTXt, iTXt (description), iCCP, sRGB, gAMA, cHRM, eXIf (orientation), cICP | A text chunk that cannot be decoded - a compression method other than the 0 of 11.3.3, a truncated keyword, a zlib stream that fails its own checks - is skipped and the image still loads, which is what libpng does with one. It is not a reason to refuse a picture. The chunks whose length the spec fixes are checked on read - gAMA 4, cHRM 32, sRGB 1, pHYs 9, tIME 7, cICP 4 - and any other length &rarr; `GIMG_ERR_FORMAT`, since there is nothing to interpret a short one as and keeping it would write the same malformation back out |
| **Ancillary, preserved only** | sBIT, tIME, sPLT, hIST, mDCv, cLLi, and any unknown chunk, kept in read order and written back by the policies that preserve what a file came with | Not acted on when decoding. sBIT is advisory and a decoder is not required to use it; mDCv and cLLi describe a mastering display and mean something only to an HDR pipeline this library does not have |
| **Ancillary the document owns** | bKGD (11.3.4.1) is reported through `gimg_doc_background_color()`, and the chunk written comes from the document rather than from the file - see "The background colour". A unit 0 pHYs (11.3.4.3) is the same arrangement for `gimg_doc_pixel_aspect_ratio()` | Otherwise the setter would be a no-op that reported success: the accessor would give the new value and the file would keep the old one, which only a round trip reveals. The file's own bytes still go back verbatim when they state what the document states, so an untouched round trip is byte for byte |
| **Ancillary whose shape follows the color type** | bKGD (11.3.4.1), sBIT (11.3.2.4) and hIST (11.3.4.2) are laid out according to the color type, and the one being written is not always the one the frame arrived as. A background color translates wherever the destination can hold it - gray becomes R=G=B, a palette index becomes the color it names, a color becomes gray only when its samples already agree - rescaling between depths by 13.12 | sBIT does not survive a change of depth: rescaling spreads the value over the whole of the new sample, so a count taken before it would misdescribe what is stored. hIST has no meaning without the palette it counts. Those are dropped, as is any of the three whose length was already wrong for the file it came from: an absent advisory chunk is a smaller lie than a wrong one |
| **pHYs (11.3.4.3)** | Read into the document's common metadata as dots per inch, and written from it when the file did not bring a pHYs of its own. An inch is exactly 0.0254 m, so the conversion is integer arithmetic - 5000/127 and back - and every resolution from 1 to 1200 dpi survives the round trip exactly | Only unit specifier 1 states a physical size. Unit 0 gives an aspect ratio, which says how a pixel is shaped and not how big it is, so it yields no dpi. A pHYs the file came with is the one written back; the metadata copy is a fallback for a document that arrived from somewhere else, such as a JPEG's JFIF density |
| **Color precedence** | cICP > sRGB > iCCP > gAMA/cHRM, which is the order the Third Edition sets | Primaries 1, 9 and 12 and transfers 1/4/5/6/8/13/14/15/16/18 are translated, PQ and HLG included. Code points outside those - theatrical DCI-P3 (11) among them - leave that half unsaid rather than approximated. A matrix other than identity, or a limited range, refuses the chunk outright: PNG 3rd ed. requires both of an RGB image. The chunk is preserved either way |
| **APNG** | acTL, fcTL, fdAT; dispose None/Background/Previous, blend Source/Over; 8- and 16-bit compositing | acTL after IDAT, duplicate acTL, out-of-order sequence numbers, more fcTL than acTL declared &rarr; `GIMG_ERR_FORMAT`. A frame must lie inside the canvas the IHDR describes - width and height above zero, `x_offset + width` at most the image width and likewise for the height - or `GIMG_ERR_FORMAT`. That is a memory-safety constraint and not a formality: compositing writes the frame into the canvas at that offset. The compositing clips as well, so no path can write past the canvas even if one reached it with bad values |
| **Orientation** | All eight of CIPA DC-008 Table 6, applied to the decoded raster | — |

## Where this library is stricter than libpng

A three-way differential over 41 deliberately non-conforming files - this
library, libpng via `pngtopnm`, and Pillow - leaves seven cases where this
library refuses what both reference decoders accept. Each is a "shall" in the
spec that libpng reports as a warning and carries on from, which is its stated
policy rather than a disagreement about the text:

| Case | Clause |
|---|---|
| PLTE present for color type 0 or 4 | 11.2.3, "shall not appear" |
| tRNS with more entries than PLTE | 11.3.2.1 |
| tRNS present for color type 4 or 6 | 11.3.2.1, "shall not appear" |
| tRNS before PLTE | Table 5.3 |
| IDAT chunks not consecutive | 11.2.4, "shall appear consecutively" |
| more scanlines than the IHDR declares | clause 8 |
| tIME of a length other than 7 | 11.3.5 |

The remaining disagreements run the other way and are all cases where a
reference decoder is stricter than the spec requires, or where the spec leaves
the choice open: data after IEND, a gamma of zero, an sRGB rendering intent
outside 0..3, and a palette index naming an entry the palette does not have -
which decodes to black here, is bounds-checked, and is what libpng does too.

## Tested scope

- **Reference files** in `tests/data/png/`, generated by `tests/data/png/generate.py`: every color type and bit depth, interlaced and non-interlaced pairs of the sub-byte depths, zlib streams with a damaged wrapper, a suggested palette on truecolor, and the Third Edition color chunks. Fixtures are generated rather than vendored, and each was checked against two independent decoders before any test was written on it.
- **Interlaced and non-interlaced pairs** carry a property that needs no reference decoder: Adam7 is a reordering, so the two members of a pair must decode alike. The `.raw` files beside them hold the expected pixels computed from 13.12, which pins both members to the spec rather than to each other.
- **Encoder output is checked by a decoder that is not ours.** `verify_png_output.py` reads back what the encoder wrote with Pillow and compares it to those same expected pixels, because our decoder agreeing with our encoder would prove nothing about either.
- **Conformance corpus.** The published PNG test suite (PngSuite, 176 images) is used as a development oracle from a scratch directory, not vendored: of the 162 valid files, 159 decode identically to libpng and 3 differ only because `pngtopnm` does not apply tRNS to a truecolor image, where Pillow and the spec agree with this library; all 14 deliberately corrupt files are rejected and none is wrongly accepted. All 162 round-trip to identical pixels, interlaced or not.
- **Fuzzing.** `fuzz_png_load` and `fuzz_png_encode` each have a seed corpus under `tests/fuzz/corpus/<name>/`, built with ASan and UBSan and `-fno-sanitize-recover=undefined`. Only `*.seed` files are tracked; campaign units are gitignored.
- **Round-trip is checked against a decoder that is not ours**, with the oracle's own limits accounted for rather than tolerated: Pillow reports tRNS in `info['transparency']` but does not apply it when converting a mode `L` image to RGBA, so the sweep applies it, scaling the value from the file's bit depth by 13.12 first. All 162 valid conformance images now round-trip with no differences at all.
- **Deliberate strictness is listed above**, under "Where this library is stricter than libpng".
- **Color a document brought from another format** is covered in
  `testPng_encode`: an ICC profile on the raster comes back byte for byte
  through a save and load, an sRGB transfer becomes `sRGB`, a gamma or a
  linear transfer becomes `gAMA`, a gamma `gAMA` cannot hold becomes nothing,
  and a PNG that arrived with its own color chunk keeps exactly that one.
  `testBmp_decode` carries the end-to-end pair: a BMP with a V5 embedded
  profile, saved as a PNG, keeps the profile intact.
- **Previously recorded deviations, all now gone:** a gamma that `gAMA` cannot
  state was converted to `uint32_t` anyway, which is undefined behaviour and
  not a large number, and was reachable from any BMP with a large V4 gamma;
  the Adam7 reassembly copied whole bytes per pixel, so below 8 bits it both smeared each sample over its neighbors and wrote past the end of every row; the palette writer stored one byte per index into a row sized for packed ones; the zlib Adler-32 and header were never read; PLTE on a truecolor image was rejected rather than ignored; only filter None was ever written; a truecolor image with tRNS lost its transparency on save; six of the eight EXIF orientations returned `GIMG_ERR_UNSUPPORTED`, which made any image carrying one fail to decode; an APNG frame declared past the edge of its canvas was composited there anyway, which was a heap write past the end of the canvas buffer reachable from any such file; a filter byte above 4 was treated as filter None rather than refused, so a corrupt row decoded to wrong pixels and reported success; bKGD and hIST were written before PLTE rather than after it, which cost the histogram entirely in libpng; and bKGD, sBIT and hIST were copied verbatim into files written with a different color type, so promoting a grayscale image with tRNS to truecolor left a two-byte bKGD where the format calls for six - which libpng reported as "bKGD: invalid".

---

Back to \ref image_format_references "Format and specification references".
