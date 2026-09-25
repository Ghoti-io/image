# Developer guide

This document describes the image library layout, error-handling policy, testing, and how to add a new codec. It is aimed at contributors and maintainers.

## Prerequisites

**A container engine, and nothing else.** Every reference decoder this library
is measured against is pinned into one image and reached through
`tools/oracle/oracle-exec`; `tools/oracle/containers/IMAGES` lists them with
the version each answers at, and `notes/suite/CONTAINERS.md` is the pattern.

```bash
make oracle-build     # build the image (podman or docker)
make oracle-tools     # compile the three oracle tools inside it
make oracle-verify    # print what answers, and at what version
```

The references are Pillow, libjpeg-turbo 2.1.5, giflib, GdkPixbuf, netpbm,
ImageMagick, piexif, bmplib and bmpsuite at pinned commits, libjpeg-turbo
3.0.4 built from source for the 12- and 16-bit codecs, and IJG v10.

This used to be a list of things to `apt install` and `pip install`, and the
list was the problem rather than the inconvenience: a machine that had them
compared against whatever versions it had, and a machine that did not skipped
the comparison and still exited 0. Neither is true now - an unreachable
reference fails the suite and names the gate to drop if you meant to skip it.

`GHOTI_ORACLE_MODE=host` runs this machine's own decoders instead, and says so
in the line every gate prints. It is not a fallback: nothing selects it
automatically, because a gate whose reference is not the one it names prints
the same green line as one whose is. `GIMG_ORACLE_REQUIRED=0` turns the
sentinels back into skips for a developer who has no engine and wants the rest
of the suite.

Python 3 is needed for the library's own tooling - the generators, the
verification scripts and the coverage triage - and is not an oracle.

## Code layout

Public API headers live under `include/ghoti.io/image/`. Internal implementation is under `src/` with the following structure:

| Directory | Purpose |
|-----------|---------|
| `src/core/` | Allocator, diagnostics, result strings, safe-math helpers. |
| `src/container/` | Document (GIMG_Doc) and item management. |
| `src/raster/` | Raster allocation and pixel layout. |
| `src/stream/` | Stream abstraction (e.g. memory stream). |
| `src/codec/` | Codec registry and dispatch; format-specific code in subdirs (e.g. `png/`). |
| `src/codec/png/` | PNG/APNG load, decode, save; shared PNG common (Adam7, row-bytes), chunk parsing, DEFLATE integration. |
| `src/meta/` | Metadata common, raw blobs, Exif parsing and normalization. |
| `src/color/` | Color info (sRGB, ICC, etc.). |
| `src/ops/` | Raster operations (e.g. apply orientation). |

Internal headers use the `_internal.h` suffix (e.g. `png_internal.h`, `doc_internal.h`). Shared constants and helpers for a format are centralized (e.g. `png_common.c` for PNG row-bytes and Adam7).

### JPEG codec layout

JPEG implementation lives under `src/codec/jpeg/`. Roles of the main files:

| File | Role |
|------|------|
| `jpeg_load.c` | Main load loop: verify SOI, read segments (via jpeg_segment), dispatch payload parsing (via jpeg_parse), enforce limits, build GIMG_Doc and meta, scan data read until next marker. |
| `jpeg_segment.c` | Low-level segment I/O: read next marker (0xFF + byte), read segment length (big-endian), read payload with limit (used by jpeg_load.c). |
| `jpeg_parse.c` | Parse segment payloads into doc state: SOF0/SOF1/SOF2, DQT, DHT (apply + record), SOS header + scan snapshot, append scan data, unknown APP/COM. No stream I/O. |
| `jpeg_decode.c` | Thin dispatch: baseline vs progressive decode. |
| `jpeg_entropy.c` | Entropy orchestration: baseline and progressive decode entry points; MCU loop; calls jpeg_block, jpeg_idct, jpeg_upsample; scan state and buffer layout. Keeps default AC refine DHT and high-level control flow. |
| `jpeg_bitstream.c` | Bitstream reader (byte/bit access, RST/stuff-byte skip). Build Huffman tables from DHT payload; decode next symbol given a table. Used by jpeg_block.c and jpeg_entropy.c. |
| `jpeg_block.c` | Decode one 8×8 block: baseline DC/AC, progressive DC initial/refinement, progressive AC initial/refinement. Calls jpeg_bitstream; outputs coefficient block. |
| `jpeg_idct.c` | Dezigzag, dequantize, 8×8 inverse DCT (float, 32-bit, and T.81 integer islow). Used by jpeg_entropy.c after block decode. |
| `jpeg_upsample.c` | Chroma upsampling (e.g. 2h2v fancy). Used by jpeg_entropy.c for component→raster assembly. |
| `jpeg_save.c` | Raster→scan, DQT/DHT/SOS/scan write, baseline and progressive body, APP/COM write. |
| `jpeg_encode.c` | FDCT, quantization, bit writer, baseline and progressive scan encode (uses shared tables from `jpeg_huffman_tables_internal.h`). |
| `jpeg_internal.h` | Constants, structs, internal API. |
| `jpeg_register.c` | Codec registration and probe. |
| `jpeg_tables.c` | Single translation unit that defines shared table data (zigzag, quant, Huffman) from `jpeg_*_internal.h` headers. |

