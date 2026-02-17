# Developer guide

This document describes the image library layout, error-handling policy, testing, and how to add a new codec. It is aimed at contributors and maintainers.

## Prerequisites

The following are required for the full test suite and JPEG verification:

- **Python 3** — Used to generate JPEG fixtures and verify encode output.
- **Pillow (PIL)** — Used by `tests/data/jpeg/generate.py` (fixture generation) and `tests/data/jpeg/verify_jpeg_output.py` (encode verification). Install with `pip install Pillow`. JPEG fixtures: `python3 tests/data/jpeg/generate.py` (run after cloning or when adding fixtures).
- **libjpeg-turbo (for decode oracle tests)** — The `Decode*PillowOracle` tests use **libjpeg** (stock, unmodified) as the decode oracle via small C tools in `tests/data/jpeg/`. Build them with `make jpeg-oracle-tools`; this requires libjpeg development headers and library (pkg-config libjpeg). If the oracle tools are not built, the decode oracle tests fail with a message to run `make jpeg-oracle-tools`. The image library **does not link to** libjpeg; the ref tools are used only by tests.

  - **Linux (e.g. WSL, Ubuntu/Debian):**
    ```bash
    sudo apt install libjpeg-turbo8-dev
    ```
  - **MSYS2 (Windows):** In a MINGW64 shell: `pacman -S mingw-w64-x86_64-libjpeg-turbo` (use `mingw-w64-i686-libjpeg-turbo` for 32‑bit).

- **libjpeg-turbo programs (optional)** — **cjpeg**/djpeg are used by `tests/data/jpeg/create_libjpeg_progressive_fixture.py` to generate a libjpeg-encoded progressive fixture. Optional; install `libjpeg-turbo-progs` (Linux) if you need that script.

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
| `jpeg_idct.c` | Dezigzag, dequantise, 8×8 inverse DCT (float, 32-bit, and T.81 integer islow). Used by jpeg_entropy.c after block decode. |
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

After `make test`, PNG output is verified with `tests/data/png/verify_png_output.py` (e.g. via PIL). JPEG decode correctness is validated against Pillow (Python) where applicable (`Decode*PillowOracle` tests run `tests/data/jpeg/pillow_decode_hash.py`); JPEG encode output is verified by `tests/data/jpeg/verify_jpeg_output.py` (PIL opens each file in `tests/out/jpeg/`). **Pillow is required** for these JPEG tests (see Prerequisites above).

**JPEG test map:** Load and segment/limit behaviour: `tests/codec/jpeg/test_jpeg_load.cpp` (e.g. SOF rejection, DNL, DHT/SOS negative tests, golden/oracle). Encode, round-trip, and save: `tests/codec/jpeg/test_jpeg_encode.cpp` (quality, chroma, progressive, 12/16-bit, DHT consistency, failure paths). Helpers: `jpeg_test_utils.cpp` / `jpeg_test_utils.h` (load_jpeg_file, raster hash, oracle helpers). Fuzz: `tests/fuzz/fuzz_jpeg_load` (load + decode; no crash on arbitrary input).

**Regenerating JPEG fixtures and oracle verification:** From the repo root: `python3 tests/data/jpeg/generate.py` to regenerate fixtures (requires Pillow). Build libjpeg oracle tools: `make jpeg-oracle-tools` (requires libjpeg-turbo dev package). Verify encode output: `make test-verify-jpeg` or `python3 tests/data/jpeg/verify_jpeg_output.py`. Decode oracle (Pillow or libjpeg): see `tests/data/jpeg/README.md` for `generate_jpeg_oracle_raws.py`, `pillow_decode_hash.py`, and libjpeg ref tool usage.

### Valgrind

Run the full suite under Valgrind to ensure no leaks and clean memory use:

```bash
make test-valgrind   # Linux only
make test-valgrind-quiet
```

Tests that pass diagnostics must call `gimg_diagnostics_clear()` or `gimg_diagnostics_destroy()` so Valgrind stays clean. JPEG changes should be validated with `make test-quiet` and `make test-valgrind-quiet`; whether Valgrind runs in CI or only as a developer checklist is project policy (see task image-phase-2.3-jpeg-quality-maintainability.md).

### Sanitizers (ASan + UBSan)

AddressSanitizer and UndefinedBehaviorSanitizer builds use a separate build directory and run the same test suite:

```bash
make test-asan   # Build with ASan+UBSan and run tests (Linux)
make test-ubsan  # Alias for test-asan
```

These targets are documented in the main Makefile and in this guide. Use them in development or CI to catch use-after-free, buffer overflows, and undefined behavior. Sanitizer support is currently implemented for Linux; the build uses `build/$(BUILD)-asan/` (e.g. `build/linux/release-asan/`).

