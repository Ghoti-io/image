@page module_geometry Geometry

# Geometry

Three operations that change where a picture's samples are rather than what
they mean: `gimg_ops_crop()`, `gimg_ops_resize()` and `gimg_ops_composite()`,
plus named spellings of the mirrors and quarter turns that
`gimg_ops_apply_orientation()` has always performed.

All of them carry the source's `GCOL_Color_Info`, embedded profile included.
Moving a sample about the picture does not change what it describes.

## What reaches which formats

| Operation | Formats |
|---|---|
| `gimg_ops_crop()` | Any whose pixel is a whole number of bytes: every channel model, every depth, planar or interleaved |
| Mirrors and turns | The same |
| `gimg_ops_composite()`, `GIMG_COMPOSITE_SOURCE` | The same, and both rasters must have the same format |
| `gimg_ops_composite()`, `GIMG_COMPOSITE_OVER` | RGBA at 8 or 16 bits |
| `gimg_ops_resize()` | GRAY, RGBA, CMYK and `GIMG_CHANNEL_UNKNOWN`, interleaved, 8/12/16 bits, every channel the same width |
| `gimg_ops_resize()` in linear light | GRAY and RGBA only |

The resampler's matrix is deliberately the one `gimg_ops_convert_bit_depth()`
already declares, so there is one rule to learn rather than two. Cropping
reaches further because it never interprets a sample: it can move a pixel it
could not average.

**`GIMG_CHANNEL_INDEXED` is refused by the resampler**, and this is the one
refusal worth spelling out. The average of two palette indices is not a
palette index - index 3 and index 7 average to index 5, which names an
unrelated colour. Resize the colour form and call `gimg_ops_quantize()`
afterwards, which is where the decision about which colours to keep already
lives (\ref module_palette "Palettes and colour reduction").

## Resizing

### The filters are one algorithm, not five

There is a single separable resampler - a horizontal pass, then a vertical one
- parameterized by a kernel and a support radius. `GIMG_FILTER_BOX`,
`TRIANGLE`, `CATMULL_ROM` and `LANCZOS3` are rows in that table.
`GIMG_FILTER_NEAREST` is a path of its own, because its purpose is to return a
sample that was in the source and averaging would defeat it.

| Filter | Kernel | Support |
|---|---|---|
| `NEAREST` | the sample the destination's centre falls on | - |
| `BOX` | 1 on (-0.5, 0.5] | 0.5 |
| `TRIANGLE` | 1 - abs(t) | 1.0 |
| `CATMULL_ROM` | the a = -0.5 cubic | 2.0 |
| `LANCZOS3` | sinc(t) sinc(t/3) | 3.0 |

### The choice is semantic, not aesthetic

This is why the library does not simply pick one. `NEAREST` returns a value
that was in the source; every other filter returns a weighted average, which
is a value that was not. For a mask, an index map, or anything whose samples
are **labels rather than colours**, the average of two of them is not a label
and only `NEAREST` is correct. For a photograph being reduced, `NEAREST` drops
pixels and aliases, and only an average is correct. Nothing about a raster
distinguishes the two cases.

### Reducing stretches the kernel

When an axis is reduced, every filter but `NEAREST` is stretched by the
reduction ratio, so that it averages the whole source region feeding one
destination pixel. Without that a reduction is a decimation: the kernel keeps
its original radius, reads one or two source samples out of however many it
should have covered, and aliases.

The measurable form, which is in the test suite: reduce incompressible noise
by eight, and a true average takes its standard deviation from about 74 to
about 9. A filter that samples instead leaves it near 74.

### `GIMG_FILTER_AUTO`

`AUTO` is the zero value, so `GIMG_Resize_Options options = {0};` means "pick
well for me" rather than selecting a filter by accident. It maps to
`CATMULL_ROM`.

**It does not switch on scale direction.** The obvious rule - box below 1:1, a
cubic above - is wrong, and measurably so. `BOX` has a support of half a pixel,
and stretched by a ratio near 1 it still covers a single sample: reducing noise
from 256 to 250, `BOX` scores 73.0 where `NEAREST` scores 73.9 and a cubic
scores 58.6. It aliases at exactly the ratios where a switching rule would
choose it. A stretched cubic low-passes correctly going down and interpolates
well going up, so one kernel serves both directions and there is no crossover
to be discontinuous at - nor any question of what to do when an image is
enlarged in one axis and reduced in the other.

`BOX` remains worth asking for by name at a real reduction, where it is the
exact area average: reducing by eight it scores 8.91 against the theoretical
9.2, closer than any other filter here.

**`AUTO` is a pinned alias, not a judgement free to drift.** This library
promises the same bytes for the same input and options; an `AUTO` that quietly
changed kernel between releases would break that where only a comparison
against an old output would find it. Changing what it maps to is a behaviour
change and is treated as one - there is a test asserting the two are
byte-identical.

**`AUTO` never selects `NEAREST`.** Deciding from the picture's contents that
its samples are labels would be a judgement about the image, which is the line
that keeps `gimg_ops_quantize()` outside the codecs.

### Alpha

RGBA is filtered **premultiplied**. Averaging straight alpha lets a
transparent pixel contribute its colour to opaque neighbours, so every edge
against transparency picks up a halo of whatever was hiding underneath -
usually black, because that is what a cleared buffer holds. The source is not
modified; the premultiplication happens on the way into the filter and is
undone on the way out.

### Colour space

`GIMG_RESAMPLE_SPACE_ENCODED`, the default, averages the stored values. It is
what Pillow, ImageMagick's default, GdkPixbuf and browsers do, and therefore
the only setting under which this library's output can be compared with
theirs.

