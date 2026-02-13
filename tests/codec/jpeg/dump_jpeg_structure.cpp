/**
 * Dump JPEG file structure: each segment in file order with offset, size, and
 * hex dump. Gives a consistent reference for where (and how big) each block
 * is for decoding and debugging.
 *
 * Usage: dump_jpeg_structure <path-to.jpeg>
 * Output: to stdout, one block per segment (SOI, APP0, DQT, SOF0, DHT, SOS,
 *         scan data, EOI, etc.) with [offset] NAME (size) and hex dump.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<unsigned char> read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return {};
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return {};
  }
  long len = ftell(f);
  if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return {};
  }
  std::vector<unsigned char> buf(static_cast<size_t>(len));
  if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
    fclose(f);
    return {};
  }
  fclose(f);
  return buf;
}

static const char* marker_name(uint8_t m) {
  switch (m) {
    case 0xD8: return "SOI";
    case 0xD9: return "EOI";
    case 0xC0: return "SOF0";
    case 0xC1: return "SOF1";
    case 0xC2: return "SOF2";
    case 0xC4: return "DHT";
    case 0xDB: return "DQT";
    case 0xDA: return "SOS";
    case 0xDD: return "DRI";
    case 0xDC: return "DNL";
    case 0xFE: return "COM";
    case 0xE0: return "APP0";
    case 0xE1: return "APP1";
    case 0xE2: return "APP2";
    case 0xE3: return "APP3";
    case 0xE4: return "APP4";
    case 0xE5: return "APP5";
    case 0xE6: return "APP6";
    case 0xE7: return "APP7";
    case 0xE8: return "APP8";
    case 0xE9: return "APP9";
    case 0xEA: return "APP10";
    case 0xEB: return "APP11";
    case 0xEC: return "APP12";
    case 0xED: return "APP13";
    case 0xEE: return "APP14";
    case 0xEF: return "APP15";
    default:
      if (m >= 0xD0 && m <= 0xD7) return "RST";  // RST0..RST7
      return "???";
  }
}

static bool marker_has_no_length(uint8_t m) {
  return m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7);
}

static void hex_dump(FILE* out, const unsigned char* data, size_t n) {
  for (size_t i = 0; i < n; i += 16) {
    size_t chunk = (i + 16 <= n) ? 16 : (n - i);
    for (size_t j = 0; j < chunk; j++)
      (void)fprintf(out, " %02x", data[i + j]);
    for (size_t j = chunk; j < 16; j++)
      (void)fprintf(out, "   ");
    (void)fprintf(out, "\n");
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <path-to.jpeg>\n", argv[0]);
    return 1;
  }
  std::vector<unsigned char> data = read_file(argv[1]);
  if (data.empty()) {
    fprintf(stderr, "Failed to read file: %s\n", argv[1]);
    return 1;
  }

  FILE* out = stdout;
  size_t i = 0;
  const size_t size = data.size();

  /* SOI */
  if (i + 2 > size || data[i] != 0xFF || data[i + 1] != 0xD8) {
    fprintf(stderr, "Not a JPEG (missing SOI)\n");
    return 1;
  }
  (void)fprintf(out, "[%zu] SOI (2 bytes)\n", i);
  hex_dump(out, &data[i], 2);
  i = 2;

  bool in_scan = false;
  while (i < size) {
    if (i + 1 >= size) {
      fprintf(stderr, "Truncated at offset %zu\n", i);
      return 1;
    }
    if (data[i] != 0xFF) {
      if (in_scan) {
        i++;
        continue;
      }
      /* Skip to next 0xFF and note gap (handles malformed / overlapping segments) */
      size_t gap_start = i;
      while (i < size && data[i] != 0xFF) i++;
      (void)fprintf(out, "[%zu] (skipped %zu bytes to next 0xFF)\n", gap_start,
                   i - gap_start);
      continue;
    }
    uint8_t m = data[i + 1];

    if (m == 0x00) {
      /* Byte stuffing (0xFF 0x00) in scan data */
      if (!in_scan) {
        fprintf(stderr, "Unexpected 0xFF 0x00 at offset %zu (outside scan)\n", i);
        return 1;
      }
      (void)fprintf(out, "  (stuffing 0xFF 0x00 at %zu)\n", i);
      i += 2;
      continue;
    }

    if (marker_has_no_length(m)) {
      if (m >= 0xD0 && m <= 0xD7) {
        if (in_scan) {
          (void)fprintf(out, "[%zu] RST%d (2 bytes)\n", i, m - 0xD0);
          hex_dump(out, &data[i], 2);
        }
        i += 2;
        continue;
      }
      if (m == 0xD9) {
        if (in_scan) in_scan = false;
        (void)fprintf(out, "[%zu] EOI (2 bytes)\n", i);
        hex_dump(out, &data[i], 2);
        i += 2;
        break;
      }
      if (m == 0xD8) {
        (void)fprintf(out, "[%zu] SOI (2 bytes) (nested?)\n", i);
        hex_dump(out, &data[i], 2);
        i += 2;
        continue;
      }
    }

    /* Segment with length field */
    if (i + 4 > size) {
      fprintf(stderr, "Truncated segment at offset %zu\n", i);
      return 1;
    }
    uint16_t length = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
    size_t seg_len = 2 + (size_t)length; /* 0xFF, marker, length 2 bytes, payload */
    if (length < 2) {
      fprintf(stderr, "Invalid segment length %u at offset %zu\n",
              (unsigned)length, i);
      return 1;
    }
    if (i + seg_len > size) {
      fprintf(stderr, "Segment at %zu extends past end (len=%u)\n", i,
              (unsigned)length);
      return 1;
    }

    const char* name = marker_name(m);
    if (m >= 0xD0 && m <= 0xD7)
      (void)fprintf(out, "[%zu] %s%d (2 bytes)\n", i, name, m - 0xD0);
    else
      (void)fprintf(out, "[%zu] %s (%zu bytes)\n", i, name, seg_len);
    hex_dump(out, &data[i], seg_len);
    i += seg_len;

    if (m == 0xDA) {
      /* SOS: following bytes are scan data until next 0xFF + non-0x00, non-RST */
      size_t scan_start = i;
      while (i < size) {
        if (data[i] != 0xFF) {
          i++;
          continue;
        }
        if (i + 1 >= size) break;
        uint8_t next = data[i + 1];
        if (next == 0x00) {
          i += 2;
          continue;
        }
        if (next >= 0xD0 && next <= 0xD7) {
          i += 2;
          continue;
        }
        break;
      }
      size_t scan_len = i - scan_start;
      in_scan = true;
      (void)fprintf(out, "[%zu] Scan data (%zu bytes)\n", scan_start, scan_len);
      hex_dump(out, &data[scan_start], scan_len);
      /* i is at 0xFF of next marker; loop will process it */
    }
  }

  if (i < size) {
    (void)fprintf(out, "[%zu] trailing (%zu bytes)\n", i, size - i);
    hex_dump(out, &data[i], size - i);
  }
  return 0;
}
