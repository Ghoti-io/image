/*
 * Reference pixel dumper: decode a JPEG with libjpeg-turbo (stock,
 * unmodified; jpeg_read_scanlines) and print FNV-1a hash over raw
 * pixel bytes in the same layout as our decoder: grayscale 1 byte/pixel,
 * color RGB expanded to RGBA (A=255), CMYK 4 bytes/pixel.
 *
 * Used as the decode oracle for Decode*PillowOracle tests (oracle is
 * libjpeg, not Pillow). Build with:
 *   cc -o dump_jpeg_pixels_ref dump_jpeg_pixels_ref.c $(pkg-config --cflags --libs libjpeg)
 *
 * Usage: dump_jpeg_pixels_ref [ -o out.raw ] <file.jpg>
 * Output: HASH <hex16> WIDTH <w> HEIGHT <h> MODE <L|RGBA|CMYK>
 * With -o out.raw, also writes oracle .raw (mode byte 0=L, 1=RGB, 2=CMYK;
 * 4 bytes width LE, 4 height LE, then raw pixels).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

#define RAW_MODE_L    0
#define RAW_MODE_RGB  1
#define RAW_MODE_CMYK 2

static void fnv1a_update(uint64_t *h, const unsigned char *p, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    *h ^= (uint64_t)p[i];
    *h *= FNV_PRIME;
  }
}

static int write_raw_header(FILE *out, unsigned int w, unsigned int h,
                            int num_components)
{
  unsigned char mode;
  if (num_components == 1)
    mode = RAW_MODE_L;
  else if (num_components == 3)
    mode = RAW_MODE_RGB;
  else if (num_components == 4)
    mode = RAW_MODE_CMYK;
  else
    return -1;
  unsigned char hdr[9];
  hdr[0] = mode;
  hdr[1] = (unsigned char)(w & 0xff);
  hdr[2] = (unsigned char)((w >> 8) & 0xff);
  hdr[3] = (unsigned char)((w >> 16) & 0xff);
  hdr[4] = (unsigned char)((w >> 24) & 0xff);
  hdr[5] = (unsigned char)(h & 0xff);
  hdr[6] = (unsigned char)((h >> 8) & 0xff);
  hdr[7] = (unsigned char)((h >> 16) & 0xff);
  hdr[8] = (unsigned char)((h >> 24) & 0xff);
  return fwrite(hdr, 1, 9, out) == 9 ? 0 : -1;
}

int main(int argc, char **argv)
{
  const char *path = NULL;
  const char *raw_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
      raw_path = argv[i + 1];
      i++;
    } else
      path = argv[i];
  }
  if (path == NULL) {
    fprintf(stderr, "Usage: %s [ -o out.raw ] <file.jpg>\n", argv[0]);
    return 1;
  }

  FILE *fp = fopen(path, "rb");
  if (!fp) {
    perror(path);
    return 1;
  }

  FILE *raw_fp = NULL;
  if (raw_path) {
    raw_fp = fopen(raw_path, "wb");
    if (!raw_fp) {
      perror(raw_path);
      fclose(fp);
      return 1;
    }
  }

  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);

  unsigned int w = cinfo.output_width;
  unsigned int h = cinfo.output_height;
  int num_components = cinfo.output_components;  // 1 gray, 3 RGB, 4 CMYK
  /* This tool names three output shapes.  Anything else is refused rather
     than reported as one of them: the mode used to fall through to "L" for a
     two-component frame while the pixel loop below hashed it two bytes per
     pixel, so the line said one thing and the hash meant another. */
  const char *mode;
  if (num_components == 1)
    mode = "L";
  else if (num_components == 3)
    mode = "RGBA";
  else if (num_components == 4)
    mode = "CMYK";
  else {
    fprintf(stderr, "%s: %d-component output has no mode this tool names\n",
            path, num_components);
    if (raw_fp) fclose(raw_fp);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  unsigned long row_bytes = (unsigned long)w * (unsigned long)num_components;
  if (row_bytes > 1024 * 1024 || h > 65535) {
    fprintf(stderr, "%s: image too large\n", path);
    if (raw_fp) fclose(raw_fp);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  if (raw_fp && write_raw_header(raw_fp, w, h, num_components) != 0) {
    fprintf(stderr, "%s: write raw header failed\n", raw_path);
    fclose(raw_fp);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  unsigned char *row = (unsigned char *)malloc(row_bytes);
  if (!row) {
    if (raw_fp) fclose(raw_fp);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return 1;
  }

  uint64_t fnv = FNV_OFFSET;

  if (num_components == 1) {
    while (cinfo.output_scanline < cinfo.output_height) {
      (void)jpeg_read_scanlines(&cinfo, &row, 1);
      fnv1a_update(&fnv, row, (size_t)(w * 1));
      if (raw_fp && fwrite(row, 1, (size_t)(w * 1), raw_fp) != (size_t)(w * 1)) {
        fprintf(stderr, "%s: write raw failed\n", raw_path);
        fclose(raw_fp);
        free(row);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return 1;
      }
    }
  } else if (num_components == 3) {
    size_t rgba_stride = (size_t)w * 4u;
    unsigned char *rgba = (unsigned char *)malloc(rgba_stride);
    if (!rgba) {
      free(row);
      if (raw_fp) fclose(raw_fp);
      jpeg_destroy_decompress(&cinfo);
      fclose(fp);
      return 1;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
      (void)jpeg_read_scanlines(&cinfo, &row, 1);
      for (unsigned int x = 0; x < w; x++) {
        rgba[x * 4 + 0] = row[x * 3 + 0];
        rgba[x * 4 + 1] = row[x * 3 + 1];
        rgba[x * 4 + 2] = row[x * 3 + 2];
        rgba[x * 4 + 3] = 255;
      }
      fnv1a_update(&fnv, rgba, rgba_stride);
      if (raw_fp && fwrite(row, 1, (size_t)w * 3, raw_fp) != (size_t)w * 3) {
        fprintf(stderr, "%s: write raw failed\n", raw_path);
        fclose(raw_fp);
        free(rgba);
        free(row);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return 1;
      }
    }
    free(rgba);
  } else {
    /* CMYK: 4 bytes per pixel.  Only reachable for four components now that
       anything this tool cannot name is refused above. */
    while (cinfo.output_scanline < cinfo.output_height) {
      (void)jpeg_read_scanlines(&cinfo, &row, 1);
      fnv1a_update(&fnv, row, (size_t)row_bytes);
      if (raw_fp && fwrite(row, 1, (size_t)row_bytes, raw_fp) != (size_t)row_bytes) {
        fprintf(stderr, "%s: write raw failed\n", raw_path);
        fclose(raw_fp);
        free(row);
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return 1;
      }
    }
  }

  free(row);
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  fclose(fp);
  if (raw_fp)
    fclose(raw_fp);

  /* Only print hash line when not writing .raw (caller parsing stdout). */
  if (raw_path == NULL)
    printf("HASH %016llx WIDTH %u HEIGHT %u MODE %s\n",
           (unsigned long long)fnv, w, h, mode);
  return 0;
}
