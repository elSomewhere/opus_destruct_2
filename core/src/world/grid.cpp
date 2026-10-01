#include "svx/world/grid.hpp"

#include <atomic>

#include <algorithm>
#include <cstring>

namespace svx {

const Chunk* VoxelGrid::chunk(const IVec3& c) const {
  const auto it = chunks_.find(key3(c[0], c[1], c[2]));
  return it == chunks_.end() ? nullptr : &it->second;
}

Chunk& VoxelGrid::chunk_mut(const IVec3& c) { return chunks_[key3(c[0], c[1], c[2])]; }

void VoxelGrid::sanitize() {
  for (auto& [k, c] : chunks_) {
    if (c.uniform) {
      if (!vox_valid(c.value)) c.value = kAir;
      continue;
    }
    for (Vox& v : c.v)
      if (!vox_valid(v)) v = kAir;
  }
}

void VoxelGrid::remap(const std::array<Vox, 256>& to) {
  for (auto& [k, c] : chunks_) {
    if (c.uniform) {
      c.value = to[c.value];
      continue;
    }
    bool any = false;
    for (Vox& v : c.v) {
      const Vox w = to[v];
      any = any || w != v;
      v = w;
    }
    if (!any) continue;
    c.solid = 0;
    c.free = 0;
    for (Vox v : c.v) {
      c.solid += vox_solid(v) ? 1 : 0;
      c.free += vox_free(v) ? 1 : 0;
    }
    c.vox_version = ++vox_seq_;
  }
}

Vox VoxelGrid::get(i32 x, i32 y, i32 z) const {
  const IVec3 p{x, y, z};
  const Chunk* c = chunk(chunk_of(p));
  if (!c) return kAir;
  return c->uniform ? c->value : c->v[chunk_index(p)];
}

void VoxelGrid::set(i32 x, i32 y, i32 z, Vox v) {
  const IVec3 p{x, y, z};
  const IVec3 cc = chunk_of(p);
  Chunk& c = chunk_mut(cc);
  if (c.uniform) {
    if (c.value == v) return;
    c.v = acquire_buffer(c.value);
    c.solid = vox_solid(c.value) ? kChunkVox : 0;
    c.free = vox_free(c.value) ? kChunkVox : 0;
    c.uniform = false;
  }
  Vox& slot = c.v[chunk_index(p)];
  if (slot == v) return;
  const Vox old = slot;
  c.solid += (vox_solid(v) ? 1 : 0) - (vox_solid(slot) ? 1 : 0);
  c.free += (vox_free(v) ? 1 : 0) - (vox_free(slot) ? 1 : 0);
  slot = v;
  note_voxels_modified(key3(cc[0], cc[1], cc[2]));
  // values bound to the voxel that was there, or to the air a solid took (a regenerable layer's
  // base value under a voxel that went stays - nothing reads it - until a voxel is placed there)
  if (c.has_layers() || any_regen_)
    for (int L = 0; L < static_cast<int>(layers_.size()); ++L) {
      const LayerSpec& spec = layers_[size_t(L)];
      const LayerBind b = spec.bind;
      bool clear = (b == LayerBind::Solid && vox_solid(old)) || (b == LayerBind::Air && vox_solid(v));
      if (spec.regenerable && b == LayerBind::Solid) clear = vox_solid(v);
      if (clear && layer(L, p)) set_layer(L, p, 0);
    }
  c.vox_version = ++vox_seq_;
  note_modified(cc);
  const u64 key = key3(cc[0], cc[1], cc[2]);
  touch_dirty(key);
  if (c.solid == 0) compact_.push_back(key);
  // a changed voxel also changes the faces of its neighbours in adjacent chunks
  for (int a = 0; a < 3; ++a) {
    IVec3 q = p;
    if ((p[a] & (kChunk - 1)) == 0) {
      q[a] -= 1;
      const IVec3 nc = chunk_of(q);
      touch_dirty(key3(nc[0], nc[1], nc[2]));
    } else if ((p[a] & (kChunk - 1)) == kChunk - 1) {
      q[a] += 1;
      const IVec3 nc = chunk_of(q);
      touch_dirty(key3(nc[0], nc[1], nc[2]));
    }
  }
}

void VoxelGrid::touch_dirty(u64 k) {
  if (k == last_dirty_ || dirty_all_) return;
  last_dirty_ = k;
  dirty_.insert(k);
  if (dirty_.size() > std::max<size_t>(65536, 4 * chunks_.size())) {
    dirty_all_ = true;  // (nobody takes them: report everything next time, keep nothing now)
    std::unordered_set<u64>().swap(dirty_);
  }
}

i64 VoxelGrid::dirty_bytes() const {
  return static_cast<i64>(dirty_.size() * (sizeof(u64) + 2 * sizeof(void*)) + dirty_.bucket_count() * sizeof(void*) +
                          compact_.capacity() * sizeof(u64));
}

std::vector<u8> VoxelGrid::acquire_buffer(u8 fill) {
  std::vector<u8> b;
  if (!spare_.empty()) {
    b = std::move(spare_.back());
    spare_.pop_back();
  }
  b.assign(kChunkVox, fill);
  return b;
}

void VoxelGrid::release_buffer(std::vector<u8>&& b) {
  if (b.capacity() >= size_t(kChunkVox) && b.capacity() <= 2 * size_t(kChunkVox) && spare_.size() < kMaxSpare) {
    spare_.push_back(std::move(b));
    return;
  }
  std::vector<u8>().swap(b);
}

void VoxelGrid::compact_changed() {
  std::sort(compact_.begin(), compact_.end());
  compact_.erase(std::unique(compact_.begin(), compact_.end()), compact_.end());
  for (u64 k : compact_) {
    const auto it = chunks_.find(k);
    if (it == chunks_.end()) continue;
    Chunk& c = it->second;
    if (c.uniform || c.solid != 0) continue;
    // all air: no voxels, no bonds, no design classes to keep (the content is unchanged)
    release_buffer(std::move(c.v));
    release_buffer(std::move(c.broken));
    release_buffer(std::move(c.strength));
    c.v = {};
    c.broken = {};
    std::vector<u32>().swap(c.broken_few);
    c.strength = {};
    std::vector<u32>().swap(c.jbroken);
    c.uniform = true;
    c.value = kAir;
    c.free = 0;
  }
  compact_.clear();
  if (compact_.capacity() > 4096) std::vector<u64>().swap(compact_);
}

void VoxelGrid::forget_modified(u64 k) {
  voxel_modified_.erase(k);
  const auto it = modified_.find(k);
  if (it == modified_.end()) return;
  const u32 i = it->second;
  const u64 last = modified_list_.back();
  modified_list_[i] = last;
  modified_[last] = i;
  modified_list_.pop_back();
  modified_.erase(k);
}

bool VoxelGrid::broken(const IVec3& p, int axis) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || !c->any_broken()) return false;
  return (c->broken_at(chunk_index(p)) >> axis) & 1;
}

