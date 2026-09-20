@page format_bmp BMP

# BMP

What this codec implements of the Windows and OS/2 device-independent bitmap,
where it deliberately differs from other decoders, and how each claim was
checked. The library's format index is the
\ref format_references "format and specification references".

BMP has no ISO or W3C standard. What exists is Microsoft's documentation of
the GDI structures a `.bmp` file is a serialization of, plus the OS/2
Presentation Manager bitmap that the 12-byte header comes from. Where the
documentation is silent - and it is silent about the two questions a decoder
most needs answered, the meaning of 16-bit `BI_RGB` and of the fourth byte of
a 32-bit `BI_RGB` pixel - this page says what this codec does and why.

## Normative references

- **File and header structures:** `BITMAPFILEHEADER`, `BITMAPCOREHEADER`,
  `BITMAPINFOHEADER`, `BITMAPV4HEADER`, `BITMAPV5HEADER` -
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-storage> and
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-header-types>
- **Header fields:**
  <https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-bitmapinfoheader>
- **Run-length encoding (BI_RLE8, BI_RLE4):**
  <https://learn.microsoft.com/windows/win32/gdi/bitmap-compression>
- **OS/2 1.x bitmap:** the 12-byte `BITMAPCOREHEADER` with 16-bit dimensions
  and 3-byte palette entries.

The BMP codec uses the document/codec allocator for every codec-owned
allocation: the palette, the stored pixel bytes, the document state, and the
encoder's row buffer.

## Parts implemented

- **File header (14 bytes):** the `BM` signature is both the probe magic and
  the first thing load checks. `bfOffBits` is honored - pixel data is read
  from wherever it points, not from immediately after the palette, because the
  gap between the two is legal and common. `bfSize` and the two reserved words
  are read past and not trusted for anything.
- **DIB headers:** recognized by the size field - 12
  (`BITMAPCOREHEADER`), 40 (`BITMAPINFOHEADER`), 52 (`BITMAPV2INFOHEADER`), 56
  (`BITMAPV3INFOHEADER`), 108 (`BITMAPV4HEADER`) and 124 (`BITMAPV5HEADER`).
  All six are normalized into one internal header, so nothing downstream knows
  which version it came from. The fields only V4 and V5 carry - color space
  endpoints, per-channel gamma, rendering intent, embedded ICC profile - are
  skipped rather than interpreted (see \ref format_bmp_gaps "Not implemented").
- **Dimensions:** 16-bit unsigned in the core header, 32-bit signed
  everywhere else. A negative height means the rows are stored top to bottom;
  decode honors the sign rather than flipping the image.
- **Bit depths:** 1, 2, 4, 8, 16, 24 and 32. Depths of 8 and below index a
  palette; 16 and 32 extract their channels through masks; 24 is a fixed BGR
  triple.
- **Compression:** `BI_RGB` (0), `BI_RLE8` (1), `BI_RLE4` (2), `BI_BITFIELDS`
  (3) and `BI_ALPHABITFIELDS` (6). Each is checked against the bit depth it
  requires, because a file that disagrees with itself cannot be decoded by
  guessing which half to believe.
- **Channel masks:** `BI_BITFIELDS` supplies red, green and blue masks and
  `BI_ALPHABITFIELDS` adds a fourth. A V2 or later header carries them inside
  itself; a plain 40-byte `BITMAPINFOHEADER` stores them in the three (or
  four) 32-bit words that follow the header, which is where the palette would
  otherwise begin. Both placements are read. A mask must be a single
  contiguous run of set bits: anything else has no unambiguous shift.
- **Implied masks for BI_RGB:** 16-bit is **5-5-5** with the top bit unused,
  24-bit is BGR, and 32-bit is 8-8-8 with the fourth byte a *candidate* alpha
  (see below). A writer that wants 5-6-5 must declare `BI_BITFIELDS`, which is
  what the format requires of it; treating every 16-bit file as 5-6-5 shifts
  green and red and is the single most common BMP decoding defect.
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
  is ignored entirely: it is attacker-controlled, and it is zero in a large
  share of real files.
- **RLE8 and RLE4:** encoded runs, absolute runs padded to a 16-bit boundary,
  end-of-line, end-of-bitmap, and the two-byte delta. An RLE4 encoded run
  alternates the two nibbles of its value byte, so `0x12` is 1, 2, 1, 2 and
  not four copies of one index. A run that overhangs its row is clipped rather
  than treated as corruption - encoders legitimately emit them - and the
  decoder refuses to advance past the last row, so no hostile stream can write
  outside the raster. Pixels a delta skipped are never written and keep the
  raster's zeroed value, which is what the format specifies for them.
