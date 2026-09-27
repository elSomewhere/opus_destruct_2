// structvox — minimal deterministic parallel-for.
//
// Work is split into fixed-size chunks that do not depend on the thread count, so any
// per-chunk result (and any reduction combined in chunk order) is bitwise identical for
// 1..N threads (plan §B9 determinism rules). Without threads (e.g. single-threaded WASM)
// everything runs inline.
#pragma once

#include <functional>
#include <vector>

#include "svx/base/types.hpp"

namespace svx {

int num_threads();
void set_num_threads(int n);  // 1 = serial; takes effect for subsequent calls

// Calls f(begin, end) for consecutive chunks of [0, n) of size `grain`. A parallel_for inside a
// chunk runs inline (in order). Callers on several threads take turns on the pool.
void parallel_for(i64 n, i64 grain, const std::function<void(i64, i64)>& f);

// Deterministic sum: per-chunk partial sums combined in chunk order.
f64 parallel_sum(i64 n, i64 grain, const std::function<f64(i64, i64)>& f);

// While alive, parallel_for / parallel_sum on this thread run their chunks inline, in order
// (bitwise the same results): for work on a background thread that must not share the pool.
class SerialScope {
 public:
  SerialScope();
  ~SerialScope();
  SerialScope(const SerialScope&) = delete;
  SerialScope& operator=(const SerialScope&) = delete;
};

// A private pool of `threads` threads (the calling thread included) for background work that
// must not share the simulation's pool. Same chunking as the shared pool, so results are
// bitwise identical to any other thread count.
class ThreadTeam {
 public:
  explicit ThreadTeam(int threads);
  ~ThreadTeam();
  ThreadTeam(const ThreadTeam&) = delete;
  ThreadTeam& operator=(const ThreadTeam&) = delete;
  int threads() const;

 private:
  friend class TeamScope;
  void* pool_;
};

// While alive, parallel_for / parallel_sum on this thread run on `team` (the innermost scope
// wins: a TeamScope inside a SerialScope uses the team, a SerialScope inside a TeamScope runs
// inline).
class TeamScope {
 public:
  explicit TeamScope(ThreadTeam& team);
  ~TeamScope();
  TeamScope(const TeamScope&) = delete;
  TeamScope& operator=(const TeamScope&) = delete;

 private:
  void* prev_;
  int prev_serial_;
};

}  // namespace svx