Data flow: **Load** — stream → jpeg_segment (marker + length + payload) → jpeg_parse (payload → state) → doc state (SOF, DQT, DHT, scans, APP/COM). **Decode** — doc state → baseline or progressive entropy → dequant/IDCT/upsample → raster. **Save** — doc + raster → DQT/DHT/SOF/SOS + scan data → stream.

## Error-handling policy

The library uses **GIMG_Result** for all API functions. Policy details (when to return which code, when to append diagnostics, behavior of output parameters on error) are documented in **Error handling** in @ref api_options "API Options and Types".

Summary:

- Use the most specific result code that fits: **GIMG_ERR_FORMAT** for invalid format/structure, **GIMG_ERR_CORRUPT** for corrupt payload (e.g. CRC), **GIMG_ERR_LIMIT** for limit exceeded, **GIMG_ERR_OOM** for allocation failure, **GIMG_ERR_INTERNAL** only for internal bugs.
- When the caller provides a non-NULL **GIMG_Diagnostics**, append diagnostics for meaningful failures (e.g. chunk CRC, limit hit) with an optional recommended_action.
- On error, **output parameters** (e.g. `GIMG_Doc ** out_doc`, `GIMG_Raster ** out_raster`) are left **unchanged**; the implementation must not leak any partially allocated state.

## Testing

### Unit and codec tests

Tests live under `tests/`. The suite uses Google Test. Build and run:

```bash
make test          # Build and run all tests
make test-quiet    # Minimal output (one line per suite)
make test-valgrind # Run under Valgrind (Linux)
```

Test layout:

- `tests/unit/` — Core and shared unit tests.
- `tests/codec/png/` — PNG/APNG decode and encode tests; shared helpers in `png_test_utils.cpp` / `png_test_utils.h`.
- Reference data under `tests/data/png/` (e.g. generated by `generate.py`); encode output under `tests/out/png/` for golden verification.

After `make test`, PNG output is verified with `tests/data/png/verify_png_output.py` (e.g. via PIL), and BMP output with `tests/data/bmp/verify_bmp_output.py`, which reads each written file back with Pillow and with GdkPixbuf and compares it against a sidecar the encode tests leave beside it (`make test-verify-bmp` runs that alone). JPEG decode correctness is validated against Pillow (Python) where applicable (`Decode*PillowOracle` tests run `tests/data/jpeg/pillow_decode_hash.py`); JPEG encode output is verified by `tests/data/jpeg/verify_jpeg_output.py` (PIL opens each file in `tests/out/jpeg/`). **Pillow is required** for these JPEG tests (see Prerequisites above).

**What those verifications do not cover, and the two gates that do.** Every
script above reads our output back with an *outside* decoder and compares
pixels. That catches pixel bugs, and the decoders in question are lenient
about everything else - measured, by breaking one thing in a known-good file
and asking: a JPEG whose `RST2` is renumbered `RST5`, a GIF whose background
index is not in its colour table, a GIF whose LZW minimum code size is 9, and
a GIF with its trailer removed are all read without complaint by ImageMagick,
and Pillow accepts the JPEG too. So a writer can emit a malformed file, every
oracle can read it, the pixels can be right, and every gate stays green. That
is how the APNG sequence numbering was wrong for a year of animations.

Two gates close it, and each was watched failing against that bug before being
trusted:

- `tests/codec/test_roundtrip.cpp` saves every fixture as every format that
  will take it and **reads the result back with this library's own decoder**,
  which is the strictest reader available - it rejects a corrupted CRC, a
  duplicate IHDR and a missing IEND, all three of which Pillow accepts. A save
  the codec refuses is an answer; writing something and then refusing to read
  it is not.
