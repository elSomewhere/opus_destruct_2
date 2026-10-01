#include "svx/base/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace svx {

namespace {

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
constexpr bool kThreadsAvailable = false;
#else
constexpr bool kThreadsAvailable = true;
#endif

// (more than any machine has cores: a host's count is held to it - a thread it cannot create
// would end the process)
constexpr int kMaxThreads = 256;

inline void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

// Low-latency pool: workers spin on an atomic generation for a short while before sleeping,
// so dispatching a job to already-spinning workers costs ~1 us instead of a condition-variable
// wake-up per worker. Chunk boundaries never depend on the thread count (determinism).
class Pool {
 public:
  static Pool& get() {
    static Pool p;
    return p;
  }

  explicit Pool(int threads) : nthreads_(kThreadsAvailable ? std::clamp(threads, 1, kMaxThreads) : 1) {}

  int threads() const { return nthreads_.load(std::memory_order_relaxed); }

  void set_threads(int n) {
    // (never under a job: a host stepping a world on another thread finishes its job first)
    std::lock_guard<std::mutex> one(run_mu_);
    stop_workers();
    nthreads_.store(std::clamp(n, 1, kMaxThreads), std::memory_order_relaxed);
  }

  void run(i64 nchunks, const std::function<void(i64)>& chunk_fn) {
    if (threads() <= 1 || nchunks <= 1 || !kThreadsAvailable) {
      for (i64 c = 0; c < nchunks; ++c) chunk_fn(c);
      return;
    }
    // one job at a time (hosts stepping several worlds on several threads share the pool)
    std::lock_guard<std::mutex> one(run_mu_);
    ensure_workers();
    job_ = &chunk_fn;
    nchunks_ = nchunks;
    next_.store(0, std::memory_order_relaxed);
    done_.store(0, std::memory_order_relaxed);
    gen_.fetch_add(1, std::memory_order_seq_cst);
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (sleepers_ > 0) cv_.notify_all();
    }
    work();
    const int nw = static_cast<int>(workers_.size());
    for (int spins = 0; done_.load(std::memory_order_acquire) != nw; ++spins) {
      if (spins < 4096) cpu_relax();
      else std::this_thread::yield();
    }
    job_ = nullptr;
  }

  ~Pool() { stop_workers(); }

 private:
  Pool() {
    const unsigned hw = std::thread::hardware_concurrency();
    nthreads_.store(kThreadsAvailable ? static_cast<int>(std::max(1u, std::min(hw, 16u))) : 1);
  }

  void work() {
    const std::function<void(i64)>* job = job_;
    const i64 n = nchunks_;
    for (;;) {
      const i64 c = next_.fetch_add(1, std::memory_order_relaxed);
      if (c >= n) break;
      (*job)(c);
    }
  }

  void ensure_workers() {
    const int n = threads();
    if (static_cast<int>(workers_.size()) == n - 1) return;
    stop_workers();
    quit_.store(false);
    // Workers start from the generation current *before* the next dispatch, or a
    // late-starting thread could skip the job the caller is about to wait on.
    const u64 g = gen_.load();
    for (int i = 0; i < n - 1; ++i) workers_.emplace_back([this, g] { loop(g); });
  }

  void stop_workers() {
    quit_.store(true);
    gen_.fetch_add(1);
    {
      std::lock_guard<std::mutex> lk(mu_);
      cv_.notify_all();
    }
    for (auto& t : workers_) t.join();
    workers_.clear();
    quit_.store(false);
  }

  void loop(u64 seen) {
    for (;;) {
      // spin briefly, then sleep until the generation changes
      u64 g = gen_.load(std::memory_order_acquire);
      for (int spins = 0; g == seen && spins < 20000; ++spins) {
        cpu_relax();
        g = gen_.load(std::memory_order_acquire);
      }
      if (g == seen) {
        std::unique_lock<std::mutex> lk(mu_);
        ++sleepers_;
        cv_.wait(lk, [&] { return gen_.load(std::memory_order_acquire) != seen; });
        --sleepers_;
        g = gen_.load(std::memory_order_acquire);
      }
      if (quit_.load()) return;
      seen = g;
      work();
      done_.fetch_add(1, std::memory_order_release);
    }
  }

  std::atomic<int> nthreads_{1};
  std::vector<std::thread> workers_;
  std::mutex run_mu_;
  std::mutex mu_;
  std::condition_variable cv_;
  int sleepers_ = 0;
  const std::function<void(i64)>* job_ = nullptr;
  i64 nchunks_ = 0;
  std::atomic<i64> next_{0};
  std::atomic<int> done_{0};
  std::atomic<u64> gen_{0};
  std::atomic<bool> quit_{false};
};

}  // namespace

namespace {
thread_local int t_serial = 0;
thread_local int t_in_job = 0;         // inside a chunk of a pool job: nested calls run inline
thread_local Pool* t_pool = nullptr;  // a ThreadTeam's pool (TeamScope), else the shared one
inline Pool& pool() { return t_pool ? *t_pool : Pool::get(); }
struct InJob {
  InJob() { ++t_in_job; }
  ~InJob() { --t_in_job; }
};
}  // namespace

SerialScope::SerialScope() { ++t_serial; }
SerialScope::~SerialScope() { --t_serial; }

ThreadTeam::ThreadTeam(int threads) : pool_(new Pool(threads)) {}
ThreadTeam::~ThreadTeam() { delete static_cast<Pool*>(pool_); }
int ThreadTeam::threads() const { return static_cast<const Pool*>(pool_)->threads(); }

TeamScope::TeamScope(ThreadTeam& team) : prev_(t_pool), prev_serial_(t_serial) {
  t_pool = static_cast<Pool*>(team.pool_);
  t_serial = 0;
}
TeamScope::~TeamScope() {
  t_pool = static_cast<Pool*>(prev_);
  t_serial = prev_serial_;
}

int num_threads() { return Pool::get().threads(); }
void set_num_threads(int n) { Pool::get().set_threads(n); }

void parallel_for(i64 n, i64 grain, const std::function<void(i64, i64)>& f) {
  if (n <= 0) return;
  grain = std::max<i64>(1, grain);
  const i64 nchunks = (n + grain - 1) / grain;
  if (nchunks == 1) {
    f(0, n);
    return;
  }
  // (inline: in a SerialScope, or nested in a chunk of another job - the pool runs one job at a
  // time, and a nested dispatch would wait on the workers busy with the outer one)
  if (t_serial || t_in_job) {
    for (i64 c = 0; c < nchunks; ++c) f(c * grain, std::min(n, (c + 1) * grain));
    return;
  }
  pool().run(nchunks, [&](i64 c) {
    InJob in;
    const i64 b = c * grain;
    f(b, std::min(n, b + grain));
  });
}

f64 parallel_sum(i64 n, i64 grain, const std::function<f64(i64, i64)>& f) {
  if (n <= 0) return 0.0;
  grain = std::max<i64>(1, grain);
  const i64 nchunks = (n + grain - 1) / grain;
  if (nchunks == 1) return f(0, n);
  std::vector<f64> part(static_cast<size_t>(nchunks), 0.0);
  auto chunk = [&](i64 c) {
    InJob in;
    const i64 b = c * grain;
    part[static_cast<size_t>(c)] = f(b, std::min(n, b + grain));
  };
  if (t_serial || t_in_job) {
    for (i64 c = 0; c < nchunks; ++c) chunk(c);
  } else {
    pool().run(nchunks, chunk);
  }
  f64 s = 0.0;
  for (f64 v : part) s += v;
  return s;
}

}  // namespace svx
