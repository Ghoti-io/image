/**
 * @file
 *
 * An allocator that fails its Nth call, for sweeping error-cleanup paths.
 *
 * A codec's OOM arms are unreachable from any file: the arms that free partial
 * state and return GIMG_ERR_OOM only run when something ran out of memory,
 * which no fixture can arrange. Injecting the failure is the only way in, and
 * sweeping N from 1 to a clean run's allocation count reaches each arm in
 * turn.
 *
 * Shared between the unit sweep over whole loads and the codec suites, which
 * have their own fixture builders: the harness is generic, the fixtures are
 * not, so this is the half that moves.
 *
 * The bound always comes from a clean run's own allocation count rather than a
 * constant, so the sweep widens by itself when the code under it grows a new
 * allocation - which is what makes it keep asking a question rather than
 * recording an answer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_FAILING_ALLOCATOR_H
#define GHOTI_IO_GIMG_TESTS_FAILING_ALLOCATOR_H

#include <cstdlib>
#include <ghoti.io/image/core.h>

namespace gimg_test {

/** An allocator that fails one chosen call and counts what it hands out. */
struct Failing {
  GIMG_Allocator a{};
  long attempts = 0;    ///< Allocation calls seen.
  long fail_at = -1;    ///< 1-based call to fail; -1 never fails.
  long outstanding = 0; ///< Blocks handed out and not yet returned.

  bool should_fail() {
    attempts++;
    return fail_at >= 0 && attempts == fail_at;
  }
};

inline void * f_malloc(void * ctx, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  void * p = malloc(size ? size : 1u);
  if (p) { f->outstanding++; }
  return p;
}
inline void * f_calloc(void * ctx, size_t n, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  if (n != 0 && size > (size_t)-1 / n) { return nullptr; } // overflow is failure
  void * p = calloc(n ? n : 1u, size ? size : 1u);
  if (p) { f->outstanding++; }
  return p;
}
inline void * f_realloc(void * ctx, void * ptr, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  void * p = realloc(ptr, size ? size : 1u);
  if (p && !ptr) { f->outstanding++; }
  return p;
}
inline void f_free(void * ctx, void * ptr) {
  Failing * f = (Failing *)ctx;
  if (ptr) {
    f->outstanding--;
    free(ptr);
  }
}

inline void init(Failing & f) {
  f.a.ctx = &f;
  f.a.malloc_fn = f_malloc;
  f.a.calloc_fn = f_calloc;
  f.a.realloc_fn = f_realloc;
  f.a.free_fn = f_free;
}

} // namespace gimg_test

#endif
