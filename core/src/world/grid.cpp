#include "svx/world/grid.hpp"

#include <algorithm>
#include <cstring>

namespace svx {

namespace {

inline u64 brick_key(const IVec3& p) { return key3(p[0] >> kBrickBits, p[1] >> kBrickBits, p[2] >> kBrickBits); }
inline int brick_index(const IVec3& p) {
  return ((p[0] & (kBrick - 1)) * kBrick + (p[1] & (kBrick - 1))) * kBrick + (p[2] & (kBrick - 1));
}

}  // namespace

const Chunk* VoxelGrid::chunk(const IVec3& c) const {
  const auto it = chunks_.find(key3(c[0], c[1], c[2]));
  return it == chunks_.end() ? nullptr : &it->second;
}

Chunk& VoxelGrid::chunk_mut(const IVec3& c) { return chunks_[key3(c[0], c[1], c[2])]; }

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
    c.v.assign(kChunkVox, c.value);
    c.solid = vox_solid(c.value) ? kChunkVox : 0;
    c.free = vox_free(c.value) ? kChunkVox : 0;
    c.uniform = false;
  }
  Vox& slot = c.v[chunk_index(p)];
  if (slot == v) return;
  c.solid += (vox_solid(v) ? 1 : 0) - (vox_solid(slot) ? 1 : 0);
  c.free += (vox_free(v) ? 1 : 0) - (vox_free(slot) ? 1 : 0);
  slot = v;
  ++c.version;
  note_modified(cc);
  dirty_.push_back(key3(cc[0], cc[1], cc[2]));
  // a changed voxel also changes the faces of its neighbours in adjacent chunks
  for (int a = 0; a < 3; ++a) {
    IVec3 q = p;
    if ((p[a] & (kChunk - 1)) == 0) {
      q[a] -= 1;
      const IVec3 nc = chunk_of(q);
      dirty_.push_back(key3(nc[0], nc[1], nc[2]));
    } else if ((p[a] & (kChunk - 1)) == kChunk - 1) {
      q[a] += 1;
      const IVec3 nc = chunk_of(q);
      dirty_.push_back(key3(nc[0], nc[1], nc[2]));
    }
  }
}

bool VoxelGrid::broken(const IVec3& p, int axis) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || c->broken.empty()) return false;
  return (c->broken[chunk_index(p)] >> axis) & 1;
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

bool VoxelGrid::cracked(const IVec3& p, int axis) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || c->broken.empty()) return false;
  return (c->broken[chunk_index(p)] >> (3 + axis)) & 1;
}

void VoxelGrid::crack_bond(const IVec3& p, int axis) {
  Chunk& c = chunk_mut(chunk_of(p));
  if (c.broken.empty()) c.broken.assign(kChunkVox, 0);
  c.broken[chunk_index(p)] |= static_cast<u8>(1u << (3 + axis));
  ++c.version;
  const IVec3 cc = chunk_of(p);
  note_modified(cc);
}

void VoxelGrid::break_bond(const IVec3& p, int axis) {
  Chunk& c = chunk_mut(chunk_of(p));
  if (c.broken.empty()) c.broken.assign(kChunkVox, 0);
  u8& b = c.broken[chunk_index(p)];
  b = static_cast<u8>((b | (1u << axis)) & ~(1u << (3 + axis)));
  ++c.version;
  const IVec3 cc = chunk_of(p);
  note_modified(cc);
  dirty_.push_back(key3(cc[0], cc[1], cc[2]));
}

u8 VoxelGrid::strength(const IVec3& p) const {
  const Chunk* c = chunk(chunk_of(p));
  if (!c || c->strength.empty()) return 0;
  return c->strength[chunk_index(p)];
}

void VoxelGrid::set_strength(const IVec3& p, u8 cls) {
  Chunk& c = chunk_mut(chunk_of(p));
  if (c.strength.empty()) {
    if (cls == 0) return;
    c.strength.assign(kChunkVox, 0);
  }
  c.strength[chunk_index(p)] = cls;
}

f32 VoxelGrid::damage(const IVec3& p, int axis) const {
  const auto& m = damage_[axis];
  const auto it = m.find(key3(p[0], p[1], p[2]));
  return it == m.end() ? 0.0f : it->second;
}

void VoxelGrid::set_damage(const IVec3& p, int axis, f32 d) {
  const u64 k = key3(p[0], p[1], p[2]);
  auto& m = damage_[axis];
  const auto it = m.find(k);
  if (d <= 0.0f) {
    if (it == m.end()) return;  // unchanged: not a modification (persistence deltas stay small)
    m.erase(it);
  } else {
    if (it != m.end() && it->second == d) return;
    m[k] = d;
  }
  touch(chunk_of(p));
  note_modified(chunk_of(p));
}

