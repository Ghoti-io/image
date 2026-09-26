@page module_palette Palettes

# Palettes

Three formats in this library store an image through a colour table - GIF,
palette PNG (ISO/IEC 15948 11.2.2, colour type 3) and palette BMP - and all
three cap that table at 256 entries. This is the part of `ops.h` that gets an
image down to a table that size, and the reasoning behind the shape it has.

## Why it is not in the codecs

Every palette writer here refuses an image with more colours than its table can
hold, and that refusal is deliberate rather than unfinished. With 256 colours
or fewer there is exactly one palette that reproduces an image, up to the order
of its entries; building it stores what is already there and decides nothing.
With more, something has to be thrown away, and *which* colours to lose is a
judgement about the picture - how much banding is acceptable, whether a small
bright region matters more than a large dull one - not a fact about the file
format.

So it lives in front of the codecs, where a caller invokes it by name.

## The shape that made this cheap

**`gimg_ops_quantize()` returns a raster, not a set of indices.** The result is
in the source's own pixel format and simply happens to hold few enough colours.

That one decision is why adding this required **no change to any codec**. A
writer that already refuses a 300,000-colour image and accepts a 256-colour one
accepts the reduced image too, through the path it already had, having been
told nothing. Checked end to end on a 512x512 photograph of 31,296 colours:

| | as decoded | after `gimg_ops_quantize()` to 256 |
|---|---|---|
| GIF | `GIMG_ERR_UNSUPPORTED` | 219,351 bytes |
| PNG | 411,523 bytes, truecolor | 179,631 bytes, colour type 3 |
| BMP | 786,486 bytes, 24-bit | 263,214 bytes, 8-bit indexed |

The PNG and BMP rows are the same option doing its existing job:
`png_palette` and `bmp_palette` default to AUTO, which builds a table when
doing so is lossless *and* smaller. Before, a photograph was never either.
Pillow and ImageMagick read all three back as palette images, pixel for pixel
identical to each other.

The alternative - an indexed raster type - would have meant a new pixel format,
a new path through every writer, and a new question at every boundary. The
formats already store indices; nothing in this library needs to.

### An image already within budget is returned untouched

Checked before anything is reduced, so `gimg_ops_quantize()` is safe to call
unconditionally: below the limit it is a copy, the palette it reports is
exactly the one the writer would have built for itself, and no pixel moves.
This is what keeps "reduce then save" from being a lossy thing to do by
default.

### Building and applying are separate

`gimg_ops_palette_build()` takes **several** rasters, because the frames of an
animation must share one table. Quantized independently, a region that does not
change between two frames still gets slightly different colours in each, and
the animation shimmers. `gimg_ops_palette_apply()` then maps each frame onto
the result - and, on its own, maps an image onto a table the caller already
has: a brand palette, a previous frame's, the one a file arrived with.

## What counts as a colour

All four samples, with one exception: **a fully transparent pixel is the same
colour as every other fully transparent pixel**, whatever lies under it. The
three palette writers already agree - what sits under alpha 0 is never shown
and never stored - and without the rule an image with a transparent border
would spend its palette on colours nobody can see.

Alpha is otherwise an ordinary axis: it is split on, averaged over, and counted
in the distance between two colours like any other channel. A pixel is as wrong
for being opaque when it should be clear as for being red when it should be
green. PNG's `tRNS` is the one of the three that can store the result at full
precision; GIF has one transparent index and BMP has none, and each of those
writers narrows it in its own way.

## The algorithm

