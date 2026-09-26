/*
 * Reference pixel dumper: decode a TIFF with libtiff (stock, unmodified) and
 * write what it read, so that this library's TIFF codec can be checked
 * against the reference implementation of the format rather than against
 * itself.
 *
 * What libtiff is an oracle for, and what it is not:
 *
 *   It is authoritative for the whole of TIFF 6.0 as it is actually written
 *   and read in the world: the IFD structure, every field type, strips and
 *   tiles, every compression method, every photometric interpretation. It is
 *   the implementation the format's other implementations were written
 *   against.
 *
 *   It is NOT a second reading of the *specification*, and the difference
 *   matters here. TIFFReadRGBAImage applies libtiff's own policies where the
 *   document is silent or where files in the wild disagree with it - the
 *   ColorMap case this library's format page records is exactly that. A
 *   disagreement is therefore a question to triage and not automatically a
 *   defect on either side.
 *
 * TIFFReadRGBAImage returns the raster **bottom-up**: its first pixel is the
 * lower-left one, which is an interface decision inherited from OpenGL and not
 * anything the file said. TIFFReadRGBAImageOriented with ORIENTATION_TOPLEFT
 * is used instead, so that a caller comparing rows is comparing pictures. The
 * trap is worth naming because the wrong one of those two produces a
 * vertically mirrored image that reads exactly like a row-order defect in the
 * codec under test.
 *
 * The samples come back as ABGR packed into a uint32 in host order, which is
 * why they are taken apart with TIFFGetR/G/B/A rather than memcpy'd: the
 * packing is little-endian-looking on a little-endian machine and is not a
 * byte layout the format defines.
 *
 * Build with: make oracle-build oracle-tools
 *   cc -o dump_tiff_pixels_libtiff dump_tiff_pixels_libtiff.c -ltiff
 *
 * Usage: dump_tiff_pixels_libtiff [--page N] [--info] [-o out] <file.tif>
 * Output: "TIFO" + u32 width LE + u32 height LE + RGBA8, on stdout unless -o.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

static void put_u32_le(FILE * out, uint32_t v) {
  unsigned char b[4] = {(unsigned char)(v & 0xFFu),
      (unsigned char)((v >> 8) & 0xFFu), (unsigned char)((v >> 16) & 0xFFu),
      (unsigned char)((v >> 24) & 0xFFu)};
  fwrite(b, 1, 4, out);
}

/** libtiff talks to stderr by default; route its chatter somewhere the
 * caller's stream parser will not read it as an answer. */
static void quiet(const char * module, const char * fmt, va_list ap) {
  (void)module;
  (void)fmt;
  (void)ap;
}

int main(int argc, char ** argv) {
  const char * path = NULL;
  const char * out_path = NULL;
  int want_page = 0;
  int info_only = 0;
  int verbose = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--page") && i + 1 < argc) {
      want_page = atoi(argv[++i]);
    }
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
      out_path = argv[++i];
    }
    else if (!strcmp(argv[i], "--info")) {
      info_only = 1;
    }
    else if (!strcmp(argv[i], "--verbose")) {
      verbose = 1;
    }
    else {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr,
        "usage: %s [--page N] [--info] [--verbose] [-o out] <file.tif>\n",
        argv[0]);
    return 2;
  }
  if (!verbose) {
    TIFFSetWarningHandler(quiet);
    TIFFSetErrorHandler(quiet);
  }

  TIFF * tif = TIFFOpen(path, "r");
  if (!tif) {
    fprintf(stderr, "%s: libtiff would not open it\n", path);
    return 1;
  }
  for (int i = 0; i < want_page; i++) {
    if (!TIFFReadDirectory(tif)) {
      fprintf(stderr, "%s: no page %d\n", path, want_page);
      TIFFClose(tif);
      return 1;
    }
  }

  uint32_t w = 0, h = 0;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
  if (info_only) {
    uint16_t bps = 0, spp = 0, photo = 0, comp = 0, planar = 0;
    uint32_t tw = 0, tl = 0, rps = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &comp);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(tif, TIFFTAG_TILELENGTH, &tl);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
    printf("width=%u height=%u bps=%u spp=%u photometric=%u compression=%u "
           "planar=%u tiled=%d tilewidth=%u tilelength=%u rowsperstrip=%u "
           "pages=%d\n",
        w, h, bps, spp, photo, comp, planar, TIFFIsTiled(tif) ? 1 : 0, tw, tl,
        rps, TIFFNumberOfDirectories(tif));
    TIFFClose(tif);
    return 0;
  }

  if (w == 0u || h == 0u || (uint64_t)w * h > (uint64_t)1 << 28) {
    fprintf(stderr, "%s: %ux%u is not a size this dumper will allocate\n",
        path, w, h);
    TIFFClose(tif);
    return 1;
  }
  uint32_t * raster = (uint32_t *)_TIFFmalloc((tmsize_t)w * h * 4);
  if (!raster) {
    fprintf(stderr, "%s: out of memory for %ux%u\n", path, w, h);
    TIFFClose(tif);
    return 1;
  }
  // Top-left origin: see the note at the top. The final 0 is "stop on the
  // first error" rather than "return what was read so far", because a partial
  // raster silently compared against a whole one is a disagreement nobody can
  // explain.
  if (!TIFFReadRGBAImageOriented(
          tif, w, h, raster, ORIENTATION_TOPLEFT, 0)) {
    fprintf(stderr, "%s: libtiff would not decode it\n", path);
    _TIFFfree(raster);
    TIFFClose(tif);
    return 1;
  }

  FILE * out = stdout;
  if (out_path) {
    out = fopen(out_path, "wb");
    if (!out) {
      fprintf(stderr, "%s: cannot write\n", out_path);
      _TIFFfree(raster);
      TIFFClose(tif);
      return 1;
    }
  }
  fwrite("TIFO", 1, 4, out);
  put_u32_le(out, w);
  put_u32_le(out, h);
  for (uint32_t i = 0; i < w * h; i++) {
    const uint32_t p = raster[i];
    const unsigned char rgba[4] = {(unsigned char)TIFFGetR(p),
        (unsigned char)TIFFGetG(p), (unsigned char)TIFFGetB(p),
        (unsigned char)TIFFGetA(p)};
    fwrite(rgba, 1, 4, out);
  }
  if (out != stdout) {
    fclose(out);
  }
  _TIFFfree(raster);
  TIFFClose(tif);
  return 0;
}
