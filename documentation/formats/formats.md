@page image_format_references Formats


One page per format. Each says which external specification the codec
implements, which parts of it, where the implementation is deliberately
stricter or deliberately different from the reference decoders, and how every
claim on the page was checked. They are the authoritative reference for chunk
and segment layout, ordering, and behavior - ahead of the source comments,
which point back here.

## Formats

| Format | Page | Capabilities | Read | Write |
|---|---|---|---|---|
| PNG, including APNG | @subpage format_png "PNG and APNG" | read, write, animation, palette, ICC, 16 bpc | ISO/IEC 15948 and the APNG extension | all color types and depths; APNG |
| JPEG | @subpage format_jpeg "JPEG" | read, write, ICC, CMYK, 16 bpc | all fourteen frame headers of ITU-T T.81 | sequential, progressive, lossless and hierarchical, Huffman and arithmetic |
| BMP | @subpage format_bmp "BMP" | read, write, palette, ICC | the Windows and OS/2 DIB header versions, 1–32 and 64 bpp, RLE4/RLE8/RLE24, OS/2 Huffman 1D, bitfields, embedded JPEG and PNG, `BA` bitmap arrays as the several images they hold, V4/V5 colour with embedded or linked profiles | 1/2/4/8-bit indexed with optional RLE4/RLE8, 24-bit `BI_RGB`, 32-bit `BI_BITFIELDS` when alpha is present, OS/2 RLE24 and Huffman 1D, and `BI_JPEG`/`BI_PNG` wrappers on request |
| TIFF | @subpage format_tiff "TIFF" | read, write, palette, ICC, 16 bpc, CMYK | TIFF 6.0, both byte orders, strips and tiles, every bit depth from 1 to 32, grayscale in both polarities, palette, RGB, RGBA, separated and YCbCr at any subsampling, PlanarConfiguration 1 and 2, uncompressed, PackBits, LZW, Deflate, CCITT Group 3 and 4, JPEG in both spellings and ThunderScan, with the horizontal predictor, multi-page and pyramids, ICC, ImageDescription, XMP and Orientation | grayscale, RGB, RGBA and CMYK at 8 and 16 bits, one page per item, uncompressed or PackBits, LZW or Deflate with an optional predictor, in either byte order |
| GIF | @subpage format_gif "GIF" | read, write, animation, palette | GIF87a and GIF89a: the logical screen, global and local colour tables, interlacing, LZW at every minimum code size, graphic control with delay, disposal and transparency, comment extensions, and the NETSCAPE2.0 loop count | one image block per frame, cropped to the rectangle that changed, 1–256 colours, transparency, optional interlacing, animation with delays and a loop count, and comments |
| ICO / CUR | @subpage format_ico "ICO and CUR" | read, write | Windows icon and cursor directories; DIB (`BI_RGB`/`BI_BITFIELDS`) and PNG payloads; AND-mask alpha with zero-alpha fallback; CUR hotspots; up to `GIMG_ICO_MAX_ENTRIES` (64) alternates | one entry per `IMAGE`/`ALTERNATE` item; DIB or PNG per `ico_payload`; CUR when a hotspot is set; RLE, OS/2 Huffman, and embedded JPEG/PNG DIBs refused |
| WebP | @subpage format_webp "WebP" | read (container + VP8L) | RIFF/`WEBP`, `VP8X` canvas, chunk inventory including ANMF-nested bitstreams, `ICCP`/`EXIF`/`XMP ` carriage, VP8L lossless decode byte-identical to `dwebp` | not yet; Phase F will write lossless only |

Later WebP phases (ALPH, VP8 lossy, animation items, lossless encode) and any
other format still on the roadmap have no separate page until they land. The
WebP page already lists what remains. A page is written with the codec, not
after it.

## What each page contains

Every page answers the same questions in the same order, so that two formats
can be compared by reading the same heading twice. A page omits a section it
has nothing to put under - PNG has no "Not implemented" list because nothing
of the specification is knowingly absent from it.

- **Normative references** — the specification, its version, and the parts of
  it linked individually where the document is split up.
- **Parts implemented** — chunks, segments or headers; color models, bit
  depths, compression methods; the options that change what gets written, and
  any preservation or priority policy.
- **Save behavior** — what the encoder chooses, and why that choice rather
  than another.
- **Compliance checklist** — a table of area, what is supported, and what is
  rejected with which `GIMG_Result`.
- **Deviations** — where the codec is stricter than a reference decoder, or
  where it differs because the specification is silent. Named cases, each
  pinned by a fixture.
- **Tested scope** — the fixtures, how they were generated, the external
  oracles and the reach of each, the fuzz harnesses, and the gaps.
- **Not implemented** — what is absent, listed so it is visible rather than
  discovered.

Format-independent options - `GIMG_Load_Options`, `GIMG_Save_Options`,
`GIMG_Meta_Policy`, `GIMG_Limits`, `GIMG_Strictness` - are described in
\ref api_options "API Options and Types"; the pages here cover only what a
given format does with them.

## Adding a format

@subpage image_format_adding "Adding an image format" is the checklist and the page template:
what a new codec's documentation has to answer before the codec is considered
done, and the skeleton to copy.
