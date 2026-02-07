/**
 * @file apng_white_block.c
 *
 * Example: programmatically create an APNG image with the Ghoti.io Image
 * library. Produces a 100×100 px animation: a 15×15 px white block on a black
 * background moves from the lower-left to the upper-right.
 *
 * This example shows how to:
 * - Create a document and set the number of frames (items)
 * - Create RGBA rasters and attach them to items (synthetic document)
 * - Set frame delay and dispose/blend for APNG
 * - Save as PNG (APNG) to a memory stream and write the result to a file
 *
 * Build: See Makefile target "examples"
 *
 * Usage: apng_white_block [output.png]
 *   Default output path: apng_white_block.png
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/image.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Animation parameters: image size, moving block size, and number of frames.
#define IMG_SIZE 100u
#define BLOCK_SIZE 15u
#define NUM_FRAMES 12u

//
// Fill the raster with opaque black. Pixels are RGBA 8-bit, row-major;
// row stride may be >= width*4 due to alignment.
//
static void fill_black_rgba8(unsigned char * pixels, size_t stride_bytes,
    uint32_t width, uint32_t height) {
  for (uint32_t y = 0; y < height; y++) {
    unsigned char * row = pixels + (size_t)y * stride_bytes;
    for (uint32_t x = 0; x < width; x++) {
      row[x * 4 + 0] = 0;
      row[x * 4 + 1] = 0;
      row[x * 4 + 2] = 0;
      row[x * 4 + 3] = 255;
    }
  }
}

//
// Draw a BLOCK_SIZE x BLOCK_SIZE white rectangle at (block_x, block_y).
// Clips to image bounds. Same RGBA layout as above.
//
static void draw_white_block(unsigned char * pixels, size_t stride_bytes,
    uint32_t block_x, uint32_t block_y) {
  uint32_t x_end = block_x + BLOCK_SIZE;
  uint32_t y_end = block_y + BLOCK_SIZE;
  if (x_end > IMG_SIZE) {
    x_end = IMG_SIZE;
  }
  if (y_end > IMG_SIZE) {
    y_end = IMG_SIZE;
  }
  for (uint32_t y = block_y; y < y_end; y++) {
    unsigned char * row = pixels + (size_t)y * stride_bytes;
    for (uint32_t x = block_x; x < x_end; x++) {
      row[x * 4 + 0] = 255;
      row[x * 4 + 1] = 255;
      row[x * 4 + 2] = 255;
      row[x * 4 + 3] = 255;
    }
  }
}

int main(int argc, char ** argv) {
  const char * out_path = (argc > 1) ? argv[1] : "apng_white_block.png";

  printf("=== Ghoti.io Image - APNG White Block Example ===\n\n");
  printf("Library version: %s\n", gimg_version_string());
  printf("Creating %u x %u APNG, %u frames, 15 x 15 white block lower-left -> "
         "upper-right\n\n",
      (unsigned)IMG_SIZE, (unsigned)IMG_SIZE, (unsigned)NUM_FRAMES);

  //
  // Step 1: Create a document and set the number of frames.
  // A "synthetic" document (not loaded from a file) has one item per frame;
  // we attach rasters to each item so the PNG encoder can save them as APNG.
  //
  GIMG_Doc * doc = NULL;
  GIMG_Result r = gimg_doc_create(&doc);
  if (r != GIMG_OK || !doc) {
    fprintf(stderr, "Error: Failed to create document\n");
    return 1;
  }

  r = gimg_doc_set_item_count(doc, NUM_FRAMES);
  if (r != GIMG_OK) {
    fprintf(stderr, "Error: Failed to set frame count\n");
    gimg_doc_destroy(doc);
    return 1;
  }

  //
  // Step 2: For each frame, create a raster, draw this frame's content,
  // attach it to the document item, and set APNG timing/dispose/blend.
  //
  for (size_t i = 0; i < NUM_FRAMES; i++) {
    GIMG_Item * item = gimg_doc_item(doc, (size_t)i);
    if (!item) {
      gimg_doc_destroy(doc);
      return 1;
    }

    // One RGBA 8-bit raster per frame; library owns the pixel buffer (NULL, 0).
    GIMG_Raster * raster = NULL;
    r = gimg_raster_create(IMG_SIZE, IMG_SIZE, &GIMG_PIXEL_RGBA8,
        GIMG_RASTER_OWNED, NULL, 0, &raster);
    if (r != GIMG_OK || !raster) {
      fprintf(stderr, "Error: Failed to create raster for frame %zu\n", i);
      gimg_doc_destroy(doc);
      return 1;
    }

    size_t stride = gimg_raster_stride_bytes(raster);
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
    fill_black_rgba8(pixels, stride, IMG_SIZE, IMG_SIZE);

    // Position the white block. Row 0 = top; so lower-left is (0, 85) and
    // upper-right is (85, 0). We interpolate linearly over the frames.
    uint32_t max_off = IMG_SIZE - BLOCK_SIZE;
    uint32_t block_x = (NUM_FRAMES > 1)
        ? (uint32_t)((unsigned long long)i * max_off / (NUM_FRAMES - 1))
        : 0u;
    uint32_t block_y = (NUM_FRAMES > 1)
        ? (uint32_t)((unsigned long long)(NUM_FRAMES - 1 - i) * max_off /
              (NUM_FRAMES - 1))
        : max_off;
    draw_white_block(pixels, stride, block_x, block_y);

    // Attach this raster to the item. The document takes ownership; it will
    // be freed when the document is destroyed. The PNG saver uses these
    // attached rasters when saving a synthetic document (no loaded_by_codec).
    gimg_item_set_raster(item, raster);

    // APNG frame timing: display this frame for delay_num/delay_den seconds
    // (10/100 = 0.1 s per frame).
    gimg_item_set_frame_delay(item, 10, 100);
    // Before the next frame, clear this frame's region to background so the
    // next frame is drawn on a clean canvas (no leftover pixels).
    gimg_item_set_dispose_op(item, GIMG_DISPOSE_BACKGROUND);
    // This frame fully replaces the frame region (no alpha blend).
    gimg_item_set_blend_op(item, GIMG_BLEND_SOURCE);
  }

  //
  // Step 3: Save the document as PNG (APNG). The library has no file stream
  // API, so we save to a memory output stream, then write the buffer to a
  // file.
  //
  GIMG_Stream * stream = NULL;
  r = gimg_stream_create_memory_output(&stream);
  if (r != GIMG_OK || !stream) {
    fprintf(stderr, "Error: Failed to create output stream\n");
    gimg_doc_destroy(doc);
    return 1;
  }

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_KEEP_COMMON_ONLY,
      .interlaced = 0,
      .jpeg_chroma_subsampling = 0,
      .jpeg_progressive = 0,
  };
  GIMG_Save_Report report = {0};

  r = gimg_doc_save(doc, stream, "png", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = NULL;

  if (r != GIMG_OK) {
    fprintf(stderr, "Error: Save failed\n");
    gimg_stream_destroy(stream);
    return 1;
  }

  const void * out_data = NULL;
  size_t out_size = 0;
  gimg_stream_output_buffer(stream, &out_data, &out_size);

  FILE * f = fopen(out_path, "wb");
  if (!f) {
    fprintf(stderr, "Error: Cannot open output file: %s\n", out_path);
    gimg_stream_destroy(stream);
    return 1;
  }
  if (out_size > 0 && fwrite(out_data, 1, out_size, f) != out_size) {
    fprintf(stderr, "Error: Write failed\n");
    fclose(f);
    gimg_stream_destroy(stream);
    return 1;
  }
  fclose(f);
  gimg_stream_destroy(stream);

  printf("Wrote %zu bytes to %s\n", out_size, out_path);
  printf("SUCCESS: APNG created.\n");

  return 0;
}
