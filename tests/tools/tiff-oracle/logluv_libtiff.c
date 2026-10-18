/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * SGI LogLuv, written and read by libtiff.
 *
 *   logluv_libtiff --write DIR          write the variants into DIR
 *   logluv_libtiff --dump OUTDIR F...   decode each F as float
 *
 * libtiff's SGILog codec is Larson's own, which makes it the reference for
 * these encodings. The sample set has three LogLuv files, all big-endian, all
 * stripped, none tiled, none negative; --write covers what they do not:
 * every form in both byte orders, strips and tiles over a picture that is
 * not a whole number of either, luminance from 1e-3 to past each form's
 * ceiling, zero, negative LogL, runs (so the run-length coder emits runs) and
 * noise (so it emits literals). Each is read back before it is kept, as the
 * fax variants are.
 *
 * --dump asks for SGILOGDATAFMT_FLOAT, which is libtiff's own decode: Y for
 * LogL, CIE XYZ for LogLuv. Each file becomes OUTDIR/<name>.f32: "LUVF",
 * then width, height and channel count as little-endian u32, then the floats
 * little-endian, top row first. One line per file on stdout:
 * "<name>\tstatus=ok\tchannels=N" or "<name>\tstatus=refused".
 *
 * Not part of the library. Built inside the pinned oracle image by
 * `make oracle-tools`; what it writes is gitignored, the pin being the image.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#define W 37u
#define H 21u

static unsigned lcg = 20261017u;
static double next(void) {
  lcg = lcg * 1103515245u + 12345u;
  return (double)((lcg >> 8) & 0xffffffu) / 16777216.0;
}

/* One pixel's XYZ: bands of the picture exercise different things. */
static void pixel(unsigned x, unsigned y, int signed_ok, float * xyz) {
  double Y;
  if (y < 4u) {
    Y = pow(10.0, -3.0 + 6.0 * (double)x / (W - 1u)); /* 1e-3 .. 1e3 */
  }
  else if (y < 8u) {
    Y = (x / 8u) % 2u ? 0.5 : 20.0; /* constant spans: runs */
  }
  else if (y < 12u) {
    Y = pow(2.0, next() * 40.0 - 20.0); /* noise: literals */
  }
  else if (y < 14u) {
    Y = (x % 3u == 0u) ? 0.0 : (x % 3u == 1u) ? 1e30 : 1e-30;
  }
  else {
    Y = 0.01 + 100.0 * next();
  }
  if (signed_ok && y >= 16u && (x % 2u)) {
    Y = -Y;
  }
  /* Chromaticity over the gamut and a little past the locus. */
  const double u = 0.05 + 0.55 * next();
  const double v = 0.05 + 0.55 * next();
  const double d = 6.0 * u - 16.0 * v + 12.0;
  const double cx = 9.0 * u / d, cy = 4.0 * v / d;
  xyz[0] = (float)(cx / cy * Y);
  xyz[1] = (float)Y;
  xyz[2] = (float)((1.0 - cx - cy) / cy * Y);
}

static int write_one(const char * dir, const char * kind, int big, int tiled) {
  const int logl = strcmp(kind, "l16") == 0;
  const int narrow = strcmp(kind, "luv24") == 0;
  const unsigned spp = logl ? 1u : 3u;
  char path[4096];
  snprintf(path, sizeof path, "%s/logluv_%s_%s_%s.tif", dir, kind,
      big ? "be" : "le", tiled ? "tiles" : "strips");
  TIFF * t = TIFFOpen(path, big ? "wb" : "wl");
  if (!t) {
    return 1;
  }
  TIFFSetField(t, TIFFTAG_IMAGEWIDTH, W);
  TIFFSetField(t, TIFFTAG_IMAGELENGTH, H);
  TIFFSetField(t, TIFFTAG_PHOTOMETRIC,
      logl ? PHOTOMETRIC_LOGL : PHOTOMETRIC_LOGLUV);
  TIFFSetField(t, TIFFTAG_COMPRESSION,
      narrow ? COMPRESSION_SGILOG24 : COMPRESSION_SGILOG);
  TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, spp);
  TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 32);
  TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
  TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(t, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT);
  TIFFSetField(t, TIFFTAG_SGILOGENCODE, SGILOGENCODE_NODITHER);
  if (!big) {
    TIFFSetField(t, TIFFTAG_STONITS, 179.0);
  }
  float * img = calloc((size_t)W * H * spp, sizeof(float));
  lcg = 20261017u;
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      float xyz[3];
      pixel(x, y, logl, xyz);
      float * p = img + ((size_t)y * W + x) * spp;
      if (logl) {
        p[0] = xyz[1];
      }
      else {
        memcpy(p, xyz, sizeof xyz);
      }
    }
  }
  int bad = 0;
  if (tiled) {
    const unsigned tw = 16u, th = 16u;
    TIFFSetField(t, TIFFTAG_TILEWIDTH, tw);
    TIFFSetField(t, TIFFTAG_TILELENGTH, th);
    float * tile = calloc((size_t)tw * th * spp, sizeof(float));
    for (unsigned ty = 0; ty < H && !bad; ty += th) {
      for (unsigned tx = 0; tx < W && !bad; tx += tw) {
        memset(tile, 0, (size_t)tw * th * spp * sizeof(float));
        for (unsigned y = 0; y < th && ty + y < H; y++) {
          for (unsigned x = 0; x < tw && tx + x < W; x++) {
            memcpy(tile + ((size_t)y * tw + x) * spp,
                img + ((size_t)(ty + y) * W + tx + x) * spp,
                spp * sizeof(float));
          }
        }
        bad |= TIFFWriteTile(t, tile, tx, ty, 0, 0) < 0;
      }
    }
    free(tile);
  }
  else {
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, 4u);
    for (unsigned y = 0; y < H && !bad; y++) {
      bad |= TIFFWriteScanline(t, img + (size_t)y * W * spp, y, 0) < 0;
    }
  }
  free(img);
  TIFFClose(t);
  if (bad) {
    fprintf(stderr, "libtiff could not write %s\n", path);
    return 1;
  }
  /* Read back before keeping it. */
  TIFF * back = TIFFOpen(path, "r");
  if (!back) {
    fprintf(stderr, "libtiff could not reopen %s\n", path);
    return 1;
  }
  TIFFClose(back);
  return 0;
}