- `tests/data/verify_structure.py` (`make test-verify-structure`) decodes
  nothing. It reads the bytes of everything in `tests/out/` against the
  specifications: PNG chunk order and CRCs, APNG sequence numbering and acTL
  agreement, GIF sub-block termination and code sizes, JPEG restart-marker
  order, BMP header self-consistency. **It self-tests first** - it builds
  known-bad inputs of its own and refuses to certify anything unless it
  catches every one, because a structural checker that quietly stops parsing
  reports zero problems on everything, which looks exactly like success.

`tests/codec/test_roundtrip.cpp` also holds the invariant those two gates grew
out of: **what a writer produces is a function of the document and the options
and of nothing else.** It was not. Every writer takes a raster already attached
to an item and decodes one when there is none; the JPEG writer additionally
took item 1 as an EXIF thumbnail *whenever that item happened to carry a
decoded raster*, so walking an animation's frames before saving changed the
file - and on eight large animations made the save fail outright. The test
saves each multi-item fixture under five decode orders and requires the bytes
to match.

Only multi-item documents are swept, because the mechanism needs one item to be
in a different state while another is read; a one-off sweep over all 256
fixtures in four output formats and six decode orders, 5142 comparisons, found
the same nothing at five seconds a run rather than one. The exhaustive check
that backs this up is not the sweep but a reading of all nine places a writer
calls `gimg_item_raster()`: eight take the attached raster or decode
unconditionally, which cannot differ, and the ninth was the bug.

Both gates need an input big enough to reach the paths they guard, and no
fixture in the tree is: the writers split their output at 32 KiB, and a 4x4
test image never gets near it. The round-trip test therefore synthesizes a
multi-frame animation of incompressible noise, asserts that the frames really
did split across chunks before asserting anything about them, and leaves the
file in `tests/out/png/` so the structural check sees the hard case too. The
first draft of that test omitted this and passed against the bug it was
written for.

**BMP conformance sweep:** `make bmpsuite BMPSUITE=<dir>` runs Jason Summers' bmpsuite through the codec, where `<dir>` is an unpacked copy fetched into a scratch directory rather than vendored - the same treatment PngSuite gets. It compares each decode against decoders written from the format description inside `tests/data/bmp/bmpsuite_sweep.py` and against whichever of Pillow, GdkPixbuf and netpbm's `bmptopnm` are installed, with each one's reach declared per file, because no two of them agree. A file nothing can check is reported as a failure. Without `BMPSUITE` the target skips and says how to get the suite.

**JPEG test map:** Load and segment/limit behavior: `tests/codec/jpeg/test_jpeg_load.cpp` (e.g. SOF rejection, DNL, DHT/SOS negative tests, golden/oracle). Encode, round-trip, and save: `tests/codec/jpeg/test_jpeg_encode.cpp` (quality, chroma, progressive, 12-bit, DHT consistency, failure paths). Helpers: `jpeg_test_utils.cpp` / `jpeg_test_utils.h` (load_jpeg_file, raster hash, oracle helpers). Fuzz: `tests/fuzz/fuzz_jpeg_load` (load + decode; no crash on arbitrary input).

**Regenerating JPEG fixtures and oracle verification:** From the repo root: `python3 tests/data/jpeg/generate.py` to regenerate fixtures (requires Pillow). Build libjpeg oracle tools: `make oracle-build oracle-tools` (sources in `tests/tools/jpeg-oracle/`; requires the libjpeg dev package). Verify encode output: `make test-verify-jpeg` or `python3 tests/data/jpeg/verify_jpeg_output.py`. Decode oracle (Pillow or libjpeg): see `tests/data/jpeg/README.md` for `generate_jpeg_oracle_raws.py`, `pillow_decode_hash.py`, and libjpeg ref tool usage.

### Valgrind

Run the full suite under Valgrind to ensure no leaks and clean memory use:

```bash
make test-valgrind   # Linux only
make test-valgrind-quiet
```

Tests that pass diagnostics must call `gimg_diagnostics_clear()` or `gimg_diagnostics_destroy()` so Valgrind stays clean. **Before release or major changes**, run the full test suite (PNG and JPEG) under Valgrind; this is expected for both codecs. JPEG changes should be validated with `make test-quiet` and `make test-valgrind-quiet`; whether Valgrind runs in CI or only as a developer checklist is project policy (see task image-phase-2.3-jpeg-quality-maintainability.md).

