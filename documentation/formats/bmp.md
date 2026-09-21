@page format_bmp BMP

# BMP

What this codec implements of the Windows and OS/2 device-independent bitmap,
where it deliberately differs from other decoders, and how each claim was
checked. The library's format index is the
\ref format_references "format and specification references".

BMP has no ISO or W3C standard. What exists is Microsoft's documentation of
the GDI structures a `.bmp` file is a serialization of, plus the OS/2
Presentation Manager bitmaps that the 12- and 64-byte headers come from. Where
the documentation is silent - and it is silent about the two questions a
decoder most needs answered, the meaning of 16-bit `BI_RGB` and of the fourth
byte of a 32-bit `BI_RGB` pixel - this page says what this codec does and why.

## Normative references

- **File and header structures:** `BITMAPFILEHEADER`, `BITMAPCOREHEADER`,
  `BITMAPINFOHEADER`, `BITMAPV4HEADER`, `BITMAPV5HEADER` -
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-storage> and
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-header-types>
- **Header fields:**
  <https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader>
- **Run-length encoding (BI_RLE8, BI_RLE4):**
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-compression>
- **Color management fields (V4, V5):** `LOGCOLORSPACE`, `CIEXYZTRIPLE`,
  `bV5Intent`, `bV5ProfileData` -
  <https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-bitmapv5header>
- **OS/2 1.x bitmap:** the 12-byte `BITMAPCOREHEADER`, with 16-bit dimensions
  and 3-byte palette entries.
- **OS/2 2.x bitmap:** `BITMAPCOREHEADER2`, 16 to 64 bytes in steps of 4, and
  the `RLE24` and Huffman 1D compressions only it names.

The BMP codec uses the document/codec allocator for every codec-owned
allocation: the palette, the stored pixel bytes, an embedded ICC profile, the
document state, and the encoder's row and encoding buffers.

## Parts implemented

- **File header (14 bytes):** the `BM` signature is both the probe magic and
  the first thing load checks. `bfOffBits` is honored - pixel data is read
  from wherever it points, not from immediately after the palette, because the
  gap between the two is legal and common. `bfSize` and the two reserved words
  are read past and not trusted for anything.
- **DIB headers:** recognized by the size field - 12 (`BITMAPCOREHEADER`), 40
  (`BITMAPINFOHEADER`), 52 (`BITMAPV2INFOHEADER`), 56 (`BITMAPV3INFOHEADER`),
  108 (`BITMAPV4HEADER`), 124 (`BITMAPV5HEADER`), and every multiple of 4 from
  16 to 64 for OS/2 2.x's `BITMAPCOREHEADER2`. All are normalized into one
  internal header, so nothing downstream knows which version it came from.
- **OS/2 2.x, and the three sizes two vocabularies share:** a
  `BITMAPCOREHEADER2` may stop at any multiple of 4 from 16 to 64, with every
  field it stops short of reading as zero, and its first 40 bytes are byte for
  byte a `BITMAPINFOHEADER`. Sizes 40, 52 and 56 are also Windows headers, and
  nothing in a file says which was meant; they are read as Windows, because
  that is what wrote them - bmpsuite's `q/rgb32h52.bmp` and `q/rgba32h56.bmp`
  both carry RGB masks in the bytes where OS/2 would put `usRecording`, and
  the masks are what makes them decode. The rest of the range is unambiguous.
- **Dimensions:** 16-bit unsigned in the core header, 32-bit signed
  everywhere else. A negative height means the rows are stored top to bottom;
  decode honors the sign rather than flipping the image.
- **Bit depths:** 1, 2, 4, 8, 16, 24 and 32. Depths of 8 and below index a
  palette; 16 and 32 extract their channels through masks; 24 is a fixed BGR
  triple.
- **Compression:** `BI_RGB` (0), `BI_RLE8` (1), `BI_RLE4` (2), `BI_BITFIELDS`
  (3), `BI_JPEG` (4), `BI_PNG` (5) and `BI_ALPHABITFIELDS` (6) under a Windows
  header; `RGB`, `RLE8`, `RLE4`, Huffman 1D (3) and `RLE24` (4) under an OS/2
  one. The two vocabularies disagree about 3 and 4, so the header reader
  resolves the number once against the vocabulary that carried it and
  everything downstream reads the resolved value - comparing the raw number
  against the wrong vocabulary is a mistake waiting to be made. Each is
  checked against the bit depth it requires, because a file that disagrees
  with itself cannot be decoded by guessing which half to believe.
