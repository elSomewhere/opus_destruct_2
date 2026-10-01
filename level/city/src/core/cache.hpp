// svx_city — the generator's caches, safe across threads (voxel_city core/lru.js and the lazy
// fields it keeps on records).
//
// Every product of the generator is a pure function of (config, key), so a cache only saves work:
// dropping a value and making it again gives the same value, and no result depends on what was
// asked before. ChunkSource::generate runs on several threads at once; here:
//
//   MemoCache<K, V>  key -> an immutable value (shared_ptr<const V>), made once outside any lock
//                    by the first thread to ask (the others wait for it), least recently used
//                    dropped beyond a count. A value lives on while anyone holds it.
//   Lazy<T>          a field computed on first use (JS: `x.f ??= make()` on a shared record),
//                    once, whichever thread asks first. Copying a record copies none of it.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace svx::city {

template <class K, class V, class Hash = std::hash<K>>
class MemoCache {
 public:
  explicit MemoCache(size_t capacity = 64, int shards = 8) : shards_(static_cast<size_t>(shards < 1 ? 1 : shards)) {
    per_shard_ = (capacity + shards_.size() - 1) / shards_.size();
    if (per_shard_ < 1) per_shard_ = 1;
  }
  MemoCache(const MemoCache&) = delete;
  MemoCache& operator=(const MemoCache&) = delete;

  // The value of key: made by make() (returning V or shared_ptr<const V>) the first time.
  template <class F>
  std::shared_ptr<const V> get(const K& key, F&& make) {
    Shard& sh = shards_[Hash{}(key) % shards_.size()];
    std::shared_ptr<Slot> slot;
    bool mine = false;
    {
      std::lock_guard<std::mutex> lk(sh.m);
      auto it = sh.map.find(key);
      if (it != sh.map.end()) {
        sh.lru.splice(sh.lru.begin(), sh.lru, it->second.pos);
        slot = it->second.slot;
      } else {
        slot = std::make_shared<Slot>();
        sh.lru.push_front(key);
        sh.map.emplace(key, Node{slot, sh.lru.begin()});
        mine = true;
        while (sh.map.size() > per_shard_) {
          sh.map.erase(sh.lru.back());
          sh.lru.pop_back();
        }
      }
    }
    if (mine) {
      std::shared_ptr<const V> v = wrap(make());
      {
        std::lock_guard<std::mutex> lk(slot->m);
        slot->value = v;
        slot->ready = true;
      }
      slot->cv.notify_all();
      return v;
    }
    std::unique_lock<std::mutex> lk(slot->m);
    slot->cv.wait(lk, [&] { return slot->ready; });
    return slot->value;
  }
  void clear() {
    for (Shard& sh : shards_) {
      std::lock_guard<std::mutex> lk(sh.m);
      sh.map.clear();
      sh.lru.clear();
    }
  }
  size_t size() const {
    size_t n = 0;
    for (const Shard& sh : shards_) {
      std::lock_guard<std::mutex> lk(sh.m);
      n += sh.map.size();
    }
    return n;
  }

 private:
  struct Slot {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::shared_ptr<const V> value;
  };
  struct Node {
    std::shared_ptr<Slot> slot;
    typename std::list<K>::iterator pos;
  };
  struct Shard {
    mutable std::mutex m;
    std::unordered_map<K, Node, Hash> map;
    std::list<K> lru;
  };
  static std::shared_ptr<const V> wrap(std::shared_ptr<const V> v) { return v; }
  static std::shared_ptr<const V> wrap(std::shared_ptr<V> v) { return v; }
  static std::shared_ptr<const V> wrap(V&& v) { return std::make_shared<const V>(std::move(v)); }
  std::vector<Shard> shards_;
  size_t per_shard_ = 1;
};

// A field made on first use, once (thread-safe). Not copied with its record.
template <class T>
class Lazy {
 public:
  Lazy() = default;
  Lazy(const Lazy&) {}
  Lazy& operator=(const Lazy&) { return *this; }
  ~Lazy() { delete p_.load(std::memory_order_acquire); }
  // (made outside any lock: two threads asking at once may both make it - the same value, as it is
  // pure - and the first stored is kept; no lock is held while a lazy field asks for another)
  template <class F>
  const T& get(F&& make) const {
    const T* p = p_.load(std::memory_order_acquire);
    if (p) return *p;
    const T* fresh = new T(make());
    const T* expected = nullptr;
    if (p_.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel)) return *fresh;
    delete fresh;
    return *expected;
  }
  bool ready() const { return p_.load(std::memory_order_acquire) != nullptr; }

 private:
  mutable std::atomic<const T*> p_{nullptr};
};

}  // namespace svx::city
