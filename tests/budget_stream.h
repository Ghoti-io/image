/**
 * @file
 *
 * A writable stream that runs out of room, for sweeping write-failure arms.
 *
 * Every writer in this library checks the result of every write and returns
 * it, and not one of those arms runs against a memory output stream: it grows
 * on demand and never refuses. So the checks are unreachable from any
 * document, and a writer that dropped one - carried on after a failed write,
 * or returned GIMG_OK having written half a file - would pass every test in
 * the suite.
 *
 * This is the failing sink. It accepts a fixed number of bytes and then fails
 * every write, so sweeping that number from zero to the length of a good file
 * puts the failure at each write boundary in turn.
 *
 * It is built out of the real memory output stream rather than a stream of
 * its own, because there is no vtable to implement: `GIMG_Stream` is one
 * struct with one implementation behind it. The budget is imposed by writing
 * that many bytes to make the buffer exist, rewinding, clamping the recorded
 * capacity to the budget, and then refusing every further allocation - at
 * which point `grow_output` has nowhere to go and `gimg_stream_write` returns
 * the error and stores it, which is exactly what a sink that has failed does.
 * The buffer really is larger than the capacity it claims, so nothing writes
 * past the end of it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_BUDGET_STREAM_H
#define GHOTI_IO_GIMG_TESTS_BUDGET_STREAM_H

#include <cstdlib>
#include <vector>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/stream.h>

#include "../src/stream/stream_internal.h"

namespace gimg_test {

/** An allocator that hands out memory until it is told to stop. */
struct Refusing {
  GIMG_Allocator a{};
  bool refuse = false;
};

inline void * r_malloc(void * ctx, size_t size) {
  Refusing * f = (Refusing *)ctx;
  return f->refuse ? nullptr : malloc(size ? size : 1u);
}
inline void * r_calloc(void * ctx, size_t n, size_t size) {
  Refusing * f = (Refusing *)ctx;
  if (f->refuse) { return nullptr; }
  if (n != 0 && size > (size_t)-1 / n) { return nullptr; }
  return calloc(n ? n : 1u, size ? size : 1u);
}
inline void * r_realloc(void * ctx, void * ptr, size_t size) {
  Refusing * f = (Refusing *)ctx;
  return f->refuse ? nullptr : realloc(ptr, size ? size : 1u);
}
inline void r_free(void * ctx, void * ptr) {
  (void)ctx;
  free(ptr);
}

/** A writable stream that takes @p budget bytes and then fails every write. */
class BudgetStream {
public:
  explicit BudgetStream(size_t budget) {
    refusing_.a.ctx = &refusing_;
    refusing_.a.malloc_fn = r_malloc;
    refusing_.a.calloc_fn = r_calloc;
    refusing_.a.realloc_fn = r_realloc;
    refusing_.a.free_fn = r_free;
    if (gimg_stream_create_memory_output_with_allocator(&refusing_.a, &s_)
        != GIMG_OK) {
      s_ = nullptr;
      return;
    }
    // Make the buffer exist at at least the budget, then rewind and say the
    // capacity is the budget.  The allocation that follows is refused, so the
    // first write that needs more than the budget is the one that fails.
    const std::vector<unsigned char> pad(budget ? budget : 1u, 0u);
    size_t n = 0;
    if (gimg_stream_write(s_, pad.data(), pad.size(), &n) != GIMG_OK) {
      gimg_stream_destroy(s_);
      s_ = nullptr;
      return;
    }
    s_->position = 0;
    s_->size = budget;
    s_->error = GIMG_OK;
    refusing_.refuse = true;
  }

  ~BudgetStream() {
    if (s_) {
      refusing_.refuse = false;
      gimg_stream_destroy(s_);
    }
  }

  BudgetStream(const BudgetStream &) = delete;
  BudgetStream & operator=(const BudgetStream &) = delete;

  GIMG_Stream * get() const { return s_; }
  bool usable() const { return s_ != nullptr; }
  /** Bytes the writer managed to place before the sink gave out. */
  size_t written() const { return s_ ? s_->position : 0u; }

private:
  Refusing refusing_;
  GIMG_Stream * s_ = nullptr;
};

} // namespace gimg_test

#endif
