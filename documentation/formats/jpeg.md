@page format_jpeg JPEG

# JPEG

What this codec implements of ITU-T T.81, how each claim was checked, and what
is bounded rather than absent. The library's format index is
\ref image_format_references "Format and specification references".

## Normative references

- **Specification:** ISO/IEC 10918-1 (ITU-T T.81) — Information technology – Digital compression and coding of continuous-tone still images: Requirements and guidelines.

The JPEG codec uses the document/codec allocator for all codec-owned
allocations.

## Scope

All fourteen frame headers of T.81 Table B.1 are read: sequential (SOF0, SOF1), progressive (SOF2), lossless (SOF3), their differential counterparts for a hierarchical sequence (SOF5–SOF7), and the arithmetic-coded form of every one of those (SOF9–SOF11, SOF13–SOF15). Both entropy coders the standard defines — Huffman (Annex F, DHT) and adaptive binary arithmetic (Annex D, DAC). Both scan arrangements of Annex A — interleaved (A.2.2) and one scan per component (A.2.3) — read and written. Sample precision 8 and 12 for a DCT frame, 2 to 16 for a lossless one, which is what Table B.2 allows. DNL (B.2.5), including the Y = 0 case where it supplies the height. DRI/RST; COM and APP0–APP15 preservation; JFIF thumbnail; chroma subsampling; successive-approximation refinement scans (Ah>0).

## Coding processes

- **Arithmetic coding (T.81 Annex D):** symmetric, and not limited to sequential and progressive frames. Decode handles SOF9, SOF10, SOF11 (lossless) and SOF13–SOF15 (the differential frames of a hierarchical sequence) at both precisions. Encode writes the arithmetic counterpart of whatever process was asked for when `GIMG_Save_Options.jpeg_arithmetic` is set — SOF9 sequential, SOF10 progressive, SOF11 lossless, SOF9/SOF13 for a hierarchical pyramid — with a DAC segment in place of DHT, carrying the B.2.4.3 default conditioning written out explicitly rather than left implicit, as libjpeg does.
- **Hierarchical (Annex J):** read and written, all three processes. A
  hierarchical sequence is a DHP header followed by several frames, each
  coded against an upsampled reference built from the ones before it
  (J.1.1.2), so a decoder can stop early and still have a smaller complete
  picture. Decode handles a sequence whose frames are sequential, progressive
  or lossless, Huffman or arithmetic, with or without EXP, and frames coded
  as several non-interleaved scans. B.3.1 requires every frame of a
  sequence to use the same process, so
  `GIMG_Save_Options.jpeg_hierarchical_levels` builds a sequential DCT pyramid
  by default, a progressive one with `jpeg_progressive`, and a lossless one
  with `jpeg_lossless_predictor`; `jpeg_arithmetic` selects the arithmetic
  counterpart of whichever. A lossless pyramid keeps RGB and says so with an
  Adobe APP14 (the YCbCr conversion is not reversible), and its differential
  frames use prediction selection 0 - Table H.1's "no prediction", which
  J.1.3.2 requires there - so the whole sequence reconstructs the original bit
  for bit. A differential DCT frame codes its DC coefficient "directly -
  without prediction" (J.1.3.1); the progressive scan encoders did not, which
  made a progressive pyramid drift by a little more at each level while still
  decoding, and is the reason the tests assert that a progressive sequence
  decodes to exactly what the sequential one does.