void VoxelGrid::touch(const IVec3& cc) {
  const auto it = chunks_.find(key3(cc[0], cc[1], cc[2]));
  if (it != chunks_.end()) ++it->second.version;
}

bool VoxelGrid::baseline(const IVec3& p, f32 out[6]) const {
  const auto it = baseline_.find(brick_key(p));
  if (it == baseline_.end()) return false;
  const int i = brick_index(p);
  const BaselineBrick& b = *it->second;
  if (!b.valid[i]) return false;
  for (int q = 0; q < 6; ++q) out[q] = b.u[6 * size_t(i) + q];
  return true;
}

void VoxelGrid::set_baseline(const IVec3& p, const f32 u[6]) {
  std::shared_ptr<BaselineBrick>& sp = baseline_[brick_key(p)];
  if (!sp) {
    sp = std::make_shared<BaselineBrick>();
    sp->u.assign(6 * kBrick * kBrick * kBrick, 0.0f);
    sp->valid.assign(kBrick * kBrick * kBrick, 0);
  } else if (sp.use_count() > 1) {
    sp = std::make_shared<BaselineBrick>(*sp);  // a snapshot still reads the old copy
  }
  const int i = brick_index(p);
  for (int q = 0; q < 6; ++q) sp->u[6 * size_t(i) + q] = u[q];
  sp->valid[i] = 1;
}

void VoxelGrid::clear_baseline(const IVec3& p) {
  const auto it = baseline_.find(brick_key(p));
  if (it == baseline_.end()) return;
  const int i = brick_index(p);
  if (!it->second->valid[i]) return;
  if (it->second.use_count() > 1) it->second = std::make_shared<BaselineBrick>(*it->second);
  it->second->valid[i] = 0;
}

VoxelGrid VoxelGrid::snapshot(std::span<const u64> keys) const {
  VoxelGrid s;
  s.h = h;
  s.lo = lo;
  s.hi = hi;
  s.partial_ = true;
  s.chunks_.reserve(keys.size());
  s.known_.insert(keys.begin(), keys.end());
  for (u64 k : keys) {
    const auto it = chunks_.find(k);
    if (it == chunks_.end()) continue;
    s.chunks_.emplace(k, it->second);
    const IVec3 cc = unkey3(k);
    const IVec3 b{cc[0] << (kChunkBits - kBrickBits), cc[1] << (kChunkBits - kBrickBits), cc[2] << (kChunkBits - kBrickBits)};
    for (int bx = 0; bx < kChunk / kBrick; ++bx)
      for (int by = 0; by < kChunk / kBrick; ++by)
        for (int bz = 0; bz < kChunk / kBrick; ++bz) {
          const u64 bk = key3(b[0] + bx, b[1] + by, b[2] + bz);
          const auto bt = baseline_.find(bk);
          if (bt != baseline_.end()) s.baseline_.emplace(bk, bt->second);
        }
  }
  for (int a = 0; a < 3; ++a)
    for (const auto& [vk, d] : damage_[a]) {
      const IVec3 p = unkey3(vk);
      const IVec3 cc = chunk_of(p);
      if (s.chunks_.count(key3(cc[0], cc[1], cc[2]))) s.damage_[a].emplace(vk, d);
    }
  return s;
}

bool VoxelGrid::offset(const IVec3& p, f32 out[3]) const {
  const auto it = offset_.find(key3(p[0], p[1], p[2]));
  if (it == offset_.end()) return false;
  for (int q = 0; q < 3; ++q) out[q] = it->second[q];
  return true;
}

void VoxelGrid::set_offset(const IVec3& p, const f32 d[3]) {
  const u64 k = key3(p[0], p[1], p[2]);
  if (offset_.count(k) || d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) note_modified(chunk_of(p));
  if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) {
    offset_.erase(k);
  } else {
    offset_[k] = {d[0], d[1], d[2]};
  }
  mark_dirty(chunk_of(p));
}

void VoxelGrid::mark_dirty(const IVec3& c) { dirty_.push_back(key3(c[0], c[1], c[2])); }