namespace {
// Sets broken bits of voxel i (true: some were not set).
bool set_broken_bits(Chunk& c, i32 i, u8 bits, VoxelGrid& g) {
  if (!c.broken.empty()) {
    u8& b = c.broken[size_t(i)];
    if ((b | bits) == b) return false;
    b = static_cast<u8>(b | bits);
    return true;
  }
  const u32 key = static_cast<u32>(i) << 3;
  auto it = std::lower_bound(c.broken_few.begin(), c.broken_few.end(), key);
  if (it != c.broken_few.end() && (*it >> 3) == static_cast<u32>(i)) {
    if (((*it & 7) | bits) == (*it & 7)) return false;
    *it |= bits;
    return true;
  }
  if (c.broken_few.size() >= kBrokenListMax) {
    // (many: a byte per voxel)
    c.broken = g.acquire_buffer(0);
    for (u32 e : c.broken_few) c.broken[e >> 3] = static_cast<u8>(e & 7);
    std::vector<u32>().swap(c.broken_few);
    c.broken[size_t(i)] = bits;
    return true;
  }
  c.broken_few.insert(it, key | bits);
  return true;
}
}  // namespace

void VoxelGrid::install_seams(const IVec3& cc, const std::vector<u8>& bits) {
  const auto it = chunks_.find(key3(cc[0], cc[1], cc[2]));
  if (it == chunks_.end() || bits.size() != size_t(kChunkVox)) return;
  Chunk& c = it->second;
  if (c.uniform && !vox_solid(c.value)) return;
  for (i32 i = 0; i < kChunkVox; ++i)
    if ((bits[size_t(i)] & 7) && vox_solid(c.uniform ? c.value : c.v[size_t(i)])) set_broken_bits(c, i, static_cast<u8>(bits[size_t(i)] & 7), *this);
}

bool VoxelGrid::bond(const IVec3& p, int axis) const {
  const Vox a = get(p);
  if (!vox_solid(a)) return false;
  IVec3 q = p;
  q[axis] += 1;
  const Vox b = get(q);
  if (!vox_solid(b)) return false;
  if (vox_anchored(a) && vox_anchored(b)) return false;
  return !broken(p, axis);
}

void VoxelGrid::break_bond(const IVec3& p, int axis) {
  // (a voxel in no chunk is air: it has no bond to break, and no chunk is made for it)
  const auto it = chunks_.find(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  if (it == chunks_.end()) return;
  Chunk& c = it->second;
  if (c.uniform && !vox_solid(c.value)) return;
  if (!set_broken_bits(c, chunk_index(p), static_cast<u8>(1u << axis), *this)) return;
  // (a broken bond changes no surface: the chunk is not reported changed)
  note_modified(chunk_of(p));
  const IVec3 bc = chunk_of(p);
  note_voxels_modified(key3(bc[0], bc[1], bc[2]));
}

bool VoxelGrid::junction_broken(const IVec3& p, int face, int sub) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || c->jbroken.empty()) return false;
  const int li = chunk_index(p);
  const u32 whole = junction_code(li, face, kJunctionFace);
  const u32 one = junction_code(li, face, sub);
  return std::binary_search(c->jbroken.begin(), c->jbroken.end(), one) ||
         (sub != kJunctionFace && std::binary_search(c->jbroken.begin(), c->jbroken.end(), whole));
}