- **Row order:** rows are written into the raster top-down whichever way the
  file stored them, by choosing the destination row rather than reversing the
  buffer afterwards.
- **Output:** always `GIMG_PIXEL_RGBA8`, one item per document. A sample
  narrower than 8 bits is expanded by `round(value x 255 / max)` rather than
  by truncation, so the maximum input maps to 255 and the midpoint does not
  drift downward.
- **Limits and diagnostics:** `max_decoded_pixels` is checked at load, before
  any large allocation, and again at decode; `max_memory` caps the pixel
  buffer. A rejected file appends a diagnostic tagged `bmp` with the stream
  offset and what was wrong.

### The two ambiguities, and what this codec does

- **16-bit `BI_RGB` is 5-5-5.** The format defines no 5-6-5 without
  `BI_BITFIELDS`, and `bmp_4x4_16bit_555.bmp` and `bmp_4x4_16bit_565.bmp` hold
  the same picture each way so that a decoder which conflates them fails
  visibly rather than plausibly.
- **A 32-bit `BI_RGB` fourth byte is alpha only if something set it.** The
  format leaves the byte undefined and writers split roughly evenly between
  storing alpha there and storing zero. Honoring a zero byte would decode the
  whole image as fully transparent, so for `BI_RGB` the high bytes are scanned
  first and the channel is treated as opaque when every one of them is zero.
  An explicit `BI_BITFIELDS` alpha mask is always honored as it stands - a
  writer that declared the mask meant it.

## Save

A saved BMP is always uncompressed, and its depth follows the raster's alpha:

| Raster | Written as | DIB header |
|---|---|---|
| Every pixel opaque | 24-bit `BI_RGB` | 40-byte `BITMAPINFOHEADER` |
| Any pixel not opaque | 32-bit `BI_BITFIELDS`, masks `00FF0000` / `0000FF00` / `000000FF` / `FF000000` | 56-byte `BITMAPV3INFOHEADER` |

24-bit `BI_RGB` is the most widely readable BMP there is, so it is the default
for anything that does not need more. Alpha is written with explicit masks
rather than as 32-bit `BI_RGB` for the reason above: the fourth byte of a
`BI_RGB` pixel is undefined and readers disagree about it, and a mask removes
the question.

- **Row order:** bottom-up with a positive height, which every reader handles.
  Top-down files are legal but less portable, and are illegal in combination
  with compression.
- **Padding:** each row is zero-padded to a 4-byte boundary.
- **Input formats:** `GIMG_PIXEL_RGBA8` and `GIMG_PIXEL_GRAY8`, the two 8-bit
  formats the library decodes to. Anything else is reported as
  `GIMG_ERR_UNSUPPORTED` rather than reinterpreted.
- **Source of the pixels:** the raster attached to item 0 if there is one,
  otherwise the item is decoded and the result owned for the duration of the
  save. `GIMG_Save_Report.bytes_written` is the file size, and the tests
  assert it against what the stream actually received.
- **Resolution:** `biXPelsPerMeter` and `biYPelsPerMeter` are written as 2835,
  which is 72 dpi. This is a constant, not the document's metadata; see
  \ref format_bmp_gaps "Not implemented".
- **Save options are not consulted.** `GIMG_Save_Options`, `metadata_policy`
  included, has no effect: this codec reads no metadata and writes none, so
  every policy would produce the same bytes.

## Compliance checklist

Short reference for headers, depths, compression, and limitations. Update when
adding or restricting features.