### Sanitizers (ASan + UBSan)

AddressSanitizer and UndefinedBehaviorSanitizer builds use a separate build directory and run the same test suite:

```bash
make test-asan   # Build with ASan+UBSan and run tests (Linux)
make test-ubsan  # Alias for test-asan
```

These targets are documented in the main Makefile and in this guide. Use them in development or CI to catch use-after-free, buffer overflows, and undefined behavior. Sanitizer support is currently implemented for Linux; the build uses `build/$(BUILD)-asan/` (e.g. `build/linux/release-asan/`).

### Fuzzing

LibFuzzer harnesses under `tests/fuzz/` cover PNG/APNG, JPEG and BMP, each with a load-and-decode harness and a round-trip (load/save/load) one. Build with `make fuzz-png`, `make fuzz-png-encode`, `make fuzz-jpeg`, `make fuzz-jpeg-encode`, `make fuzz-bmp` or `make fuzz-bmp-encode` (requires clang); run with a corpus as described in the Makefile and `tests/fuzz/README.md`.

**New codecs:** Add at least (1) a load (and decode) fuzz harness so that arbitrary or truncated input does not crash and returns appropriate errors (`GIMG_ERR_FORMAT`, `GIMG_ERR_CORRUPT`, or `GIMG_ERR_LIMIT`), and (2) if the codec supports save, a round-trip fuzz harness (load→save→load). PNG and JPEG are the reference; see `tests/fuzz/README.md` for harness layout.

### Limits and failure-path tests

Tests under `tests/` include:

- **Limits:** `max_chunk_size`, `max_decoded_pixels`, `max_frame_count` set so the library returns **GIMG_ERR_LIMIT**; diagnostics may report the limit.
- **Save failure paths:** `gimg_doc_save` with NULL doc, invalid/unsupported format, or unsupported raster format; expect documented error codes and no crash.

## Adding a new codec

**Checklist:** (1) Use the codec allocator for all codec-owned allocations. (2) Enforce **GIMG_Limits** (max_chunk_size, max_decoded_pixels, max_frame_count if applicable). (3) Use `safe_math_internal.h` for size calculations. (4) Set `*out_doc` / `*out_raster` to NULL on error and free any partial state before returning. See **Codec implementation checklist** below for details and **Codec contract** for the full allocator, limits, diagnostics, and error-cleanup requirements (PNG and JPEG are the reference implementations).

1. **Stub and register:** Create a codec stub (e.g. `gimg_codec_create_stub()` or `gimg_codec_create_stub_with_allocator()`), implement probe (magic bytes / peek), and register with `gimg_codec_register()`. Probe result `format_name` is used by load/save; document that `format_name` lifetime is only until the next registry-mutating call (see @ref api_options "API Options and Types").

2. **Load:** Implement load in the codec: parse container structure, create **GIMG_Doc** and **GIMG_Item**(s), fill frame timing and blend/dispose for animation if applicable. Enforce **GIMG_Limits** (chunk size, decoded pixels, frame count); return **GIMG_ERR_LIMIT** when exceeded. Use **GIMG_Diagnostics** when provided (e.g. CRC errors, unsupported features). Populate metadata common from format-specific metadata (e.g. Exif orientation) when present.

3. **Decode:** Implement decode from **GIMG_Item** to **GIMG_Raster**: respect **GIMG_Decode_Options** limits, use safe pixel-count helpers (e.g. from `safe_math_internal.h`) to avoid overflow, return **GIMG_ERR_CORRUPT** or **GIMG_ERR_LIMIT** as appropriate.

4. **Save:** Implement save from **GIMG_Doc** to stream for the codec’s format name; apply metadata policy (PRESERVE_ALL, STRIP_GPS, NORMALIZE_EXIF, etc.) as documented in the format's page under `documentation/formats/` and in api-options.

5. **Tests:** Add decode/encode tests (and round-trip if applicable); use shared test helpers where possible. Add a `tests/data/<format>/` directory with a manifest or generator and a verify script (or equivalent), consistent with PNG and JPEG. Add fuzz coverage for load/decode and, if feasible, save. Ensure limits and failure-path tests cover the new codec. Document round-trip and oracle strategy: at least one round-trip test (save→load→decode) and, if available, an external oracle (e.g. reference decoder) for decode correctness; see the format pages under `documentation/formats/` and tests/data/png/ and tests/data/jpeg/ for reference.