std::vector<u64> VoxelGrid::take_dirty() {
  std::vector<u64> out;
  out.swap(dirty_);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

u64 VoxelGrid::overlay_hash() const {
  u64 h = 1469598103934665603ull;
  auto mix = [&](u64 v) { h = (h ^ v) * 1099511628211ull; };
  for (int a = 0; a < 3; ++a) {
    std::vector<std::pair<u64, f32>> d(damage_[a].begin(), damage_[a].end());
    std::sort(d.begin(), d.end());
    for (const auto& [k, v] : d) {
      u32 bits;
      std::memcpy(&bits, &v, 4);
      mix(k * 3 + u64(a));
      mix(bits);
    }
  }
  std::vector<std::pair<u64, std::array<f32, 3>>> o(offset_.begin(), offset_.end());
  std::sort(o.begin(), o.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  for (const auto& [k, v] : o) {
    mix(k);
    for (f32 x : v) {
      u32 bits;
      std::memcpy(&bits, &x, 4);
      mix(bits);
    }
  }
  return h;
}

i64 VoxelGrid::solid_count() const {
  i64 s = 0;
  for (const auto& [k, c] : chunks_) s += c.uniform ? (vox_solid(c.value) ? kChunkVox : 0) : c.solid;
  return s;
}

i64 VoxelGrid::memory_bytes() const {
  i64 b = 0;
  for (const auto& [k, c] : chunks_) b += sizeof(Chunk) + i64(c.v.size()) + i64(c.broken.size()) + i64(c.strength.size());
  for (const auto& m : damage_) b += i64(m.size()) * 24;
  b += i64(baseline_.size()) * (kBrick * kBrick * kBrick * 25 + 64);
  b += i64(offset_.size()) * 32;
  return b;
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
      ++c.version;
    }
    z = zend;
  }
}

void VoxelGrid::compact() {
  // mixed chunks whose voxels are all equal (and carry no broken bonds) become uniform
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    Chunk& c = it->second;
    if (!c.uniform && c.broken.empty() && c.strength.empty()) {
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
        std::vector<Vox>().swap(c.v);
        c.solid = vox_solid(v0) ? kChunkVox : 0;
        c.free = vox_free(v0) ? kChunkVox : 0;
      }
    }
    if (c.uniform && c.value == kAir && c.broken.empty() && c.strength.empty()) it = chunks_.erase(it);
    else ++it;
  }
}

