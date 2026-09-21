/**
 * @file
 *
 * Header-only helpers for the GIF decode tests: loading a fixture, running it
 * through load + decode, and reading pixels back as RGBA8.  The build defines
 * GIMG_TEST_DATA_GIF as the path to tests/data/gif/.
 *
 * A GIF item is a frame composited onto the logical screen, so decode(i)
 * returns the whole canvas as it stands after frame i, not the patch that
 * frame i happens to carry.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_CODEC_GIF_GIF_TEST_UTILS_H
#define GIMG_TESTS_CODEC_GIF_GIF_TEST_UTILS_H

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string>
#include <vector>

namespace gif_test {

/** One pixel as the tests spell it out. */
struct Rgba {
  uint8_t r, g, b, a;

  bool operator==(const Rgba & o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
};

/** Path to a fixture in tests/data/gif/. */
inline std::string data_path(const char * filename) {
  return std::string(GIMG_TEST_DATA_GIF) + "/" + filename;
}

/** Read a fixture into memory.  Returns false when the file is missing. */
inline bool load_file(const char * filename, std::vector<uint8_t> & out) {
  std::ifstream in(data_path(filename), std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in),
      std::istreambuf_iterator<char>());
  return !out.empty();
}

/**
 * Hold a loaded document and its stream so both outlive the caller's use of
 * the decoded raster.
 */
class Loaded {
public:
  Loaded() = default;

  ~Loaded() {
    if (raster_) {
      gimg_raster_destroy(raster_);
    }
    if (doc_) {
      gimg_doc_destroy(doc_);
    }
    if (stream_) {
      gimg_stream_destroy(stream_);
    }
  }

  Loaded(const Loaded &) = delete;
  Loaded & operator=(const Loaded &) = delete;

  /** Load a fixture; returns the load result. */
  GIMG_Result load(const char * filename,
      const GIMG_Load_Options * options = nullptr) {
    if (!load_file(filename, bytes_)) {
      return GIMG_ERR_IO;
    }
    return load_bytes(bytes_, options);
  }

  /** Load from a buffer the caller already holds. */
  GIMG_Result load_bytes(const std::vector<uint8_t> & bytes,
      const GIMG_Load_Options * options = nullptr) {
    GIMG_Result r =
        gimg_stream_create_memory(bytes.data(), bytes.size(), &stream_);
    if (r != GIMG_OK) {
      return r;
    }
    return gimg_doc_load(stream_, options, nullptr, &doc_);
  }

  /** Decode one frame, composited onto the canvas.  Defaults to the first. */
  GIMG_Result decode(const GIMG_Decode_Options * options = nullptr,
      size_t item_index = 0) {
    if (raster_) {
      gimg_raster_destroy(raster_);
      raster_ = nullptr;
    }
    GIMG_Item * item = gimg_doc_item(doc_, item_index);
    if (!item) {
      return GIMG_ERR_INTERNAL;
    }
    return gimg_item_decode(item, options, &raster_);
  }

  GIMG_Doc * doc() const { return doc_; }
  GIMG_Raster * raster() const { return raster_; }

  uint32_t width() const { return gimg_raster_width(raster_); }
  uint32_t height() const { return gimg_raster_height(raster_); }

  /** Read one pixel of the decoded RGBA8 raster. */
  Rgba at(uint32_t x, uint32_t y) const {
    const uint8_t * pixels =
        static_cast<const uint8_t *>(gimg_raster_pixels_const(raster_));
    const uint8_t * px =
        pixels + (y * gimg_raster_stride_bytes(raster_)) + (x * 4u);
    return Rgba{px[0], px[1], px[2], px[3]};
  }

private:
  std::vector<uint8_t> bytes_;
  GIMG_Stream * stream_ = nullptr;
  GIMG_Doc * doc_ = nullptr;
  GIMG_Raster * raster_ = nullptr;
};

/** Print an Rgba readably in an assertion failure. */
inline std::ostream & operator<<(std::ostream & os, const Rgba & p) {
  char buf[32];
  snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%u)", p.r, p.g, p.b, p.a);
  return os << buf;
}

} // namespace gif_test

#endif // GIMG_TESTS_CODEC_GIF_GIF_TEST_UTILS_H
