@page format_references Format and Specification References

# Format and specification references

One page per format. Each says which external specification the codec
implements, which parts of it, where the implementation is deliberately
stricter or deliberately different from the reference decoders, and how every
claim on the page was checked. They are the authoritative reference for chunk
and segment layout, ordering, and behavior - ahead of the source comments,
which point back here.

## Formats

| Format | Page | Capabilities | Read | Write |
|---|---|---|---|---|
| PNG, including APNG | \ref format_png "PNG and APNG" | read, write, animation, palette, ICC, 16 bpc | ISO/IEC 15948 and the APNG extension | all color types and depths; APNG |
| JPEG | \ref format_jpeg "JPEG" | read, write, ICC, CMYK, 16 bpc | all fourteen frame headers of ITU-T T.81 | sequential, progressive, lossless and hierarchical, Huffman and arithmetic |
| BMP | \ref format_bmp "BMP" | read, write, palette, ICC | the Windows and OS/2 DIB header versions, 1–32 bpp, RLE4/RLE8/RLE24, bitfields, embedded JPEG and PNG, V4/V5 colour | 1/4/8-bit indexed with optional RLE8, 24-bit `BI_RGB`, or 32-bit `BI_BITFIELDS` when alpha is present |

Formats with no codec yet - GIF, TIFF, WebP and the rest of the roadmap - have
no page here. A page is written with the codec, not after it.

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

\ref format_adding "Adding a format" is the checklist and the page template:
what a new codec's documentation has to answer before the codec is considered
done, and the skeleton to copy.
