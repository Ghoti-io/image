/**
 * @file
 *
 * Decode BMP files with this library and write each one's RGBA8 raster to
 * <outdir>/<name>.rgba, printing one status line per file.
 *
 * It exists for tests/data/bmp/bmpsuite_sweep.py, which runs the published
 * BMP conformance suite through this decoder and compares the result against
 * decoders that are not ours.  The status line is the sweep's record of what
 * this codec did with a file it refused, which is as much a part of
 * conformance as what it did with one it accepted.
 *
 * Build with: make bmp-dump-raster
 * Run: build/.../dump_bmp_raster <outdir> <file.bmp>...
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

/**
 * Caps large enough for the conformance suite and small enough that a header
 * claiming a gigantic image is refused rather than allocated.  bmpsuite's
 * b/reallybig.bmp is exactly that file.
 */
const GIMG_Limits * sweep_limits() {
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

} // namespace

int main(int argc, char ** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dump_bmp_raster <outdir> <file.bmp>...\n");
    return 2;
  }
  const std::string outdir = argv[1];

  for (int i = 2; i < argc; i++) {
    const char * path = argv[i];
    const char * slash = std::strrchr(path, '/');
    const std::string name = slash ? slash + 1 : path;

    std::vector<unsigned char> bytes;
    if (!read_file(path, bytes)) {
      std::printf("%s\tunreadable\n", name.c_str());
      continue;
    }

    GIMG_Load_Options load_options;
    std::memset(&load_options, 0, sizeof(load_options));
    load_options.limits = sweep_limits();
    GIMG_Decode_Options decode_options;
    std::memset(&decode_options, 0, sizeof(decode_options));
    decode_options.limits = sweep_limits();

    GIMG_Stream * stream = nullptr;
    if (gimg_stream_create_memory(bytes.data(), bytes.size(), &stream) !=
        GIMG_OK) {
      std::printf("%s\tstream-failed\n", name.c_str());
      continue;
    }

    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(stream, &load_options, nullptr, &doc);
    gimg_stream_destroy(stream);
    if (r != GIMG_OK || !doc) {
      std::printf("%s\tload:%s\n", name.c_str(), gimg_result_string(r));
      if (doc) {
        gimg_doc_destroy(doc);
      }
      continue;
    }

    GIMG_Raster * raster = nullptr;
    r = gimg_item_decode(gimg_doc_item(doc, 0), &decode_options, &raster);
    if (r != GIMG_OK || !raster) {
      std::printf("%s\tdecode:%s\n", name.c_str(), gimg_result_string(r));
      if (raster) {
        gimg_raster_destroy(raster);
      }
      gimg_doc_destroy(doc);
      continue;
    }

    const uint32_t width = gimg_raster_width(raster);
    const uint32_t height = gimg_raster_height(raster);
    const size_t stride = gimg_raster_stride_bytes(raster);
    const unsigned char * pixels =
        static_cast<const unsigned char *>(gimg_raster_pixels_const(raster));

    // Written without the raster's row padding, so the sweep can compare it
    // against an oracle's tightly packed rows without knowing our stride.
    const std::string out = outdir + "/" + name + ".rgba";
    std::FILE * o = std::fopen(out.c_str(), "wb");
    if (!o) {
      std::printf("%s\tcannot-write-dump\n", name.c_str());
    }
    else {
      for (uint32_t y = 0; y < height; y++) {
        std::fwrite(pixels + (static_cast<size_t>(y) * stride), 1,
            static_cast<size_t>(width) * 4u, o);
      }
      std::fclose(o);
      std::printf("%s\tok\t%u\t%u\n", name.c_str(), width, height);
    }

    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  }
  return 0;
}