void VoxelGrid::mark_all_dirty() {
  for (const auto& [k, c] : chunks_) dirty_.push_back(k);
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
    if (p + n > b.size()) ok = false;
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
  bool rle(std::vector<u8>& v, size_t n) {
    v.assign(n, 0);
    const u32 runs = u32_();
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

constexpr u32 kDeltaMagic = 0x44585653;  // "SVXD"
constexpr u32 kDeltaVersion = 1;

}  // namespace

std::vector<u8> VoxelGrid::chunk_record(u64 k) const {
  Out o;
  o.u64_(k);
  const IVec3 cc = unkey3(k);
  const Chunk* c = chunk(cc);
  std::vector<u8> vox(kChunkVox, kAir), brk(kChunkVox, 0);
  if (c) {
    if (c->uniform) std::fill(vox.begin(), vox.end(), c->value);
    else vox = c->v;
    if (!c->broken.empty()) brk = c->broken;
  }
  o.rle(vox);
  o.rle(brk);
  // damage and render offsets of the chunk's voxels
  const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  std::vector<std::pair<u32, std::array<f32, 3>>> dmg, off;
  for (int i = 0; i < kChunkVox; ++i) {
    const IVec3 p{b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
    const u64 pk = key3(p[0], p[1], p[2]);
    std::array<f32, 3> d{0, 0, 0};
    bool any = false;
    for (int a = 0; a < 3; ++a) {
      const auto it = damage_[a].find(pk);
      if (it != damage_[a].end()) {
        d[a] = it->second;
        any = true;
      }
    }
    if (any) dmg.push_back({static_cast<u32>(i), d});
    const auto it = offset_.find(pk);
    if (it != offset_.end()) off.push_back({static_cast<u32>(i), it->second});
  }
  o.u32_(static_cast<u32>(dmg.size()));
  for (const auto& [i, d] : dmg) {
    o.u32_(i);
    for (f32 v : d) o.f32_(v);
  }
  o.u32_(static_cast<u32>(off.size()));
  for (const auto& [i, d] : off) {
    o.u32_(i);
    for (f32 v : d) o.f32_(v);
  }
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
  if (in.u32_() != kDeltaMagic || in.u32_() != kDeltaVersion) return false;
  const u32 n = in.u32_();
  std::vector<std::pair<u64, std::vector<u8>>> out;
  for (u32 k = 0; k < n; ++k) {
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz) || sz < 8) return false;
    std::vector<u8> rec(bytes.begin() + static_cast<long>(in.p), bytes.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    u64 key = 0;
    for (int i = 0; i < 8; ++i) key |= u64(rec[i]) << (8 * i);
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
  std::vector<u8> vox, brk;
  std::vector<std::pair<u32, std::array<f32, 3>>> dmg, off;
};

bool parse_record(const std::vector<u8>& rec, ChunkDelta* cd) {
  In in{rec};
  cd->key = in.u64_();
  if (!in.rle(cd->vox, kChunkVox) || !in.rle(cd->brk, kChunkVox)) return false;
  const u32 nd = in.u32_();
  for (u32 i = 0; i < nd && in.ok; ++i) {
    const u32 idx = in.u32_();
    std::array<f32, 3> d{in.f32_(), in.f32_(), in.f32_()};
    if (idx >= u32(kChunkVox)) return false;
    cd->dmg.push_back({idx, d});
  }
  const u32 no = in.u32_();
  for (u32 i = 0; i < no && in.ok; ++i) {
    const u32 idx = in.u32_();
    std::array<f32, 3> d{in.f32_(), in.f32_(), in.f32_()};
    if (idx >= u32(kChunkVox)) return false;
    cd->off.push_back({idx, d});
  }
  return in.ok && in.p == rec.size();
}

}  // namespace

bool VoxelGrid::apply_record(const std::vector<u8>& rec, u64* key_out) {
  ChunkDelta cd;
  if (!parse_record(rec, &cd)) return false;
  const IVec3 cc = unkey3(cd.key);
  Chunk& c = chunk_mut(cc);
  c.uniform = false;
  c.v = std::move(cd.vox);
  c.solid = 0;
  c.free = 0;
  for (Vox v : c.v) {
    c.solid += vox_solid(v) ? 1 : 0;
    c.free += vox_free(v) ? 1 : 0;
  }
  bool anyb = false;
  for (u8 x : cd.brk) anyb = anyb || x != 0;
  if (anyb) c.broken = std::move(cd.brk);
  else std::vector<u8>().swap(c.broken);
  ++c.version;
  const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  for (int i = 0; i < kChunkVox; ++i) {
    const IVec3 p{b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
    const u64 pk = key3(p[0], p[1], p[2]);
    for (int a = 0; a < 3; ++a) damage_[a].erase(pk);
    offset_.erase(pk);
    clear_baseline(p);
  }
  for (const auto& [i, d] : cd.dmg) {
    const IVec3 p{b[0] + int(i) / (kChunk * kChunk), b[1] + (int(i) / kChunk) % kChunk, b[2] + int(i) % kChunk};
    for (int a = 0; a < 3; ++a)
      if (d[a] > 0.0f) damage_[a][key3(p[0], p[1], p[2])] = d[a];
  }
  for (const auto& [i, d] : cd.off) {
    const IVec3 p{b[0] + int(i) / (kChunk * kChunk), b[1] + (int(i) / kChunk) % kChunk, b[2] + int(i) % kChunk};
    offset_[key3(p[0], p[1], p[2])] = d;
  }
  note_modified(cc);
  dirty_.push_back(cd.key);
  for (int d = 0; d < 3; ++d) {  // neighbour faces may change
    IVec3 q = cc;
    q[d] -= 1;
    dirty_.push_back(key3(q[0], q[1], q[2]));
    q[d] += 2;
    dirty_.push_back(key3(q[0], q[1], q[2]));
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
    apply_record(r, &key);
    if (touched) touched->push_back(key);
  }
  return true;
}

void VoxelGrid::insert_chunk(const IVec3& cc, std::vector<Vox>&& voxels) {
  Chunk& c = chunk_mut(cc);
  c.v = std::move(voxels);
  c.uniform = false;
  c.broken.clear();
  c.strength.clear();
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
    std::vector<Vox>().swap(c.v);
  }
  ++c.version;
}

void VoxelGrid::remove_chunk(const IVec3& cc) {
  const u64 k = key3(cc[0], cc[1], cc[2]);
  const IVec3 b{cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
  const bool overlays = !offset_.empty() || !damage_[0].empty() || !damage_[1].empty() || !damage_[2].empty();
  for (int i = 0; overlays && i < kChunkVox; ++i) {
    const IVec3 p{b[0] + i / (kChunk * kChunk), b[1] + (i / kChunk) % kChunk, b[2] + i % kChunk};
    const u64 pk = key3(p[0], p[1], p[2]);
    for (int a = 0; a < 3; ++a) damage_[a].erase(pk);
    offset_.erase(pk);
  }
  // baselines are stored per 8^3 brick: drop the chunk's 64 bricks
  for (int bx = 0; bx < kChunk / kBrick; ++bx)
    for (int by = 0; by < kChunk / kBrick; ++by)
      for (int bz = 0; bz < kChunk / kBrick; ++bz)
        baseline_.erase(key3((b[0] >> kBrickBits) + bx, (b[1] >> kBrickBits) + by, (b[2] >> kBrickBits) + bz));
  chunks_.erase(k);
  if (modified_.erase(k)) modified_list_.erase(std::remove(modified_list_.begin(), modified_list_.end(), k), modified_list_.end());
}

}  // namespace svx