| Area | Supported | Rejected / limitation |
|------|-----------|-----------------------|
| **File header** | `BM` magic, `bfOffBits` honored for the pixel data | Magic other than `BM` &rarr; `GIMG_ERR_FORMAT`. `bfOffBits` below 14 or past the end of a sized stream &rarr; `GIMG_ERR_CORRUPT`. `bfSize` and the reserved words are not trusted |
| **DIB header** | Sizes 12, 40, 52, 56, 108 and 124, normalized into one internal header | Any other size &rarr; `GIMG_ERR_UNSUPPORTED`. Guessing at an unknown size shifts everything after the header, so it is refused rather than approximated. This includes the 64-byte OS/2 `BITMAPCOREHEADER2` |
| **Dimensions** | 16-bit unsigned (core header), 32-bit signed elsewhere; a negative height means top-down rows | Zero width or height &rarr; `GIMG_ERR_CORRUPT`; a width above `INT32_MAX`, which is a negative `biWidth`, &rarr; `GIMG_ERR_CORRUPT` |
| **Bit depth** | 1, 2, 4, 8 (palette), 16, 24, 32 | Any other value &rarr; `GIMG_ERR_UNSUPPORTED` |
| **Compression** | `BI_RGB`, `BI_RLE8`, `BI_RLE4`, `BI_BITFIELDS`, `BI_ALPHABITFIELDS` | `BI_JPEG` (4), `BI_PNG` (5) and unknown values &rarr; `GIMG_ERR_UNSUPPORTED`. `BI_RLE8` at other than 8 bpp, `BI_RLE4` at other than 4, or bitfields at other than 16 or 32 &rarr; `GIMG_ERR_CORRUPT` |
| **Channel masks** | Read from a V2+ header, or from the words following a 40-byte header; alpha from a V3+ header or from `BI_ALPHABITFIELDS` | A red, green or blue mask of zero &rarr; `GIMG_ERR_CORRUPT`; a mask whose set bits are not one contiguous run &rarr; `GIMG_ERR_CORRUPT`, because there is no unambiguous shift to read it by. An alpha mask of zero is legal and means no alpha |
| **Palette** | 4-byte entries after an info header, 3-byte after a core header; `biClrUsed` of 0 means 2^bpp | A `biClrUsed` above 2^bpp is clamped to 2^bpp, not rejected: the surplus entries are unreachable, not wrong. An index past the entries the file carried &rarr; `GIMG_ERR_CORRUPT` on decode, since reading it would be an out-of-bounds read |
| **Pixel data** | `stride x height` bytes read from `bfOffBits`; stride is `(width x bpp + 31) / 32 x 4` in checked `size_t` arithmetic | A file shorter than that &rarr; the stream's error. A stride or buffer size that overflows &rarr; `GIMG_ERR_LIMIT`. `biSizeImage` is ignored |
| **RLE** | Encoded runs, absolute runs padded to 16 bits, end-of-line, end-of-bitmap, delta. Overhanging runs are clipped | An absolute run or a delta that runs off the end of the data &rarr; `GIMG_ERR_CORRUPT`. RLE data needs a sized stream, since its length is not predictable from the header and it is taken from `bfOffBits` to the end of the file; an unsized stream &rarr; `GIMG_ERR_UNSUPPORTED` |
| **Document shape** | One item, decoded to `GIMG_PIXEL_RGBA8` | BMP holds a single image; an item index above 0 &rarr; `GIMG_ERR_UNSUPPORTED` |
| **Limits** | `max_decoded_pixels` at load and at decode, `max_memory` on the pixel buffer | Exceeded &rarr; `GIMG_ERR_LIMIT`, before the allocation rather than after |
| **Save** | 24-bit `BI_RGB`, or 32-bit `BI_BITFIELDS` with a V3 header when alpha is present; bottom-up rows | A raster that is not `RGBA8` or `GRAY8` &rarr; `GIMG_ERR_UNSUPPORTED`. A zero dimension &rarr; `GIMG_ERR_FORMAT`. A file larger than `UINT32_MAX` &rarr; `GIMG_ERR_LIMIT`, since `bfSize` cannot describe it. No palette, no RLE, no top-down output |

## Where this codec differs from other decoders

Four cases, each one a place the format is silent and implementations have
chosen differently. Each is pinned by a fixture and named at the test that
asserts it.

| Case | This codec | Elsewhere |
|---|---|---|
| 16-bit `BI_RGB` | 5-5-5, the only layout defined without `BI_BITFIELDS` | Decoders that assume 5-6-5 read green and red from the wrong bit spans |
| 32-bit `BI_RGB` with every high byte zero | Opaque | Honoring the byte decodes the whole image as transparent |
| A `BI_BITFIELDS` alpha mask | Honored as declared | Pillow opens `bmp_4x4_32bit_alpha.bmp` as RGB and discards the alpha, so it is not a reference for this case |
| RLE delta | Encoding resumes at the offset position; skipped pixels stay at the raster's zeroed value | Pillow stops emitting after the delta in `bmp_8x2_rle8_delta.bmp`, so it is not a reference for this case either |

A `biClrUsed` larger than the depth allows is clamped rather than refused, and
a non-contiguous channel mask is refused rather than guessed at. Both are
judgments about which malformations are recoverable: an unreachable palette
entry changes no pixel, while a mask with a hole in it has no shift that would
reconstruct one.

## Tested scope

- **Fixtures** live in `tests/data/bmp/` and are generated by
  `tests/data/bmp/generate.py` rather than vendored. Pillow writes the four
  variants it can - 24-bit, 8-bit palette, 1x1 and 1-bit - and everything else
  is assembled byte by byte, because the interesting cases for a decoder are
  exactly the ones common writers never produce: 5-5-5 against 5-6-5,
  top-down rows, the core header, the RLE encodings, and the deliberately
  malformed files.