- **Lossless (SOF3):** read and written. T.81 Annex H is a separate coding process — each sample is predicted from its neighbors and the difference entropy-coded, so the reconstruction is exact. All seven predictors (Table H.1), the point transform, restart intervals, grayscale and RGB, both entropy coders — SOF11 is the arithmetic form, and Thomas Richter's codec is the only other implementation of it available here, so it is the oracle. Non-interleaved scans too (A.2.3), each carrying its own predictor and point transform, which is a property of the scan and not of the frame. This is also the only place a JPEG may carry a precision other than 8 or 12: Table B.2 allows P from 2 to 16, so **a 16-bit raster survives a lossless JPEG round trip unchanged** — the DCT-based writers narrow one to 12 bits because a 16-bit DCT frame does not exist. Any component count from 1 to 255, read and written: Annex H has no color concept of its own and B.2.2 counts components the same way it does for a DCT frame, so a lossless CMYK file - an ordinary thing in prepress - is as legal as a lossless RGB one. B.2.3 still caps a scan at 4, so a frame wider than that is written as one scan per component, each with the Huffman table its own differences generated (B.2.4.2 lets a table at the same destination stand until redefined). Select it with `GIMG_Save_Options.jpeg_lossless_predictor`; the encoder generates a Huffman table from the actual difference statistics (Annex K.2), because the fixed tables of Annex K stop at category 11 and a lossless difference reaches 16.
- **Reference components are not clamped between frames.** J.2.1 adds a differential frame to its reference modulo 2^16, which would be pointless if the intermediate were first clipped to the sample range; the range of A.3.1 is imposed once, on the finished components. `hier_gray_noexp.jpg` is the fixture that settles it — a base frame undershoots to -9 at a near-black pixel and the frame after it corrects by +13, which is unreachable if the -9 was flattened to 0.
- **Component count:** T.81 B.2.2 gives Nf as 1 to 255, and every count is read and written. One, three and four have color conventions attached from outside the standard (JFIF, the Adobe APP14 marker, the component identifiers) and are decoded through them; every other count carries no meaning and its samples pass through unchanged, into a raster of model **GIMG_CHANNEL_UNKNOWN** (libjpeg calls the same thing JCS_UNKNOWN). B.2.3 Table B.3 caps one *scan* at 4 components whatever Nf is, and A.2.2 caps an interleaved MCU at ten data units, so a frame of five or more cannot be interleaved at all: it is written as one non-interleaved scan per component (A.2.3) whether or not `jpeg_non_interleaved` asked, and a progressive one splits its DC scan the same way. Both limits are checked at the scan header, where they belong; checking the MCU limit against the frame refused every wide frame outright. libjpeg cannot read a frame wider than four back — its decoder matches a scan's Cs against only the first MAX_COMPS_IN_SCAN components (`jdmarker.c` `get_sos`) — so the fixtures for those are assembled from files it did write; see `tests/data/jpeg/mk_wide.py`.
- **Abbreviated formats (B.4):** both, read and written. B.4 describes two
  streams that are not complete JPEGs and only mean anything as a pair: one of
  table-specification data with no frame - SOI, DQT, DHT, DAC and DRI, EOI -
  and one carrying a frame whose tables are absent. They exist so that a set of
  images can share one copy of its tables. Neither half can go through
  `gimg_doc_load` on its own, so they have a small API instead:
  `gimg_jpeg_tables_load()` reads a table stream into a `GIMG_JPEG_Tables`,
  `GIMG_Load_Options.jpeg_tables` installs it before a frame is read (the
  stream's own segments still override it, which is what B.2.4.1's "until
  redefined" means), and `GIMG_Save_Options.jpeg_abbreviated` writes either
  half. The tables are kept as the segments themselves rather than as parsed
  tables, so installing them is the same code that installs a table segment
  found in an ordinary file. Checked in both directions against libjpeg-turbo,
  which implements B.4 natively.

## Stream structure

- **Segment structure:** SOI (0xFF 0xD8), EOI (0xFF 0xD9); segments: 0xFF + marker + length (big-endian, length includes the 2 length bytes) + payload. **Fill bytes** (B.1.1.2 — any marker may be preceded by any number of 0xFF) are skipped wherever they appear, including between a scan's entropy data and the marker that ends it, where a 0xFF is otherwise always the 0x00 of byte stuffing. The markers that carry no length field at all (B.1.1.3 Table B.1) are SOI, EOI, RST0–RST7 and **TEM** (0xFF01). Parsing enforces max segment size (bomb protection) and overflow-safe pixel count; returns `GIMG_ERR_LIMIT` when exceeded.
- **SOF types and precision:** Every frame header of T.81 Table B.1 is accepted: **SOF0** (0xC0, baseline DCT, 8-bit only), **SOF1** (0xC1, extended sequential), **SOF2** (0xC2, progressive), **SOF3** (0xC3, lossless), **SOF5**–**SOF7** (0xC5–0xC7, the differential forms used by a hierarchical sequence), and the arithmetic-coded counterpart of each — **SOF9**, **SOF10**, **SOF11** (0xC9–0xCB) and **SOF13**–**SOF15** (0xCD–0xCF). The three codes in that range which are not frame headers are **0xC4** (DHT), **0xC8** (reserved, JPEG extensions) and **0xCC** (DAC); the classification is derived from the marker code rather than listed case by case, because the pattern is regular and a list is a place to make a mistake. Invalid combinations (SOF0 with precision ≠ 8, a DCT frame at 16-bit) return `GIMG_ERR_FORMAT` or `GIMG_ERR_UNSUPPORTED`. DQT may use 16-bit table entries (Pq=1) at 12-bit; decode uses int32_t dequant/IDCT and outputs GRAY16 or RGBA16. 12-bit samples are stored left-justified in 16-bit (sample<<4).
- **Segment order (load and save):** SOI first; then in any order before the first frame header: APP0–APP15 (JFIF, EXIF, XMP, ICC, COM, IPTC/Adobe, unknown), DQT, DHT or DAC, DRI. A frame header follows — any of the fourteen — then SOS + entropy-coded scan data. A progressive frame has further SOS segments and their scan data; so does a sequential or lossless frame written as one scan per component (A.2.3). A hierarchical sequence (B.3.1) instead begins with **DHP**, and then carries several frames, each optionally preceded by **EXP** (B.3.3) and by its own tables, until EOI. DNL (Define Number of Lines, 0xFF 0xDC) may appear after the first scan; see below. RST0–RST7 (0xD0–0xD7) appear only within scan data, not as standalone segments. Unknown markers (e.g. other APPn or future extensions) are skipped and do not break parsing; their payload is discarded unless explicitly stored for round-trip (e.g. unknown APP segments are preserved in meta_raw).
- **DNL (Define Number of Lines, 0xFF 0xDC):** Optional. Segment length is 4 (2-byte length field + 2-byte payload). Payload is the number of lines (image height) as big-endian 16-bit. DNL may appear after the first scan (SOS + scan data). If SOF specified a non-zero height, DNL (when present) must match that height; otherwise the loader returns `GIMG_ERR_FORMAT`. If SOF specified height 0 (streaming case), the loader uses DNL to set the image height; if DNL is absent or height remains 0 after load, the loader returns `GIMG_ERR_FORMAT`. Save does not emit DNL; height is always written in SOF.
- **APP segments:** APP0 (JFIF), APP1 (EXIF, XMP), APP2 (ICC profile). EXIF is parsed via the shared Exif module; metadata common (orientation, DPI, etc.) is populated. Raw APP payloads are preserved for round-trip. Save policies (PRESERVE_ALL, DROP_ALL, STRIP_GPS, NORMALIZE_EXIF, KEEP_RAW_ONLY, KEEP_COMMON_ONLY) are honored.
- **JFIF density, both unit specifiers.** Units 1 makes the two density fields dots per inch, and they reach `gimg_meta_common_dpi()`. **Units 0 makes them a pixel aspect ratio and no size at all**, which is the same statement PNG makes with a `pHYs` of unit 0 and GIF with its Pixel Aspect Ratio byte, so all three reach `gimg_doc_pixel_aspect_ratio()`. Units 0 was previously ignored, so a JPEG declaring a non-square pixel declared nothing here. Equal densities under units 0 are **not** recorded: 1:1 is the boilerplate every encoder writes whether it knows anything or not, and treating it as a declaration would put a `pHYs` in every PNG converted from a JPEG. A ratio the caller sets or clears is written into the APP0 even when that segment was preserved verbatim from the source — otherwise `gimg_doc_set_pixel_aspect_ratio()` would be a no-op that reported success. A physical resolution wins over a ratio, the two sharing one pair of fields. See \ref format_gif "GIF" for the model shared across the three formats and for what other decoders do with it.
- **No background colour.** JPEG has nowhere to state one: JFIF's APP0 carries a density and a thumbnail, Adobe's APP14 a colour transform, and neither Exif nor the JPEG standard itself has a field for the colour behind the image. So `gimg_doc_background_color()` reports nothing for a JPEG, and a background carried in from a PNG's `bKGD` or a GIF's Background Color Index is dropped on save rather than written somewhere it does not belong. The format also has no alpha, so there is nothing for a background to show through.

## Color and sampling

- **Color:** Grayscale (1 component); three components as YCbCr or as RGB; four as CMYK or, with an Adobe APP14 transform of 2, YCCK; any other count as channels with no color meaning. Four-component frames are **written** as well as read: `GIMG_Save_Options.jpeg_cmyk_transform` picks 0 (CMYK, the components unchanged, so a raster from a CMYK JPEG survives a round trip) or 2 (YCCK). That marker is the only thing in a JPEG that distinguishes the two, so it is written whatever the metadata policy says - it is not metadata - and JFIF, which declares three-component data to be YCbCr, is not written beside it. Chroma subsampling applies to a YCCK frame’s two chrominance components and to nothing else: C, M, Y and K are four ink amounts, and libjpeg gives all four 1x1 as well. A three-component frame whose channels carry no color meaning also gets an Adobe marker of transform 0, because three is the count a decoder would otherwise guess at. T.81 describes no color space at all, so which of these a frame carries is decided the way libjpeg decides it (`jdapimin.c`): a JFIF APP0 means YCbCr, else an Adobe APP14 transform, else component identifiers 'R', 'G', 'B', else YCbCr. A three-component frame that already carries RGB is passed through rather than converted — treating one as YCbCr changes every pixel.
- **Sampling factors:** every H and V from 1 to 4 that T.81 B.2.2 allows and A.2.3's ten-data-unit MCU limit permits — twelve combinations — decoded byte-exactly against libjpeg-turbo 3.0.4 in both upsampling modes. The fancy filters are libjpeg's: h2v1 for 4:2:2, h2v2 for 4:2:0, h1v2 for 4:4:0; every other ratio replicates, as libjpeg's `int_upsample` does. A ratio that does not divide is refused by libjpeg itself (`JERR_FRACT_SAMPLE_NOTIMPL`) and is nothing this codec can be checked against.

## CMYK polarity

A JPEG's four components are the Adobe convention - 0 is full ink - which is
what `GIMG_CMYK_POLARITY_INK` names. The decoder states it on **every**
four-component frame, whatever the coding process; it used to be set by the
baseline path alone, so the same image came back saying 0 is full ink when it
was baseline and saying nothing at all when it was progressive or twelve-bit.

The writer reads it too. A raster that says `GIMG_CMYK_POLARITY_REFLECTION`
holds the complement, and writing those samples as they stand produced a
photographic negative of the picture the caller had labelled; such a raster is
complemented on the way out. An **unstated** polarity is written as it stands
rather than refused: a caller building CMYK samples for a JPEG is building
them the way a JPEG holds them.

To turn a CMYK raster into RGB - the only route into a PNG or a BMP, neither
of which has CMYK - use `gimg_ops_convert_pixel_format`; see
\ref api_options "API options". No writer does it on your behalf.

## Color on save

A JPEG has one place to state a color space: APP2 segments introduced by
`ICC_PROFILE\0` (ICC.1:2010 Annex B.4). There is no equivalent of PNG's
`gAMA` or `cHRM`, so a color model that arrived without a profile has nowhere
of its own to go - only an ICC profile survives a save.

**So one is built for it.** A BMP with a calibrated V4 header naming Adobe RGB
and a gamma of 2.2 carries no profile at all; saved as a JPEG it used to keep
neither half, because there was nowhere in the format to put them. Such a
raster now gets an ICC v2.1 RGB matrix/TRC profile synthesized from what
`GIMG_Color_Info` states: `rXYZ`, `gXYZ`, `bXYZ` and `wtpt` for the gamut,
and a shared tone curve for the transfer function.

This is the only place in the library that manufactures a color statement
rather than repeating one, and the rules that keep it honest are worth
stating:

- **A profile the source carried always wins.** Synthesis happens only where
  there is nothing to repeat. A raster that carries a profile has that
  profile written byte for byte, whatever else its color info says.
- **Half a model is written as nothing.** A gamut without a transfer
  function, or a transfer function without a gamut, cannot become a
  matrix/TRC profile without inventing the missing half, so nothing is
  written. That is what such a raster got before this existed.
- **A gamut with no tabulated colorants gets no profile either.** The writer
  holds published D50-adapted colorants for sRGB and Adobe RGB, which cannot
  be derived from *x,y* without a Bradford adaptation, so a raster whose
  gamut is named but absent from that table - Display P3, say - is written
  without one rather than with the wrong colorants.
- **Only a three-component frame.** What is built describes an RGB image.
  A gray or CMYK frame would need a different kind of profile and gets none.

The profile is 492 bytes for a stated gamma and 976 for the sRGB transfer
function, whose curve has to be tabulated: 256 sample points, which measures
within 0.78 of 65535 of the true curve, the floor set by rounding the samples
themselves. For comparison littleCMS's own sRGB profile is 588 bytes, the
difference being that it is a v4 profile stating the curve in closed form.
The colorants are the published D50-adapted values, so a consumer comparing
this profile to the one everyone else ships finds the same primaries rather
than a rounding of them.

**The segments the file came with win.** A document loaded from a JPEG that
carried APP2 ICC has those segments written back verbatim, single or
multi-part, and nothing is synthesized on top of them.

**A document that brought none gets one from its raster.** That is the case
for anything that did not arrive as a JPEG. The profile on the raster's
`GIMG_Color_Info` is written as APP2, split when it does not fit in one
segment: each carries `ICC_PROFILE\0`, its own 1-based number and the count,
which is how a reader reassembles it. One segment holds 65519 bytes of
profile, so a 121908-byte press profile takes two.

The count is one byte, so 255 segments - about 16.7 MB - is the format's
ceiling. A profile past it is written as no profile rather than a truncated
one, because a reader has no way to tell an incomplete profile from a whole
one.

`GIMG_META_DROP_ALL` and `GIMG_META_KEEP_RAW_ONLY` write no profile:
DROP_ALL is asked for a file with nothing attached, and KEEP_RAW_ONLY for the
segments the file arrived with and no others.
`GIMG_META_KEEP_COMMON_ONLY` does write it, as PNG's *Color on save* does -
a color space describes what the samples mean rather than being something
attached to them.

Until this was wired up the APP2 writer read only from raw metadata, so a BMP
carrying a V5 embedded ICC profile came out as an untagged JPEG and the
profile was read only to be dropped. The check is that bmpsuite's
`q/rgb24prof.bmp`, saved as a JPEG, hands Pillow 3048 bytes identical to those
in the BMP, which littleCMS reads as *sRGB IEC61966-2-1 black scaled*.

**The Adobe APP14 marker is not metadata** and is written whatever the policy
says; see *Color and sampling* above. It is what tells a decoder whether four
components are CMYK or YCCK, and a file without it is read by libjpeg's
fallback rather than by anything the file said.

## Precision and 12-bit support

- **Extended precision (12-bit):** T.81 Table B.2 gives a DCT-based frame a sample precision of 8 or 12; precision up to 16 exists only for lossless (SOF3). This codec therefore writes and accepts 8 and 12 only. **12-bit encode:** sequential at precision 12 emits SOF1 (extended sequential DCT), single scan, DQT Pq=1; progressive at precision 12 emits SOF2 with two scans (DC then AC), the same extended DHT and 12-bit coefficient range. Both **native 12-bit raster** (GRAY12/RGBA12, uint16_t 0..4095) and the **save option** (`jpeg_precision=12`, with library bit-depth conversion from an 8- or 16-bit raster) are supported; a 16-bit raster is written at 12-bit, and `jpeg_precision=16` returns `GIMG_ERR_UNSUPPORTED`. The extended DHT uses a 242-symbol AC table (162 + 80); note that T.81 F.1.2.2 only needs SSSS up to 14 at 12-bit (226 symbols), so this table is wider than the spec requires — it is not a spec requirement and is under review. 12-bit encode is checked against libjpeg-turbo 3.0.4 at every quality from 80 to 100, sequential and progressive, and the decoded pixels agree exactly; an earlier defect at qualities 88–90 and 94–99, where the encoder produced a file its own decoder rejected, is gone.
- **Decode capabilities (advertised via codec caps):** **Progressive decode:** SOF2 (progressive DCT), multi-scan (DC, AC initial, AC/DC refinement per T.81 Annex G); non-seekable streams supported. **12-bit decode:** SOF1 (extended sequential) and SOF2 with precision 12; DQT Pq=1 (16-bit quant table entries); output format GRAY16 or RGB16, 12-bit samples left-justified. 12-bit decode and encode take any component count: a four-component frame at P=12 decodes to **GIMG_PIXEL_CMYK16** (samples left-justified) and is written back from one, and wider counts go to a GIMG_CHANNEL_UNKNOWN raster the same way. The JPEG codec registers `GIMG_CAP_16BPC` to say that its output raster can be 16 bits per channel, not that it reads 16-bit JPEG — no such frame exists.
- **12-bit raster (encode input):** Native 12-bit formats **GIMG_PIXEL_GRAY12** and **GIMG_PIXEL_RGBA12** use uint16_t per sample, value 0..4095 (no left-shift in raster). Bit-depth conversion (8↔12↔16) is a first-class library API (`ghoti.io/image/bitdepth.h`, `gimg_ops_convert_bit_depth`); JPEG (and other codecs) use it when raster depth differs from codec precision.

## Conformance and tested scope

Implementation follows ISO 10918-1 baseline and progressive for 8-bit and 12-bit. SOS scan header is as in Annex B: after Ns and the component entries (Cs, Td|Ta per component), the segment ends with Ss (1 byte), Se (1 byte), and one byte with Ah in the high 4 bits and Al in the low 4 bits (successive approximation bit positions). Decode and encode support refinement scans (Ah>0): DC refinement (Ss=0, Se=0) and AC refinement with run-length then one bit per coefficient; a dedicated AC refinement DHT (Th=2) is emitted when needed. **Huffman tables (DHT):** Hard-coded tables in the encoder and DHT writer are spec-matching: 8-bit tables (DC/AC luminance and chrominance) match T.81 Annex K Tables K.3–K.6 (bit-count arrays and symbol order); cross-checked against libjpeg-turbo standard tables. Extended precision tables use 17-symbol DC (size 0..16) and 242-symbol AC (162 + 80 for size 11..15); T.81 F.1.2.2 needs only DC 0..15 and AC 1..14 at 12-bit, so these are wider than required and are under review; DHT payload length obeys B.2.4 (value bytes = sum of 16 bit counts). Decoder rejects DHTs where the number of value bytes does not equal that sum. Golden decode tests use reference files in `tests/data/jpeg/` (generated by `tests/data/jpeg/generate.py`); pixel hashes (FNV-1a 64-bit) and metadata are compared. **External oracles.** Three, chosen per claim, because no single one covers the standard:

- **libjpeg-turbo 3.0.4**, built from source, is the byte-exact oracle wherever it can read or write the case at all. Decode is compared exactly across 8- and 12-bit, sequential/progressive/arithmetic with and without restart intervals, lossless at 8/12/16-bit with all seven predictors, and every sampling factor in both upsampling modes. Encode is checked the other way: files this library writes, decoded by it, across subsampling x arithmetic x progressive x non-interleaved x restart — all exact. **There is now a 12-bit oracle**, contrary to what this document said before.
- **Thomas Richter's codec** (the ISO group's reference for T.81) is the only implementation here of hierarchical frames and of SOF11, so it is the oracle for Annex J and for arithmetic lossless. Its IDCT and chroma upsampler are not libjpeg's, so those comparisons carry a small tolerance, justified by single-frame controls decoded through the same path.
- **Pillow** validates the Python-generated fixtures (`pillow_decode_hash.py`) and the encode output in `tests/out/jpeg/` (`verify_jpeg_output.py`). For CMYK it is *not* the oracle: Pillow inverts Adobe CMYK and libjpeg does not, and matching libjpeg is what this codec does, so those references come from libjpeg directly (`tests/data/jpeg/cmyk_ref.c`).

