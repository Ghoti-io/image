/**
 * @file
 *
 * Shared JPEG test helpers: load reference files, raster hash, compare.
 * Link jpeg_test_utils.o into test_jpeg_load and test_jpeg_encode.
 * Requires GIMG_TEST_DATA_JPEG when compiling for file-load and output-dir
 * helpers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_CODEC_JPEG_JPEG_TEST_UTILS_H
#define GIMG_TESTS_CODEC_JPEG_JPEG_TEST_UTILS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GIMG_Raster;

namespace jpeg_test {

/** Load a JPEG reference file into a buffer. Requires GIMG_TEST_DATA_JPEG. */
bool load_jpeg_file(const char * filename, std::vector<uint8_t> & out);

/**
 * FNV-1a 64-bit hash over raster pixel bytes (row-major, width*bpp per row).
 * Only pixel data is hashed, not stride padding.
 */
uint64_t raster_pixel_hash(const GIMG_Raster * raster);

/** Return true if two rasters have same dimensions, format, and pixel data. */
bool rasters_equal(const GIMG_Raster * a, const GIMG_Raster * b);

#ifdef GIMG_TEST_DATA_JPEG
/** Output directory for encoded JPEGs (tests/out/jpeg). */
std::string jpeg_output_dir(void);

/** Write JPEG bytes to tests/out/jpeg/ for verification. */
void write_jpeg_output(
    const char * filename, const uint8_t * data, size_t size);
#endif

} // namespace jpeg_test

#endif // GIMG_TESTS_CODEC_JPEG_JPEG_TEST_UTILS_H