Median cut (Heckbert, *Color Image Quantization for Frame Buffer Display*,
SIGGRAPH '82). Every colour present starts in one box; the box worth splitting
most is cut at the median of its longest axis; repeat until there are as many
boxes as entries wanted; each box contributes the average of the colours in it,
weighted by how many pixels carry each.

Three choices in that sketch are not forced by it:

- **The colours are kept exactly, not binned.** Quantizers commonly reduce to
  five bits per channel first, to keep the histogram small. That would make the
  common case wrong: an image of 200 colours has one palette that reproduces
  it, and two of those colours landing in one bin would average them and lose
  an image nothing needed to lose. The histogram is an open-addressed index
  over a growing array, which costs about 12 bytes per distinct colour - 1.2 MB
  for a photograph with 100,000 of them.

  That cost is per *call*, so passing every frame of a long animation to
  `gimg_ops_palette_build()` accumulates the colours of all of them at once.
  For the animations this library decodes that is bounded by what the source
  format held - the 358-frame GIF in the corpus uses 248 colours across every
  frame - but an APNG of photographic frames has no such bound, and the answer
  when the histogram will not fit is `GIMG_ERR_OOM` rather than a silent
  approximation.
- **"Worth splitting most" is pixel count times the extent of the longest
  axis.** Pixel count alone - Heckbert's original - spends entries on large
  flat regions that need one colour each. Extent alone spends them on a handful
  of outlying pixels.
- **The median is the colour at which half the box's *pixels* have been
  passed**, not half its colours. A colour carried by one pixel and one carried
  by a million should not pull equally.

Splitting sorts a box by one channel with a **counting sort** over the 256
values a byte has, which is linear where a comparison sort would be n log n.

Mapping a colour to an entry is a linear scan of the table with the squared
distance over all four samples. Without dithering that search runs **once per
distinct colour** rather than once per pixel, which on a photograph is a
hundred thousand searches instead of a quarter of a million, and on a
screenshot a few hundred.

### Dithering

`GIMG_DITHER_FLOYD_STEINBERG` diffuses what the rounding lost into the
neighbours not yet visited - 7/16 ahead, then 3/16, 5/16 and 1/16 into the row
below - scanning alternate rows in alternate directions so the error does not
march to one side and leave a drift down that edge. Two rows of error are held
rather than the whole image, because the weights only ever reach one row ahead.

A fully transparent pixel takes no error and gives none. It has no colour to be
wrong about, and diffusing into it would put its neighbour's colour underneath
something invisible, where the next operation to touch the image would find it
and use it.

Dithering costs a nearest-entry search per pixel, since the error-adjusted
colour is not one the image contains and so cannot be cached.

## How good is it

Beating our own previous self would prove nothing, so the same reductions were
run through two quantizers that are not ours, on nine photographs from the
LibreOffice gallery (65,536 to 262,144 pixels, 17,000 to 121,000 colours) -
images nobody here chose for the purpose.

Two scores, both mean squared error per channel against the original:

- **raw** - the straightforward comparison.
- **blur** - the same after a 3x3 box blur of both. Error diffusion always
  makes `raw` worse, on purpose: it puts the wrong colour in a pixel so that a
  neighbourhood averages to the right one. `blur` is the score that asks
  whether the neighbourhood does average out, which is the thing dithering is
  for.

Lower is better in every column.

| | | here | | Pillow | | ImageMagick | |
|---|---|---|---|---|---|---|---|
| **colours** | **dither** | **raw** | **blur** | **raw** | **blur** | **raw** | **blur** |
| 256 | no | **10.7** | 2.7 | 17.3 | 6.6 | 11.4 | **2.6** |
| 256 | yes | **13.4** | 1.8 | 24.0 | 3.8 | 18.0 | **1.4** |
| 64 | no | **29.8** | 10.0 | 42.0 | 18.1 | 32.3 | **9.7** |
| 64 | yes | **43.8** | 6.5 | 68.7 | 13.6 | 54.2 | **5.4** |
| 16 | no | **94.1** | 43.1 | 125.7 | 68.2 | 97.5 | **41.9** |
| 16 | yes | **150.1** | 29.9 | 217.7 | 60.6 | 172.1 | **27.9** |

Pillow is `Image.quantize(method=MEDIANCUT)`, then applied with and without
`Image.Dither.FLOYDSTEINBERG`; ImageMagick is `magick -colors N` with `-dither
FloydSteinberg` or `+dither`. Both oracles have to be *made* to dither: an
earlier run of this table had `-dither` after `-colors`, where ImageMagick
treats it as a setting for an operator that has already happened, and Pillow's
`quantize(dither=...)` is ignored unless a palette is supplied. Both silently
returned undithered output, which looked like our dithering being uniquely bad
until the two oracles' dithered and undithered numbers turned out to be
identical to each other.

Reading it: **we are better than Pillow's median cut everywhere, by a wide
margin, and level with ImageMagick** - slightly better on `raw`, slightly worse
on `blur`. Being ahead of Pillow is not a boast so much as a confirmation that
the three choices above are the ones worth making; Pillow's median cut splits
on pixel count alone and bins to five bits.

The rows also show the trade dithering makes, in both directions: at 16 colours
it takes `raw` from 94.1 to 150.1 and `blur` from 43.1 to 29.9. Neither
number alone would have told the truth about it.

**Speed**, 512x512 with 98,924 colours, best of three: 0.12 s undithered,
0.14 s dithered, against ImageMagick's 0.14 s and Pillow's 0.26 s. Peak RSS
11.8 MB for the whole program, decode included.

## Limits

- **RGBA8 and GRAY8 only.** Every palette format here stores eight-bit
  entries, so reducing at a greater precision and then discarding the precision
  is not a reduction anyone asked for: a 16-bit raster is refused with
  `GIMG_ERR_UNSUPPORTED` and the caller converts first, where they can see they
  did.
- A **grayscale raster and a colour palette** is refused for the same reason:
  one sample cannot hold three, and keeping a third of each colour would be
  wrong quietly. A palette built from grayscale pixels is all opaque grays, so
  the pairing that arises in practice works.
- **Median cut is the only method.** An octree or a k-means refinement would
  each be a different set of trade-offs; the enumeration has room and the
  measurement above is the bar a second method would have to clear.

## Tested

`tests/unit/test_palette.cpp`. The tests assert the properties a caller can
rely on rather than the quality, which has no threshold that is not arbitrary:
that an image within budget comes back byte for byte, that the result never
holds a colour outside the palette (dithered included, where the value looked
up is not one the source contained), that the count asked for is never
exceeded, that a transparent pixel stays transparent through error diffusion,
and that the refusals above are refusals rather than quiet approximations.
Quality is the table above, and it is a measurement, not an assertion.