`GIMG_RESAMPLE_SPACE_LINEAR` linearizes through sRGB's transfer function,
averages, and re-encodes. It is more nearly correct: reduce a black-and-white
checkerboard to one pixel and half the light is present, which sRGB encodes
near 188, where averaging the encoded values gives 128 - the encoding of about
21% of the light.

It is **opt-in precisely because the library cannot know the transfer
function**, and this is the important part of the contract: *a caller passing
`LINEAR` is asserting sRGB. The library is not inferring it.*
`GCOL_Color_Info` is not consulted to guess one, for the same reason the CMYK
conversion refuses to guess a polarity - a plausible wrong answer is worse than
a refusal, and the code that knows where the image came from is the code that
should decide.

Three consequences:

- **CMYK and unnamed channels are refused** in linear light. Ink amounts are
  not light. They still resize in the encoded space.
- **Alpha is not transferred.** It is a coverage fraction, not a light level;
  the curve would report a half-covered pixel as about three quarters covered.
- **The work happens at 16 bits** whatever the source width, because sRGB
  spends a quarter of its range on the bottom two percent of the light and
  rounding straight back to 8 bits throws most of that away.
- **`NEAREST` is unaffected**, since it averages nothing.

### Precision

Coefficients are quantized to fixed point and accumulated in integers, not
floats. A float accumulator gives different answers under FMA contraction and
at different optimization levels, and a byte-exact comparison that only holds
on one build is not a check.

## Compositing

`gimg_ops_composite()` draws a source onto a destination in place, at a
**signed** offset, clipped. A source lying entirely outside draws nothing and
returns `GIMG_OK`: a frame scrolled off the canvas is an ordinary thing for an
animation to do, not an error.

Alpha is straight on the way in and on the way out. Where the composite is
wholly transparent the colour channels are set to zero rather than left
holding the destination's, so that compositing the same source over two
different destinations gives the same bytes wherever nothing is visible.

`GIMG_COMPOSITE_OVER` is refused for a format with no alpha. Such a format has
not said what covers what, and nothing would distinguish the result from
`GIMG_COMPOSITE_SOURCE`.

## Mirrors and quarter turns

`gimg_ops_flip_horizontal()`, `gimg_ops_flip_vertical()`,
`gimg_ops_rotate_90_cw()`, `gimg_ops_rotate_90_ccw()` and
`gimg_ops_rotate_180()` are names for five of the eight CIPA DC-008 Table 6
orientations. Each forwards to `gimg_ops_apply_orientation()`, which has
implemented all eight all along; they exist because
`GIMG_ORIENTATION_TRANSVERSE` is not what anybody searches for when they want
to turn a picture. There is one implementation, not two.

The four transforms that exchange the axes rebuild the buffer, so a raster's
dimensions and stride change while the caller's pointer stays valid.

## Where this differs from other resamplers

One case, and only one. At a destination pixel whose centre falls **exactly**
on a source pixel boundary, `NEAREST` here takes the pixel to the right - what
the half-open interval [i, i+1) says, and what ImageMagick's Point filter
does. Pillow advances its source coordinate by repeated addition, so it
arrives a fraction below the boundary and takes the pixel to the left.

Across every pair of sizes from 1 to 59, that is the only case in which the
two disagree: 724 differing destination pixels, every one of them at an exact
integer centre and none anywhere else.

## Tested scope

- **Against Pillow 11.1.0**, byte for byte:
  `tests/data/verify_resample.py`, run by `make test` and on its own by
  `make test-verify-resample`. 486 of 490 resamplings are byte-identical; the
  four are the `NEAREST` tie above, and the script asserts that every
  disagreement sits on an exact tie rather than tolerating a count of them.
  Pillow is required - an absent oracle fails the run rather than skipping it.
  The comparison is on grayscale and on **opaque** RGBA, where premultiplied
  and straight filtering are identical; the premultiplication itself is held by
  the unit tests instead.
- **Properties, needing no oracle**, in `tests/unit/test_resample.cpp` and
  `tests/unit/test_geometry.cpp`: a constant image stays constant at every
  ratio and filter; resizing to the same size is the identity; an integer
  enlargement with `NEAREST` replicates exactly; halving with `BOX` is the mean
  of each quad; a reduction measurably low-passes, at mild ratios as well as
  large; `AUTO` is byte-identical to the filter it aliases; opaque stays
  opaque; transparency does not tint its neighbours; a translucent colour
  survives the premultiply round trip; linear light is exactly reversible;
  alpha is left alone by the transfer.
- **The division of labour is real.** Mutating the Catmull-Rom parameter, the
  Lanczos window, the coefficient rounding or the precision passes every
  property and is caught only by Pillow. Dropping the kernel normalization,
  the support stretch, the half-pixel centre or the premultiplication passes
  the comparison on opaque images and is caught only by the properties.

### Gaps in the testing

- **16-bit has the weaker oracle.** Pillow's 16-bit resize support is thin, so
  the comparison runs at 8 bits; the 16-bit path is held by the properties
  alone.
- **Nothing compares output quality against another resampler.** Byte
  equality with Pillow settles the four filters that match it exactly and says
  nothing about whether those are the right filters to have chosen.

## Not implemented

- **Resampling to square pixels.** `gimg_ops_resize()` is now the tool for it,
  but no codec applies a pixel aspect ratio on the caller's behalf - GIF's
  ratio is still read, reported and written without being acted on. Applying
  it is a decision about the picture; see \ref format_gif "GIF".
- **Arbitrary-angle rotation.** Only the quarter turns, which need no
  resampling. A general rotation needs a decision about what fills the corners
  and what happens to the bounding box, and neither has an obvious answer.
- **A colour-managed resize.** `LINEAR` assumes sRGB because the caller says
  so. Running samples through an embedded ICC profile would need a colour
  engine, which this library does not have.

---

Back to \ref image_modules "Modules".