### Fuzzing

LibFuzzer harnesses under `tests/fuzz/` cover PNG/APNG load and decode, PNG round-trip (load/save/load), JPEG load and decode, and JPEG round-trip (load/save/load). Build with `make fuzz-png`, `make fuzz-png-encode`, `make fuzz-jpeg`, or `make fuzz-jpeg-encode` (requires clang); run with a corpus as described in the Makefile and `tests/fuzz/README.md`.

### Limits and failure-path tests

Tests under `tests/` include:

- **Limits:** `max_chunk_size`, `max_decoded_pixels`, `max_frame_count` set so the library returns **GIMG_ERR_LIMIT**; diagnostics may report the limit.
- **Save failure paths:** `gimg_doc_save` with NULL doc, invalid/unsupported format, or unsupported raster format; expect documented error codes and no crash.

## Adding a new codec

1. **Stub and register:** Create a codec stub (e.g. `gimg_codec_create_stub()` or `gimg_codec_create_stub_with_allocator()`), implement probe (magic bytes / peek), and register with `gimg_codec_register()`. Probe result `format_name` is used by load/save; document that `format_name` lifetime is only until the next registry-mutating call (see @ref api_options "API Options and Types").

2. **Load:** Implement load in the codec: parse container structure, create **GIMG_Doc** and **GIMG_Item**(s), fill frame timing and blend/dispose for animation if applicable. Enforce **GIMG_Limits** (chunk size, decoded pixels, frame count); return **GIMG_ERR_LIMIT** when exceeded. Use **GIMG_Diagnostics** when provided (e.g. CRC errors, unsupported features). Populate metadata common from format-specific metadata (e.g. Exif orientation) when present.

3. **Decode:** Implement decode from **GIMG_Item** to **GIMG_Raster**: respect **GIMG_Decode_Options** limits, use safe pixel-count helpers (e.g. from `safe_math_internal.h`) to avoid overflow, return **GIMG_ERR_CORRUPT** or **GIMG_ERR_LIMIT** as appropriate.

4. **Save:** Implement save from **GIMG_Doc** to stream for the codec’s format name; apply metadata policy (PRESERVE_ALL, STRIP_GPS, NORMALIZE_EXIF, etc.) as documented in format-references and api-options.

5. **Tests:** Add decode/encode tests (and round-trip if applicable); use shared test helpers where possible. Add fuzz coverage for load/decode and, if feasible, save. Ensure limits and failure-path tests cover the new codec.

6. **Docs:** Update `documentation/format-references.md` and option docs for format-specific behavior and limits.

## JPEG debug and recovery options

JPEG debug and trace output are controlled by **compile-time** defines in
`src/codec/jpeg/jpeg_debug_internal.h`. All default to 0. To enable a category
for a debug build, define it when compiling (e.g. `-DGIMG_JPEG_DEBUG_LOAD=1`).
See that header for the full list (e.g. `GIMG_JPEG_DEBUG_LOAD`,
`GIMG_JPEG_TRACE_DC_BLOCK`, `GIMG_JPEG_DUMP_FIRST_MCU_COEF`). Dump options that
write files may still read the output path from the same-named environment
variable when the category is enabled at compile time.

**Behaviour-altering options** `recover_stuff_zero` and `pad_at_eob` remain
**runtime** (environment variable) for field debugging of truncated or
non-byte-aligned streams. They are not part of T.81; use only for recovery.
Set `GIMG_JPEG_RECOVER_STUFF_ZERO=1` to enable recover_stuff_zero (treat missing
bits at segment end as 0). Do not set pad_at_eob for normal decode.

## Sanitizers (ASan / UBSan)

The Makefile provides:

| Target | Description |
|--------|-------------|
| `make test-asan` | Build with AddressSanitizer + UndefinedBehaviorSanitizer and run the full test suite (Linux). |
| `make test-ubsan` | Alias for `test-asan`. |

Build artifacts go to a separate directory (e.g. `build/linux/release-asan/`) so the normal build is unchanged. Dependencies (e.g. the compress library) are linked from their normal build; only the image library and tests are instrumented. Document in CI or local workflow: run `make test-asan` in addition to `make test` and `make test-valgrind` to catch memory and undefined-behavior bugs.

## Probe result lifetime

`GIMG_Probe_Result.format_name` is valid only until the next call that mutates the codec registry (e.g. `gimg_codec_register()`). Callers must not store the pointer long-term; copy the string if they need to keep it. See @ref api_options "API Options and Types".