void VoxelGrid::clear_junction_breaks() {
  for (auto& [k, c] : chunks_) {
    if (c.jbroken.empty()) continue;
    c.jbroken.clear();
    c.jbroken.shrink_to_fit();
    note_modified(unkey3(k));
    note_voxels_modified(k);
  }
}

void VoxelGrid::break_junction(const IVec3& p, int face, int sub) {
  // (like a broken bond: a voxel in no chunk, or in an all-air one, has no junction to break)
  const auto it = chunks_.find(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  if (it == chunks_.end()) return;
  Chunk& c = it->second;
  if (c.uniform && !vox_solid(c.value)) return;
  const u32 code = junction_code(chunk_index(p), face, sub);
  const auto at = std::lower_bound(c.jbroken.begin(), c.jbroken.end(), code);
  if (at != c.jbroken.end() && *at == code) return;
  c.jbroken.insert(at, code);
  const IVec3 bc = chunk_of(p);
  note_modified(bc);
  note_voxels_modified(key3(bc[0], bc[1], bc[2]));
}

u8 VoxelGrid::strength(const IVec3& p) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || c->strength.empty()) return 0;
  return c->strength[chunk_index(p)];
}

void VoxelGrid::set_strength(const IVec3& p, u8 cls) {
  const auto it = chunks_.find(key3(p[0] >> kChunkBits, p[1] >> kChunkBits, p[2] >> kChunkBits));
  if (it == chunks_.end()) return;  // (air: nothing to strengthen)
  Chunk& c = it->second;
  if (c.strength.empty()) {
    if (cls == 0) return;
    c.strength = acquire_buffer(0);
  }
  c.strength[chunk_index(p)] = cls;
}

void VoxelGrid::mark_dirty(const IVec3& c) { touch_dirty(key3(c[0], c[1], c[2])); }

std::vector<u64> VoxelGrid::take_dirty() {
  std::vector<u64> out;
  if (dirty_all_) {
    for (const auto& [k, c] : chunks_) out.push_back(k);
    dirty_all_ = false;
  } else {
    out.assign(dirty_.begin(), dirty_.end());
  }
  std::unordered_set<u64>().swap(dirty_);
  last_dirty_ = ~0ull;
  std::sort(out.begin(), out.end());
  return out;
}

int VoxelGrid::add_layer(const LayerSpec& spec) {
  const int existing = layer_index(spec.name);
  if (existing >= 0) {
    // (the same layer again; a different one under the same name is refused)
    const LayerSpec& l = layers_[size_t(existing)];
    return l.persistent == spec.persistent && l.bind == spec.bind && l.regenerable == spec.regenerable ? existing : -1;
  }
  if (static_cast<int>(layers_.size()) >= kMaxLayers || spec.name.empty() || spec.name.size() > 255) return -1;
  layers_.push_back(spec);
  any_regen_ = any_regen_ || spec.regenerable;
  return static_cast<int>(layers_.size()) - 1;
}

int VoxelGrid::layer_index(const std::string& name) const {
  for (size_t i = 0; i < layers_.size(); ++i)
    if (layers_[i].name == name) return static_cast<int>(i);
  return -1;
}

void VoxelGrid::adopt_layers(const std::vector<LayerSpec>& specs) {
  std::array<int, kMaxLayers> to{};  // old index -> new (-1: dropped)
  to.fill(-1);
  for (size_t i = 0; i < layers_.size(); ++i)
    for (size_t j = 0; j < specs.size(); ++j)
      if (specs[j].name == layers_[i].name) to[i] = static_cast<int>(j);
  bool identity = layers_.size() <= specs.size();
  for (size_t i = 0; i < layers_.size() && identity; ++i) identity = to[i] == static_cast<int>(i);
  if (!identity)
    for (auto& [k, c] : chunks_) {
      std::array<LayerValues, kMaxLayers> nl;
      std::array<u16, kMaxLayers> nc{};
      for (size_t i = 0; i < kMaxLayers; ++i) {
        if (c.layer[i].empty() && !c.layer[i].own_) continue;
        if (i < layers_.size() && to[i] >= 0) {
          nl[size_t(to[i])] = std::move(c.layer[i]);
          nc[size_t(to[i])] = c.layer_count[i];
        } else {
          release_buffer(std::move(c.layer[i].v_));
        }
      }
      c.layer = std::move(nl);
      c.layer_count = nc;
    }
  layers_ = specs;
  any_regen_ = false;
  for (const LayerSpec& l : layers_) any_regen_ = any_regen_ || l.regenerable;
}

namespace {
// (a uniform layer's values: a shared array per value, made once - at most 255 of them)
const u8* uniform_array(u8 v) {
  static std::array<std::atomic<const u8*>, 256> arrays{};
  const u8* a = arrays[v].load(std::memory_order_acquire);
  if (a) return a;
  u8* made = new u8[size_t(kChunkVox)];
  std::fill(made, made + kChunkVox, v);
  const u8* expected = nullptr;
  if (!arrays[v].compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
    delete[] made;
    return expected;
  }
  return made;
}
std::atomic<u64> g_base_epochs{0};
}  // namespace

