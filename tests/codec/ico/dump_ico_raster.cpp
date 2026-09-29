/**
 * @file
 *
 * Decode ICO/CUR files with this library and write each entry's packed RGBA8
 * raster to <outdir>/<stem>.<index>.rgba, printing one status line per entry.
 *
 * Used by tests/data/ico/verify_ico_pixels.py to compare our decode against
 * Pillow, ImageMagick and GdkPixbuf per entry.
 *
 * Build with: make ico-dump-raster
 * Run: build/.../dump_ico_raster <outdir> <file.ico>...
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
#include <string>
#include <vector>

namespace {

const GIMG_Limits * dump_limits() {
  static GIMG_Limits limits;
  std::memset(&limits, 0, sizeof(limits));
  limits.max_decoded_pixels = 64u * 1024u * 1024u;
  limits.max_memory = 512u * 1024u * 1024u;
  return &limits;
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
  bool ok = out.empty() ||
      std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

std::string stem_of(const char * path) {
  const char * slash = std::strrchr(path, '/');
  std::string name = slash ? slash + 1 : path;
  const size_t dot = name.rfind('.');
  if (dot != std::string::npos) {
    name.resize(dot);
  }
  return name;
}

} // namespace

int main(int argc, char ** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dump_ico_raster <outdir> <file.ico>...\n");
    return 2;
  }
  const std::string outdir = argv[1];

  for (int i = 2; i < argc; i++) {
    const char * path = argv[i];
    const std::string stem = stem_of(path);

    std::vector<unsigned char> bytes;
    if (!read_file(path, bytes)) {
      std::printf("%s\tunreadable\n", stem.c_str());
      continue;
    }

    GIMG_Load_Options load_options;
    std::memset(&load_options, 0, sizeof(load_options));
    load_options.limits = dump_limits();
    GIMG_Decode_Options decode_options;
    std::memset(&decode_options, 0, sizeof(decode_options));
    decode_options.limits = dump_limits();

    GIMG_Stream * stream = nullptr;
    if (gimg_stream_create_memory(bytes.data(), bytes.size(), &stream) !=
        GIMG_OK) {
      std::printf("%s\tstream-failed\n", stem.c_str());
      continue;
    }

    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(stream, &load_options, nullptr, &doc);
    gimg_stream_destroy(stream);
    if (r != GIMG_OK || !doc) {
      std::printf("%s\tload:%s\n", stem.c_str(), gimg_result_string(r));
      if (doc) {
        gimg_doc_destroy(doc);
      }
      continue;
    }

    const size_t count = gimg_doc_item_count(doc);
    for (size_t ei = 0; ei < count; ei++) {
      GIMG_Raster * raster = nullptr;
      r = gimg_item_decode(
          gimg_doc_item(doc, ei), &decode_options, &raster);
      if (r != GIMG_OK || !raster) {
        std::printf("%s\t%d\tdecode:%s\n", stem.c_str(), (int)ei,
            gimg_result_string(r));
        if (raster) {
          gimg_raster_destroy(raster);
        }
        continue;
      }

      const uint32_t width = gimg_raster_width(raster);
      const uint32_t height = gimg_raster_height(raster);
      const size_t stride = gimg_raster_stride_bytes(raster);
      const unsigned char * pixels =
          static_cast<const unsigned char *>(gimg_raster_pixels_const(raster));

      const std::string out =
          outdir + "/" + stem + "." + std::to_string(ei) + ".rgba";
      std::FILE * o = std::fopen(out.c_str(), "wb");
      if (!o) {
        std::printf("%s\t%d\tcannot-write-dump\n", stem.c_str(), (int)ei);
      }
      else {
        for (uint32_t y = 0; y < height; y++) {
          std::fwrite(pixels + (static_cast<size_t>(y) * stride), 1,
              static_cast<size_t>(width) * 4u, o);
        }
        std::fclose(o);
        std::printf("%s\t%d\tok\t%u\t%u\n", stem.c_str(), (int)ei, width,
            height);
      }
      gimg_raster_destroy(raster);
    }
    gimg_doc_destroy(doc);
  }
  return 0;
}
