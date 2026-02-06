@page examples Examples

# Examples

Example programs are built with the **examples** Makefile target and appear under the build directory (e.g. `build/linux/release/apps/examples/`).

## Programmatic APNG creation (synthetic document)

The **apng_white_block** example shows how to generate an animated PNG from in-memory rasters without loading an existing file:

1. **Create a document** with `gimg_doc_create()` and set the number of frames with `gimg_doc_set_item_count()`.
2. **For each frame:** create a raster (`gimg_raster_create()`), fill pixels, then attach it to the corresponding item with `gimg_item_set_raster()`. Set frame delay and dispose/blend for APNG with `gimg_item_set_frame_delay()`, `gimg_item_set_dispose_op()`, and `gimg_item_set_blend_op()`.
3. **Save** with `gimg_doc_save(doc, stream, "png", ...)`. The PNG codec obtains pixel data from each item’s attached raster (synthetic document path) and writes acTL, fcTL, IDAT, and fdAT chunks.

This pattern applies to any format that supports write: documents can be created programmatically and saved once rasters are attached to items. See the spec §3.5 (Document creation and synthetic documents) and §18.3 (Example usage).
