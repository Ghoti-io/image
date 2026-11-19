@page format_ico ICO and CUR

# ICO and CUR

One codec named `"ico"` reads and writes both Windows Icon (`type = 1`) and
Cursor (`type = 2`) files. The claims below are checked; see
\ref image_format_references "Format and specification references".

## Normative references

- **Specification:** there is no ISO standard. The layout is the Microsoft
  Windows Icon and Cursor resource format: a six-byte `ICONDIR`, sixteen-byte
  `ICONDIRENTRY` records, and per-entry payloads that are either a headerless
  DIB (starting at `BITMAPINFOHEADER`) or a complete PNG file. CUR reuses the
  same bytes; the two fields ICO uses for colour planes and bit depth hold the
  hotspot instead.
- **Entry cap:** `#define GIMG_ICO_MAX_ENTRIES 64u`. These are alternates of one
  picture, not animation frames, so `max_frame_count` does not apply.

Codec-owned allocations use the codec's allocator (the library default when
none was set).

## Parts implemented

- `ICONDIR` / `ICONDIRENTRY` for both ICO and CUR.
- DIB payloads at 1, 4, 8, 16, 24 and 32 bpp, `BI_RGB` and `BI_BITFIELDS`, via
  `gimg_bmp_load_dib` with a height override so only the XOR bitmap is read.
  The BMP reader's wider set is also accepted, because nothing here narrows
  it: 2 bpp, 64 bpp `BI_RGB`, V4 and V5 headers, and the OS/2 2.x header sizes
  (44, 48, 60, 64) with compression 0. Writers do not produce those for icons;
  they are tolerated, not part of the format.
- The colour table. It is as long as `biClrUsed` says (2^bpp when that is 0 at
  8 bpp or below) and the pixels follow it. Above 8 bpp the table is optional
  but is skipped when stated, and entries beyond 2^bpp at 8 bpp or below are
  unreachable but also skipped, so the pixels are found where the writer put
  them. A table longer than the entry is `GIMG_ERR_CORRUPT`.
- Height. `biHeight` is twice the image height (XOR plus AND), and is halved.
  When the directory names a width and height equal to `biWidth` and
  `biHeight` themselves, the writer
  stored the real height; the bitmap is then the whole image (a mask that
  follows is still read). Otherwise an even `biHeight` is halved whatever the
  directory says, as Windows does, and a disagreement is a warning.
- The 1-bit AND mask, applied as alpha. For 32-bpp XOR bitmaps whose alpha
  channel is identically zero, the AND mask is used instead (§5.3 of the
  implementation plan). Exact zero only — no threshold.
- Row order. A negative `biHeight` (top-down DIB) is honoured for the XOR
  bitmap, its alpha bytes and the AND mask alike: the mask is stored in the
  same row order as the XOR bitmap it follows.
- Which XOR bitmaps have an alpha channel. A 32-bpp `BI_RGB` entry carries
  alpha in the fourth byte of each pixel. A 32-bpp `BI_BITFIELDS` or V4/V5
  entry carries it where its alpha mask says, and has none if the mask is
  empty; such an entry is then treated like a 1-24 bpp one, and the AND mask
  is its transparency.
- Absent or short masks. An entry with no alpha channel and no AND mask at all
  is opaque; one whose mask is present but shorter than the image needs is
  `GIMG_ERR_CORRUPT`. An entry with an alpha channel never needs the mask
  unless the channel is identically zero, so a short or absent mask is not an
  error there: with real alpha it is ignored, and with all-zero alpha it
  leaves the image opaque (the writer set neither, so nothing says otherwise).
- PNG payloads, loaded through the nested PNG codec (by name, not by probe).
- Directory dimensions and bpp are advisory: the payload is trusted, and a
  `GIMG_DIAG_WARNING` is recorded when they disagree.
- CUR hotspots on the item (`gimg_item_hotspot` / `gimg_item_set_hotspot`).
- Items: entry 0 is `GIMG_ITEM_IMAGE`; the rest are `GIMG_ITEM_ALTERNATE` of
  subject 0.

## Save

`GIMG_Save_Options.ico_payload`:

| Value | Behaviour |
|-------|-----------|
| `GIMG_ICO_PAYLOAD_AUTO` (0, default) | PNG when either dimension is greater than 128 or the source has non-trivial alpha; otherwise DIB. A 12- or 16-bit unsigned gray, RGB or RGBA raster is narrowed to 8 bits first, so the alpha test and the DIB see what is written; no other conversion is made |
| `GIMG_ICO_PAYLOAD_DIB` | every entry as a 32-bpp DIB with AND mask; a raster that is not 8-bit interleaved gray, RGB or RGBA (planar, float, CMYK) is `GIMG_ERR_UNSUPPORTED` |
| `GIMG_ICO_PAYLOAD_PNG` | every entry as a PNG |

