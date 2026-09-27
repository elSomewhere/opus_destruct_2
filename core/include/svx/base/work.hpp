// structvox — deterministic work accounting (the engine's work budget, plan §B5).
//
// Background jobs (bubble setups and steps, settle projections, static settles) count the work
// they do in node-iterations through the running thread's counter: a PCG iteration over n
// nodes (or cells) counts n, a multigrid build n * kWorkMgBuild, and so on. The counts depend
// only on the computation, never on timing, so the engine can commit a job ceil(work /
// tick_work) ticks after its launch and stay bit-identical on every machine and thread count;
// a slower machine only makes the simulation thread wait.
#pragma once

#include <atomic>

#include "svx/base/types.hpp"

namespace svx {

// relative costs, per node (calibrated against PCG iterations; see docs/STATUS.md)
inline constexpr i64 kWorkMgBuild = 12;      // a multigrid hierarchy build
inline constexpr i64 kWorkLawSweep = 1;      // a law sweep over a lattice's bonds (per cell)
inline constexpr i64 kWorkStepFixed = 2;     // a bubble step's projections and state updates
inline constexpr i64 kWorkComposite = 1;     // building a composite (per window cell / 4)
inline constexpr i64 kWorkForces = 1;        // an internal-force pass over a lattice (per cell)
inline constexpr i64 kWorkExtract = 6;       // finding and extracting a structure (per cell)
// (Phases count as they start: a job's count is then a lower bound of its total that runs
// ahead of the work, and the engine seldom has to wait to know a job is not due yet.)

struct WorkCounter {
  std::atomic<i64> done{0};
};

inline thread_local WorkCounter* tl_work = nullptr;
inline thread_local i64 tl_work_div = 1;

inline void add_work(i64 w) {
  if (tl_work) tl_work->done.fetch_add(w / tl_work_div, std::memory_order_relaxed);
}

// Counts the calling thread's work into `c` (nullptr: none) for its lifetime, divided by `div`
// (work shared with a thread team).
class WorkScope {
 public:
  explicit WorkScope(WorkCounter* c, i64 div = 1) : prev_(tl_work), prev_div_(tl_work_div) {
    tl_work = c;
    tl_work_div = div < 1 ? 1 : div;
  }
  ~WorkScope() {
    tl_work = prev_;
    tl_work_div = prev_div_;
  }
  WorkScope(const WorkScope&) = delete;
  WorkScope& operator=(const WorkScope&) = delete;

 private:
  WorkCounter* prev_;
  i64 prev_div_;
};

}  // namespace svx