const u8* LayerValues::data() const {
  if (one_) return uniform_array(one_);
  return v_.empty() ? nullptr : v_.data();
}

void VoxelGrid::set_layer_base(const LayerBase* b) {
  base_ = b;
  base_epoch_ = ++g_base_epochs;
}

const u8* VoxelGrid::base_values(int L, const IVec3& cc) const {
  struct Memo {
    u64 epoch = 0, key = 0;
    int L = -1;
    std::shared_ptr<const void> hold;
    const u8* p = nullptr;
  };
  thread_local Memo m;
  const u64 key = key3(cc[0], cc[1], cc[2]);
  if (m.epoch == base_epoch_ && m.L == L && m.key == key) return m.p;
  std::shared_ptr<const void> hold;
  const u8* p = base_->base(L, cc, &hold);
  m.epoch = base_epoch_;
  m.key = key;
  m.L = L;
  m.hold = std::move(hold);
  m.p = p;
  return p;
}

const u8* VoxelGrid::layer_values(int L, const IVec3& cc, std::shared_ptr<const void>* hold) const {
  if (L < 0 || L >= static_cast<int>(layers_.size())) return nullptr;
  const Chunk* c = chunk(cc);
  if (!c) return nullptr;
  const LayerValues& a = c->layer[size_t(L)];
  if (!a.empty()) return a.data();
  if (a.own_ || !base_ || !layers_[size_t(L)].regenerable) return nullptr;
  return base_->base(L, cc, hold);
}

void VoxelGrid::set_layer_values(int L, const IVec3& cc, const u8* values) {
  if (L < 0 || L >= static_cast<int>(layers_.size()) || !values) return;
  u32 n = 0;
  bool same = true;
  for (i32 i = 0; i < kChunkVox; ++i) {
    n += values[i] != 0;
    same = same && values[i] == values[0];
  }
  const u64 key = key3(cc[0], cc[1], cc[2]);
  auto it = chunks_.find(key);
  if (it == chunks_.end()) {
    if (n == 0) return;
    it = chunks_.emplace(key, Chunk{}).first;
  }
  Chunk& c = it->second;
  LayerValues& a = c.layer[size_t(L)];
  if (a.empty() && n == 0 && (a.own_ || !layers_[size_t(L)].regenerable)) return;
  release_buffer(std::move(a.v_));
  a.v_ = {};
  a.one_ = 0;
  a.own_ = layers_[size_t(L)].regenerable;
  if (n == static_cast<u32>(kChunkVox) && same) {
    a.one_ = values[0];
  } else if (n > 0) {
    a.v_ = acquire_buffer(0);
    std::copy(values, values + kChunkVox, a.v_.data());
  }
  c.layer_count[size_t(L)] = static_cast<u16>(std::min<u32>(n, 0xFFFF));
  std::unordered_set<u64>& d = layer_dirty_[size_t(L)];
  d.insert(key);
  if (d.size() > std::max<size_t>(65536, 4 * chunks_.size())) d.clear();  // (nobody takes them)
  if (layers_[size_t(L)].persistent) note_modified(cc);
}

u8 VoxelGrid::layer(int L, const IVec3& p) const {
  if (L < 0 || L >= kMaxLayers) return 0;
  const IVec3 cc = chunk_of(p);
  const Chunk* c = chunk(cc);
  if (!c) return 0;
  const LayerValues& a = c->layer[size_t(L)];
  if (!a.empty()) return a[size_t(chunk_index(p))];
  if (a.own_ || !base_ || L >= static_cast<int>(layers_.size()) || !layers_[size_t(L)].regenerable) return 0;
  const u8* b = base_values(L, cc);  // (a regenerable layer's base value)
  return b ? b[size_t(chunk_index(p))] : 0;
}

bool VoxelGrid::set_layer(int L, const IVec3& p, u8 v) {
  if (L < 0 || L >= static_cast<int>(layers_.size())) return false;
  const IVec3 cc = chunk_of(p);
  const u64 key = key3(cc[0], cc[1], cc[2]);
  auto it = chunks_.find(key);
  if (it == chunks_.end()) {
    if (v == 0) return false;
    it = chunks_.emplace(key, Chunk{}).first;
  }
  Chunk& c = it->second;
  LayerValues& a = c.layer[size_t(L)];
  if (layers_[size_t(L)].regenerable && !a.own_) {
    // (written in play: the base values become the chunk's own)
    const u8* b = base_ ? base_values(L, cc) : nullptr;
    if (b && b[size_t(chunk_index(p))] == v) return false;
    if (b) {
      a.v_ = acquire_buffer(0);
      std::copy(b, b + kChunkVox, a.v_.data());
      u32 n = 0;
      for (u8 x : a.v_) n += x != 0;
      c.layer_count[size_t(L)] = static_cast<u16>(n);
      if (n == 0) {
        release_buffer(std::move(a.v_));
        a.v_ = {};
      }
    }
    a.own_ = true;
  }
  if (a.one_) {  // (one value over the chunk: an array now)
    a.v_ = acquire_buffer(a.one_);
    a.one_ = 0;
  }
  if (a.v_.empty()) {
    if (v == 0) return false;
    a.v_ = acquire_buffer(0);
  }
  u8& slot = a.v_[size_t(chunk_index(p))];
  if (slot == v) return false;
  if (slot == 0) ++c.layer_count[size_t(L)];
  if (v == 0) --c.layer_count[size_t(L)];
  slot = v;
  if (c.layer_count[size_t(L)] == 0) {
    release_buffer(std::move(a.v_));
    a.v_ = {};
  }
  std::unordered_set<u64>& d = layer_dirty_[size_t(L)];
  d.insert(key);
  if (d.size() > std::max<size_t>(65536, 4 * chunks_.size())) d.clear();  // (nobody takes them)
  if (layers_[size_t(L)].persistent) note_modified(cc);
  return true;
}

