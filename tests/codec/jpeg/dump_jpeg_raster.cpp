/**
 * One-off: decode a JPEG file and write raw raster to stdout or to a file.
 * Used by compare_progressive_pixels.py and compare_pillow_ours to compare our
 * decoder vs libjpeg/Pillow.
 *
 * Usage: dump_jpeg_raster [ -o out.raw ] <path-to.jpeg>
 * Output: 4 bytes width (LE), 4 bytes height (LE), then width*height*4 bytes (RGBA).
 * GRAY8 is expanded to R=G=B, A=255. With -o, write to file instead of stdout.
 */
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<unsigned char> read_file(const char * path) {
  FILE * f = fopen(path, "rb");
  if (!f) return {};
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return {}; }
  long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return {}; }
  std::vector<unsigned char> buf(static_cast<size_t>(len));
  if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return {}; }
  fclose(f);
  return buf;
}

int main(int argc, char ** argv) {
  const char * out_path = nullptr;
  const char * jpeg_path = nullptr;
  bool skip_next = false;
  for (int i = 1; i < argc; i++) {
    if (skip_next) {
      skip_next = false;
      continue;
    }
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      out_path = argv[i + 1];
      skip_next = true;
    } else {
      jpeg_path = argv[i];
    }
  }
  if (!jpeg_path) {
    fprintf(stderr, "Usage: %s [ -o out.raw ] <path-to.jpeg>\n", argv[0]);
    return 1;
  }
  std::vector<unsigned char> jpeg = read_file(jpeg_path);
  if (jpeg.empty()) {
    fprintf(stderr, "Failed to read file: %s\n", jpeg_path);
    return 1;
  }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s) != 0 || !s) {
    fprintf(stderr, "Failed to create stream\n");
    return 1;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) != 0 || !doc) {
    gimg_stream_destroy(s);
    fprintf(stderr, "Failed to load document\n");
    return 1;
  }
  gimg_stream_destroy(s);
  if (gimg_doc_item_count(doc) == 0) {
    gimg_doc_destroy(doc);
    fprintf(stderr, "No item\n");
    return 1;
  }
  GIMG_Decode_Options opts = {};
  if (getenv("GIMG_JPEG_FANCY_UPSAMPLE") && getenv("GIMG_JPEG_FANCY_UPSAMPLE")[0] == '1') {
    opts.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_FANCY;
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), &opts, &raster) != 0 || !raster) {
    gimg_doc_destroy(doc);
    fprintf(stderr, "Failed to decode\n");
    return 1;
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = fmt ? gimg_raster_bytes_per_pixel(fmt) : 0;
  if (bpp == 0 || (bpp != 1 && bpp != 4)) {
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    fprintf(stderr, "Unsupported format (need GRAY8 or RGBA8)\n");
    return 1;
  }
  size_t stride = gimg_raster_stride_bytes(raster);
  const void * pixels = gimg_raster_pixels_const(raster);
  size_t row_bytes = w * bpp;
  if (!pixels || stride < row_bytes) {
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    fprintf(stderr, "Invalid raster\n");
    return 1;
  }
  FILE * out = stdout;
  FILE * out_file = nullptr;
  if (out_path) {
    out_file = fopen(out_path, "wb");
    if (!out_file) {
      fprintf(stderr, "Failed to open output: %s\n", out_path);
      gimg_raster_destroy(raster);
      gimg_doc_destroy(doc);
      return 1;
    }
    out = out_file;
  }

  uint32_t dims[2] = { w, h };
  if (fwrite(dims, 4, 2, out) != 2) {
    if (out_file) fclose(out_file);
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    return 1;
  }
  /* Output RGBA (4 bytes per pixel). GRAY8 expanded to R=G=B, A=255 (T.81 level shift + output). */
  if (bpp == 1) {
    const unsigned char * row = static_cast<const unsigned char *>(pixels);
    for (uint32_t y = 0; y < h; y++) {
      for (uint32_t x = 0; x < w; x++) {
        unsigned char v = row[y * stride + x];
        unsigned char rgba[4] = { v, v, v, 255 };
        if (fwrite(rgba, 1, 4, out) != 4) {
          if (out_file) fclose(out_file);
          gimg_raster_destroy(raster);
          gimg_doc_destroy(doc);
          return 1;
        }
      }
    }
  } else {
    for (uint32_t y = 0; y < h; y++) {
      if (fwrite(static_cast<const char *>(pixels) + y * stride, 1, row_bytes, out) != row_bytes) {
        if (out_file) fclose(out_file);
        gimg_raster_destroy(raster);
        gimg_doc_destroy(doc);
        return 1;
      }
    }
  }
  if (out_file)
    fclose(out_file);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  return 0;
}
