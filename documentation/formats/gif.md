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
  the Global Color Table flag and size, the background colour index - reported
  as a colour by `gimg_doc_background_color()`, resolved through the table -
  and the pixel aspect ratio, reported by `gimg_doc_pixel_aspect_ratio()` as
  the (N + 15) / 64 that 89a 18 defines. The colour resolution and sort flags
  are read past; no decoder acts on either. The three version bytes are read
  to consume them and to refuse a header that stops inside them, and then
  discarded: a file saying "89a" may use nothing 89a added and one saying
  "87a" is read identically, so there is no decision the spelling informs.
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
  counts, reported through `gimg_doc_loop_count()`.
- **Comment Extension** (89a 24): every one a file carries, kept rather than
  walked past. See "Comments" below.
- **Plain Text** (89a 25) is walked past by its sub-block chain rather than
  parsed.
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

### Comments

A Comment Extension (89a 24) is the only place a GIF has to put text, so it is
read, written, and editable rather than discarded. A file may carry **any
number** of them, at any point between blocks, and all of them are kept.

They arrive two ways, because one of them is lossy on purpose:

- **Every comment, verbatim and in stream order**, in the document's raw
  metadata under the format id `"gif"` and `tag_or_chunk_id` `0xFE` — the
  extension's own label. Within that block each comment is framed as a
  **four-byte big-endian length followed by that many bytes**. Four rather
  than the two the JPEG codec uses for the same job, because a JPEG COM
  segment cannot exceed 65535 bytes and a GIF comment can: its sub-block chain
  declares no total and is bounded only by `max_chunk_size`.
- **The first one that reads as text**, in `gimg_meta_common_description()`,
  which is the field a caller reads without knowing what format it loaded.
  "Reads as text" is the same test the JPEG codec applies to a COM segment:
  printable ASCII plus tab, newline and carriage return, stopping at the first
  NUL. A comment that is not text stays in the raw block and is not presented
  as a description.

Create, update and delete are `gimg_meta_common_set_description()` and the
save-time `GIMG_Meta_Policy`, with no GIF-specific API:

| | |
|---|---|
| Read | load fills both of the above |
| Create / update | set the description; save writes a Comment Extension |
| Delete one | set the description to `NULL` |
| Delete all | `GIMG_META_DROP_ALL` |
| Keep only what was loaded | `GIMG_META_KEEP_RAW_ONLY` |
| Keep only what you set | `GIMG_META_KEEP_COMMON_ONLY` |

The raw block wins over the description when both are present, because it is
the more specific statement — it names every comment rather than one.
`GIMG_META_KEEP_COMMON_ONLY` is how a caller says the description is the one
they mean. Comments are written **before the first image**: 89a places no
constraint, but that is where every writer puts them and where a reader
looking for a file-level comment looks.

**Non-ASCII text is written as given, and this is a deliberate deviation.**
89a 24 calls a comment 7-bit ASCII; `gimg_meta_common_description()` is UTF-8.
Refusing non-ASCII would make the field useless for most of the world's text,
and transliterating would corrupt it quietly, so the bytes go out as they came
in — which is what every GIF writer in use does, and what every reader in use
copes with. A reader that assumes ASCII will see the encoded bytes. This is
stated on `gimg_meta_common_set_description()` too, so a caller meets it
before writing rather than after.

### How many times to play

`gimg_doc_loop_count()` reports what the NETSCAPE2.0 Application Extension
said (89a 26). It is a document-level property rather than a per-item one,
because that is where the format puts it, and APNG's `acTL` `num_plays` is
reported through the same accessor - a caller animating either format asks one
question.

It answers three things, not two. A count of **zero means forever**, which is
the format's own convention and not a stand-in for "absent"; a file carrying
no NETSCAPE2.0 block at all declares **nothing**, and the accessor says so by
returning 0 rather than inventing a number. Every browser plays such a file
once, but that is a viewer's policy, and a library that quietly applied it
would leave the caller unable to tell a policy from a reading.