- **The pattern the fixtures carry** is a 4x4 block in which every component
  differs across the image, so a channel swap, a row flip or a stride error
  shows up as a specific wrong pixel rather than as a plausible-looking image.
  The 1-bit fixture is a checkerboard for the same reason: a shift error in
  the bit unpacking inverts it instead of producing something that still looks
  like a checkerboard.
- **Cross-checked against Pillow** for the valid fixtures, with the two cases
  where this decoder deliberately differs called out at the tests concerned
  rather than silently tolerated.
- **A property that needs no oracle:** `bmp_8x2_4bit.bmp` and
  `bmp_8x2_8bit.bmp` encode the same indices through the same palette, so
  their decoded output must be identical. That catches a defect in exactly one
  of the two unpacking paths, which comparing either against a reference
  separately would not.
- **Both mask placements are covered:** the three masks following a 40-byte
  `BITMAPINFOHEADER` (`bmp_4x4_16bit_565.bmp`) and the four inside a 56-byte
  `BITMAPV3INFOHEADER` (`bmp_4x4_32bit_alpha.bmp`).
- **Malformed input** has its own fixtures and its own expected result each:
  wrong magic, zero width, an unknown header size, a non-contiguous mask,
  truncated pixel data, an offset past the end of the file, and a palette
  index with no entry behind it. A rejected file is also asserted to have said
  why, through the diagnostics.
- **Limits** are asserted in both directions: a `max_decoded_pixels` of 15 and
  a `max_memory` of 8 each reject the 4x4 fixture, and limits that fit let it
  through and still decode correctly.
- **Encode** is checked for the header fields it writes (depth, compression,
  header size, masks) and for round-trip fidelity: opaque exact, alpha exact,
  a single pixel, a loaded file re-saved to the same pixels, and a palette
  fixture surviving a save and load as 24-bit.
- **Counts:** 31 tests in `testBmp_decode` across `BmpCodec`, `BmpDecode` and
  `BmpLoad`, and 7 in `testBmp_encode`. Both binaries are built and run under
  ASan and UBSan by `make test-asan` as well.

@anchor format_bmp_gaps

## Not implemented

Listed so the absences are visible rather than discovered.

- **`BI_JPEG` and `BI_PNG`.** A BMP may wrap a whole JPEG or PNG stream as its
  "pixel data". Both are refused with `GIMG_ERR_UNSUPPORTED`. The library has
  codecs for both formats, so handing the payload to one of them is the
  obvious implementation; nothing depends on it yet.
- **OS/2 Huffman 1D.** `ulCompression` 3 in an OS/2 2.x header is CCITT
  Group 3 one-dimensional Huffman coding, not `BI_BITFIELDS`. It is
  recognized as such and refused with `GIMG_ERR_UNSUPPORTED` rather than
  misread as a channel layout; bmpsuite's `q/pal1huffmsb.bmp` is the case.
  No other decoder reachable from here implements it either.
- **Color management.** The color space endpoints, per-channel gamma,
  rendering intent and embedded ICC profile of a V4 or V5 header are skipped,
  and no `GIMG_Color_Info` is attached to the decoded raster. An image
  therefore decodes as untagged, which for the overwhelming majority of BMP
  files - which are `LCS_sRGB` or carry no color block at all - is what it
  is, but a V5 file with a real profile loses it.
- **Writing anything but 24- and 32-bit uncompressed.** No palette is ever
  written, so an indexed file that is loaded and saved comes back as 24-bit;
  no RLE is ever written; no top-down output.
- **2 bits per pixel is accepted but has no fixture.** The depth is a Windows
  CE addition and decodes through the same path as 1, 4 and 8, but nothing in
  `tests/data/bmp/` exercises it.
- **No fuzz harness.** PNG and JPEG each have load and encode harnesses under
  `tests/fuzz/`; BMP has none, and its parser is the one in this library that
  most directly indexes a buffer from header-supplied sizes. `fuzz_bmp_load`
  is the gap most worth closing.
- **No external verification of encoder output.** The build defines
  `GIMG_TEST_OUT_BMP` and nothing writes to it. PNG and JPEG both check what
  they wrote with a decoder that is not ours (`verify_png_output.py`,
  `verify_jpeg_output.py`); BMP round-trips only through itself, which proves
  the pair consistent rather than either correct.
- **No conformance corpus.** There is no BMP equivalent of PngSuite in use.
  Jason Summers' `bmpsuite` is the obvious candidate and would exercise the
  header versions and malformations far past what the hand-written fixtures
  reach.

---

Back to \ref format_references "Format and specification references".
