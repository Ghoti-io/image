@page functions_index Function Index

# Function Index

The complete API reference is generated from the header files. Below is a categorized index of the main public APIs; see the headers and \ref api_options "API Options and Types" for details.

## Codec (load, save, decode, probe)

- **Registry:** `gimg_codec_create_stub`, `gimg_codec_create_stub_with_allocator`, `gimg_codec_register`, `gimg_codec_count`, `gimg_codec_by_index`, `gimg_codec_by_name`, `gimg_codec_name`, `gimg_codec_capabilities` — see `ghoti.io/image/codec.h`.
- **Probe and dispatch:** `gimg_probe`, `gimg_doc_load`, `gimg_doc_save`, `gimg_item_decode` — see `ghoti.io/image/codec.h`. Options and types (e.g. **GIMG_Load_Options**, **GIMG_Save_Options**, **GIMG_Decode_Options**, **GIMG_Save_Report**) are documented in \ref api_options "API Options and Types".

## Document and item

- **Document:** `gimg_doc_create`, `gimg_doc_create_with_allocator`, `gimg_doc_destroy`, `gimg_doc_item_count`, `gimg_doc_item`, `gimg_doc_set_item_count`, `gimg_doc_meta_raw`, `gimg_doc_ensure_meta_raw` — see `ghoti.io/image/doc.h`.
- **Item animation (frame delay, dispose, blend):** `gimg_item_frame_delay`, `gimg_item_set_frame_delay`, `gimg_item_dispose_op`, `gimg_item_set_dispose_op`, `gimg_item_blend_op`, `gimg_item_set_blend_op` — see `ghoti.io/image/doc.h` and \ref api_options_animation "Animation (item frame API)".

## Core and diagnostics

- **Result and diagnostics:** `gimg_result_string`, `gimg_diagnostics_append`; **GIMG_Result**, **GIMG_Strictness**, **GIMG_Diagnostics**, **GIMG_Diagnostic** — see `ghoti.io/image/core.h`.

## Metadata and stream

- **Metadata:** `gimg_meta_common_*`, `gimg_meta_raw_*`; **GIMG_Meta_Policy**, **GIMG_Orientation** — see `ghoti.io/image/meta.h`.
- **Stream and limits:** `gimg_stream_*`, `gimg_limits_default`; **GIMG_Limits** — see `ghoti.io/image/stream.h`.
