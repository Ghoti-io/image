/*
 * Reference pixel dumper: decode a GIF with giflib (stock, unmodified) and
 * write what it read, so that this library's GIF codec can be checked against
 * a decoder that is not ours.
 *
 * What giflib is an oracle for, and what it is not:
 *
 *   It is authoritative for LZW expansion, the four-pass interlace order, the
 *   colour tables, the per-image geometry and the graphic control blocks -
 *   everything up to and including "what indices does this image hold, and
 *   what colours do they mean".
 *
 *   It is NOT an oracle for compositing.  giflib hands back each image as it
 *   was stored and leaves disposal, transparency-over-canvas and the logical
 *   screen to the caller; there is no giflib answer to "what does frame 3 look
 *   like".  The --composite mode below implements that from the specification,
 *   which makes it a second reading of the same text rather than independent
 *   evidence, and it says so.  Pillow is the independent compositor, and
 *   tests/data/gif/ uses it for exactly that.
 *
 * DGifSlurp() de-interlaces for you: RasterBits is always in top-to-bottom
 * order, whatever the image descriptor's interlace flag says.  This tool
 * therefore does not touch the row order, and the flag is reported by --info
 * only so that a caller can see it.  The trap is real - gif2rgb, shipped in
 * the same package, runs the four-pass table itself, because it reads with
 * DGifGetLine() rather than slurping.  De-interlacing a slurped raster a
 * second time scrambles exactly the interlaced files and leaves every
 * progressive one correct, which reads like a codec defect and is not one.
 *
 * Build with: make oracle-build oracle-tools
 *   cc -o dump_gif_pixels_giflib dump_gif_pixels_giflib.c -lgif
 *
 * Usage: dump_gif_pixels_giflib [--frame N] [--composite] [--info] <file.gif>
 * Output: "GIFO" + u32 width LE + u32 height LE + RGBA8, on stdout unless -o.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gif_lib.h>

static void put_u32(FILE * out, uint32_t v) {
  unsigned char b[4];
  b[0] = (unsigned char)(v & 0xff);
  b[1] = (unsigned char)((v >> 8) & 0xff);
  b[2] = (unsigned char)((v >> 16) & 0xff);
  b[3] = (unsigned char)((v >> 24) & 0xff);
  fwrite(b, 1, 4, out);
}

static const ColorMapObject * map_for(
    const GifFileType * gif, const SavedImage * img) {
  return img->ImageDesc.ColorMap ? img->ImageDesc.ColorMap : gif->SColorMap;
}

int main(int argc, char ** argv) {
  const char * path = NULL;
  const char * out_path = NULL;
  int want_frame = 0;
  int composite = 0;
  int info = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--frame") && i + 1 < argc) {
      want_frame = atoi(argv[++i]);
    }
    else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
      out_path = argv[++i];
    }
    else if (!strcmp(argv[i], "--composite")) {
      composite = 1;
    }
    else if (!strcmp(argv[i], "--info")) {
      info = 1;
    }
    else {
      path = argv[i];
    }
  }
  if (!path) {
    fprintf(stderr,
        "Usage: %s [--frame N] [--composite] [--info] [-o out] <file.gif>\n",
        argv[0]);
    return 2;
  }

  int err = 0;
  GifFileType * gif = DGifOpenFileName(path, &err);
  if (!gif) {
    fprintf(stderr, "%s: %s\n", path, GifErrorString(err));
    return 1;
  }
  if (DGifSlurp(gif) != GIF_OK) {
    fprintf(stderr, "%s: %s\n", path, GifErrorString(gif->Error));
    DGifCloseFile(gif, &err);
    return 1;
  }

  if (info) {
    printf("canvas %d %d images %d background %d\n", gif->SWidth, gif->SHeight,
        gif->ImageCount, gif->SBackGroundColor);
    for (int i = 0; i < gif->ImageCount; i++) {
      const SavedImage * s = &gif->SavedImages[i];
      GraphicsControlBlock gcb;
      int have = DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_OK;
      const ColorMapObject * cm = map_for(gif, s);
      printf("image %d at %d,%d size %dx%d interlace %d colors %d"
             " local %d delay %d disposal %d transparent %d\n",
          i, s->ImageDesc.Left, s->ImageDesc.Top, s->ImageDesc.Width,
          s->ImageDesc.Height, s->ImageDesc.Interlace,
          cm ? cm->ColorCount : 0, s->ImageDesc.ColorMap ? 1 : 0,
          have ? gcb.DelayTime : -1, have ? gcb.DisposalMode : -1,
          have ? gcb.TransparentColor : -1);
    }
    DGifCloseFile(gif, &err);
    return 0;
  }

  if (want_frame < 0 || want_frame >= gif->ImageCount) {
    fprintf(stderr, "%s: no image %d (file holds %d)\n", path, want_frame,
        gif->ImageCount);
    DGifCloseFile(gif, &err);
    return 1;
  }

  FILE * out = stdout;
  if (out_path) {
    out = fopen(out_path, "wb");
    if (!out) {
      perror(out_path);
      DGifCloseFile(gif, &err);
      return 1;
    }
  }

  int W = composite ? gif->SWidth : gif->SavedImages[want_frame].ImageDesc.Width;
  int H =
      composite ? gif->SHeight : gif->SavedImages[want_frame].ImageDesc.Height;
  unsigned char * canvas = (unsigned char *)calloc((size_t)W * (size_t)H, 4);
  unsigned char * saved = NULL;
  if (!canvas) {
    fprintf(stderr, "out of memory\n");
    DGifCloseFile(gif, &err);
    return 1;
  }

  int first = composite ? 0 : want_frame;
  for (int i = first; i <= want_frame; i++) {
    const SavedImage * s = &gif->SavedImages[i];
    const ColorMapObject * cm = map_for(gif, s);
    GraphicsControlBlock gcb;
    int have = DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_OK;
    int transparent = have ? gcb.TransparentColor : NO_TRANSPARENT_COLOR;
    int disposal = have ? gcb.DisposalMode : DISPOSAL_UNSPECIFIED;

    int w = s->ImageDesc.Width, h = s->ImageDesc.Height;
    const unsigned char * idx = s->RasterBits;

    if (composite && i < want_frame && disposal == DISPOSE_PREVIOUS) {
      if (!saved) {
        saved = (unsigned char *)malloc((size_t)W * (size_t)H * 4);
      }
      if (saved) {
        memcpy(saved, canvas, (size_t)W * (size_t)H * 4);
      }
    }

    int ox = composite ? s->ImageDesc.Left : 0;
    int oy = composite ? s->ImageDesc.Top : 0;
    for (int y = 0; y < h; y++) {
      int cy = oy + y;
      if (cy < 0 || cy >= H) {
        continue;
      }
      for (int x = 0; x < w; x++) {
        int cx = ox + x;
        if (cx < 0 || cx >= W) {
          continue;
        }
        int v = idx[(size_t)y * (size_t)w + (size_t)x];
        if (transparent != NO_TRANSPARENT_COLOR && v == transparent) {
          continue;
        }
        if (!cm || v >= cm->ColorCount) {
          continue;
        }
        unsigned char * px = canvas + (((size_t)cy * (size_t)W) + (size_t)cx) * 4;
        px[0] = (unsigned char)cm->Colors[v].Red;
        px[1] = (unsigned char)cm->Colors[v].Green;
        px[2] = (unsigned char)cm->Colors[v].Blue;
        px[3] = 255;
      }
    }

    if (composite && i < want_frame) {
      if (disposal == DISPOSE_BACKGROUND) {
        for (int y = 0; y < h; y++) {
          int cy = s->ImageDesc.Top + y;
          if (cy < 0 || cy >= H) {
            continue;
          }
          for (int x = 0; x < w; x++) {
            int cx = s->ImageDesc.Left + x;
            if (cx < 0 || cx >= W) {
              continue;
            }
            memset(canvas + (((size_t)cy * (size_t)W) + (size_t)cx) * 4, 0, 4);
          }
        }
      }
      else if (disposal == DISPOSE_PREVIOUS && saved) {
        memcpy(canvas, saved, (size_t)W * (size_t)H * 4);
      }
    }
  }

  fwrite("GIFO", 1, 4, out);
  put_u32(out, (uint32_t)W);
  put_u32(out, (uint32_t)H);
  fwrite(canvas, 1, (size_t)W * (size_t)H * 4, out);
  if (out != stdout) {
    fclose(out);
  }
  free(saved);
  free(canvas);
  DGifCloseFile(gif, &err);
  return 0;
}
