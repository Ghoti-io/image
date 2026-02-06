/**
 * @file
 *
 * Shared PNG test helpers: load reference files, raster hash, compare,
 * output-dir and write for encode tests. Link png_test_utils.o into
 * test_png_decode and test_png_encode. Requires GIMG_TEST_DATA_PNG to be
 * defined when compiling the .cpp for file-load and output-dir helpers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_CODEC_PNG_PNG_TEST_UTILS_H
#define GIMG_TESTS_CODEC_PNG_PNG_TEST_UTILS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GIMG_Raster;

namespace png_test {

/** Load a PNG reference file into a buffer. Requires GIMG_TEST_DATA_PNG. */
bool load_png_file(const char * filename, std::vector<uint8_t> & out);

/**
 * FNV-1a 64-bit hash over raster pixel bytes (row-major, width*bpp per row).
 * Only pixel data is hashed, not stride padding.
 */
uint64_t raster_pixel_hash(const GIMG_Raster * raster);

/** Return true if two rasters have same dimensions, format, and pixel data. */
bool rasters_equal(const GIMG_Raster * a, const GIMG_Raster * b);

#ifdef GIMG_TEST_DATA_PNG
/** Output directory for encoded PNGs (tests/out/png). */
std::string png_output_dir(void);

/** Write PNG bytes to tests/out/png/ for verification. */
void write_png_output(const char * filename, const uint8_t * data, size_t size);
#endif

} // namespace png_test

#endif /* GIMG_TESTS_CODEC_PNG_PNG_TEST_UTILS_H */