6. **Docs:** Add `documentation/formats/<format>.md` and link it from the table in \ref format_references "Format and specification references"; update option docs for format-specific behavior and limits. \ref format_adding "Adding a format" is the checklist and the page template.

### Codec implementation checklist

When implementing a new codec (or auditing an existing one), ensure:

- **Allocator:** Use the codec allocator (`codec->allocator`) for all codec-owned allocations (load, decode, save). Document and raster creation use `doc->allocator` (set at load from the codec allocator). Do not use `gimg_allocator_default()` when a codec or document allocator is available; pass the allocator explicitly so tests and embedders can use custom allocators end-to-end.
- **Limits:** Enforce **GIMG_Limits** at the appropriate points: `max_chunk_size` (or equivalent segment/payload size) before reading large payloads; `max_decoded_pixels` before allocating decode buffers; `max_frame_count` for animated formats. Return **GIMG_ERR_LIMIT** when exceeded; append diagnostics when provided. See @ref api_options "API Options and Types" and the “Limits per codec” subsection in this document.
- **Safe math:** Use `gimg_safe_pixel_count()` from `safe_math_internal.h`, and the general helpers from `<ghoti.io/cutil/safemath.h>` (`gcu_safe_mul_size()`, `gcu_safe_add_size()`, `gcu_safe_mul_add_size()`), for all size and pixel-count calculations that feed allocations or comparisons to limits. The general ones used to be local `gimg_safe_*` duplicates of the same functions in compress. **Checklist for new codecs:** Validate all length/size fields read from the stream before allocating or indexing; use safe_math for any derived buffer size.
- **Output parameters and cleanup:** Set `*out_doc` or `*out_raster` to **NULL** before any work in load/decode. On error, free any partially allocated state (e.g. via the codec’s free_doc_state or equivalent) and return without setting the output parameter. The central dispatch in `codec.c` also clears `*out_raster` on decode callback failure.
- **Spec alignment:** Document implemented parts, conformance scope, and rejected/unsupported features on the format's own page under `documentation/formats/` (see \ref format_adding "Adding a format" for the checklist and template; PNG and JPEG are the reference structure).

### Limits per codec