- **Channel masks:** `BI_BITFIELDS` supplies red, green and blue masks and
  `BI_ALPHABITFIELDS` adds a fourth. A V2 or later header carries them inside
  itself; a plain 40-byte `BITMAPINFOHEADER` stores them in the three (or
  four) 32-bit words that follow the header, which is where the palette would
  otherwise begin. Both placements are read. A mask must be a single
  contiguous run of set bits: anything else has no unambiguous shift.
- **Implied masks for BI_RGB:** 16-bit is **5-5-5** with the top bit unused,
  24-bit is BGR, and 32-bit is 8-8-8 with the fourth byte undefined (see
  below). A writer that wants 5-6-5 must declare `BI_BITFIELDS`, which is what
  the format requires of it; treating every 16-bit file as 5-6-5 shifts green
  and red and is the single most common BMP decoding defect.
- **Palette:** entries are 4 bytes (blue, green, red, unused) after a
  `BITMAPINFOHEADER` or later, and 3 bytes (blue, green, red) after a
  `BITMAPCOREHEADER`, which has no `biClrUsed` field at all. `biClrUsed` of
  zero means the full 2^bpp; a count larger than the indices can address is
  clamped rather than rejected, since such files are common and the extra
  entries are simply unreachable. Every index is checked against the entries
  the file actually carried, on the RLE paths as well.
- **Row stride:** `(width x bpp + 31) / 32 x 4`, computed in `size_t` with
  checked multiplication, so a wide image cannot wrap the arithmetic into a
  small buffer.
- **Uncompressed pixel data:** exactly `stride x height` bytes are read from
  `bfOffBits`, and a file that does not carry them is an error. `biSizeImage`
  is ignored for these: it is attacker-controlled, and it is zero in a large
  share of real files.
- **RLE8, RLE4 and RLE24:** encoded runs, absolute runs padded to a 16-bit
  boundary, end-of-line, end-of-bitmap, and the two-byte delta. All three
  share one escape structure and differ only in what a pixel costs in the
  stream, so they share one loop: RLE8 and RLE4 name palette entries, and
  RLE24 carries a BGR triple per pixel and uses no palette at all. An RLE4
  encoded run alternates the two nibbles of its value byte, so `0x12` is
  1, 2, 1, 2 and not four copies of one index. A run that overhangs its row is
  clipped rather than treated as corruption - encoders legitimately emit them
  - and the decoder refuses to advance past the last row, so no hostile stream
  can write outside the raster. Pixels a delta skipped are never written and
  keep the raster's zeroed value, which is transparent rather than opaque
  black.
- **`BI_JPEG` and `BI_PNG`:** the pixel data of such a file is a whole JPEG or
  PNG stream. It is handed to that format's codec and the decode delegated to
  the resulting document. The payload goes to the codec `biCompression` named
  and never to the prober: probing would let a `BI_PNG` wrapper hold a BMP,
  which could hold another, with no bound on the nesting this side of the
  stack. The caller's `GIMG_Load_Options` travel down unchanged, so a
  `GIMG_Limits` cap applies to what is inside a wrapper exactly as it would to
  a file that arrived on its own. `biSizeImage` states the payload's length
  and is the only field that does, so it is used - but it may only shorten
  what is taken from `bfOffBits`, never extend it past the end of the file.
- **Color (V4 and V5):** `LCS_sRGB` and `LCS_WINDOWS_COLOR_SPACE` name sRGB.
  `LCS_CALIBRATED_RGB` describes the space instead: its endpoints are declared
  `CIEXYZ` and written by every writer in reach as xyY chromaticities
  normalized to sum to one, and are matched against the two gamuts
  `GIMG_Color_Info` names - sRGB and Adobe RGB, which share their red and blue
  primaries and differ only in green. Three per-channel gammas that agree
  become one transfer function; three that disagree describe a space this
  model cannot hold and leave the transfer unsaid rather than averaged.
  `bV5Intent` maps onto the four rendering intents. `PROFILE_EMBEDDED` reads
  the ICC profile out of the file at the offset the header gives, bounds
  checked against the file's length and against `max_memory`. Anything else -
  a gamut that matches neither, a color space type not listed - is left
  unknown rather than approximated, which is the rule the PNG codec applies to
  `cICP` for the same reason: saying nothing beats saying the wrong thing.
- **Resolution:** `biXPelsPerMeter` and `biYPelsPerMeter` are read into the
  document's common metadata as dots per inch, and written back from it. A BMP
  states its resolution in the header rather than in an optional chunk, so the
  common metadata is the only place it can go - which is where the PNG and
  JPEG codecs already put theirs, so a resolution survives a conversion in
  either direction. An inch is exactly 0.0254 m, so the conversion is integer
  arithmetic and every resolution from 1 to 1200 dpi round-trips exactly. A
  negative value is read as "not stated" rather than as an enormous density.