Each oracle has a reach, and past it the tests say so rather than
pretending.  libjpeg cannot read a frame of more than four components back
(`jdmarker.c` `get_sos` matches a scan's Cs against only the first
MAX_COMPS_IN_SCAN of them), so the wide-frame fixtures are spliced from
grayscale files it did write, with its decode of each kept as the expected
plane.  The reference codec is the only other implementation of Annex J here;
it reads every form this encoder writes, and its lossless hierarchical decode
differs from the original at two samples of 984, by one each, where ours is
exact - J.1.1.2 says of the expansion that "the division indicates
truncation, not rounding", which is what both halves of this codec do, so
that difference is recorded rather than matched.  A lossless pyramid needs no
oracle at all: lossless is a claim about the pixels, and the test asserts
equality with the original.

Where nothing can check a case, the tests say so rather than pretending: a subsampled lossless frame has no correct writer anywhere (libjpeg-turbo declines to subsample a lossless frame; the reference codec's own round trip through one is off by 215 of 255), so those fixtures assert memory safety and dimensions only. **Official reference software:** ISO/IEC 10918-7:2023 (ITU-T T.873) specifies the JPEG 1 reference software; it is not freely distributed (ISO/ITU purchase). See `tests/data/jpeg/README.md` for oracle options. **Fuzzing.** Four libFuzzer harnesses — `fuzz_jpeg_load`, `fuzz_jpeg_encode`, `fuzz_png_load`, `fuzz_png_encode` — share one corpus under `tests/fuzz/corpus`, built with ASan and UBSan. Errors are returned as `GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT` or `GIMG_ERR_LIMIT`, never a crash. The sanitizer test build passes `-fno-sanitize-recover=undefined`, so undefined behavior fails the run instead of printing a line into a log: a signed-overflow shift in the EXIF reader had been reported by all four harnesses at once and gone unacted upon precisely because nothing failed. **Previously recorded deviations, both now gone:** progressive AC scans were written with Ns>1, which G.1.2.2 forbids — they are written one component per scan and libjpeg reads them; and the SOS parameters Ss/Se/Ah/Al were read without being checked, so a file could name a spectral band running backwards and steer the entropy decoder with it — they are validated on load against B.2.3, with the lossless reading of those same fields (H.1: Ss is the predictor, Se is zero, Al the point transform) handled separately.

## Compliance checklist

Short reference for supported markers, precision, entropy, and limitations. Update when adding or restricting features.

| Area | Supported | Rejected / limitation |
|------|------------|-------------------------|
| **Frame (SOF)** | All fourteen of Table B.1: SOF0 (8-bit only), SOF1/SOF2 (8/12-bit), SOF3 (lossless, P=2–16, read **and written**), SOF5–SOF7 (differential, in a hierarchical sequence), and the arithmetic counterpart of each — SOF9, SOF10, SOF11, SOF13–SOF15. Nf 1–255, read and written | SOF0 with P≠8 → GIMG_ERR_FORMAT; a DCT-based SOF with P=16 → GIMG_ERR_UNSUPPORTED (Table B.2 allows 16 only in a lossless frame) |
| **SOS (lossless)** | Ss = predictor selection 1–7 (T.81 H.1, Table H.1); Se = 0; Ah = 0; Al = point transform | predictor outside 1–7, Se≠0, Ah≠0, or Al ≥ precision → GIMG_ERR_FORMAT |
| **DAC** | Parsed; conditioning applied per T.81 B.2.4.3, including between scans | Tc>1, Tb>3, DC L>U, or AC Kx outside 1..63 → GIMG_ERR_FORMAT |
| **Entropy** | Huffman (Annex F, DHT) and adaptive binary arithmetic (Annex D, DAC), both read and written, for every process including lossless (SOF11) and hierarchical | — |
| **DHT** | T.81 B.2.4: TcTh + 16 bit counts + value bytes; value bytes = sum of counts | Payload length &lt; 17 or &lt; 17+sum(bits) → build fails / GIMG_ERR_FORMAT |
| **DRI/RST** | Restart interval, RST0–RST7 within scan data; DRI may be redefined between scans (B.2.4.4) | — |
| **Scan arrangement** | Interleaved (A.2.2) and one non-interleaved scan per component (A.2.3), read and written, for sequential, progressive, lossless and hierarchical frames. Write with `GIMG_Save_Options.jpeg_non_interleaved`; a frame of more than four components is split whether asked or not, because B.2.3 caps Ns at 4 | Refused together with `jpeg_progressive` (Annex G has its own scan script), a lossless frame, or `jpeg_hierarchical_levels` |
| **Hierarchical (Annex J)** | DHP, EXP, differential frames; sequential, progressive and lossless sequences, Huffman and arithmetic, **read and written** (`jpeg_hierarchical_levels`, with `jpeg_progressive` or `jpeg_lossless_predictor` choosing the process as B.3.1 requires of every frame alike). A lossless pyramid reconstructs the original bit for bit. One, three and four components, so a CMYK pyramid too | `jpeg_precision` other than 8: a pyramid’s reconstruction is built at 8 bits → GIMG_ERR_UNSUPPORTED. More than four components → GIMG_ERR_UNSUPPORTED: every frame would have to be split (B.2.3), and there is no other implementation of Annex J to check a wide sequence against — libjpeg has no hierarchical mode and the reference codec does not take one this wide, so a round trip through this library alone could not tell a private misreading from a correct one |
| **DNL** | After first scan only; must match SOF height if SOF height ≠ 0; **supplies the height when SOF gives Y = 0** (B.2.2, B.2.5) | DNL before first scan → GIMG_ERR_FORMAT; DNL height ≠ SOF height → GIMG_ERR_FORMAT; save never emits DNL |
| **Precision** | 8 (baseline), 8/12 (SOF1), 8/12 (SOF2) — T.81 Table B.2 allows no other value in a DCT frame; 2–16 in a lossless frame | SOF0 with 12-bit → GIMG_ERR_FORMAT; P=16 in a DCT frame → GIMG_ERR_UNSUPPORTED (16-bit belongs to lossless SOF3 only) |
| **Color** | Grayscale; YCbCr or RGB (3 components); CMYK or YCCK (4 components, 8- and 12-bit, every process), read **and written**; any other count as GIMG_CHANNEL_UNKNOWN; every H/V sampling factor 1–4 that A.2.2's MCU limit allows | `jpeg_cmyk_transform` other than 0 or 2 → GIMG_ERR_UNSUPPORTED |
| **APP/COM** | APP0 (JFIF/JFXX), APP1 (EXIF/XMP), APP2 (ICC), APP13/14, COM, unknown APP round-trip | — |
| **Abbreviated formats (B.4)** | Both, read and written: a table-specification stream (`gimg_jpeg_tables_load`, `GIMG_Save_Options.jpeg_abbreviated = 2`) and an image with no tables (`GIMG_Load_Options.jpeg_tables`, `jpeg_abbreviated = 1`). Checked both ways against libjpeg-turbo, which implements them natively | Refused with `jpeg_hierarchical_levels` (its frames carry their own tables, B.3.1) and with a lossless frame (its Huffman table is generated from the coefficients it codes) |

## Edge cases and test coverage

Short list of dimension/stream edge cases; update when adding tests. See `image/tasks/image-phase-2.3-jpeg-quality-maintainability.md` task 2.3.3.3.

| Edge case | Covered by |
|-----------|------------|
| **1×1** | SaveRgb1x1ThenLoadDecode (encode→load→decode round-trip) |
| **8×8** | make_minimal_jpeg (SOF0 8×8), baseline_8x8_gray fixtures, ProgressiveMinimalDimensions8x8And16x16, DecodeMinimalJpegReturnsError |
| **16×16** | baseline_16x16_ycbcr fixtures, ProgressiveMinimalDimensions8x8And16x16 |
| **Non-MCU width/height** | Supported; dimensions in SOF; decode uses padded MCUs; no dedicated “non-MCU” fixture (same as 8×8 / 16×16 for minimal) |
| **Restart interval 1** | SmallRestartIntervalsChangeNothingButWhereTheCoderResets — intervals 0, 1, 2 and 3 across subsampling × progressive × arithmetic × non-interleaved, each asserted to put RST markers in the file and to decode to the same pixels as no interval at all. Also DRI=4 and 8 (make_minimal_jpeg_with_dri, EncodeRestartIntervalThenLoadDecodeAndLibjpegOracle, ProgressiveWithRestartIntervalRoundTrip), and 54 combinations of interval 1–3 checked byte-exactly against libjpeg-turbo while this was written |
| **Empty / minimal scan** | make_minimal_jpeg (minimal scan data), make_minimal_progressive_jpeg; DecodeMinimalJpegReturnsError (decode fails on empty scan) |
| **DNL-only height** | `dnl_zero_height.jpg` + DnlSuppliesAHeightTheFrameHeaderLeftAtZero. libjpeg refuses such a file outright, so the reference is its decode of `dnl_stated_height.jpg` (the same image with the height in the SOF); the ISO reference codec, which does implement DNL, reads the zero-height one and agrees |
| **DNL after first scan** | make_minimal_jpeg_with_dnl_after_scan (DNL matches SOF 8); LoadJpegWithDnlMismatchFails, LoadJpegDnlBeforeScanFails (negative) |
| **RST in scan** | make_minimal_jpeg_with_rst_in_scan (RST5), LoadJpegWithRstInScanSucceeds |

## Not implemented

Nothing of T.81 is known to be missing. What is bounded rather than absent, and why: a DCT frame is written at 8 or 12 bits, which is all Table B.2 allows; a hierarchical sequence is built at 8 (its reconstruction would need its own pass at 12) and at up to four components (see the checklist — no oracle exists for a wider one, and a self-consistent round trip is not evidence). A lossless frame writes any precision from 2 to 16 and any component count.

---

Back to \ref image_format_references "Format and specification references".