The count is **not** applied on save automatically: `gif_loop_count` is a save
option whose 0 already means forever, so it has no way to say "unset" and
cannot fall back to the document without changing what an existing caller's 0
means. A caller that wants a round trip to preserve the count reads it and
passes it, which is two lines and is visible in the code rather than implied
by it. `gimg_doc_copy()` does carry it, so "load, copy, edit, save" keeps it
as long as the save option is set from it.

### The canvas cache

Replaying from the start would make decoding frame N cost N frame expansions,
so walking a whole animation would be quadratic in its length. The decoder
keeps the canvas it arrived at, together with the index of the frame that
canvas is the input to, and a decode that asks for a frame at or after that
index starts from it instead of from an empty screen. A forward walk - the
order every player uses - is then linear.

Measured on the largest animations installed on this machine, forward against
the same walk taken backwards, where the cache is always ahead of what is
asked for and so every frame replays from the beginning:

| Frames | Size | Cached | Full replay |
|---|---|---|---|
| 39 | 1200x1200 | 0.22 s | 2.64 s |
| 50 | 1200x1200 | 0.28 s | 4.14 s |
| 60 | 831x779 | 0.31 s | 4.88 s |
| 358 | 1200x1200 | 1.09 s | 49.91 s |

The last row gains most because 357 of that file's 358 frames are 1x1 pixels
padding a still image's duration: each replay of them is nearly free, and what
the replay actually costs is clearing and compositing a 1200x1200 canvas
sixty-four thousand times.

Beating our own previous self says nothing about whether the result is fast,
so the same walk was timed against two decoders that are not ours, each
decoding every frame to RGBA. Best of several runs; fixed startup is 26 ms for
Pillow and about 3 ms for the other two, so it is not hiding in the margins.

| Frames | Size | Here | ImageMagick | Pillow |
|---|---|---|---|---|
| 39 | 1200x1200 | 0.18 s | 0.69 s | 0.33 s |
| 50 | 1200x1200 | 0.24 s | 0.86 s | 0.44 s |
| 60 | 831x779 | 0.27 s | 1.03 s | 0.51 s |
| 358 | 1200x1200 | 0.96 s | *refuses* | 2.15 s |

ImageMagick does not lose the last row, it declines it: `-coalesce`
materializes all 358 composited frames at once, about 2 GB, and stops with
"cache resources exhausted". Decoding one frame at a time is why that does not
arise here - **peak RSS walking every frame is 28 MB for the 358-frame file
and 28 MB for the 39-frame one**, flat in frame count, because what is live is
one canvas and one cache no matter how long the animation is.

Where the remaining time goes was profiled rather than guessed, because the
guess was wrong - the cache's own copying is not the dominant cost on an
ordinary animation:

| | 39 frames | 358 frames |
|---|---|---|
| LZW expansion (in `compress`) | 42% | 36% |
| Compositing | 17% | 12% |
| `memmove`, which is the cache's copies | 11% | 30% |
| `memset` | 4% | 5% |

So what a normal animation spends its time on is decoding, which is the work
that cannot be removed. The cache's copies only dominate on the pathological
file, where there is almost no LZW to do. Updating the cache in place - on a
forward step it holds exactly the pre-frame canvas, so replaying the paint
into it would be bounded by the frame rectangle rather than the canvas, and
disposal 3 would become a no-op - would buy that `memmove` share and no more.
It is left alone deliberately: a single-digit percentage is not worth the
re-check under the lock that it would need.

Three things are worth stating about it, because a cache is only ever as good
as its guarantees:

- **The answer does not depend on it.** The cache is populated only from a
  prefix that decoded successfully, and replay is deterministic, so the pixels
  are those a decoder with no cache would produce - including the results for
  corrupt files, where the frame that fails is the same one either way. That
  is checked rather than argued: see "Tested scope".