- **Row order:** rows are written into the raster top-down whichever way the
  file stored them, by choosing the destination row rather than reversing the
  buffer afterwards.
- **Output:** always `GIMG_PIXEL_RGBA8`, one item per document. A sample
  narrower than 8 bits is expanded by `round(value x 255 / max)` rather than
  by truncation, so the maximum input maps to 255 and the midpoint does not
  drift downward - see "Where this codec differs" for what the alternatives
  cost.
- **Limits and diagnostics:** `max_decoded_pixels` is checked at load, before
  any large allocation, and again at decode; `max_memory` caps the pixel
  buffer and an embedded ICC profile. A rejected file appends a diagnostic
  tagged `bmp` with the stream offset and what was wrong.

### The two ambiguities, and what this codec does

- **16-bit `BI_RGB` is 5-5-5.** The format defines no 5-6-5 without
  `BI_BITFIELDS`, and `bmp_4x4_16bit_555.bmp` and `bmp_4x4_16bit_565.bmp` hold
  the same picture each way so that a decoder which conflates them fails
  visibly rather than plausibly.
- **The fourth byte of a 32-bit `BI_RGB` pixel is ignored.** The format leaves
  it undefined, and writers split roughly evenly between storing alpha there
  and storing zero, with nothing in the file to tell the two apart. The
  default is therefore to decode such an image opaque, which is what the
  format says the byte means and what GDI, Pillow, GdkPixbuf and netpbm all
  do. `GIMG_Load_Options.bmp_rgb32_alpha` = `GIMG_BMP_RGB32_ALPHA_HEURISTIC`
  reads it as alpha when any pixel in the image sets it, for a caller whose
  writers are known to put it there. A file that *declares* its alpha -
  `BI_BITFIELDS`, `BI_ALPHABITFIELDS`, or a V3 or later header - is unaffected
  by either: its mask is honored as written.

  This codec used to apply the heuristic by default. The reasoning was sound
  for the case it was written for - a file whose spare bytes are all zero must
  not decode invisible - but it does not follow that a non-zero byte means
  transparency, and a file whose spare bits are merely dirty came out full of
  holes. bmpsuite's `q/rgb32fakealpha.bmp` is exactly that file.

## Save

The form follows the raster and the caller's options, in this order:

| Raster | Written as | DIB header |
|---|---|---|
| Any pixel not opaque | 32-bit `BI_BITFIELDS`, masks `00FF0000` / `0000FF00` / `000000FF` / `FF000000` | 56-byte `BITMAPV3INFOHEADER` |
| Opaque, at most 256 distinct colors, and indexed is smaller | 1-, 4- or 8-bit indexed, `BI_RGB` or `BI_RLE8` | 40-byte `BITMAPINFOHEADER` |
| Anything else | 24-bit `BI_RGB` | 40-byte `BITMAPINFOHEADER` |

- **Palette.** `GIMG_Save_Options.bmp_palette` defaults to
  `GIMG_BMP_PALETTE_AUTO`, which stores an image through a palette when it is
  opaque, has no more than 256 distinct colors, and the indexed form is the
  smaller file - at the smallest depth that holds the indices, 1, 4 or 8. Like
  the PNG writer's palette that is a lossless choice and not color
  quantization: with 256 colors or fewer there is exactly one palette that
  reproduces the image, so nothing is decided about the picture, only about
  how it is stored. Unlike PNG's, which form is smaller is arithmetic rather
  than a measurement - a BMP's size follows from its stride and height, where
  DEFLATE makes PNG's unpredictable - so both forms need not be written to
  find out. `GIMG_BMP_PALETTE_NEVER` turns it off. An image with more colors
  than that stays true color: reducing them would be an image-processing
  decision and not a codec's. Transparency rules the palette out whatever the
  color count, because a BMP palette has no alpha.
- **Compression.** `GIMG_Save_Options.bmp_rle` defaults to
  `GIMG_BMP_RLE_NEVER`. `GIMG_BMP_RLE_AUTO` writes `BI_RLE8` for an 8-bit
  indexed image when the encoded rows come out smaller. It is opt-in rather
  than automatic because an uncompressed BMP is the most widely readable image
  there is, which is most of why the format is still worth writing.
- **Row order.** Bottom-up with a positive height by default, which every
  reader handles. `GIMG_Save_Options.bmp_top_down` writes them the other way,
  and is refused together with `GIMG_BMP_RLE_AUTO`: the format does not allow
  compression and top-down rows together, and writing the pair would produce
  something this library will not read back.
- **Alpha.** Written with explicit masks rather than as 32-bit `BI_RGB`, for
  the reason above: the fourth byte of a `BI_RGB` pixel is undefined and
  readers disagree about it, and a mask removes the question.
