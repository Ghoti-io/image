@page functions_index Function Index

# Function Index

The complete API reference is generated from the header files. Below is a categorized index of the main public APIs; see the headers and \ref api_options "API Options and Types" for details.

## Codec (load, save, decode, probe)

- **Registry:** `gimg_codec_create_stub`, `gimg_codec_create_stub_with_allocator`, `gimg_codec_register`, `gimg_codec_count`, `gimg_codec_by_index`, `gimg_codec_by_name`, `gimg_codec_name`, `gimg_codec_capabilities` — see `ghoti.io/image/codec.h`.
- **Probe and dispatch:** `gimg_probe`, `gimg_doc_load`, `gimg_doc_save`, `gimg_item_decode`, `gimg_item_ensure_decoded` — see `ghoti.io/image/codec.h`. Options and types (e.g. **GIMG_Load_Options**, **GIMG_Save_Options**, **GIMG_Decode_Options**, **GIMG_Save_Report**) are documented in \ref api_options "API Options and Types".

## Document and item

- **Document:** `gimg_doc_create`, `gimg_doc_create_with_allocator`, `gimg_doc_destroy`, `gimg_doc_item_count`, `gimg_doc_item`, `gimg_doc_set_item_count`, `gimg_doc_meta_raw`, `gimg_doc_ensure_meta_raw`, `gimg_doc_copy`, `gimg_doc_copy_with_allocator`, `gimg_doc_from_raster`, `gimg_doc_from_raster_with_allocator` — see `ghoti.io/image/doc.h`.
- **Document screen properties:** `gimg_doc_background_color`, `gimg_doc_set_background_color`, `gimg_doc_clear_background_color`, `gimg_doc_pixel_aspect_ratio`, `gimg_doc_set_pixel_aspect_ratio`, `gimg_doc_clear_pixel_aspect_ratio` — what the file says belongs behind the image and what shape a pixel is; reported, never applied. See `ghoti.io/image/doc.h`.
- **Document animation (how many times to play):** `gimg_doc_loop_count`, `gimg_doc_set_loop_count`, `gimg_doc_clear_loop_count` — GIF's NETSCAPE2.0 count and APNG's `acTL` `num_plays` under one accessor; 0 means forever, and a file may declare nothing at all. See `ghoti.io/image/doc.h`.
- **Item:** `gimg_item_raster`, `gimg_item_set_raster`, `gimg_item_copy` — see `ghoti.io/image/doc.h`.
- **Item animation (frame delay, dispose, blend):** `gimg_item_frame_delay`, `gimg_item_set_frame_delay`, `gimg_item_dispose_op`, `gimg_item_set_dispose_op`, `gimg_item_blend_op`, `gimg_item_set_blend_op` — see `ghoti.io/image/doc.h` and \ref api_options_animation "Animation (item frame API)".

## Core and diagnostics

- **Result and diagnostics:** `gimg_result_string`, **GIMG_Result**, **GIMG_Strictness**, **GIMG_Diagnostics**, **GIMG_Diagnostic** — see `ghoti.io/image/core.h`.
- **Diagnostics lifecycle:** `gimg_diagnostics_init`, `gimg_diagnostics_append`, `gimg_diagnostics_clear`, `gimg_diagnostics_destroy` — see `ghoti.io/image/core.h` and \ref api_options_diagnostics_functions "Diagnostics API (lifecycle)".

## Raster copy and comparison

- **Raster copy:** `gimg_raster_copy`, `gimg_raster_copy_with_allocator` — allocate a new raster with the same dimensions and format, copy pixel data (row-by-row, source stride respected) and color info; caller owns the result. See `ghoti.io/image/raster.h`.
- **Raster comparison:** `gimg_ops_raster_equal` — returns true if dimensions, format, and pixel data match (strides may differ); color info is not compared. See `ghoti.io/image/ops.h`.

## Bit-depth conversion (8, 12, 16 bits per channel)

- **Sample-level** (see `ghoti.io/image/bitdepth.h`): `gimg_bitdepth_8_to_12`, `gimg_bitdepth_8_to_16`, `gimg_bitdepth_12_to_8`, `gimg_bitdepth_12_to_16`, `gimg_bitdepth_16_to_8`, `gimg_bitdepth_16_to_12` — bitshift/scale and clamp; 12-bit range 0..4095, 16-bit 0..65535. Codecs (e.g. JPEG) use these when raster depth differs from codec precision.
- **Raster-level:** `gimg_ops_convert_bit_depth` — convert a raster to another bit depth (8, 12, or 16) with the same channel model (GRAY or RGBA); uses the library bit-depth functions. See `ghoti.io/image/ops.h`.

## Copy and convenience helpers

- **Document copy:** `gimg_doc_copy` / `gimg_doc_copy_with_allocator` — duplicate document structure (item count, per-item frame delay/dispose/blend) and attached rasters (each copied via `gimg_raster_copy`); doc-level meta_common and meta_raw are deep-copied if present. The copy is synthetic (no `loaded_by_codec`). Use to save a variant or duplicate a doc.
- **Doc from raster:** `gimg_doc_from_raster` / `gimg_doc_from_raster_with_allocator` — create a one-item document with a copy of the given raster attached to item 0. Use with `gimg_doc_save` to "save this one raster".
- **Ensure decoded:** `gimg_item_ensure_decoded` — if the item already has an attached raster, no-op; otherwise decode via the document's codec and attach the raster to the item (document owns it). Simplifies load → modify → save: load, ensure_decoded on needed items, modify rasters, save.
- **Item copy:** `gimg_item_copy` — copy one item into another (frame delay, dispose, blend, and attached raster). Source and destination may be in the same or different documents. Use to duplicate a frame or copy an item into another doc.

Ownership: caller owns rasters and documents returned from `gimg_raster_copy`, `gimg_doc_copy`, and `gimg_doc_from_raster`. `gimg_item_ensure_decoded` attaches the decoded raster to the item, so the document owns it.

## Metadata and stream

- **Metadata:** `gimg_meta_common_*`, `gimg_meta_raw_*`, `gimg_meta_raw_copy`, `gimg_meta_raw_copy_with_allocator`; **GIMG_Meta_Policy**, **GIMG_Orientation** — see `ghoti.io/image/meta.h`.
- **Stream and limits:** `gimg_stream_*`, `gimg_limits_default`; **GIMG_Limits** — see `ghoti.io/image/stream.h`.