static void le32(FILE * f, uint32_t v) {
  const unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8),
      (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
  fwrite(b, 1, 4, f);
}

static int dump_one(const char * outdir, const char * path) {
  const char * name = strrchr(path, '/');
  name = name ? name + 1 : path;
  TIFF * t = TIFFOpen(path, "r");
  uint32_t w = 0, h = 0;
  uint16_t photo = 0;
  if (!t || !TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w) ||
      !TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h) ||
      !TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photo) ||
      (photo != PHOTOMETRIC_LOGL && photo != PHOTOMETRIC_LOGLUV) ||
      !TIFFSetField(t, TIFFTAG_SGILOGDATAFMT, SGILOGDATAFMT_FLOAT)) {
    if (t) {
      TIFFClose(t);
    }
    printf("%s\tstatus=refused\n", name);
    return 0;
  }
  const unsigned spp = photo == PHOTOMETRIC_LOGL ? 1u : 3u;
  float * img = calloc((size_t)w * h * spp, sizeof(float));
  int ok = img != NULL;
  if (ok && TIFFIsTiled(t)) {
    uint32_t tw = 0, th = 0;
    TIFFGetField(t, TIFFTAG_TILEWIDTH, &tw);
    TIFFGetField(t, TIFFTAG_TILELENGTH, &th);
    float * tile = calloc((size_t)tw * th * spp, sizeof(float));
    for (uint32_t ty = 0; ok && ty < h; ty += th) {
      for (uint32_t tx = 0; ok && tx < w; tx += tw) {
        ok = TIFFReadTile(t, tile, tx, ty, 0, 0) >= 0;
        for (uint32_t y = 0; ok && y < th && ty + y < h; y++) {
          for (uint32_t x = 0; x < tw && tx + x < w; x++) {
            memcpy(img + ((size_t)(ty + y) * w + tx + x) * spp,
                tile + ((size_t)y * tw + x) * spp, spp * sizeof(float));
          }
        }
      }
    }
    free(tile);
  }
  else {
    for (uint32_t y = 0; ok && y < h; y++) {
      ok = TIFFReadScanline(t, img + (size_t)y * w * spp, y, 0) >= 0;
    }
  }
  TIFFClose(t);
  if (!ok) {
    free(img);
    printf("%s\tstatus=refused\n", name);
    return 0;
  }
  char out[4096];
  snprintf(out, sizeof out, "%s/%s.f32", outdir, name);
  FILE * f = fopen(out, "wb");
  if (!f) {
    free(img);
    return 1;
  }
  fwrite("LUVF", 1, 4, f);
  le32(f, w);
  le32(f, h);
  le32(f, spp);
  for (size_t i = 0; i < (size_t)w * h * spp; i++) {
    uint32_t bits;
    memcpy(&bits, &img[i], 4);
    le32(f, bits);
  }
  fclose(f);
  free(img);
  printf("%s\tstatus=ok\tchannels=%u\n", name, spp);
  return 0;
}

int main(int argc, char ** argv) {
  if (argc == 3 && strcmp(argv[1], "--write") == 0) {
    static const char * kinds[] = {"l16", "luv32", "luv24"};
    int bad = 0;
    for (int k = 0; k < 3; k++) {
      for (int big = 0; big < 2; big++) {
        for (int tiled = 0; tiled < 2; tiled++) {
          bad |= write_one(argv[2], kinds[k], big, tiled);
        }
      }
    }
    return bad;
  }
  if (argc >= 3 && strcmp(argv[1], "--dump") == 0) {
    int bad = 0;
    for (int i = 3; i < argc; i++) {
      bad |= dump_one(argv[2], argv[i]);
    }
    return bad;
  }
  fprintf(stderr,
      "usage: logluv_libtiff --write DIR | --dump OUTDIR FILE...\n");
  return 2;
}
