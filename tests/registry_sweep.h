/**
 * @file
 *
 * The list of codecs a cross-codec sweep runs over, taken from the registry.
 *
 * A sweep that hardcodes {"png", "jpeg", "bmp", "gif"} keeps passing when a
 * fifth codec is added and silently stops being a sweep: it covers what it
 * was written against rather than what the library ships. Five of them did,
 * and each would have needed hand-editing for a new format - the kind of edit
 * that is easy to forget precisely because nothing fails when you do.
 *
 * So the population comes from `gimg_codec_count()` and `gimg_codec_by_index()`,
 * and a codec's fixtures come from a directory named after it. Registering a
 * codec and putting its fixtures in `tests/data/<name>/` is all a new format
 * has to do to be swept.
 *
 * Two things this deliberately does not do:
 *
 *   - It does not classify a file by asking the library what format it is.
 *     A sweep that used `gimg_probe()` to choose the inputs it then feeds to
 *     the loader would agree with itself by construction. Where a test needs
 *     to know what a file claims to be, it says so independently - see
 *     `signature_of()` in test_refusal_reasons.cpp.
 *   - It does not replace the inventory assertion in test_limits.cpp, which
 *     names the four codecs on purpose. That test's job is to state what
 *     ships; stating it by reading the registry would assert nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_REGISTRY_SWEEP_H
#define GHOTI_IO_GIMG_TESTS_REGISTRY_SWEEP_H

#include <dirent.h>
#include <ghoti.io/image/codec.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <sys/stat.h>
#endif

namespace gimg_test {

/** One registered codec, with where its fixtures live. */
struct SweptCodec {
  std::string name;
  unsigned int capabilities = 0u;
  std::string data_dir;

  bool animates() const { return (capabilities & GIMG_CAP_ANIMATION) != 0u; }
  bool writes() const { return (capabilities & GIMG_CAP_WRITE) != 0u; }
  bool reads() const { return (capabilities & GIMG_CAP_READ) != 0u; }
};

/**
 * Every registered codec, in name order so a run is reproducible - the
 * registry is in constructor order, which is link order, which is not.
 */
inline std::vector<SweptCodec> swept_codecs() {
  std::vector<SweptCodec> out;
  const size_t n = gimg_codec_count();
  for (size_t i = 0; i < n; i++) {
    GIMG_Codec * c = gimg_codec_by_index(i);
    const char * name = c ? gimg_codec_name(c) : nullptr;
    if (!name) {
      continue;
    }
    SweptCodec s;
    s.name = name;
    s.capabilities = gimg_codec_capabilities(c);
    s.data_dir = std::string(GIMG_TEST_DATA_ROOT) + "/" + s.name;
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(),
      [](const SweptCodec & a, const SweptCodec & b) {
        return a.name < b.name;
      });
  return out;
}

/** Just the names, for a sweep that only needs to pass one to a save. */
inline std::vector<std::string> swept_codec_names() {
  std::vector<std::string> out;
  for (const SweptCodec & c : swept_codecs()) {
    out.push_back(c.name);
  }
  return out;
}

/** Whether directory entry @p e of @p dir is itself a directory. MinGW's
 * struct dirent has no d_type, so Windows asks stat() instead. */
inline bool sweep_entry_is_directory(
    const std::string & dir, const struct dirent * e) {
#if defined(_WIN32)
  struct stat st;
  return stat((dir + "/" + e->d_name).c_str(), &st) == 0 &&
      S_ISDIR(st.st_mode);
#else
  (void)dir;
  return e->d_type == DT_DIR;
#endif
}

/**
 * Every plain file in @p dir, sorted. Generators, manifests and verify
 * scripts sit in these directories beside the fixtures; a caller that cares
 * filters on content rather than on a name, because a fixture's extension is
 * not a promise about what is inside it.
 */
inline std::vector<std::string> sweep_files_in(const std::string & dir) {
  std::vector<std::string> names;
  DIR * dp = opendir(dir.c_str());
  if (!dp) {
    return names;
  }
  while (struct dirent * e = readdir(dp)) {
    const std::string n = e->d_name;
    if (n == "." || n == ".." || sweep_entry_is_directory(dir, e)) {
      continue;
    }
    names.push_back(n);
  }
  closedir(dp);
  std::sort(names.begin(), names.end());
  return names;
}

/**
 * The files in @p dir that are images rather than the tooling beside them.
 *
 * Stated as what to leave out rather than what to take in: a generator, an
 * oracle dump or a README has a known extension, and a new codec's fixtures
 * do not. An allow-list of image extensions would have to be edited for every
 * format added, which is the thing this header exists to stop.
 */
inline std::vector<std::string> sweep_image_files_in(const std::string & dir) {
  static const char * const not_images[] = {".py", ".pyc", ".raw", ".ppm",
      ".pgm", ".txt", ".md", ".json", ".bin", ".c", ".h", ".sh"};
  std::vector<std::string> out;
  for (const std::string & name : sweep_files_in(dir)) {
    bool skip = false;
    for (const char * ext : not_images) {
      const size_t n = strlen(ext);
      if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) {
        skip = true;
        break;
      }
    }
    if (!skip) {
      out.push_back(name);
    }
  }
  return out;
}

} // namespace gimg_test

#endif // GHOTI_IO_GIMG_TESTS_REGISTRY_SWEEP_H
