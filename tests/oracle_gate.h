/**
 * @file
 *
 * Whether a reference decoder can be reached, and what to do when it cannot.
 *
 * Every oracle in this library is pinned into one container image
 * (tools/oracle/containers/IMAGES) and reached through tools/oracle/
 * oracle-exec. This header is how a test binary asks the same question the
 * Python tools ask by importing oracle_env: *can I reach the reference this
 * comparison names, and is it the version the pin names.*
 *
 * It exists because of what was true before it. Twenty-one tests here reached
 * for an oracle and called GTEST_SKIP() when they could not find one. On this
 * machine none of them has ever fired, because Pillow, libjpeg, giflib and a
 * built bmplib are all present. On a machine without them they fire, gtest
 * prints SKIPPED, `make test` exits 0, and nothing in the headline output
 * separates "compared 151 fixtures against libjpeg" from "did not look".
 *
 *     An oracle can be correctly pinned and still absent, and absence is the
 *     failure mode that reads as success.
 *
 * The shape here is `compress`'s, which notes/suite/CONTAINERS.md section 4a
 * found to be the best in the suite: **one sentinel test per reference per
 * file**, which fails when that reference is unreachable. The individual
 * comparisons may still skip, because the sentinel guarantees the run goes red
 * either way, and one failure naming the missing reference reads better than
 * eleven failures naming its consequences.
 *
 * The trap the same section records is why it is *per reference* and not per
 * file: `compress` had a file holding two references and one sentinel, which
 * asserted the wrong one, and sixteen tests could vanish with the suite green.
 * A sentinel's presence is not evidence it covers what the file claims.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GIMG_TESTS_ORACLE_GATE_H
#define GIMG_TESTS_ORACLE_GATE_H

#include <cstdio>
#include <cstdlib>
#include <string>

/* Deliberately not including <gtest/gtest.h>. The gtest spellings below live
 * inside a macro body, which is not expanded until a test file uses it, so
 * this header stays includable from jpeg_test_utils.cpp - a translation unit
 * compiled without the gtest include path, and the one that most needs
 * repo_root() to be the same function the sentinels use rather than a second
 * copy of it. */

namespace oracle_gate {

/** The repository root, or an empty string when it cannot be found.
 *
 * GIMG_IMAGE_ROOT first, which `make test` sets. Otherwise walk up from the
 * working directory looking for tools/oracle/oracle-exec, which covers running
 * a test binary by hand from build/linux/release/apps - the way most of this
 * library's tests are actually run while something is being fixed. */
inline std::string repo_root(void) {
  static std::string cached;
  static bool done = false;
  if (done) {
    return cached;
  }
  done = true;
  const char * env = std::getenv("GIMG_IMAGE_ROOT");
  if (env && env[0] != '\0') {
    cached = env;
    return cached;
  }
  std::string dir = ".";
  for (int up = 0; up < 8; ++up) {
    std::string probe = dir + "/tools/oracle/oracle-exec";
    if (std::FILE * f = std::fopen(probe.c_str(), "r")) {
      std::fclose(f);
      cached = dir;
      return cached;
    }
    dir += "/..";
  }
  return cached;
}

/** Is a run allowed to treat an unreachable reference as a skip?
 *
 * Default no. GIMG_ORACLE_REQUIRED=0 says yes, and is meant for a developer
 * who knows their machine has no container engine and wants the rest of the
 * suite anyway. It is deliberately not the default: the whole point of this
 * header is that the lenient answer used to be the only one. */
inline bool required(void) {
  const char * env = std::getenv("GIMG_ORACLE_REQUIRED");
  return !(env && env[0] == '0' && env[1] == '\0');
}

/** Ask oracle-exec whether `reference` resolves, and to what.
 *
 * Returns true and sets @p out to the provenance line on success; returns
 * false and sets @p out to the reason on failure. The answer is cached per
 * reference: resolving one costs a container start, and a test binary asks the
 * same question dozens of times.
 *
 * `--print` rather than running anything: it resolves the pin, checks the
 * version and prints the argv it would use, which is exactly the question
 * without the cost of the answer. */
inline bool resolve(const char * reference, std::string * out) {
  static std::string names[8];
  static std::string answers[8];
  static bool oks[8];
  static int count = 0;
  for (int i = 0; i < count; ++i) {
    if (names[i] == reference) {
      if (out) {
        *out = answers[i];
      }
      return oks[i];
    }
  }
  std::string root = repo_root();
  std::string answer;
  bool ok = false;
  if (root.empty()) {
    answer = "could not find the repository root, so tools/oracle/oracle-exec "
             "could not be run; set GIMG_IMAGE_ROOT";
  } else {
    /* stderr is folded into stdout here on purpose: when this fails the reason
     * is on stderr and it is the whole value of the call. The engine's own
     * banner lands there too, which is why the *answers* of an oracle are read
     * from a separate call that keeps the streams apart. */
    std::string cmd = "\"" + root + "/tools/oracle/oracle_env.py\" " +
        reference + " 2>&1";
    std::string text;
    if (std::FILE * pipe = popen(cmd.c_str(), "r")) {
      char buf[512];
      while (std::fgets(buf, static_cast<int>(sizeof(buf)), pipe)) {
        text += buf;
      }
      int status = pclose(pipe);
      ok = (status == 0);
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
      }
      answer = text.empty() ? "oracle_env.py printed nothing" : text;
    } else {
      answer = "could not run tools/oracle/oracle_env.py";
    }
  }
  if (count < 8) {
    names[count] = reference;
    answers[count] = answer;
    oks[count] = ok;
    ++count;
  }
  if (out) {
    *out = answer;
  }
  return ok;
}