A single `GIMG_ITEM_IMAGE` becomes a one-entry icon. `IMAGE` plus
`ALTERNATE`s (or all `ALTERNATE`s) become one entry each. Any
`GIMG_ITEM_FRAME` returns `GIMG_ERR_UNSUPPORTED`. A non-zero hotspot on any
item writes `type = 2` (CUR), and so does a document loaded from a CUR file
whose hotspots are all (0, 0).

An icon holds pictures and nothing else: the metadata kinds, the loop count and
the frame delays have no place, an item whose role is a thumbnail or a level is
skipped, and a save with `GIMG_Save_Report.diagnostics` set names each one
(@ref api_options_save_loss). The hotspot is written, which is what makes the
file a cursor.

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|------------------------|
| ICO and CUR directory | yes | count outside `1..GIMG_ICO_MAX_ENTRIES`, or an entry past EOF → `GIMG_ERR_CORRUPT` |
| DIB payloads | 1–32 bpp `BI_RGB` / `BI_BITFIELDS` (2 and 64 bpp, V4/V5 and OS/2 2.x header sizes are also read, see above) | `BI_RLE4` / `BI_RLE8`, OS/2 Huffman 1D / RLE24, and embedded `BI_JPEG` / `BI_PNG` → `GIMG_ERR_UNSUPPORTED` |
| PNG payloads | via nested PNG codec | whatever PNG refuses; nested load and DIB load honour the caller's `max_decoded_pixels` |
| AND mask / alpha | yes, with zero-alpha fallback; top-down DIBs | a short mask on an entry that needs it → `GIMG_ERR_CORRUPT` |
| Hotspot | per-item API | — |
| Animation as icon | — | `GIMG_ITEM_FRAME` on save → `GIMG_ERR_UNSUPPORTED` |

## Where this codec differs from reference readers

- **32-bpp alpha fallback.** If every alpha byte of a 32-bpp XOR bitmap is
  zero, the AND mask supplies transparency. Pillow and modern writers usually
  leave AND empty and put real alpha in the XOR; older writers do the opposite.
  Fixtures: `ico_zero_alpha_and.ico` and ordinary 32-bpp DIBs.
- **Directory vs payload.** Dimensions in the directory can disagree with the
  DIB or PNG. This codec trusts the payload and warns. Fixture:
  `ico_dir_mismatch.ico`.
- **Which entry is "the" icon.** Outside readers disagree. Comparisons are
  always per entry, never against a single chosen size.

## Tested scope

- Fixtures from `tests/data/ico/generate.py` (Pillow and ImageMagick inside the
  pinned oracle image, plus hand-assembled DIBs and corrupt cases).
- Outside-written fixtures fetched by `tools/oracle/fetch.sh ico-refs` into
  `third_party/ico-refs/` at the commits `tools/oracle/VERSIONS` names: Pillow's
  `pillow.ico` (MIT-CMU; mixed PNG + 32-bpp DIB) and Wine's
  `dlls/shell32/resources/blank.ico` as `wine_blank.ico` (LGPL-2.1+; mixed PNG
  + 1-bpp DIBs). Neither is committed. `pillow.ico` is skipped for the
  `icotool -l` structure gate: icotool rejects Pillow's oversized
  `dwBytesInRes` on the 48×48 DIB and then lists only the PNG entry; the pixel
  gate still covers all five entries.
- Structure vs `icotool -l` when icoutils is available in the oracle image.
- Per-entry pixels vs Pillow, ImageMagick and GdkPixbuf
  (`verify_ico_pixels.py`). GdkPixbuf refuses PNG-compressed icons and
  mis-reads mixed DIB+PNG files; those entries are checked by the other two.
  `ico_zero_alpha_and.ico` is the intentional §5.3 deviation (outside readers
  leave XOR alpha at zero; this codec restores the AND mask) and is not
  compared. Pillow's ICO path also disagrees with this library and ImageMagick
  on `wine_blank.ico` entry 0 (1-bit greyscale PNG with tRNS); that entry is
  checked by ImageMagick only.
- Encoder output vs the same three readers (`verify_ico_output.py`), with
  files published by the encode tests into `tests/out/ico/`.
- Round-trip save/load of DIB payloads in `test_ico.cpp`.
- Colour tables above 8 bpp, an undoubled `biHeight`, and the writer's refusal of
  rasters a DIB cannot hold, from hand-built entries in
  `tests/codec/test_ico_shape_and_writer.cpp`.
- Fuzz: `fuzz_ico_load`, `fuzz_ico_encode` with seeds under
  `tests/fuzz/corpus/ico_{load,encode}/`.

## Not implemented

- Reading icons from PE resources (`.exe` / `.dll`).
- RLE, OS/2 Huffman 1D, OS/2 RLE24, or embedded JPEG/PNG DIB payloads
  inside icons. Compression 3 and 4 on a Windows header size remain
  `BI_BITFIELDS` / `BI_JPEG` as BMP reads them; on an OS/2 header size they
  are Huffman 1D / RLE24 and are refused before the nested BMP path expands
  them.
