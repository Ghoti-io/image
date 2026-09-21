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
  the (N + 15) / 64 that 89a 18 defines and written back out on save - see
  "The pixel aspect ratio". The colour resolution and sort flags
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

### Writing how many times to play

The count goes back into the file from the document, so a load and a save
preserve it with no help from the caller. That was not always so: the writer
ignored the document and put `gif_loop_count` in every animation it produced,
which meant an animation asking to repeat five times came back asking to repeat
for ever. ImageMagick and Pillow both preserve the count, and both preserve its
absence; this was the odd one out in both directions.

A document declaring **no** count gets **no NETSCAPE2.0 block**, which is a
different instruction from a count of zero - browsers play such a file once.
The writer used to put a zero-count block in every animation, turning "said
nothing" into "repeat for ever"; `gimg_doc_clear_loop_count()` had documented
the correct behaviour all along.

`gif_loop_count` stays a save option, but as an **override** rather than the
source. Its zero already means "repeat for ever" and so cannot also mean "not
set", so a non-zero value means the caller asked and gets it, and zero means
they did not ask and the document answers. A caller who wants "for ever" on a
document that says otherwise says so with `gimg_doc_set_loop_count(doc, 0)`,
which the document model can express and the option cannot.

Frame count does not gate the block. A count on a still image has nothing to
repeat, but real files carry one - `gif_4x2_netscape_loop.gif` is a single-frame
GIF with the block - and this codec's loader reports it, so dropping it on write
would lose what the accessor reads. Pillow keeps it too; ImageMagick drops it
and loses the round trip.

89a 26's count is two bytes where APNG's `num_plays` is four, so a count above
65535 arriving from an APNG is written as 65535. Truncating would land on 0,
the one value that means something else entirely.

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
- **The `gif_background` setting is part of the key.** The two settings give
  different pixels wherever no frame has drawn, so a canvas cached under one
  cannot seed a decode under the other; a decode that asks for the other
  setting replaces the cache rather than reading it. A caller who alternates
  gets no head start, and a caller who does not - which is all of them - is
  unaffected.
- **A second forward walk gets no benefit unless the cache is cleared first.**
  Moving only forward is what stops two walks in opposite directions from
  dragging it back and forth, and the price is that starting over at frame 0
  finds it at the end. The encoder is the one caller that does start over -
  once to see whether the frames share a palette, once to write them - and it
  clears the cache itself between the two. See "One table or many".
- **It is the one thing decode writes through a `const` document**, and the
  mutex in `gimg_gif_doc_state_t` is what makes that safe. A single-image GIF
  is left out: there is no later frame to hand a head start to, so the cache
  would be a canvas-sized allocation nothing would ever read. The cost for an
  animation is one extra canvas held for the life of the document.

## Save

The encoder writes GIF89a, one image block per frame. A frame after the first
is written as **the rectangle that changed**, and inside that rectangle the
pixels that did not change are written as **the transparent index**; see
"Frame optimization" below. An animation whose frames share few enough colours
gets **one Global Color Table** instead of one table per frame; see "One table
or many".

- **A palette is built, never chosen.** An image of 256 colours or fewer has
  exactly one palette that reproduces it, up to the order of its entries, and
  building that table stores what is already there. An image with more is
  refused with `GIMG_ERR_UNSUPPORTED` rather than quantized, because deciding
  which colours to discard is an image-processing decision and not a codec's.
  The PNG writer in this library draws the line in the same place. A caller
  who wants a photograph as a GIF reduces it first with `gimg_ops_quantize()`
  - see \ref module_palette "Palettes and colour reduction" - which hands back
  an ordinary raster that this writer then accepts through the path above,
  having been told nothing.
- **Transparency is the same argument one bit down.** GIF designates a single
  index transparent and treats every other pixel as opaque (89a 23); a
  half-covered edge has nowhere to go. A partially transparent pixel is
  refused unless `gif_alpha_threshold` says which way to round. Fully opaque
  and fully transparent pixels need no option.
- **Index 0 is the transparent one** when a frame has transparency, so that a
  decoder ignoring the control block shows the background rather than a stray
  colour. That costs one of the 256 entries.
- `gif_interlace` writes the four-pass order; it changes no pixel.
- `gif_loop_count` overrides the count the document declares when it is
  non-zero; the NETSCAPE2.0 Application Extension is written from the document
  otherwise, and not at all when the document declares no count.