| Codec | max_chunk_size | max_decoded_pixels | max_frame_count |
|-------|----------------|--------------------|-----------------|
| **PNG** | Enforced in chunk reader before reading payload | Enforced in decode (frame and full-image paths) | Enforced in load for APNG (acTL num_frames vs actual fcTL/fdAT) |
| **JPEG** | Applied via max segment payload in load | Enforced in load (after SOF) and in entropy decode | N/A (still image; multi-item from EXIF thumbnail counts as one “frame”) |
| **BMP** | N/A (a DIB has no chunk structure; `max_memory` bounds a linked ICC profile instead) | Enforced in load (after the DIB header) and in decode | N/A (still image; an OS/2 `BA` array's entries are capped at 64 by the codec) |
| **GIF** | Enforced while joining a sub-block chain, which has no declared total and would otherwise be believed 255 bytes at a time | Enforced in load (per image descriptor) and in decode (the logical screen) | Enforced in load, before each image block is read |

New codecs should enforce the same limits that apply to their format and document which of these (or format-specific limits) they use.

### Codec contract

This subsection spells out the contract that every codec must satisfy. PNG and JPEG are the reference implementations; when in doubt, follow their behavior.

- **Allocator source:** The codec receives an allocator at creation (`gimg_codec_create_stub_with_allocator()`); store it in `codec->allocator`. Use **`codec->allocator`** for all codec-owned allocations during load, decode, and save (e.g. segment/chunk buffers, Huffman tables, scan data, document-private state). When creating the **GIMG_Doc** at load, set `doc->allocator` from the codec allocator; subsequent document and raster creation use **`doc->allocator`**. Streams created for in-memory buffers (e.g. save output, or load paths that need a stream over decoded data) should use the same allocator via `gimg_stream_create_memory_*_with_allocator(alloc, ...)`. Do not use `gimg_allocator_default()` when a codec or document allocator is available; pass the allocator explicitly so tests and embedders can use custom allocators end-to-end.

- **Limits application points:** Apply **GIMG_Limits** at these points: (1) **Before reading large payloads** — check segment/chunk size against `max_chunk_size` (or the codec’s equivalent) and return **GIMG_ERR_LIMIT** if exceeded. (2) **Before allocating decode buffers** — compute pixel count with `gimg_safe_pixel_count()` (or safe_math helpers) and check against `max_decoded_pixels`; return **GIMG_ERR_LIMIT** if exceeded. (3) **For animated formats** — enforce `max_frame_count` in load (e.g. when reading frame count or appending frames). See the “Limits per codec” table above for where PNG and JPEG apply each limit.

- **Diagnostics when provided:** If the caller passes a **GIMG_Diagnostics** pointer to load or decode, append diagnostic entries on error (e.g. codec name, stream offset, format-specific code, and a short message such as “CRC error”, “segment too large”, “limit exceeded”). Use **GIMG_DIAG_ERROR** (or the appropriate level) so that API users can log or display failures. Do not require diagnostics to be non-NULL; treat it as optional.

- **Error-cleanup requirements:** On any error path: (1) Free any partially allocated state (e.g. buffers, doc-private state) using the same allocator that was used to allocate it. (2) Call the codec’s free_doc_state (or equivalent) if doc state was partially built before the error. (3) Do **not** set `*out_doc` or `*out_raster` on error; leave the output parameter unchanged (or ensure the central dispatch clears it, as in `codec.c` for decode). (4) Return the appropriate **GIMG_Result** (e.g. **GIMG_ERR_OOM**, **GIMG_ERR_LIMIT**, **GIMG_ERR_CORRUPT**, **GIMG_ERR_FORMAT**). Load and decode must set `*out_doc` / `*out_raster` to **NULL** before any work so that a later error leaves the output unset.

## JPEG debug and recovery options

JPEG debug and trace output are controlled by **compile-time** defines in
`src/codec/jpeg/jpeg_debug_internal.h`. All default to 0. To enable a category
for a debug build, define it when compiling (e.g. `-DGIMG_JPEG_DEBUG_LOAD=1`).
See that header for the full list (e.g. `GIMG_JPEG_DEBUG_LOAD`,
`GIMG_JPEG_DEBUG_RST_DEC`, `GIMG_JPEG_DUMP_FIRST_MCU_COEF`). Dump options that
write files may still read the output path from the same-named environment
variable when the category is enabled at compile time.

The progressive decoder's own step tracing is **not** among them any more. It
was nineteen categories driving 785 lines across `jpeg_block.c`,
`jpeg_entropy.c`, `jpeg_bitstream.c` and `jpeg_encode.c`, and every one of
them was unreachable: the four block decoders took their trace settings as
parameters, and the single call site of each passed `0` and `-1` as literals.
No build could turn them on and no environment variable was ever read, which
is why the comparison scripts under `tests/data/jpeg/` cannot drive our side
(see the README there). They were written to bring the progressive decoder up
against libjpeg, that work is done, and they were deleted rather than left
looking like a debugging facility that works.

**Behavior-altering options** `recover_stuff_zero` and `pad_at_eob` remain
**runtime** (environment variable) for field debugging of truncated or
non-byte-aligned streams. They are not part of T.81; use only for recovery.
Set `GIMG_JPEG_RECOVER_STUFF_ZERO=1` to enable recover_stuff_zero (treat missing
bits at segment end as 0). Do not set pad_at_eob for normal decode.

**Other codecs (e.g. PNG):** If adding debug or trace in the future, use
compile-time defines in a single internal header (e.g. `png_debug_internal.h`)
rather than environment variables, consistent with JPEG.

## Sanitizers (ASan / UBSan)

The Makefile provides:

| Target | Description |
|--------|-------------|
| `make test-asan` | Build with AddressSanitizer + UndefinedBehaviorSanitizer and run the full test suite (Linux). |
| `make test-ubsan` | Alias for `test-asan`. |

Build artifacts go to a separate directory (e.g. `build/linux/release-asan/`) so the normal build is unchanged. Dependencies (e.g. the compress library) are linked from their normal build; only the image library and tests are instrumented. Document in CI or local workflow: run `make test-asan` in addition to `make test` and `make test-valgrind` to catch memory and undefined-behavior bugs. **Before release or major changes**, run the full test suite (PNG and JPEG) under Valgrind and under ASan/UBSan; this is expected for both codecs.

## Probe result lifetime

`GIMG_Probe_Result.format_name` is valid only until the next call that mutates the codec registry (e.g. `gimg_codec_register()`). Callers must not store the pointer long-term; copy the string if they need to keep it. See @ref api_options "API Options and Types".