/** True when `reference` is reachable and matches its pin. */
inline bool reachable(const char * reference) {
  std::string ignored;
  return resolve(reference, &ignored);
}

/** What to print beside a comparison's numbers, e.g.
 *  "oracle(container): Pillow 11.1.0". */
inline std::string provenance(const char * reference) {
  std::string line;
  resolve(reference, &line);
  return line;
}

} // namespace oracle_gate

/**
 * Define the sentinel for one reference in one test file.
 *
 * `ORACLE_SENTINEL(JpegLoad, pillow)` defines
 * JpegLoad.PillowOracleIsReachable, which fails - not skips - when Pillow
 * cannot be reached at the version tools/oracle/containers/IMAGES pins.
 *
 * The opt-out is printed at the point of failure rather than being a thing to
 * know, and it is per gate rather than a global boolean, which is the shape
 * notes/suite/CONTAINERS.md section 2.5 argues for: the decision to drop a
 * check should land in the command that was typed, so it shows up in shell
 * history and CI configuration rather than in an environment nobody prints.
 */
#define ORACLE_SENTINEL(suite, reference)                                     \
  TEST(suite, reference##OracleIsReachable) {                                 \
    std::string why;                                                          \
    if (oracle_gate::resolve(#reference, &why)) {                             \
      SUCCEED() << why;                                                       \
      return;                                                                 \
    }                                                                         \
    if (!oracle_gate::required()) {                                           \
      GTEST_SKIP() << "GIMG_ORACLE_REQUIRED=0, and the " #reference           \
                      " reference is unreachable: " << why;                   \
    }                                                                         \
    FAIL() << "The " #reference " reference is unreachable, so every "        \
              "comparison in this file that names it compared nothing:\n  "   \
           << why                                                             \
           << "\n\nBuild it with `make oracle-build`. To run the rest of "    \
              "this suite without it, drop this one gate:\n"                  \
              "  --gtest_filter=-" #suite "." #reference                      \
              "OracleIsReachable\n"                                           \
              "or, for every oracle in the suite, GIMG_ORACLE_REQUIRED=0.";   \
  }

#endif // GIMG_TESTS_ORACLE_GATE_H
