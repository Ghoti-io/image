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
- The 1-bit AND mask, applied as alpha. For 32-bpp XOR bitmaps whose alpha
  channel is identically zero, the AND mask is used instead (§5.3 of the
  implementation plan). Exact zero only — no threshold.
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
| `GIMG_ICO_PAYLOAD_AUTO` (0, default) | PNG when either dimension is greater than 128 or the source has non-trivial alpha; otherwise DIB |
| `GIMG_ICO_PAYLOAD_DIB` | every entry as a 32-bpp DIB with AND mask |
| `GIMG_ICO_PAYLOAD_PNG` | every entry as a PNG |

A single `GIMG_ITEM_IMAGE` becomes a one-entry icon. `IMAGE` plus
`ALTERNATE`s (or all `ALTERNATE`s) become one entry each. Any
`GIMG_ITEM_FRAME` returns `GIMG_ERR_UNSUPPORTED`. A non-zero hotspot on any
item writes `type = 2` (CUR).

## Compliance checklist

| Area | Supported | Rejected / limitation |
|------|-----------|------------------------|
| ICO and CUR directory | yes | count outside `1..GIMG_ICO_MAX_ENTRIES`, or an entry past EOF → `GIMG_ERR_CORRUPT` |
| DIB payloads | 1–32 bpp `BI_RGB` / `BI_BITFIELDS` | `BI_RLE4` / `BI_RLE8`, OS/2 Huffman 1D / RLE24, and embedded `BI_JPEG` / `BI_PNG` → `GIMG_ERR_UNSUPPORTED` |
| PNG payloads | via nested PNG codec | whatever PNG refuses; nested load and DIB load honour the caller's `max_decoded_pixels` |
| AND mask / alpha | yes, with zero-alpha fallback | — |
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

- Fixtures from `tests/data/ico/generate.py` (Pillow inside the pinned oracle
  image, plus hand-assembled DIBs and corrupt cases).
- Structure vs `icotool -l` when icoutils is available in the oracle image.
- Round-trip save/load of DIB payloads in `test_ico.cpp`.
- Fuzz: `fuzz_ico_load`, `fuzz_ico_encode` with seeds under
  `tests/fuzz/corpus/ico_{load,encode}/`.

## Not implemented

- Reading icons from PE resources (`.exe` / `.dll`).
- RLE, OS/2 Huffman 1D, OS/2 RLE24, or embedded JPEG/PNG DIB payloads
  inside icons. Compression 3 and 4 on a Windows header size remain
  `BI_BITFIELDS` / `BI_JPEG` as BMP reads them; on an OS/2 header size they
  are Huffman 1D / RLE24 and are refused before the nested BMP path expands
  them.