- A document whose frames differ in size is refused: the canvas is the first
  frame's, so a differing size asks a placement question the caller has not
  been asked.

### One table or many

89a 18 lets one table serve every frame. This writer used to decline it and
give each frame a Local Color Table, on the grounds that two frames of an
animation rarely share a palette. Measured on the 19 multi-frame files in the
corpus, that was wrong: **17 of them use 255 colours or fewer across every
frame**, and their per-frame tables run from 2% to 29% of the file. A
twelve-frame spinner of sixteen colours was spending 576 of its 1964 bytes on
twelve copies of the same table.

So the frames are walked once before anything is written, to collect the
colours they use between them. If they fit one table it is written once and no
frame carries its own; if they do not - more than 255 colours between them, a
frame that cannot be read, or a partly transparent pixel with no threshold to
round it by - nothing is written and every frame carries its own, exactly as
before. Index 0 is kept for transparency whether or not any frame turns out to
need it, because nearly every frame after the first does (that is what masking
is) and a table cannot be extended once it is written.

The walk is an **upper bound** rather than the exact answer: it counts the
colours in the frames as they were handed over, and masking only ever removes
some. A bound is what is wanted, because a table large enough stays large
enough.

It costs **one extra decode of every frame**, which on the slowest file in the
corpus is about a quarter again: 5.4 s against 4.3 s for a 358-frame
1200x1200 animation. It is skipped entirely for a single-frame document, where
one global table and one local table are the same size and the question does
not arise.

That quarter was 25x before the interaction with the canvas cache was
measured. The cache only ever moves forward (see "The canvas cache"), so the
walk left it parked at the last frame, where it helped nothing, and the
encoding pass then replayed every frame from the beginning - 110 seconds
instead of 4.3. The writer now clears the cache between the two walks rather
than the cache weakening its forward-only rule, which earns its keep
everywhere else: it is the only caller in the library that knowingly walks a
document twice, so it is the one that has to say so.

The **background colour index** is written as 0, which the global table makes
the transparent entry - the conventional choice, and consistent with this
codec's own reading that the logical screen starts empty. With no global table
it names nothing and is written as 0 for want of anything better.

### Frame optimization

A frame handed to this encoder is the whole canvas as it should look. A GIF
frame is a patch. Writing every frame at full size is always correct and is
what this codec did at first; two things turn one into the other, and both are
checked by decoding the result rather than by reasoning about it.

Both rest on the same value: **what the logical screen holds at the moment a
frame is drawn**. Comparing against the previous frame instead would be almost
right and wrong where it matters, because disposal 2 blanks a rectangle after
its frame has been shown, and the frame after that paints onto the hole rather
than onto its predecessor. The encoder carries the screen explicitly and
advances it past each frame and its disposal before looking at the next one.

Two pixels count as equal when the encoder would store the same thing for both.
Both transparent is equal whatever colour sits under the transparency, because
that colour is never shown and never written.

**Cropping.** If a frame is drawn with disposal 1 - leave it in place - and its
rectangle covers every pixel in which it differs from the screen, then after
drawing it the screen holds that frame. By induction the screen is always
right.

**Masking.** Inside that rectangle, a pixel equal to the screen need not be
written at all: the transparent index leaves what is already there, which is
the pixel wanted. It costs one palette entry and buys long runs of a single
index, which is what the LZW stream is good at. This is where most of the
saving is.

A frame identical to what is already on screen gets a **one-pixel** rectangle:
a GIF image block cannot be zero-sized, and one pixel repainted its own colour
is the cheapest way to say that nothing happened.

#### The one hole

**Painting cannot make an opaque pixel transparent** - GIF has no eraser except
disposal 2, which blanks exactly the rectangle of the frame carrying it. So
where a frame needs a pixel see-through that the screen has opaque, the
previous frame is grown to cover what must be erased and given disposal 2.
The same test applies to the wrap from the last frame back to the first, since
a loop makes that a transition like any other, and a first frame that is
transparent where the last was opaque would otherwise be wrong on every pass
but the first.

Nothing has to be grown to repaint the hole. The frame after a disposal 2 is
compared against the screen *after* that disposal, so whatever the hole must
show is a difference and cropping picks it up on its own; what the hole should
leave blank is not a difference and is left alone.