- **What is cached is the canvas *after* the frame's disposal**, which is not
  the canvas returned to the caller. The picture a frame describes is what it
  painted; the next frame starts from what disposal left behind (89a 23). The
  two differ for disposal 2 and 3, so the cache holds a copy that has been
  disposed of rather than the raster itself.
- **It is the one thing decode writes through a `const` document**, and the
  mutex in `gimg_gif_doc_state_t` is what makes that safe. A single-image GIF
  is left out: there is no later frame to hand a head start to, so the cache
  would be a canvas-sized allocation nothing would ever read. The cost for an
  animation is one extra canvas held for the life of the document.

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
| Logical Screen Descriptor | Size and Global Color Table; the background index resolved to a colour and the Pixel Aspect Ratio byte resolved to a ratio, both reported but neither applied | Truncated &rarr; `GIMG_ERR_FORMAT`. The colour resolution and sort flags are read past |
| Colour tables | Global, local, 2–256 entries, absent | Truncated &rarr; `GIMG_ERR_FORMAT` |
| LZW | Minimum code size 2–8 | Outside that &rarr; `GIMG_ERR_CORRUPT`; a stream yielding fewer pixels than the descriptor promises &rarr; `GIMG_ERR_CORRUPT` |
| Interlace | Four-pass, decoded and written | — |
| Image geometry | Any position and size within a 65535 canvas | Zero width or height &rarr; `GIMG_ERR_CORRUPT` |
| Graphic Control Extension | Delay, disposal 0–3, transparent index | A block length other than 4, or an unterminated one &rarr; `GIMG_ERR_CORRUPT` |
| Application Extension | NETSCAPE2.0 and ANIMEXTS1.0 loop counts | Others walked past |
| Comment Extension | All of them read and written; the first normalized into the common description | Text that is not 7-bit ASCII is kept raw but not normalized |
| Plain Text | Walked past | Never rendered — see below |
| Trailer | Read; a file that ends without one keeps the frames already read | — |
| Frame count | `max_frame_count` enforced at load | Exceeded &rarr; `GIMG_ERR_LIMIT` |
| Sub-block chains | Joined before interpreting | `max_chunk_size` exceeded &rarr; `GIMG_ERR_LIMIT` |
| Write | 1–256 colours, transparency, interlace, animation with delays and loop | More than 256 colours, or partial alpha with no threshold &rarr; `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from the reference implementations

| Case | This codec | Elsewhere |
|---|---|---|
| An index the colour table does not have | Draws nothing; the pixel keeps what the canvas already held | giflib does the same. Pillow paints it opaque black. Pinned by `gif_8x2_index_past_palette.gif`, where a four-entry table is addressed with index 7 — expressible because the code size is set independently of the table size |
| The colour under a fully transparent pixel | Zero in all three channels | giflib the same; Pillow writes the palette colour. Invisible by definition, and the reason `verify_gif_output.py` does not compare those channels |
| The logical screen before any frame is drawn | Transparent | 89a 18 names a background colour index, and the viewers every real file was authored against ignore it. Filling it would put a colour on screen that no other decoder shows. The colour that index names is reported by `gimg_doc_background_color()`, resolved through the Global Color Table, so a caller who wants to honour it can |
| Plain Text extension | Walked past, never rendered | No decoder in use renders it either. A file using it looks the same here as everywhere |
| A file that ends without a trailer | Keeps the frames already read | Common enough in the wild that the frames are worth more than the refusal. A file truncated **inside** a frame is still refused |
| What "restore to background" leaves behind (disposal 2, 89a 23) | Transparent | ImageMagick's `-coalesce` agrees. Pillow gives an opaque pixel instead - sometimes the background colour, sometimes the previous frame showing through. Adding a Global Color Table, the obvious suspect, changes nothing. Pinned by `tests/out/gif/animation_6x3.gif`, whose second frame is transparent exactly where the first was opaque |

## Tested scope

- **Fixtures**: twelve files from `tests/data/gif/generate.py`. Pillow writes
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

- **The canvas cache is checked against a decoder that has none.** The cache
  is an optimization, so it has exactly one thing to prove: that the answer is
  the same without it. Every frame of every corpus file is decoded three ways
  - forward through one document, where the cache is warm; backward through
  one document, where it is always ahead and so never usable; and one
  freshly-loaded document per frame, which is a decoder with no cache at all -
  and the three must agree. **1233 frames across all 121 corpus files, byte
  for byte identical in all three orders.** That includes the 358-frame
  1200x1200 file, which was previously only spot-checked because walking it
  was too slow to do in a harness - the cache is what made checking it whole
  affordable.

  `gif_10x6_disposal_cycle.gif` is the fixture written for this: seven frames
  using every disposal method over overlapping patches, with transparency
  under each, so that a cache which stored the wrong canvas shows up as a
  pixel rather than as nothing. ImageMagick agrees with our compositing of all
  seven. The tests in `test_gif_decode.cpp` fix the forward, backward and
  scattered orders against a cold decode of the same frame, rather than
  against pixels written out by hand, so they keep checking the invariant even
  if the fixture changes.

  Deliberate breakage confirms they bite: caching the canvas before its
  disposal is applied, and seeding from a cache one frame too far along, are
  both caught. Labelling the cache with `index` instead of `index + 1` is
  **not** caught - and that turns out to be right, because it is an equivalent
  mutation: repainting the last cached frame and re-applying its disposal
  lands on the same canvas for all four disposal methods. Diffing its output
  over 131 files found no difference at all, so it costs a frame of work per
  decode and changes nothing. That was settled by running the corpus, not by
  the argument just given.

- **The lock is shown to be load-bearing.** Four threads decoding one document
  at once is a test, and under ThreadSanitizer the committed code reports no
  race while the same code with the mutex removed reports one immediately, in
  `gif_cache_store` and in the seeding read. ASan alone does not catch it: the
  window is a 240-byte `memcpy` and it simply never lost. There is no TSan
  target in this repository - the check was a one-off build - which is worth
  knowing when judging what `make test-asan` passing does and does not cover.

- **Real files carry them.** Nine of the 121 corpus GIFs hold a Comment
  Extension, and what they hold is what the field is for in practice - writer
  credits: "GifBuilder 0.3.2 by Yves Piguet", "Made with GIMP" on three, and
  " -dl-" on five. All nine are plain ASCII, so all nine normalize into the
  description as well as being kept raw. That the feature was being discarded
  was not theoretical.

- **Comments are read back by decoders that are not ours too.** The encode
  tests publish `commented_16x8.gif` with a `.expected.comment` sidecar, and
  `verify_gif_output.py` asks Pillow and ImageMagick to read the comment out of
  it; `make test` fails when neither finds it or either disagrees.

  It checks **containment rather than equality**, because the two decoders
  disagree about a file holding several comments and neither is wrong. Reading
  one GIF carrying two, ImageMagick reports the last, Pillow reports both
  joined by a newline, and this library reports the first as the description
  and all of them in the raw block. There is no convention to be right about,
  so what is checked is that the text written is there and readable.

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
  2.1 million executions found nothing in GIF. The harness now decodes every
  item twice, forward and then backward, because that is what reaches both
  sides of the canvas cache - reading one back and building on it, and
  recognizing one that is for a later frame as unusable - and because a
  document whose later frames fail leaves a cache behind for the prefix that
  succeeded, which only a second pass reaches. 256 thousand executions of the
  two-pass harness on the cached decoder found nothing new.

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

- **Plain Text rendering** (89a 25). The block is walked past. No decoder in
  use renders it, and doing so would mean shipping a bitmap font.
- **The background colour is not painted.** The canvas starts transparent; the
  colour is reported through `gimg_doc_background_color()` for a caller who
  wants it. See the deviations table for why nothing here paints it.

---

Back to the \ref format_references "format and specification references".
