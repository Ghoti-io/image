@page format_gif GIF

# GIF

What this codec implements of the Graphics Interchange Format, where it
deliberately differs from other decoders, and how each claim was checked. The
library's format index is the
\ref format_references "format and specification references".

GIF is one of the few formats here with a single, short, freely available
specification that says what it means. Nearly everything below cites a section
of it. The exceptions are named: the animation loop count is a convention
CompuServe never wrote down, and what a decoder shows for a pixel it has
declared transparent is left open.

## Normative references

- **Specification:** [GIF89a](https://www.w3.org/Graphics/GIF/spec-gif89a.txt),
  CompuServe, 31 July 1990. Cited below as "89a" and a section number.
- **Predecessor:** [GIF87a](https://www.w3.org/Graphics/GIF/spec-gif87.txt),
  CompuServe, 15 June 1987. The header is read and recorded; nothing branches
  on it, because files in the wild carry either version regardless of which
  blocks they use.
- **LZW:** the variable-width, LSB-first variant 89a 22 describes. It is
  implemented in the `compress` library as that library's `gif` LZW profile,
  not here.

Codec-owned allocations use `codec->allocator`, as
`documentation/development.md` requires; the decoded raster and the document
come from `doc->allocator`.

## Parts implemented

- **Header** (89a 17) and **Logical Screen Descriptor** (89a 18): canvas size,
  the Global Color Table flag and size, the background colour index, and the
  pixel aspect ratio. The colour resolution and sort flags are read past; no
  decoder acts on either.
- **Global and Local Color Tables** (89a 18, 20), 2 to 256 entries. A frame is
  resolved at load to the table it is read through, so decode never reaches
  back to the screen descriptor. A file with **no** Global Color Table is
  legal and is read.
- **Image Descriptor** (89a 20): position on the logical screen, size, the
  Local Color Table flag and size, and the interlace flag.
- **Interlacing** (89a 20): the four-pass row order, undone at decode so that
  nothing downstream knows the frame was interlaced.
- **LZW image data** (89a 22) at every minimum code size the format allows,
  2 through 8.
- **Graphic Control Extension** (89a 23): delay, disposal method, and the
  transparency flag with its colour index.
- **Application Extension** (89a 26): the NETSCAPE2.0 and ANIMEXTS1.0 loop
  counts are read into the document state.
- **Comment** (89a 24) and **Plain Text** (89a 25) extensions are walked past
  by their sub-block chains rather than parsed.
- **Trailer** (89a 27).
- **Data Sub-blocks** (89a 15) everywhere they appear. A chain is joined
  before it is interpreted, because an LZW code may straddle the boundary
  between two sub-blocks.

### Frames are patches, not pictures

A GIF image block is a rectangle at a position on a logical screen, and what a
viewer sees at frame N depends on the frames before it and on how each was
disposed of (89a 23). `gimg_item_decode()` therefore returns the **whole
canvas as it stands after frame N**, having replayed frames 0 through N - the
same arrangement the APNG path uses, and the reason the container model needed
nothing new for GIF: items, frame delay, dispose and blend were already there.

Replaying from the start makes decoding frame N cost N frame expansions, so
walking a whole animation is quadratic in its length. Measured: a 36-frame
1200x1200 animation decodes all its frames in 1.9 s; a 358-frame one of the
same size takes minutes. Nothing caches the canvas between calls, so a caller
walking an animation in order pays the full replay every frame. See
"Not implemented" for what fixing it would take.

## Save

The encoder writes GIF89a, one image block per frame, **each the full size of
the canvas**, with no Global Color Table - every frame carries its own, which
89a 18 permits and which costs less than a global table that suits neither of
two frames.

- **A palette is built, never chosen.** An image of 256 colours or fewer has
  exactly one palette that reproduces it, up to the order of its entries, and
  building that table stores what is already there. An image with more is
  refused with `GIMG_ERR_UNSUPPORTED` rather than quantized, because deciding
  which colours to discard is an image-processing decision and not a codec's.
  The PNG writer in this library draws the line in the same place.
- **Transparency is the same argument one bit down.** GIF designates a single
  index transparent and treats every other pixel as opaque (89a 23); a
  half-covered edge has nowhere to go. A partially transparent pixel is
  refused unless `gif_alpha_threshold` says which way to round. Fully opaque
  and fully transparent pixels need no option.
- **Index 0 is the transparent one** when a frame has transparency, so that a
  decoder ignoring the control block shows the background rather than a stray
  colour. That costs one of the 256 entries.
- `gif_interlace` writes the four-pass order; it changes no pixel.
- `gif_loop_count` writes the NETSCAPE2.0 Application Extension, and is
  written only for a document of more than one frame. 0 means forever.
- A document whose frames differ in size is refused: this writer produces
  full-canvas frames, so a differing size asks a placement question the caller
  has not been asked.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|
| Header | 87a and 89a, read identically | Anything not beginning `GIF` &rarr; `GIMG_ERR_FORMAT` |
| Logical Screen Descriptor | Size, background index, aspect ratio, Global Color Table | Truncated &rarr; `GIMG_ERR_FORMAT` |
| Colour tables | Global, local, 2–256 entries, absent | Truncated &rarr; `GIMG_ERR_FORMAT` |
| LZW | Minimum code size 2–8 | Outside that &rarr; `GIMG_ERR_CORRUPT`; a stream yielding fewer pixels than the descriptor promises &rarr; `GIMG_ERR_CORRUPT` |
| Interlace | Four-pass, decoded and written | — |
| Image geometry | Any position and size within a 65535 canvas | Zero width or height &rarr; `GIMG_ERR_CORRUPT` |
| Graphic Control Extension | Delay, disposal 0–3, transparent index | A block length other than 4, or an unterminated one &rarr; `GIMG_ERR_CORRUPT` |
| Application Extension | NETSCAPE2.0 and ANIMEXTS1.0 loop counts | Others walked past |
| Comment, Plain Text | Walked past | Never rendered — see below |
| Trailer | Read; a file that ends without one keeps the frames already read | — |
| Frame count | `max_frame_count` enforced at load | Exceeded &rarr; `GIMG_ERR_LIMIT` |
| Sub-block chains | Joined before interpreting | `max_chunk_size` exceeded &rarr; `GIMG_ERR_LIMIT` |
| Write | 1–256 colours, transparency, interlace, animation with delays and loop | More than 256 colours, or partial alpha with no threshold &rarr; `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from the reference implementations

| Case | This codec | Elsewhere |
|---|---|---|
| An index the colour table does not have | Draws nothing; the pixel keeps what the canvas already held | giflib does the same. Pillow paints it opaque black. Pinned by `gif_8x2_index_past_palette.gif`, where a four-entry table is addressed with index 7 — expressible because the code size is set independently of the table size |
| The colour under a fully transparent pixel | Zero in all three channels | giflib the same; Pillow writes the palette colour. Invisible by definition, and the reason `verify_gif_output.py` does not compare those channels |
| The logical screen before any frame is drawn | Transparent | 89a 18 names a background colour index, and the viewers every real file was authored against ignore it. Filling it would put a colour on screen that no other decoder shows. The index is kept in the document state for a caller that wants it |
| Plain Text extension | Walked past, never rendered | No decoder in use renders it either. A file using it looks the same here as everywhere |
| A file that ends without a trailer | Keeps the frames already read | Common enough in the wild that the frames are worth more than the refusal. A file truncated **inside** a frame is still refused |
| What "restore to background" leaves behind (disposal 2, 89a 23) | Transparent | ImageMagick's `-coalesce` agrees. Pillow gives an opaque pixel instead - sometimes the background colour, sometimes the previous frame showing through. Adding a Global Color Table, the obvious suspect, changes nothing. Pinned by `tests/out/gif/animation_6x3.gif`, whose second frame is transparent exactly where the first was opaque |

## Tested scope

- **Fixtures**: eleven files from `tests/data/gif/generate.py`. Pillow writes
  the two ordinary ones; the rest are assembled byte by byte, because the
  cases a decoder gets wrong are the ones common writers never produce - no
  Global Color Table, an index past the end of the palette, extensions that
  must be walked rather than parsed, an 87a header, disposal 3, and a file
  that stops inside its code stream.

  The byte-assembled fixtures carry an **LZW encoder written out in
  `generate.py`**, rather than taken from a library, so that a fixture does
  not depend on the code it checks. giflib decodes all of them exactly, at
  every minimum code size from 2 to 8.

- **Three oracles, each asked only what it is good for.**
  **giflib** is authoritative for LZW expansion, the interlace order, the
  colour tables and the per-image geometry. It is **not** an oracle for
  compositing: it hands back each image as stored and leaves disposal,
  transparency-over-canvas and the logical screen to the caller, so there is
  no giflib answer to "what does frame 3 look like". The `--composite` mode of
  `tests/tools/gif-oracle/dump_gif_pixels_giflib.c` implements that from the
  specification, which makes it a second reading of the same text rather than
  independent evidence, and it says so in its own comment.

  **ImageMagick** (`magick -coalesce`) has its own GIF reader rather than
  linking giflib, so it is the independent opinion about compositing - which
  is the half giflib cannot supply.

  **Pillow** composites too, but disagrees about disposal 2 (see the
  deviations table), so `verify_gif_output.py` asks it about the first frame
  only, where it checks the palette, the LZW and the transparent index with no
  disposal involved. That the encoder's animation output needed a third
  decoder to settle is the clearest argument here for not stopping at two.

- **A corpus nobody here wrote.** The 121 GIFs installed on this Debian
  machine - Tk, LibreOffice, OpenCascade, CUPS and others, written by unknown
  encoders over about thirty years. All 121 decode. Against giflib there are
  **no disagreements at all**. Against Pillow, 128 frames differ and every one
  of those differences is in the colour beneath a fully transparent pixel:
  checked pixel by pixel, **not one differs in a pixel either side calls
  visible**.

- **Encoder output is read back by decoders that are not ours.** The encode
  tests leave each file in `tests/out/gif/` with one sidecar per frame holding
  the pixels that frame was meant to show, and `verify_gif_output.py` decodes
  them with giflib, ImageMagick, Pillow and GdkPixbuf; `make test` fails when
  they disagree. Our decoder agreeing with our encoder proves nothing about
  either - and it was exactly this check that caught the encoder writing
  animation frames with disposal left unspecified, which shows the frame
  before through any pixel meant to be transparent. Frame 0 is correct either
  way, so a single-frame test would not have found it.

- **What the corpus taught that the fixtures could not.** Pillow writes every
  GIF at LZW minimum code size 8, whatever the palette holds. A corpus
  generated with Pillow and read back with Pillow therefore never asks what
  happens at any other width. Of the 121 real files, 71 hold at least one
  image below width 8 - and all 71 failed, against none of the width-8 files,
  because the compression library underneath returned a hardcoded clear code,
  end-of-information code and opening code width that are correct only at
  eight-bit literals. That is fixed in `compress`; the lesson is that a
  generator's constants are as much a part of a corpus as its variety.

- **Fuzzing.** `make fuzz-gif` builds a libFuzzer harness over load and decode
  with ASan and UBSan. Seeded with the fixtures and the encoder's own output,
  2.1 million executions found nothing in GIF.

  It did find something in BMP. The magic probe hands any input to whichever
  codec claims it, so the GIF harness reached the BMP loader, and a file whose
  OS/2 `BA` array contains another `BA` recursed until the stack ran out - not
  a result this library can return. The entry cap and the strictly-advancing
  `offNext` check both bound the breadth of one level and neither bounds the
  depth. Nesting is now refused; the minimised input is kept verbatim in
  `tests/codec/bmp/test_bmp_decode.cpp`. Worth recording as the argument for
  pointing a new codec's fuzzer at the whole registry rather than one format.

### Gaps in the testing

- The encoder's animation output is checked frame by frame against outside
  decoders, but no outside decoder is asked whether the **timing** is right,
  because none of them reports it in a form worth comparing.
- Nothing measures output size against another encoder. The writer is not
  trying to be small (see below), so the number would record a choice rather
  than a defect - but it is unmeasured either way.

## Not implemented

- **Colour quantization.** An image of more than 256 colours cannot be
  written. Closing this means choosing a quantizer - median cut, octree - and
  a dithering policy, which is an image-processing feature that belongs beside
  the other operations rather than inside a codec. The `ops` module is where
  it would go, and then GIF save would need no change at all.
- **Frame optimization.** Every frame is written at full canvas size, with
  disposal 2 so that each starts from an empty screen. An optimizing encoder
  writes each frame as the smallest rectangle that changed and marks unchanged
  pixels transparent.

  What that costs here was measured by re-encoding real animations. Three
  36-frame files came back at 1.0 to 1.1 times their original size, because
  their frames genuinely differ. One came back **eight times** larger - 1.3 MB
  to 10.7 MB - and it is worth saying why: 357 of its 358 frames are 1x1
  pixels, a way of padding a still image's duration, and each of those no-ops
  is written here as a full 1200x1200 frame. The inflation is not uniform; it
  is proportional to how little each frame actually changes.

  Closing it means a per-frame difference against the previous canvas and a
  choice of disposal per frame. The decoder already reads everything such a
  file would contain.

- **Nothing caches the composited canvas between decodes.** Decoding frames
  0..N in order replays 0..i for each i. Keeping the canvas after each frame,
  together with the disposal that follows it, would make a forward walk linear
  instead of quadratic. It is left out for now because the decode callback
  takes the document as const and two threads decoding one document would race
  on such a cache - so it is a question about the library's threading contract
  rather than about GIF.
- **Plain Text rendering** (89a 25). The block is walked past. No decoder in
  use renders it, and doing so would mean shipping a bitmap font.
- **The loop count is not exposed.** It is read into the document state and
  written from `gif_loop_count`, but there is no public accessor for a caller
  to ask what a loaded file said. The item model has nowhere for a
  document-level property to live yet.
- **The background colour index is not applied.** It is read and kept; the
  canvas starts transparent. See the deviations table for why.

---

Back to the \ref format_references "format and specification references".