- **Padding:** each row is zero-padded to a 4-byte boundary.
- **Input formats:** `GIMG_PIXEL_RGBA8` and `GIMG_PIXEL_GRAY8`, the two 8-bit
  formats the library decodes to. A **12- or 16-bit** raster of the same
  channels - `GRAY12`, `GRAY16`, `RGBA12`, `RGBA16` - is restated at 8 bits
  and then written: a BMP sample is a byte at most, so there is nothing deeper
  to write it as, and refusing meant a 16-bit PNG could not be saved as a BMP
  at all. The narrowing is `round(v x 255 / max)`, the same rule the decoder
  uses widening a sub-byte channel, and it carries the color info across with
  it. Anything else - CMYK above all, where the four channels are ink amounts -
  is reported as `GIMG_ERR_UNSUPPORTED` rather than reinterpreted. A CMYK
  raster has an explicit route: `gimg_ops_convert_pixel_format` will turn one
  into RGBA, and the result saves here like any other. The writer does not do
  it on your behalf, because it is a color conversion and this library does
  not change an image's color without being asked.
- **Source of the pixels:** the raster attached to item 0 if there is one,
  otherwise the item is decoded and the result owned for the duration of the
  save. `GIMG_Save_Report.bytes_written` is the file size, and the tests
  assert it against what the stream actually received.
- **Resolution:** written from the document's common metadata when it states
  one, and left at zero - "not stated", which is legal and what most writers
  emit - when it does not. `GIMG_META_DROP_ALL` and `GIMG_META_KEEP_RAW_ONLY`
  drop it as they drop the rest.

### Color on save

A BMP says what its samples mean only in the header, so stating a color space
means writing a longer one. The version is chosen by what there is to say -
the inverse of the read above, and bounded the same way: only what
`GIMG_Color_Info` holds is written, and a color this model cannot state
produces no color header at all rather than the nearest thing it can say.

| Raster's `GIMG_Color_Info` | Header | What it carries |
|---|---|---|
| An ICC profile is attached | `BITMAPV5HEADER` (124) | `bV5CSType` = `PROFILE_EMBEDDED`, the profile after the pixel data, `bV5ProfileData` and `bV5ProfileSize` locating it |
| `transfer` is sRGB | `BITMAPV4HEADER` (108) | `bV4CSType` = `LCS_sRGB` |
| Known primaries, or a gamma, or linear | `BITMAPV4HEADER` (108) | `LCS_CALIBRATED_RGB`, the endpoints as xyY chromaticities, the gamma in 16.16 on all three channels |
| Any of the above with an intent other than perceptual | `BITMAPV5HEADER` (124) | as above plus `bV5Intent`; that field exists only in a V5 header |
| None of the above | `BITMAPINFOHEADER` (40), or `BITMAPV3INFOHEADER` (56) when alpha needs masks | nothing about color |

Either half of a calibrated header may be left at zero. A triple of zeros does
not sum to one and so reads back as an unnamed gamut; a gamma of zero reads
back as no transfer stated. Saying only the half that is known beats inventing
the other.

`LCS_sRGB` asserts the whole of sRGB, its transfer curve included, so it takes
the *transfer* actually saying so rather than the primaries - the same rule
the PNG writer applies to its `sRGB` chunk, and for the same reason:
bmpsuite's `g/pal8v4.bmp` names sRGB's primaries with a gamma of 2.2 and is
not an sRGB image.

`bV4Gamma` is 16.16 fixed point and so states nothing above 65535; a gamma
past that goes unsaid, for the same reason PNG's `gAMA` leaves one out.

An embedded profile is capped at **4 MiB** in both directions, which is what
the PNG and JPEG codecs allow and far above any real profile - a press profile
runs to a few hundred kilobytes. `bV5ProfileSize` is 32 bits, so without a
ceiling a file could name a profile of four gigabytes and the loader would try
to allocate it. A file naming one past the cap decodes untagged, and a raster
carrying one is written without it rather than into a file this codec could
not read whole; whatever else its color info states is still written.

The profile goes **after** the pixel data. Putting it before would make
`bfOffBits` depend on it, and every reader that ignores the profile still has
to find the pixels. `bV5ProfileData` is measured from the start of the DIB
header, which is where the reader here expects it.

`GIMG_META_DROP_ALL` and `GIMG_META_KEEP_RAW_ONLY` state no color space, as
they state no resolution.