#### When masking gives up

Masking spends a palette entry on the transparent index, and a frame of exactly
256 colours has none to spare. Such a frame was writable before masking
existed, so it must still be: the encoder falls back to writing every pixel its
own colour rather than refusing it. Reaching that needs a frame which both
fills the table with pixels that changed and has at least one that did not,
because masking usually *reduces* the colour count - a masked pixel contributes
no colour at all.

#### Measured

By re-encoding real animations and comparing every decoded frame against the
original's. "Cropping only" is the state this codec was in before masking:

| | Full frames | Cropping | And masking | And one table |
|---|---|---|---|---|
| 39 frames, 1200x1200 | 1.05x | 1.05x | 0.95x | **0.94x** |
| 50 frames, 1200x1200 | 1.02x | 1.02x | 0.83x | **0.82x** |
| 60 frames, 831x779 | 1.10x | 1.10x | 1.03x | **1.00x** |
| 358 frames, 1200x1200 | 8.0x | 2.03x | 0.97x | **0.95x** |

Cropping alone does nothing for the first three, because their frames genuinely
differ across most of the canvas - there is no rectangle to shrink to. Masking
is what reaches inside the rectangle, and it is what moves them. The 358-frame
file gains from both: 357 of its frames change one pixel, which cropping
reduces to one pixel each, and the 89 that change a large region are where
masking then does its work. A shared table is worth a percent or two on all of
them, and much more on a short animation of few colours, where per-frame tables
were most of the file.

**Across the whole corpus of 121 files written by unknown encoders over about
thirty years, 21,370,660 bytes become 18,744,067** - 0.877x in aggregate, with
every visible pixel of every frame unchanged, and **no file larger than the one
it came from**: the worst is 1.00x. Being smaller than the original was never
the goal, and on a format this old it says more about what the original
encoders left on the table than about this one. It is worth recording because
the same number was 8x on the file that mattered most, before any of the three
changes in that table.