std::vector<u64> VoxelGrid::take_layer_dirty(int L) {
  std::vector<u64> out;
  if (L < 0 || L >= kMaxLayers) return out;
  out.assign(layer_dirty_[size_t(L)].begin(), layer_dirty_[size_t(L)].end());
  std::unordered_set<u64>().swap(layer_dirty_[size_t(L)]);
  std::sort(out.begin(), out.end());
  return out;
}

i64 VoxelGrid::solid_count() const {
  i64 s = 0;
  for (const auto& [k, c] : chunks_) s += c.uniform ? (vox_solid(c.value) ? kChunkVox : 0) : c.solid;
  return s;
}

i64 VoxelGrid::memory_bytes(Bytes kind) const {
  i64 b = 0;
  for (const auto& [k, c] : chunks_) b += c.memory_bytes(kind);
  return b;
}

i64 VoxelGrid::bookkeeping_bytes() const {
  return static_cast<i64>(modified_.size() * (sizeof(u64) + 4 + 2 * sizeof(void*)) + modified_.bucket_count() * sizeof(void*) +
                          modified_list_.capacity() * sizeof(u64) + spare_.size() * kChunkVox);
}

void VoxelGrid::fill_column(i32 x, i32 y, i32 z0, i32 z1, Vox v) {
  for (i32 z = z0; z < z1;) {
    const IVec3 p{x, y, z};
    const IVec3 cc = chunk_of(p);
    Chunk& c = chunk_mut(cc);
    const i32 zend = std::min(z1, (cc[2] + 1) * kChunk);
    if (c.uniform && c.value != v) {
      c.v.assign(kChunkVox, c.value);
      c.solid = vox_solid(c.value) ? kChunkVox : 0;
      c.free = vox_free(c.value) ? kChunkVox : 0;
      c.uniform = false;
    }
    if (!c.uniform) {
      for (i32 zz = z; zz < zend; ++zz) {
        Vox& slot = c.v[chunk_index({x, y, zz})];
        c.solid += (vox_solid(v) ? 1 : 0) - (vox_solid(slot) ? 1 : 0);
        c.free += (vox_free(v) ? 1 : 0) - (vox_free(slot) ? 1 : 0);
        slot = v;
      }
            c.vox_version = ++vox_seq_;
    }
    z = zend;
  }
}

void VoxelGrid::compact() {
  // mixed chunks whose voxels are all equal (and carry no broken bonds) become uniform
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    Chunk& c = it->second;
    if (!c.uniform && !c.any_broken() && c.strength.empty() && c.jbroken.empty()) {
      const Vox v0 = c.v[0];
      bool same = true;
      for (Vox v : c.v)
        if (v != v0) {
          same = false;
          break;
        }
      if (same) {
        c.uniform = true;
        c.value = v0;
        release_buffer(std::move(c.v));
        c.v = {};
        c.solid = vox_solid(v0) ? kChunkVox : 0;
        c.free = vox_free(v0) ? kChunkVox : 0;
      }
    }
    if (c.uniform && c.value == kAir && !c.any_broken() && c.strength.empty() && c.jbroken.empty() && !c.has_layers()) it = chunks_.erase(it);
    else ++it;
  }
}

void VoxelGrid::mark_all_dirty() {
  for (const auto& [k, c] : chunks_) touch_dirty(k);
}

