/**
 * @file
 *
 * Load an image with this library and write a lossy WebP (stub VP8).
 * Used by tests/data/webp/verify_webp_rd.py.
 *
 * Build with: make webp-encode-lossy
 * Run: encode_webp_lossy <in.png|...> <out.webp>
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <vector>

namespace {

const GIMG_Limits * limits() {
  static GIMG_Limits lim;
  std::memset(&lim, 0, sizeof(lim));
  lim.max_decoded_pixels = 64u * 1024u * 1024u;
  lim.max_memory = 512u * 1024u * 1024u;
  return &lim;
}

bool read_file(const char * path, std::vector<unsigned char> & out) {
  std::FILE * f = std::fopen(path, "rb");
  if (!f) {
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    return false;
  }
  out.resize(static_cast<size_t>(size));
  bool ok =
      out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool write_file(const char * path, const void * data, size_t n) {
  std::FILE * f = std::fopen(path, "wb");
  if (!f) {
    return false;
  }
  bool ok = std::fwrite(data, 1, n, f) == n;
  std::fclose(f);
  return ok;
}

} // namespace

int main(int argc, char ** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: encode_webp_lossy <input> <out.webp>\n");
    return 2;
  }
  const char * in_path = argv[1];
  const char * out_path = argv[2];

  std::vector<unsigned char> bytes;
  if (!read_file(in_path, bytes)) {
    std::fprintf(stderr, "encode_webp_lossy: cannot read %s\n", in_path);
    return 1;
  }

  GIMG_Load_Options load_opts;
  std::memset(&load_opts, 0, sizeof(load_opts));
  load_opts.limits = limits();

  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    std::fprintf(stderr, "encode_webp_lossy: stream failed\n");
    return 1;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, &load_opts, nullptr, &doc);
  gimg_stream_destroy(in);
  if (r != GIMG_OK || !doc) {
    std::fprintf(stderr, "encode_webp_lossy: load failed (%d)\n", (int)r);
    return 1;
  }

  GIMG_Item * item = gimg_doc_item(doc, 0);
  if (!item) {
    gimg_doc_destroy(doc);
    std::fprintf(stderr, "encode_webp_lossy: no items\n");
    return 1;
  }
  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(item, nullptr, &raster);
  if (r != GIMG_OK || !raster) {
    gimg_doc_destroy(doc);
    std::fprintf(stderr, "encode_webp_lossy: decode failed (%d)\n", (int)r);
    return 1;
  }
  gimg_item_set_raster(item, raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    std::fprintf(stderr, "encode_webp_lossy: output stream failed\n");
    return 1;
  }
  GIMG_Save_Options save_opts;
  std::memset(&save_opts, 0, sizeof(save_opts));
  save_opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  save_opts.webp_effort = 4;
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  r = gimg_doc_save(doc, out, "webp", &save_opts, &report);
  if (r != GIMG_OK) {
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    std::fprintf(stderr, "encode_webp_lossy: save failed (%d)\n", (int)r);
    return 1;
  }

  const void * buf = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &buf, &n);
  if (!buf || n == 0u || !write_file(out_path, buf, n)) {
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    std::fprintf(stderr, "encode_webp_lossy: write failed\n");
    return 1;
  }
  std::printf("%zu\n", n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return 0;
}