**Every claim above is checked by decoding.** All 121 corpus files survive
decode, re-encode and decode again with every frame's visible pixels identical,
and the re-encoded file is read back by giflib as well as by this decoder. The
encode tests publish animations for giflib, ImageMagick and Pillow to composite
and check - among them one whose second frame spans the canvas and is opaque,
and so can only be small if masking happened, and one whose four frames carry
no table of their own.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|
| Header | 87a and 89a, read identically | Anything not beginning `GIF` &rarr; `GIMG_ERR_FORMAT` |
| Logical Screen Descriptor | Size and Global Color Table; the background index resolved to a colour, painted only when `gif_background` asks; the Pixel Aspect Ratio byte resolved to a ratio, reported and written back but never applied | Truncated &rarr; `GIMG_ERR_FORMAT`. The colour resolution and sort flags are read past |
| Colour tables | Read: global, local, 2–256 entries, absent. Write: one global table when the frames share 255 colours or fewer, otherwise one per frame | Truncated &rarr; `GIMG_ERR_FORMAT` |
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
| Write | 1–256 colours, transparency, interlace, animation with delays and loop, comments, a shared global table where one serves, frames cropped to what changed with unchanged pixels masked out | More than 256 colours (reduce first with `gimg_ops_quantize()`), or partial alpha with no threshold &rarr; `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from the reference implementations

| Case | This codec | Elsewhere |
|---|---|---|
| An index the colour table does not have | Draws nothing; the pixel keeps what the canvas already held | giflib does the same. Pillow paints it opaque black. Pinned by `gif_8x2_index_past_palette.gif`, where a four-entry table is addressed with index 7 — expressible because the code size is set independently of the table size |
| The colour under a fully transparent pixel | Zero in all three channels | giflib the same; Pillow writes the palette colour. Invisible by definition, and the reason `verify_gif_output.py` does not compare those channels |
| The logical screen before any frame is drawn | Transparent by default; the declared colour with `gif_background` = `GIMG_GIF_BACKGROUND_PAINT` | See "The background colour" below: ImageMagick paints it and Pillow paints something, but only for a file that never mentions transparency. The colour is reported by `gimg_doc_background_color()` either way |
| Plain Text extension | Walked past, never rendered | No decoder in use renders it either. A file using it looks the same here as everywhere |
| A file that ends without a trailer | Keeps the frames already read | Common enough in the wild that the frames are worth more than the refusal. A file truncated **inside** a frame is still refused |
| What "restore to background" leaves behind (disposal 2, 89a 23) | Transparent by default; the declared colour under the same option | Browsers and ImageMagick agree with the default; Pillow paints the declared background colour, which is what 89a 23 literally says. See "The background colour" below. Pinned by `gif_12x8_background_index.gif` and by `tests/out/gif/animation_6x3.gif` |

## The background colour

89a 18 gives the Logical Screen Descriptor a Background Color Index, and says
two things about it that this codec does not do. Because it is a deliberate
deviation, here is exactly what the specification says, what everyone does, and
why.

**What the specification says.** Section 18 defines the index as naming the
colour for the pixels on the screen that no image covers, and adds that when
the Global Color Table Flag is zero the field should be zero and should be
ignored. Section 23 then gives disposal method 2 the name "restore to
background color" and says the area the graphic used must be restored to it.
Both are unambiguous.

**What everyone does.** Measured, with a 12x8 canvas whose largest frame is
4x4, declaring magenta, with nothing drawn in magenta - which is
`gif_12x8_background_index.gif` in the fixtures:

| | here | ImageMagick | Pillow | Chromium | giflib |
|---|---|---|---|---|---|
| A pixel no frame covers | transparent | **magenta** | opaque red | transparent | n/a |
| After disposal 2 | transparent | transparent | **magenta** | transparent | n/a |

Bold is the specification's answer. **Nobody implements both, and the two that
implement one implement different ones.** ImageMagick honours the background
for the uncovered screen and not for disposal 2; Pillow does the reverse, and
for the uncovered screen paints entry 0 rather than the entry the index names.
giflib has no row because it does not composite at all - it hands back each
image as stored, reports `SBackGroundColor`, and leaves the screen to the
caller. Its own documentation lists the canvas background under unimplemented
features, alongside the frame positioning, and says browsers and modern viewers
ignore them.

There is one more variable, and it explains most of the disagreement:
**ImageMagick and Pillow both switch on whether the file declares a transparent
index anywhere.** Add one to the file above and both of them return a
transparent uncovered screen, matching this codec exactly. The deviation is
therefore narrower than it looks: it applies only to files that never mention
transparency at all.

**Why the default is transparent.** Not because the specification is wrong,
but because the two possible mistakes are not equal in cost. Painting the
background makes every such GIF opaque, and a caller who wanted the alpha
cannot get it back - the information is destroyed in the decoder, before they
ever see it. Not painting it loses nothing: `gimg_doc_background_color()`
reports the colour, resolved through the Global Color Table, and compositing
over it is one line in the caller. Given a choice between a lossy default and a
recoverable one, a library takes the recoverable one and leaves the policy to
the caller.

Disposal 2 is the same argument with history behind it as well: the animations
in the wild were authored against browsers, which restore to transparent, so a
file that depends on the literal reading is vanishingly rare and a file that
depends on the browser reading is everywhere.

### Reading it, and the one thing the alpha says

A GIF has no way to leave the Background Color Index out. A file with a Global
Color Table always names one of its entries, so "this file states no
background" is not directly expressible - what an encoder writes instead is the
index of an entry that is **marked transparent**, which says the same thing by
naming the absence of a colour.

So the index alone does not answer the question; the transparency flag beside
it does. `gimg_doc_background_color()` reports both: the entry's colour, and an
alpha of 0 when the first frame's control block marks that entry transparent.
A caller that composites over the result gets the no-op the file asked for, and
a caller that wants to know which entry was named can still see it.

It is the **first** frame's flag, because that is the control block in force
when the screen is first shown. ImageMagick does the same, and the pair
`gif_4x4_background_masked_first.gif` and `gif_4x4_background_masked_later.gif`
differ in nothing else:

| | here | ImageMagick |
|---|---|---|
| frame 0 marks the entry transparent | `255,0,0,0` | `srgba(255,0,0,0)` |
| only frame 1 does | `255,0,0,255` | `red` |

### Writing it

The writer used to put a zero in the field always, which made
`gimg_doc_set_background_color()` a no-op that reported success: the accessor
gave the new colour and the file kept naming entry 0. Only a round trip showed
it. Both of the libraries that matter do better - `magick -background lime`
repoints the index at an entry it adds, and Pillow honours
`info["background"]` - so a GIF this library wrote was the odd one out.

Now:

| the document declares | the file says |
|---|---|
| a colour the global table holds | that entry's index |
| a colour it does not hold | a new entry, appended, and its index |
| nothing, or a colour at alpha 0 | index 0, which the first frame marks transparent |

Appending can take the table up to the next power of two and cost a few bytes.
That is the honest price of stating something the file would otherwise not
state, and it is only paid when a caller actually asks for a colour the frames
do not paint - across the 41-file measurement corpus, **not one output changed
size**.

The last row is why the first frame's control block is made to mark entry 0
transparent even when that frame has no masked pixels of its own. Without it a
document declaring no background produced a file naming entry 0, which is
black, and reading it back reported a black background nobody had declared.
Saying so costs nothing: the flag and the index byte are already in the control
block every animated frame carries, and index 0 is the mask index the planner
reserves, so no colour is ever stored there and no pixel changes.

A single-frame GIF is written with a local colour table and no global one, so
it has no entry to name; 89a 18 says the field is then to be zero and ignored,
and `gimg_doc_background_color()` reports nothing for such a file in both
directions.

### Asking for the other behaviour

`GIMG_Decode_Options.gif_background` selects it:

| value | screen before any frame | disposal 2 |
|---|---|---|
| `GIMG_GIF_BACKGROUND_TRANSPARENT` (0, default) | transparent | transparent |
| `GIMG_GIF_BACKGROUND_PAINT` | the declared colour | the declared colour |

The default lives at zero, so a zero-initialized `GIMG_Decode_Options` and a
NULL one decode identically - the same rule the JPEG upsampling option
follows, and for the same reason: adding an option must never quietly change
what an existing caller gets.

`GIMG_GIF_BACKGROUND_PAINT` has **no effect on a file with no Global Color
Table**, because 89a 18 says the index is to be ignored there and there is no
colour to paint. `gimg_doc_background_color()` returns 0 for such a file, and
the option cannot invent what the accessor declines to report.

**Setting it does not make this decoder agree with another one.** It makes it
agree with the specification, and nothing in the table above does that. A
caller who is chasing ImageMagick's output wants the painted screen and the
transparent disposal; one chasing Pillow wants the reverse and a different
palette entry again. Neither is expressible here, deliberately - two more
values would encode two other decoders' inconsistencies as though they were
choices worth offering.

### If you are not seeing what you expect

Most reports of "the background is wrong" turn out to be one of these:

- **The file declares a transparent index.** Then ImageMagick and Pillow both
  return a transparent screen too, and the default already agrees with them.
  The disagreement only exists for files that never mention transparency.
- **The file has no Global Color Table.** Nothing is declared, so nothing is
  painted under either setting, and `gimg_doc_background_color()` says so.
- **The comparison is against a flattened image.** A PNG or a screenshot of a
  browser has already composited the transparency onto something - usually
  white, sometimes a page colour. A transparent pixel here and a white pixel
  there are the same result viewed differently.
- **The expectation came from `magick convert` without `-coalesce`.** That
  writes the *image block*, at the size the image block has, not the logical
  screen: on the fixture above it produces a 4x4 file, not a 12x8 one, and the
  question of what surrounds it never arises.
- **Disposal 2 was expected to restore the background.** It is the clause the
  specification is clearest about and the one browsers ignore most
  universally. The option honours it; the default does not.

**Every part of this is pinned by a test that was watched failing.** Making
the decoder paint the screen fails
`AnUncoveredPixelIsTransparentNotTheBackgroundColour` and nothing else; making
disposal 2 restore the colour fails
`RestoreToBackgroundLeavesTransparentNotTheBackgroundColour` and nothing else;
flipping the default to painting fails both, plus
`TheDefaultOptionsDecodeExactlyAsNoOptionsDo`; and dropping the setting from
the canvas cache's key fails
`TheTwoBackgroundSettingsDoNotShareACachedCanvas` in both directions.

That last one took two attempts, and the first attempt is the more useful
half. It decoded frame 0 and frame 1 with the default and then asked for frame
0 again with the option set - and passed against a decoder with no cache key at
all, because the cache only moves forward and so never seeds a request for a
frame *behind* it. A cache test has to ask for a frame at or after where the
cache is parked, or it is testing nothing. The mutant is what said so.

That fixture exists because the deviation was previously untested. The test
that claimed to cover it used `gif_12x8_offset_frame.gif`, whose first frame
covers the whole canvas - so it has no uncovered pixel - and whose background
index is 0, which resolves to the same red that frame paints. It passed whether
or not the background was painted.

## The pixel aspect ratio

The other field of the Logical Screen Descriptor this codec reads and does not
act on, and the one where the disagreement is not about behaviour but about
whether anybody looks at it at all.

**What the specification says.** 89a 18 calls the byte a factor for computing
an approximation of the aspect ratio of a pixel in the original image, and
gives the formula: when the value is not 0, the ratio is (N + 15) / 64. Zero
means no information was given - which is not the same as "square", and the
distinction matters, because square is a claim and silence is not.

**What this codec does.** Reads it, applies the formula, and reports the result
through `gimg_doc_pixel_aspect_ratio()` as an unreduced numerator over 64.
Nothing is resampled. Since this codec also **writes** it (see below), a GIF
that declares a non-square pixel still declares it after a load and save.

**What everyone else does.** Measured on four files identical but for the byte:

| | reports it | applies it |
|---|---|---|
| here | yes, as a ratio | no |
| giflib | yes, as the raw byte | no |
| ImageMagick | no | no |
| Pillow | no | no |
| GdkPixbuf | no | no |
| Chromium | no | no |

An image with the byte at 255 - a pixel more than four times as wide as tall -
comes out of every one of them as an unstretched square-pixel raster of the
declared dimensions. Chromium lays it out at its intrinsic 16x16 with no CSS
sizing at all. `giftext` prints `Aspect = 255` and giflib's own documentation
lists the field under unimplemented features, noting that it was ignored
entirely before 5.0 and is now read and preserved. Neither ImageMagick's nor
Pillow's verbose output mentions it in any form.

So the count is: **nobody applies it, and this codec and giflib are the only
two that will tell you it is there** - this one being the only one that turns
it into a ratio rather than handing back the raw byte.

**In the wild it is rare and it is always 1.** Seven of the 121 corpus files
set the byte, and all seven set it to 49, which is exactly 64/64. Those writers
were not describing a non-square pixel; they were saying square out loud rather
than staying silent, which is a distinction the format allows and which this
codec preserves - `gimg_doc_pixel_aspect_ratio()` reports 64:64 for such a file
and reports nothing for a file with a zero byte. All seven come back with the
byte intact through a load and save.

### Why there is no option to apply it

The background colour got one because painting it is a decision about *pixel
values*, which is what a decoder produces. Applying an aspect ratio is a
decision about *geometry*: it means resampling the raster to square pixels,
which is a different kind of thing and does not belong behind a decode flag.

- It would change the raster's dimensions, so `gimg_item_decode()` would stop
  returning the logical screen the file describes. Everything downstream
  assumes it does - the encoder most of all, which composites frames against a
  canvas of exactly that size.
- It needs a resampling filter, and choosing one is an image-processing
  decision. This library puts those in `ops` and says so: the CMYK conversion
  and the colour quantization are both there for the same reason.
- There is no resampler here to call. A general `gimg_ops_resize()` would be
  the right home, and writing one is a feature rather than a flag - it is
  listed under "Not implemented" rather than pretended at.

Reporting the ratio is what lets a caller do it themselves, at the moment they
know what they want: a viewer scales its window, a converter scales the raster,
and each needs a different filter.

### Reading, writing, changing and removing it

The ratio is a property of the document, and the four operations on it are
`gimg_doc_pixel_aspect_ratio()`, `gimg_doc_set_pixel_aspect_ratio()` and
`gimg_doc_clear_pixel_aspect_ratio()`. Every format in this library that can
state one carries all four through a save:

| | states it as | read | create | update | delete |
|---|---|---|---|---|---|
| GIF | Pixel Aspect Ratio byte, 89a 18 | yes | yes | yes | yes |
| PNG | `pHYs` with unit 0, 11.3.4.3 | yes | yes | yes | yes |
| JPEG | JFIF APP0 density with units 0 | yes | yes | yes | yes |
| BMP | — | — | — | — | — |

BMP has no row because it cannot say this: `biXPelsPerMeter` and
`biYPelsPerMeter` are a physical resolution, which is the *other* thing - it
reaches a caller through `gimg_meta_common_dpi()`, and a non-square pixel
expressed that way is a consequence of the two differing rather than a
statement in its own right.

The ratio crosses formats, because all three are saying the same thing: a JPEG
declaring 2:1 saved as a GIF comes back as 128:64, which is that ratio in the
only form 89a 18 can hold.

**Update and delete were the half that was silently broken**, in PNG and JPEG
rather than here. Both preserve the chunk or segment the file arrived with, so
setting a ratio changed what the accessor reported and not what was written: a
caller saw success, saved, and got the old value back on the next load. The
document now wins for the aspect-ratio-carrying forms specifically - a unit 0
`pHYs`, a units 0 APP0 - while a physical resolution in the same chunk is left
to the rule it already had. A load and save with nothing edited still produces
the same bytes, because the loader put the document's value there in the first
place.

### Writing it

The writer emits the byte the document's ratio implies, and **needs no save
option to do it**. Zero here means "no information given" in the format and
"declares nothing" in the document model - the same statement twice - so
reading the document is unambiguous, and a caller who wants no ratio in the
file declares none.

The loop count, twenty lines away in the same header, is the case where that
does not hold: `gif_loop_count`'s zero already means "play for ever", so it has
no spelling for "unset". That is why it survives as an override on top of the
document rather than being replaced by it - see "Writing how many times to
play".

89a 18 can hold only (N + 15) / 64 for N of 1 to 255, which is 16/64 to 270/64.
A document declaring anything outside that range gets a zero byte: saying
nothing is true, and writing the nearest expressible ratio would put a number
in the file the caller never asked for and could not tell apart from one they
did.

Before this the writer put a zero there always, so a file that declared a
non-square pixel came back declaring nothing - the loader read it, the accessor
reported it, and the save threw it away. PNG did not lose a straight round trip,
for an unrelated reason: its `pHYs` survives as raw metadata under
`GIMG_META_PRESERVE_ALL`, so the ratio came back without the writer knowing it
existed - which is also why *changing* it there did not work until now.

### Tested

`gif_8x8_pixel_aspect.gif` declares byte 113, which the formula turns into
128/64. The value is chosen to be discriminating rather than round: a reader
that drops the + 15 reports 113/64, and one that divides by the wrong constant
reports something else again. Until that fixture existed no test ran the
arithmetic against a file - the only aspect-ratio test loaded a fixture whose
byte was zero and checked that nothing was reported.

Both halves were watched failing. Making the reader drop the + 15 fails
`ANonZeroAspectByteIsReadAsTheFormulaDefinesIt` and, downstream,
`APixelAspectRatioSurvivesALoadAndSave`; making the writer go back to a
constant zero fails that round-trip test and
`ARatioTheFormatCannotHoldIsWrittenAsNoneRatherThanTheNearest`, which also
checks that the four ratios at and outside the representable edges are written
and refused correctly.

## Tested scope

- **Fixtures**: fourteen files from `tests/data/gif/generate.py`. Pillow writes
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

- **Colour quantization, inside the codec.** An image of more than 256 colours
  is still refused here, and deliberately: which colours to discard is a
  judgement about the picture, not a fact about the format. It is available
  one call away, in `gimg_ops_quantize()` - see
  \ref module_palette "Palettes and colour reduction" - which hands back an
  ordinary raster this writer accepts. GIF save needed no change at all for
  that, which was the point of putting it there.
- **Plain Text rendering** (89a 25). The block is walked past. No decoder in
  use renders it, and doing so would mean shipping a bitmap font.
- **Resampling to square pixels.** The Pixel Aspect Ratio is read, reported and
  written, and never applied - applying it means resizing, and there is no
  resampler in this library to do it with. A general `gimg_ops_resize()` is
  where it would go, and every other decoder measured leaves it alone too. See
  "The pixel aspect ratio".
- **The background colour is not painted by default**, and disposal 2 restores
  to transparent rather than to it. Both are deliberate and both are available:
  `GIMG_Decode_Options.gif_background` = `GIMG_GIF_BACKGROUND_PAINT` gives the
  specification's reading of 89a 18 and 23. See "The background colour" for
  what the specification says, what the other decoders do, why the default is
  the other way, and why turning the option on still does not reproduce any
  other decoder.

---

Back to the \ref format_references "format and specification references".
