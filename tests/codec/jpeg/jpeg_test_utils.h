/**
 * @file
 *
 * Shared JPEG test helpers: load reference files, raster hash, compare.
 * Link jpeg_test_utils.o into test_jpeg_load and test_jpeg_encode. The build
 * defines GIMG_TEST_DATA_JPEG to the path of tests/data/jpeg/.
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

/** Load a JPEG reference file into a buffer (from GIMG_TEST_DATA_JPEG path). */
bool load_jpeg_file(const char * filename, std::vector<uint8_t> & out);

/**
 * FNV-1a 64-bit hash over raster pixel bytes (row-major, width*bpp per row).
 * Only pixel data is hashed, not stride padding.
 */
uint64_t raster_pixel_hash(const GIMG_Raster * raster);

/** Return true if two rasters have same dimensions, format, and pixel data. */
bool rasters_equal(const GIMG_Raster * a, const GIMG_Raster * b);

/**
 * If a and b differ, return a short string describing the first differing
 * pixel (e.g. "First diff at (2,1): ..."). Otherwise return empty string.
 */
std::string raster_first_diff(const GIMG_Raster * a, const GIMG_Raster * b);

/**
 * Return true if two rasters have same dimensions/format and no channel
 * difference exceeds max_diff (for baseline vs progressive decode tolerance).
 */
bool rasters_equal_with_tolerance(
    const GIMG_Raster * a, const GIMG_Raster * b, int max_diff);

/**
 * Read a binary PNM (P5 grey or P6 colour) from GIMG_TEST_DATA_JPEG.
 *
 * The lossless fixtures keep their source image beside them as a PNM rather
 * than as an opaque .raw dump, because a lossless codec has to return exactly
 * what went in: the source is the expected output, and in this form it can be
 * viewed and regenerated without a tool.  @p out_bits is the sample precision
 * implied by maxval, and samples come back one per channel, row-major.
 */
bool load_pnm_file(const char * filename, uint32_t * out_w, uint32_t * out_h,
    int * out_channels, int * out_bits, std::vector<uint32_t> & out_samples);

/**
 * Widen a sample the way the JPEG decoder does when it puts a P-bit lossless
 * sample into an 8- or 16-bit raster (bit replication, not a shift), so a
 * fixture's own values can be compared with what the decoder produced.
 */
uint32_t widen_sample(uint32_t v, int from_bits, int to_bits);

/** Output directory for encoded JPEGs (tests/out/jpeg). */
std::string jpeg_output_dir(void);

/** Write JPEG bytes to tests/out/jpeg/ for verification. */
void write_jpeg_output(
    const char * filename, const uint8_t * data, size_t size);

/**
 * Write expected decoded pixels (raw RGB, row-major) for Pillow oracle check.
 * File is written as <filename_base>.expected; verify_jpeg_output.py will
 * decode the corresponding .jpg with Pillow and compare to this.
 */
void write_jpeg_expected_pixels(
    const char * filename_base, const uint8_t * rgb_data, size_t size);

/**
 * Run tests/data/jpeg/dump_jpeg_pixels_ref (libjpeg-based decode oracle) on
 * a fixture and parse expected hash and dimensions. Returns true and sets
 * out_* if the tool succeeds; otherwise false (e.g. ref not built).
 * Build the ref with: make jpeg-oracle-tools (see tests/data/jpeg/README.md).
 */
bool pillow_oracle_hash(const char * fixture_filename, uint64_t * out_hash,
    uint32_t * out_width, uint32_t * out_height);

/**
 * Same as pillow_oracle_hash but for an arbitrary file path (e.g. from
 * jpeg_output_dir()). Ref tool is run from tests/data/jpeg/.
 */
bool pillow_oracle_hash_from_path(const char * file_path, uint64_t * out_hash,
    uint32_t * out_width, uint32_t * out_height);

/** Load a JPEG file from an arbitrary path (e.g. jpeg_output_dir() + filename). */
bool load_jpeg_from_path(const char * file_path, std::vector<uint8_t> & out);

/** Oracle .raw format: 1 byte mode (0=L, 1=RGB, 2=CMYK), 4 bytes width LE, 4 bytes height LE, then pixels. */
enum JpegOracleRawMode { kOracleL = 0, kOracleRgb = 1, kOracleCmyk = 2 };

/**
 * Load oracle .raw file (Pillow decode of a JPEG). Returns true and fills
 * out_pixels, out_width, out_height, out_mode. The .raw path is
 * GIMG_TEST_DATA_JPEG / (fixture_base + ".raw").
 */
bool load_jpeg_oracle_raw(const char * fixture_base, std::vector<uint8_t> & out_pixels,
    uint32_t * out_width, uint32_t * out_height, int * out_mode);

/** Load oracle .raw from an arbitrary path (same 9-byte header + pixels format). */
bool load_jpeg_oracle_raw_from_path(const char * raw_path,
    std::vector<uint8_t> & out_pixels, uint32_t * out_width,
    uint32_t * out_height, int * out_mode);

/**
 * Run dump_jpeg_pixels_ref -o raw_path jpeg_path (libjpeg decode), then load
 * the .raw. Returns true and fills out_* if the ref succeeded and the .raw
 * was loaded. Use this to compare our decoder output to libjpeg byte-for-byte.
 */
bool libjpeg_decode_to_oracle_raw(const char * jpeg_path, const char * raw_path,
    std::vector<uint8_t> & out_pixels, uint32_t * out_width,
    uint32_t * out_height, int * out_mode);

/**
 * Run encode_libjpeg_baseline_scan to produce a full JPEG (oracle-encoded).
 * Writes width x height grayscale (x+y)&0xFF at quality, optional restart_interval.
 * jpeg_path is where the tool writes the JPEG. Returns true if the tool succeeded
 * and jpeg_path exists. Requires make jpeg-oracle-tools (or jpeg-encode-oracle).
 */
bool libjpeg_encode_baseline_to_file(const char * jpeg_path,
    unsigned int width, unsigned int height, int quality,
    unsigned int restart_interval);

/**
 * Return true if decoded raster matches oracle raw pixels (same dimensions,
 * pixel values within tolerance). Converts our raster to L or RGB to match
 * oracle mode. tolerance = max per-component difference (0 = exact).
 */
bool raster_matches_oracle_raw(const GIMG_Raster * raster,
    const uint8_t * raw_pixels, uint32_t raw_w, uint32_t raw_h, int raw_mode,
    int tolerance);

} // namespace jpeg_test

#endif // GIMG_TESTS_CODEC_JPEG_JPEG_TEST_UTILS_H