namespace {

// little-endian writer / reader
struct Out {
  std::vector<u8> b;
  void u8_(u8 v) { b.push_back(v); }
  void u32_(u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void u64_(u64 v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void f32_(f32 v) {
    u32 x;
    std::memcpy(&x, &v, 4);
    u32_(x);
  }
  // run-length bytes: (count u16, value u8) pairs
  void rle(const std::vector<u8>& v) {
    u32 runs = 0;
    const size_t mark = b.size();
    u32_(0);
    for (size_t i = 0; i < v.size();) {
      size_t j = i;
      while (j < v.size() && v[j] == v[i] && j - i < 65535) ++j;
      const u32 n = static_cast<u32>(j - i);
      b.push_back(static_cast<u8>(n & 0xFF));
      b.push_back(static_cast<u8>(n >> 8));
      b.push_back(v[i]);
      ++runs;
      i = j;
    }
    for (int i = 0; i < 4; ++i) b[mark + i] = static_cast<u8>(runs >> (8 * i));
  }
};

struct In {
  const std::vector<u8>& b;
  size_t p = 0;
  bool ok = true;
  bool need(size_t n) {
    if (n > b.size() - p) ok = false;  // (p <= size always; no overflow on 32-bit size_t)
    return ok;
  }
  u8 u8_() { return need(1) ? b[p++] : 0; }
  u32 u32_() {
    if (!need(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(b[p++]) << (8 * i);
    return v;
  }
  u64 u64_() {
    if (!need(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64(b[p++]) << (8 * i);
    return v;
  }
  f32 f32_() {
    const u32 x = u32_();
    f32 v;
    std::memcpy(&v, &x, 4);
    return v;
  }
  // allow_empty: no runs at all is valid (v left empty)
  bool rle(std::vector<u8>& v, size_t n, bool allow_empty = false) {
    v.assign(n, 0);
    const u32 runs = u32_();
    if (runs == 0 && allow_empty && ok) {
      v.clear();
      return true;
    }
    size_t o = 0;
    for (u32 r = 0; r < runs && ok; ++r) {
      if (!need(3)) return false;
      const u32 cnt = u32(b[p]) | (u32(b[p + 1]) << 8);
      const u8 val = b[p + 2];
      p += 3;
      if (o + cnt > n) return (ok = false);
      std::fill(v.begin() + static_cast<long>(o), v.begin() + static_cast<long>(o + cnt), val);
      o += cnt;
    }
    return ok && o == n;
  }
};

// A v1 record (voxels, broken bonds, damage and offset lists) as a v2 record (no strength
// classes: the chunk keeps the ones it has).
bool upgrade_v1(std::vector<u8>* rec) {
  In in{*rec};
  const u64 key = in.u64_();
  std::vector<u8> vox, brk;
  if (!in.rle(vox, kChunkVox) || !in.rle(brk, kChunkVox)) return false;
  for (int list = 0; list < 2; ++list) {
    const u32 n = in.u32_();
    if (!in.ok || u64(n) * 16 > rec->size()) return false;
    for (u32 i = 0; i < n && in.ok; ++i) {
      if (in.u32_() >= u32(kChunkVox)) return false;
      in.f32_();
      in.f32_();
      in.f32_();
    }
  }
  if (!in.ok || in.p != rec->size()) return false;
  Out o;
  o.u64_(key);
  o.rle(vox);
  o.rle(brk);
  o.u32_(0);  // (strength: no runs = keep the chunk's)
  o.u8_(0);   // (no layers)
  *rec = std::move(o.b);
  return true;
}

// A v2 record (voxels, broken bonds, strength classes) as a v3 record (no layers).
void upgrade_v2(std::vector<u8>* rec) { rec->push_back(0); }
// A v3 record as a v4 record (no broken junctions).
void upgrade_v3(std::vector<u8>* rec) { rec->insert(rec->end(), 4, u8(0)); }

constexpr u32 kDeltaMagic = 0x44585653;  // "SVXD"
// v1: per chunk voxels, broken bonds, (unused) damage and offset lists; v2: voxels, broken
// bonds, strength classes; v3: and the persistent layers by name; v4: and the broken junction
// samples (bonds to other grids). Records are written as v4; v1 to v3 deltas are read.
constexpr u32 kDeltaVersion = 4;

}  // namespace

std::vector<u8> VoxelGrid::chunk_record(u64 k) const {
  Out o;
  o.u64_(k);
  const IVec3 cc = unkey3(k);
  const Chunk* c = chunk(cc);
  std::vector<u8> vox(kChunkVox, kAir), brk(kChunkVox, 0), str(kChunkVox, 0);
  if (c) {
    if (c->uniform) std::fill(vox.begin(), vox.end(), c->value);
    else vox = c->v;
    if (c->any_broken()) c->broken_dense(brk);
    if (!c->strength.empty()) str = c->strength;
  }
  o.rle(vox);
  o.rle(brk);
  o.rle(str);
  // persistent layers with values here, by name
  std::vector<int> ls;
  for (size_t L = 0; L < layers_.size(); ++L)
    if (layers_[L].persistent && c && (!c->layer[L].empty() || c->layer[L].own_)) ls.push_back(static_cast<int>(L));
  o.u8_(static_cast<u8>(ls.size()));
  for (int L : ls) {
    const std::string& name = layers_[size_t(L)].name;
    o.u8_(static_cast<u8>(name.size()));
    for (char ch : name) o.u8_(static_cast<u8>(ch));
    // (a regenerable layer's own values: zeros too, over its base)
    const LayerValues& lv = c->layer[size_t(L)];
    std::vector<u8> vals(kChunkVox, 0);
    if (!lv.empty()) std::copy(lv.begin(), lv.end(), vals.begin());
    o.rle(vals);
  }
  // broken junction samples
  const u32 nj = c ? static_cast<u32>(c->jbroken.size()) : 0u;
  o.u32_(nj);
  for (u32 k = 0; k < nj; ++k) o.u32_(c->jbroken[k]);
  return std::move(o.b);
}

std::vector<u8> VoxelGrid::pack_delta(const std::vector<std::vector<u8>>& records) {
  Out o;
  o.u32_(kDeltaMagic);
  o.u32_(kDeltaVersion);
  o.u32_(static_cast<u32>(records.size()));
  for (const auto& r : records) {
    o.u32_(static_cast<u32>(r.size()));
    o.b.insert(o.b.end(), r.begin(), r.end());
  }
  return std::move(o.b);
}

bool VoxelGrid::unpack_delta(const std::vector<u8>& bytes, std::vector<std::pair<u64, std::vector<u8>>>* records) {
  In in{bytes};
  if (in.u32_() != kDeltaMagic) return false;
  const u32 version = in.u32_();
  if (version < 1 || version > kDeltaVersion) return false;
  const u32 n = in.u32_();
  std::vector<std::pair<u64, std::vector<u8>>> out;
  for (u32 k = 0; k < n; ++k) {
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz) || sz < 8) return false;
    std::vector<u8> rec(bytes.begin() + static_cast<long>(in.p), bytes.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    u64 key = 0;
    for (int i = 0; i < 8; ++i) key |= u64(rec[i]) << (8 * i);
    if (version == 1 && !upgrade_v1(&rec)) return false;
    if (version == 2) upgrade_v2(&rec);
    if (version <= 3) upgrade_v3(&rec);
    out.emplace_back(key, std::move(rec));
  }
  if (!in.ok) return false;
  *records = std::move(out);
  return true;
}

std::vector<u8> VoxelGrid::save_delta() const {
  std::vector<u64> keys = modified_list_;
  std::sort(keys.begin(), keys.end());
  std::vector<std::vector<u8>> recs;
  for (u64 k : keys) recs.push_back(chunk_record(k));
  return pack_delta(recs);
}

namespace {

struct ChunkDelta {
  u64 key = 0;
  std::vector<u8> vox, brk, str;
  std::vector<std::pair<std::string, std::vector<u8>>> layers;
  std::vector<u32> jbroken;
};

bool parse_record(const std::vector<u8>& rec, ChunkDelta* cd) {
  In in{rec};
  cd->key = in.u64_();
  // a canonical chunk key (21 bits per axis), of a chunk within +-2^19 chunks of the origin
  const IVec3 cc = unkey3(cd->key);
  if (!in.ok || key3(cc[0], cc[1], cc[2]) != cd->key) return false;
  for (int a = 0; a < 3; ++a)
    if (cc[a] < -(1 << 19) || cc[a] >= (1 << 19)) return false;
  if (!in.rle(cd->vox, kChunkVox) || !in.rle(cd->brk, kChunkVox) || !in.rle(cd->str, kChunkVox, true)) return false;
  const u32 nl = in.u8_();
  for (u32 k = 0; k < nl && in.ok; ++k) {
    const u32 len = in.u8_();
    if (!in.need(len)) return false;
    std::string name(rec.begin() + static_cast<long>(in.p), rec.begin() + static_cast<long>(in.p + len));
    in.p += len;
    std::vector<u8> data;
    if (!in.rle(data, kChunkVox)) return false;
    cd->layers.emplace_back(std::move(name), std::move(data));
  }
  // broken junction samples: sorted, of voxels of the chunk, faces 0..5
  const u32 nj = in.u32_();
  if (!in.ok || u64(nj) * 4 > rec.size() - in.p) return false;
  cd->jbroken.resize(nj);
  for (u32 k = 0; k < nj && in.ok; ++k) {
    const u32 c = in.u32_();
    if ((c >> 9) >= u32(kChunkVox) || ((c >> 6) & 7) >= 6 || (k > 0 && c <= cd->jbroken[k - 1])) return false;
    cd->jbroken[k] = c;
  }
  if (!in.ok || in.p != rec.size()) return false;
  // valid voxel values only (air, or a material; not "anchored air")
  for (u8 v : cd->vox)
    if (!vox_valid(v)) return false;
  return true;
}

}  // namespace

bool VoxelGrid::check_record(const std::vector<u8>& rec) {
  ChunkDelta cd;
  return parse_record(rec, &cd);
}

bool VoxelGrid::apply_record(const std::vector<u8>& rec, u64* key_out) {
  ChunkDelta cd;
  if (!parse_record(rec, &cd)) return false;
  const IVec3 cc = unkey3(cd.key);
  Chunk& c = chunk_mut(cc);
  c.uniform = false;
  release_buffer(std::move(c.v));  // (the old array goes back to the pool)
  c.v = std::move(cd.vox);
  c.solid = 0;
  c.free = 0;
  for (Vox v : c.v) {
    c.solid += vox_solid(v) ? 1 : 0;
    c.free += vox_free(v) ? 1 : 0;
  }
  size_t nb = 0;
  for (u8& x : cd.brk) {
    x &= 7;  // (a face per axis)
    nb += x != 0;
  }
  release_buffer(std::move(c.broken));
  c.broken = {};
  std::vector<u32>().swap(c.broken_few);
  if (nb > kBrokenListMax) {
    c.broken = std::move(cd.brk);
  } else if (nb > 0) {
    c.broken_few.reserve(nb);
    for (i32 i = 0; i < kChunkVox; ++i)
      if (cd.brk[size_t(i)]) c.broken_few.push_back((static_cast<u32>(i) << 3) | cd.brk[size_t(i)]);
  }
  if (!cd.str.empty()) {  // (empty: a v1 record, the chunk keeps its classes)
    bool anys = false;
    for (u8 x : cd.str) anys = anys || x != 0;
    release_buffer(std::move(c.strength));
    if (anys) c.strength = std::move(cd.str);
    else c.strength = {};
  }
  c.jbroken = std::move(cd.jbroken);
  // persistent layers: the record's (those it does not hold are zero); transient ones stay
  for (size_t L = 0; L < layers_.size(); ++L) {
    if (!layers_[L].persistent) continue;
    std::vector<u8>* data = nullptr;
    for (auto& [name, d] : cd.layers)
      if (name == layers_[L].name) data = &d;
    // (a layer cleared by the record is a change too)
    LayerValues& a = c.layer[L];
    if (c.layer_count[L] || a.own_) layer_dirty_[L].insert(cd.key);
    release_buffer(std::move(a.v_));
    a.v_ = {};
    a.one_ = 0;
    a.own_ = false;
    c.layer_count[L] = 0;
    if (!data) continue;
    // (a regenerable layer the record holds: the chunk's own values, over its base)
    a.own_ = layers_[L].regenerable;
    if (a.own_) layer_dirty_[L].insert(cd.key);
    u32 n = 0;
    bool same = true;
    for (u8 x : *data) {
      n += x != 0;
      same = same && x == (*data)[0];
    }
    if (n == 0) continue;
    if (n == static_cast<u32>(kChunkVox) && same) a.one_ = (*data)[0];
    else a.v_ = std::move(*data);
    c.layer_count[L] = static_cast<u16>(std::min<u32>(n, 0xFFFF));
    layer_dirty_[L].insert(cd.key);
  }
  if (c.solid == 0) compact_.push_back(cd.key);  // (an emptied chunk restored: compacted after the tick)
  c.vox_version = ++vox_seq_;
  note_modified(cc);
  touch_dirty(cd.key);
  for (int d = 0; d < 3; ++d) {  // neighbour faces may change
    IVec3 q = cc;
    q[d] -= 1;
    touch_dirty(key3(q[0], q[1], q[2]));
    q[d] += 2;
    touch_dirty(key3(q[0], q[1], q[2]));
  }
  if (key_out) *key_out = cd.key;
  return true;
}

bool VoxelGrid::load_delta(const std::vector<u8>& bytes, std::vector<u64>* touched) {
  std::vector<std::pair<u64, std::vector<u8>>> recs;
  if (!unpack_delta(bytes, &recs)) return false;
  // validate every record before changing anything
  for (const auto& [k, r] : recs) {
    ChunkDelta cd;
    if (!parse_record(r, &cd)) return false;
  }
  for (const auto& [k, r] : recs) {
    u64 key = 0;
    if (!apply_record(r, &key)) return false;  // (validated above: never)
    if (touched) touched->push_back(key);
  }
  return true;
}

void VoxelGrid::insert_chunk(const IVec3& cc, std::vector<Vox>&& voxels) {
  Chunk& c = chunk_mut(cc);
  release_buffer(std::move(c.v));
  release_buffer(std::move(c.broken));
  release_buffer(std::move(c.strength));
  for (size_t L = 0; L < kMaxLayers; ++L) {
    release_buffer(std::move(c.layer[L].v_));
    c.layer[L] = {};
    c.layer_count[L] = 0;
  }
  c.v = std::move(voxels);
  c.broken = {};
  std::vector<u32>().swap(c.broken_few);
  c.strength = {};
  std::vector<u32>().swap(c.jbroken);
  c.uniform = false;
  c.solid = 0;
  c.free = 0;
  for (Vox v : c.v) {
    c.solid += vox_solid(v) ? 1 : 0;
    c.free += vox_free(v) ? 1 : 0;
  }
  const Vox v0 = c.v.empty() ? kAir : c.v[0];
  bool same = true;
  for (Vox v : c.v)
    if (v != v0) {
      same = false;
      break;
    }
  if (same) {
    c.uniform = true;
    c.value = v0;
    release_buffer(std::move(c.v));
    c.v = {};
  }
  c.vox_version = ++vox_seq_;
}

void VoxelGrid::remove_chunk(const IVec3& cc) {
  const u64 k = key3(cc[0], cc[1], cc[2]);
  const auto it = chunks_.find(k);
  if (it != chunks_.end()) {
    release_buffer(std::move(it->second.v));
    release_buffer(std::move(it->second.broken));
    release_buffer(std::move(it->second.strength));
    for (auto& l : it->second.layer) release_buffer(std::move(l.v_));
    chunks_.erase(it);
  }
  dirty_.erase(k);  // (gone: the host hears of it as evicted)
  for (auto& d : layer_dirty_) d.erase(k);
  forget_modified(k);
}

}  // namespace svx