Until this was written the writer emitted only a 40- or 56-byte header, so a
BMP loaded and saved as a BMP lost the color it arrived with - the one
conversion of the three that did not keep it. A profile also reaches a PNG
saved from the same document (\ref format_png "PNG"'s *Color on save*) and a
JPEG (\ref format_jpeg "JPEG"'s *Color on save*). A calibrated V4 header
states a gamut and a gamma while carrying no profile at all; that reaches a
PNG as `cHRM` and `gAMA`, and a JPEG as a profile synthesized to say it, since
APP2 is the only place a JPEG can. Pillow and GdkPixbuf both read the V4 and
V5 files this writes.

## Compliance checklist

Short reference for headers, depths, compression, and limitations. Update when
adding or restricting features.

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|
| **File header** | `BM` magic, `bfOffBits` honored for the pixel data | Magic other than `BM` &rarr; `GIMG_ERR_FORMAT`. `bfOffBits` below 14 or past the end of a sized stream &rarr; `GIMG_ERR_CORRUPT`. `bfSize` and the reserved words are not trusted |
| **DIB header** | Sizes 12, 40, 52, 56, 108 and 124 as Windows headers; every multiple of 4 from 16 to 64 as an OS/2 `BITMAPCOREHEADER2`. All normalized into one internal header | Any other size &rarr; `GIMG_ERR_UNSUPPORTED`. Guessing at an unknown size shifts everything after the header, so it is refused rather than approximated. 40, 52 and 56 belong to both vocabularies and are read as Windows |
| **Dimensions** | 16-bit unsigned (core header), 32-bit signed elsewhere; a negative height means top-down rows | Zero width or height &rarr; `GIMG_ERR_CORRUPT`; a width above `INT32_MAX`, which is a negative `biWidth`, &rarr; `GIMG_ERR_CORRUPT` |
| **Bit depth** | 1, 2, 4, 8 (palette), 16, 24, 32, 64 | Any other value &rarr; `GIMG_ERR_UNSUPPORTED`, except under `BI_JPEG` and `BI_PNG`, where the embedded stream carries its own depth and `biBitCount` is not a constraint on it |
| **Compression** | Windows: `BI_RGB`, `BI_RLE8`, `BI_RLE4`, `BI_BITFIELDS`, `BI_JPEG`, `BI_PNG`, `BI_ALPHABITFIELDS`. OS/2 2.x: RGB, RLE8, RLE4, RLE24, Huffman 1D | Huffman 1D at other than 1 bit per pixel &rarr; `GIMG_ERR_CORRUPT`; a stream that ends early or holds bits no T.4 code matches &rarr; `GIMG_ERR_CORRUPT`. Unknown values &rarr; `GIMG_ERR_UNSUPPORTED`. `BI_RLE8` at other than 8 bpp, `BI_RLE4` at other than 4, RLE24 at other than 24, or bitfields at other than 16 or 32 &rarr; `GIMG_ERR_CORRUPT`. RLE with a negative height &rarr; `GIMG_ERR_CORRUPT`: an RLE stream's end-of-line walks one way only |
| **Channel masks** | Read from a V2+ header, or from the words following a 40-byte header; alpha from a V3+ header or from `BI_ALPHABITFIELDS` | A red, green or blue mask of zero &rarr; `GIMG_ERR_CORRUPT`; a mask whose set bits are not one contiguous run &rarr; `GIMG_ERR_CORRUPT`, because there is no unambiguous shift to read it by. An alpha mask of zero is legal and means no alpha |
| **Palette** | 4-byte entries after an info header, 3-byte after a core header; `biClrUsed` of 0 means 2^bpp | A `biClrUsed` above 2^bpp is clamped to 2^bpp, not rejected: the surplus entries are unreachable, not wrong. An index past the entries the file carried &rarr; `GIMG_ERR_CORRUPT` on decode, since reading it would be an out-of-bounds read |
| **Pixel data** | `stride x height` bytes read from `bfOffBits`; stride is `(width x bpp + 31) / 32 x 4` in checked `size_t` arithmetic | A file shorter than that &rarr; the stream's error. A stride or buffer size that overflows &rarr; `GIMG_ERR_LIMIT`. `biSizeImage` is ignored except as an upper bound on an embedded stream |
| **Huffman 1D** | CCITT Group 3 one-dimensional, ITU-T T.4 Tables 1 and 2 plus the shared extended makeup codes. Expanded at load into the packed 1-bit rows an uncompressed image would have held, so nothing downstream knows the file was compressed | T.4 names its runs "white" and "black" and no BMP document says which palette index each is. Settled by measurement: `q/pal1huffmsb.bmp` and `g/pal1.bmp` are one picture stored twice, and only black = index 1 decodes them alike. Runs overhanging a line are clipped, as RLE's are |
| **RLE** | RLE8, RLE4 and RLE24: encoded runs, absolute runs padded to 16 bits, end-of-line, end-of-bitmap, delta. Overhanging runs are clipped | An absolute run or a delta that runs off the end of the data &rarr; `GIMG_ERR_CORRUPT`. RLE data needs a sized stream, since its length is not predictable from the header and it is taken from `bfOffBits` to the end of the file; an unsized stream &rarr; `GIMG_ERR_UNSUPPORTED` |
| **Embedded streams** | `BI_JPEG` and `BI_PNG` are loaded through that format's own codec and decoded by it, with the caller's load options unchanged | The payload goes to the codec the header named, never to the prober, so a wrapper cannot nest. A payload that is not of that format &rarr; that codec's error |
| **64 bits per pixel** | `BI_RGB` only, BGRA of s2.13 fixed point in linear light; each sample goes through the sRGB transfer function to reach the 8-bit raster | Microsoft publishes no specification for it; what is implemented is what bmplib and GIMP agree on, pinned by `q/rgba64.bmp` and `bmp_4x1_rgba64.bmp`. Any compression other than `BI_RGB` at 64bpp &rarr; `GIMG_ERR_CORRUPT`: no channel mask can describe a sample that is not an integer. Samples outside 0.0 to 1.0, which s2.13 can hold, clamp |
| **Color** | V4: `LCS_sRGB`, `LCS_WINDOWS_COLOR_SPACE`, `LCS_CALIBRATED_RGB` endpoints and gammas. V5: `bV5Intent`, `PROFILE_EMBEDDED` | `PROFILE_LINKED` names a file and is not followed - the image decodes untagged. A gamut matching neither sRGB nor Adobe RGB, or three gammas that disagree, leaves that field unknown rather than approximated. A profile running past the end of the file, or exceeding `max_memory`, yields no profile rather than no image |
| **Resolution** | `biXPelsPerMeter` / `biYPelsPerMeter` read into and written from the document's common metadata | Both axes must be stated: one alone describes a pixel's shape rather than its size. A negative value reads as "not stated" |
| **Document shape** | One item, decoded to `GIMG_PIXEL_RGBA8`; an OS/2 `BA` container is as many items as it holds entries | A plain BMP holds a single image, so an item index above 0 &rarr; `GIMG_ERR_UNSUPPORTED`. The `BA` exception is real: its entries are one picture rendered for different displays, and which to use is the caller's choice, not this codec's |
| **`BA` bitmap array** | The `'BA'` chain is walked and each entry loaded as the ordinary bitmap it is, through the same re-entry a `BI_JPEG` wrapper uses | An entry's `bfOffBits` counts from the start of the *container*, not the entry. The chain must advance: an `offNext` at or before the header holding it &rarr; `GIMG_ERR_CORRUPT`, which is also what makes a cycle impossible. At most 64 entries, and a container needs a sized stream since the chain is walked by absolute offset |
| **Limits** | `max_decoded_pixels` at load and at decode, `max_memory` on the pixel buffer and on an embedded ICC profile | Exceeded &rarr; `GIMG_ERR_LIMIT`, before the allocation rather than after |
| **Save** | 32-bit `BI_BITFIELDS` with a V3 header when alpha is present; 1-, 4- or 8-bit indexed, optionally `BI_RLE8`; 24-bit `BI_RGB` otherwise. Bottom-up or top-down. A V4 or V5 header when the raster states a color space | A 12- or 16-bit GRAY or RGBA raster is narrowed to 8 bits first; any other raster &rarr; `GIMG_ERR_UNSUPPORTED`. A zero dimension &rarr; `GIMG_ERR_FORMAT`. A file larger than `UINT32_MAX` &rarr; `GIMG_ERR_LIMIT`, since `bfSize` cannot describe it. Top-down together with RLE &rarr; `GIMG_ERR_UNSUPPORTED`. No RLE4, no RLE24, no 2-bit output |

## Where this codec differs from other decoders

Each is a place the format is silent, or an installed decoder is wrong, and
each is pinned by a fixture and named at the test that asserts it.

| Case | This codec | Elsewhere |
|---|---|---|
| 16-bit `BI_RGB` | 5-5-5, the only layout defined without `BI_BITFIELDS` | Decoders that assume 5-6-5 read green and red from the wrong bit spans |
| Expanding a sub-byte channel to 8 bits | `round(v x 255 / max)`, the rule PNG 13.12 states and the correctly rounded value | Pillow and netpbm truncate; GdkPixbuf replicates bits. All four agree at 0 and at the maximum and differ by at most 1 between, so no single one of them is a byte-exact reference for a 16-bit file |
| 32-bit `BI_RGB` fourth byte | Ignored by default; alpha only when the caller asks for the heuristic | The same, in every decoder reachable from here. This codec was previously the odd one out |
| A declared alpha mask | Honored as written | Pillow opens `bmp_4x4_32bit_alpha.bmp` as RGB and discards the alpha, so it is not a reference for this case |
| RLE4 | Each encoded run alternates the two nibbles of its value byte | Pillow decodes RLE4 incorrectly: on bmpsuite's `g/pal4rle.bmp` it disagrees with GdkPixbuf, with netpbm, and with a decoder written from the specification, all three of which agree with this library |
| RLE delta | Encoding resumes at the offset position; skipped pixels keep the raster's zeroed value | Pillow stops emitting after the delta in `bmp_8x2_rle8_delta.bmp`, so it is not a reference for this case either |
| RLE with top-down rows | Refused: the format does not allow the pair | Pillow decodes bmpsuite's `b/rletopdown.bmp` anyway. GdkPixbuf and netpbm refuse it, as this codec now does |

A `biClrUsed` larger than the depth allows is clamped rather than refused, and
a non-contiguous channel mask is refused rather than guessed at. Both are
judgments about which malformations are recoverable: an unreachable palette
entry changes no pixel, while a mask with a hole in it has no shift that would
reconstruct one.

## Tested scope

- **Fixtures** live in `tests/data/bmp/` - 42 BMP files, and beside them the
  JPEG and PNG payloads the two wrapper fixtures carry, so that a test can
  decode a wrapper and its payload separately. All are generated by
  `tests/data/bmp/generate.py` rather than vendored. Pillow writes the four
  variants it can, and everything else is assembled byte by byte, because the
  interesting cases for a decoder are exactly the ones common writers never
  produce: 5-5-5 against 5-6-5, top-down rows, the core and OS/2 2.x headers,
  the three RLE encodings, the V4 and V5 color fields, and the deliberately
  malformed files.
- **The pattern the fixtures carry** is a 4x4 block in which every component
  differs across the image, so a channel swap, a row flip or a stride error
  shows up as a specific wrong pixel rather than as a plausible-looking image.
  The 1-bit fixture is a checkerboard for the same reason: a shift error in
  the bit unpacking inverts it instead of producing something that still looks
  like a checkerboard.
- **The conformance corpus is bmpsuite**, Jason Summers' published BMP test
  suite, used as a development oracle from a scratch directory and not
  vendored - the same treatment PngSuite gets for PNG. `make bmpsuite
  BMPSUITE=<dir>` runs all 91 files through the codec and reports what agreed
  with what; today, 8 are refused as intended, 37 are checked against a
  decoder written from the specification, 82 are corroborated by another
  decoder, and nothing disagrees. `tools/oracle/fetch.sh` provides the corpus,
  at the commit `tools/oracle/VERSIONS` pins.
- **No other decoder is treated as the answer.** Four may be present - Pillow,
  GdkPixbuf, netpbm's `bmptopnm`, and bmplib, which is built from source by
  `tools/oracle/fetch.sh` because no package here ships it - and no two of them
  agree: see the differences table above. What checks the codec is a set of decoders
  written from the format description inside `bmpsuite_sweep.py`, for RLE4,
  RLE8, RLE24 and the `BI_BITFIELDS` channel layouts, with the installed ones
  as corroboration and each one's reach declared per file. They earn their
  place: `q/rgb16-231.bmp` and `q/rgb16-3103.bmp` have channel layouts none of
  the three installed decoders reads correctly, and `q/rgb24rle24.bmp` is
  refused by all of them. bmplib earns its place differently: it is the only
  one that reads OS/2 Huffman 1D, the OS/2 `BA` container and 64-bit files, the
  three the "Not implemented" section below names. It is used as a separate
  process and never linked, because it is LGPL/GPL and this library is not.
- **Properties that need no oracle at all.** `bmp_8x2_4bit.bmp` and
  `bmp_8x2_8bit.bmp` encode the same indices through the same palette, so
  their decoded output must be identical - which catches a defect in exactly
  one of the two unpacking paths, where comparing either against a reference
  separately would not. The same shape covers OS/2: one picture written under
  a 16-byte, a 40-byte and a 64-byte header must decode three times the same
  way. And a `BI_JPEG` or `BI_PNG` wrapper must decode to what its extracted
  payload decodes to on its own, which isolates the wrapper from the JPEG and
  PNG codecs whose own suites cover them. The sweep checks all three.
- **A file nothing can check is a failure** in the sweep, not a pass. An
  unverified pass is not a pass, and leaving those silent is how a sweep comes
  to mean less than it appears to.
- **Encoder output is read back by decoders that are not ours.** The encode
  tests leave each file they write in `tests/out/bmp/` with a sidecar holding
  the pixels it was meant to hold, and `verify_bmp_output.py` decodes them
  with Pillow and with GdkPixbuf; `make test` fails when they disagree. Our
  decoder agreeing with our encoder proves nothing about either - a channel
  swap, a row flip or a stride error that both halves share reads as success
  from the inside. Every form the writer can produce is covered: 1-, 4- and
  8-bit indexed, RLE8, top-down, 24-bit and 32-bit with alpha.
- **Both mask placements are covered:** the three masks following a 40-byte
  `BITMAPINFOHEADER` (`bmp_4x4_16bit_565.bmp`) and the four inside a 56-byte
  `BITMAPV3INFOHEADER` (`bmp_4x4_32bit_alpha.bmp`).
- **Malformed input** has its own fixtures and its own expected result each:
  wrong magic, zero width, an unknown header size, a non-contiguous mask,
  truncated pixel data, an offset past the end of the file, a palette index
  with no entry behind it, a top-down RLE bitmap, and an ICC profile pointing
  past the end of the file. A rejected file is also asserted to have said why,
  through the diagnostics.
- **Limits** are asserted in both directions: a `max_decoded_pixels` of 15 and
  a `max_memory` of 8 each reject the 4x4 fixture, limits that fit let it
  through and still decode correctly, and a limit smaller than an embedded
  PNG's payload refuses the wrapper that carries it.
- **Fuzzing.** `fuzz_bmp_load` and `fuzz_bmp_encode` share the corpus under
  `tests/fuzz/corpus` with the PNG and JPEG harnesses, built with ASan and
  UBSan and `-fno-sanitize-recover=undefined`. BMP is the parser in this
  library that most directly indexes a buffer from sizes the header supplied -
  the stride from `biWidth` and `biBitCount`, a palette index against an entry
  count, an RLE run against a row - so it is the one that most needs them.
  Because `gimg_doc_load` dispatches on the bytes rather than on the harness's
  name, `fuzz_bmp_encode` also feeds the BMP writer rasters decoded from PNG
  and JPEG.
- **One invariant ties the writer's choices together:** every fixture that
  loads is saved back under all eight combinations of palette, RLE and row
  order, and each result must load and decode to the same picture. It walks
  the fixture directory rather than a list, so a fixture added for some other
  reason is covered the moment it lands. Run over bmpsuite as well while this
  was written - 666 round trips across its 91 files - it found nothing, which
  is the answer that was wanted from it.
- **Coverage is read for the branches nobody tested, not for the number.**
  Doing that turned up three: `BI_ALPHABITFIELDS`, whose four masks follow a
  40-byte header rather than living inside it; the clamping of a `biClrUsed`
  larger than the indices can address; and the whole absolute-run branch of
  the RLE8 *encoder*, which the run-heavy fixture never reached because every
  row of it was runs from end to end. All three have fixtures now. What
  remains uncovered is allocation failure, I/O failure, and internal-error
  guards - paths that need fault injection to reach, and that the fuzzers
  exercise structurally.
- **Counts:** 63 tests in `testBmp_decode` - 3 in `BmpCodec`, 39 in
  `BmpDecode`, 18 in `BmpLoad` and 3 in `BmpToPng`, which carries the
  conversions that start from a BMP - and 43 in `testBmp_encode`. Both
  binaries are built and run under ASan and UBSan by `make test-asan` as
  well.
- **Defects this found, all now fixed:** a top-down RLE bitmap was decoded
  bottom-up and came out silently upside down; the undefined fourth byte of a
  32-bit `BI_RGB` pixel was read as alpha, which put holes in any file whose
  spare bits were merely dirty; a resolution was lost in both directions and a
  fabricated 72 dpi written in its place; and every OS/2 2.x header, every
  `BI_JPEG` and `BI_PNG` wrapper, and every V4 or V5 color field was refused
  or skipped.

@anchor format_bmp_gaps

## Not implemented

Listed so the absences are visible rather than discovered.

- **Following a linked color profile.** `PROFILE_LINKED` states a file path
  rather than carrying a profile. It is deliberately not followed: opening a
  path an image file names is acting on data, and is the shape of a directory
  traversal. Such a file decodes untagged. `PROFILE_EMBEDDED` is read.
- **Writing RLE4 and RLE24.** `BI_RLE8` is written on request; the other two
  are not. RLE4's alternating nibbles make it larger than RLE8 on most images
  that are not synthetic, and RLE24 is an OS/2 encoding Windows never reads.
- **Writing 2 bits per pixel.** It is read but never written: a Windows CE
  addition the desktop API does not accept, and an image that fits in four
  colors fits in 1 or 4 bits as well.
- **Writing a `BI_JPEG` or `BI_PNG` wrapper.** Both are read; neither is
  written. Wrapping a JPEG or a PNG inside a BMP produces a file most readers
  refuse - the wrapper is worth reading and not worth making, and a caller who
  wants a JPEG can save one.

---

Back to \ref format_references "Format and specification references".
