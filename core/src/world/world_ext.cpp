// structvox — the world's extension points: layers (and the damage layer's strengths), loads on
// the static world, forces on pieces, systems (docs/CORE.md §5).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using namespace world_detail;

// ---------------------------------------------------------------------------------------------
// Layers

int World::add_layer(const LayerSpec& spec) {
  const int i = grid_.add_layer(spec);
  if (i >= 0) layer_specs_ = grid_.layers();
  return i;
}

i32 World::set_layer(int L, const std::vector<LayerEdit>& edits) {
  if (L < 0 || L >= static_cast<int>(layer_specs_.size())) return 0;
  i32 changed = 0;
  std::vector<IVec3> damaged;
  for (const LayerEdit& e : edits) {
    if (!in_voxel_range(e.p)) continue;
    // (a chunk of a streamed world not generated yet has no voxels to hold values: a chunk
    // made for them would stand in for the generated one)
    if (source_ && !chunk_resident(chunk_of(e.p))) continue;
    if (!grid_.set_layer(L, e.p, e.v)) continue;
    ++changed;
    if (L == kDamageLayer) damaged.push_back(e.p);
  }
  if (!damaged.empty()) {
    refresh_strengths(damaged);
    // structures nobody registered are extracted and judged: their sections are weaker (a seed
    // per fragment)
    std::unordered_set<u64> seeded;
    for (const IVec3& p : damaged) {
      if (!vox_free(grid_.get(p))) continue;
      FragKey f;
      if (!frag_at(p, &f) || owner_of(f) != 0) continue;
      if (seeded.insert(f.chunk * 4099u + static_cast<u64>(f.idx)).second) seeds_.push_back(p);
    }
  }
  return changed;
}

u8 World::piece_layer(i64 id, int L, const IVec3& p) const {
  const Body* b = piece(id);
  if (!b || L < 0 || L >= kMaxLayers) return 0;
  return b->shape.layer_at(L, b->shape.index(p));
}

i32 World::set_piece_layer(i64 id, int L, const std::vector<LayerEdit>& edits) {
  Body* b = rigid_.find(id);
  if (!b || L < 0 || L >= static_cast<int>(layer_specs_.size())) return 0;
  BodyShape& S = b->shape;
  std::vector<u8>& a = S.layer[size_t(L)];
  i32 changed = 0;
  bool zeroed = false;
  for (const LayerEdit& e : edits) {
    const i32 i = S.index(e.p);
    if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
    if (a.empty()) {
      if (e.v == 0) continue;
      a.assign(S.vox.size(), 0);
    }
    if (a[size_t(i)] == e.v) continue;
    zeroed = zeroed || e.v == 0;
    a[size_t(i)] = e.v;
    ++changed;
  }
  // (a layer all zero again goes: looked for only when a value went to zero)
  if (zeroed && std::all_of(a.begin(), a.end(), [](u8 v) { return v == 0; })) std::vector<u8>().swap(a);
  if (changed && L == kDamageLayer) {
    // (its sections are weaker: its bond graph is built again, its stress checked again)
    b->graph_dirty = true;
    b->recheck = true;
    rigid_.wake(*b);
  }
  return changed;
}

bool World::remove_piece_voxels(i64 id, const std::vector<IVec3>& voxels, bool dust) {
  Body* bp = rigid_.find(id);
  if (!bp) return false;
  Body& b = *bp;
  BodyShape& S = b.shape;
  const f64 h = grid_.h;
  V3 at;
  i32 removed = 0;
  for (const IVec3& p : voxels) {
    const i32 i = S.index(p);
    if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
    S.vox[size_t(i)] = kAir;
    S.frag[size_t(i)] = 0;
    S.brk[size_t(i)] = 0;
    for (auto& l : S.layer)
      if (!l.empty()) l[size_t(i)] = 0;
    at += V3{h * p[0], h * p[1], h * p[2]};
    ++removed;
  }
  if (!removed) return false;
  if (dust) {
    const V3 X = b.to_world(at * (1.0 / removed));
    dust_event(X, b.v + cross(b.w, X - b.x), removed, true);
  }
  refragment_body(b);
  rigid_.wake(b);
  if (b.shape.count == 0) {
    remove_bodies({id}, PieceEnd::Split);
    return true;
  }
  b.graph_dirty = true;
  split_body(b, false, true);  // (what is left: new pieces, this one's parts)
  flush_body_changes();
  return true;
}

void World::refresh_strengths(const std::vector<IVec3>& voxels) {
  // The bonds with a face at a voxel whose damage changed are measured again: every face of a
  // free voxel is in a bond of its node, and those of an anchored one in bonds of its free
  // neighbours' nodes (the other bonds' sections did not change).
  std::map<i64, std::vector<i32>> marked;  // structure -> nodes
  auto mark = [&](const IVec3& q) {
    FragKey f;
    if (!frag_at(q, &f)) return;
    const i64 id = owner_of(f);
    const Structure* s = id ? structure(id) : nullptr;
    const i32 i = s ? s->node(f) : -1;
    if (i >= 0) marked[id].push_back(i);
  };
  for (const IVec3& p : voxels) {
    const Vox v = grid_.get(p);
    if (vox_free(v)) {
      mark(p);
    } else if (vox_anchored(v)) {
      for (int d = 0; d < 6; ++d) {
        IVec3 q = p;
        q[d / 2] += (d & 1) ? -1 : 1;
        if (vox_free(grid_.get(q))) mark(q);
      }
    }
  }
  for (const auto& [id, nodes] : marked) {
    Structure* s = structure(id);
    if (!s) continue;
    std::vector<u8> m(s->P.nodes.size(), 0);
    for (i32 i : nodes) m[size_t(i)] = 1;
    for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
      SBond& B = s->P.bonds[size_t(b)];
      if (B.broken || !(m[size_t(B.a)] || (B.b >= 0 && m[size_t(B.b)]))) continue;
      const i32 f0 = s->face_start[size_t(b)], f1 = s->face_start[size_t(b) + 1];
      const f32 ft = B.ft, fb = B.fb, fc = B.fc, coh = B.coh;
      section_strengths(&s->face_p[size_t(f0)], &s->face_axis[size_t(f0)], size_t(f1 - f0), [&](const IVec3& p) { return voxel_at(p); }, B);
      if (B.ft != ft || B.fb != fb || B.fc != fc || B.coh != coh) s->rejudge = true;
    }
  }
}

VoxelAt World::voxel_at(const IVec3& p) const { return {grid_.get(p), grid_.layer(kDamageLayer, p)}; }

VoxelAt World::piece_voxel_at(const Body& b, const IVec3& p) const {
  const i32 i = b.shape.index(p);
  return {i < 0 ? kAir : b.shape.vox[size_t(i)], b.shape.layer_at(kDamageLayer, i)};
}

// ---------------------------------------------------------------------------------------------
// Loads and forces

namespace {

u64 load_hash(const VoxelLoad& l) {
  u64 h = key3(l.voxel[0], l.voxel[1], l.voxel[2]) * 0x9E3779B97F4A7C15ull;
  for (f64 v : {l.force.x, l.force.y, l.force.z}) {
    u64 b;
    std::memcpy(&b, &v, sizeof b);
    h = (h ^ b) * 0x100000001B3ull;
  }
  return h;
}

}  // namespace

void World::set_loads(u64 group, std::vector<VoxelLoad> loads) {
  std::vector<VoxelLoad> ok;
  ok.reserve(loads.size());
  for (const VoxelLoad& l : loads)
    if (finite3(l.force) && in_voxel_range(l.voxel)) ok.push_back(l);
  // (the structures under loads that are new or changed are extracted, if they are not
  // registered: loads on a structure nobody touched would go unnoticed; registered structures
  // see the current loads every tick)
  std::unordered_set<u64> before;
  if (const auto it = loads_.find(group); it != loads_.end())
    for (const VoxelLoad& l : it->second) before.insert(load_hash(l));
  u64 last = ~0ull;
  for (const VoxelLoad& l : ok) {
    if (before.count(load_hash(l))) continue;
    const IVec3 cc = chunk_of(l.voxel);
    const u64 k = key3(cc[0], cc[1], cc[2]) ^ (static_cast<u64>(chunk_index(l.voxel)) << 1);
    if (k == last) continue;
    last = k;
    seeds_.push_back(l.voxel);
  }
  if (ok.empty()) loads_.erase(group);
  else loads_[group] = std::move(ok);
}

void World::add_external_loads() {
  // (in the loads' own order - the same sums on every platform - with what a load's chunk
  // resolves to kept for the next load: producers write their loads chunk by chunk)
  u64 ck = ~0ull;
  const Chunk* ch = nullptr;
  FragChunk* fc = nullptr;
  const std::vector<i64>* own = nullptr;
  i64 sid = -1;
  Structure* s = nullptr;
  const std::vector<i32>* nodes = nullptr;
  for (const auto& [group, list] : loads_)
    for (const VoxelLoad& l : list) {
      const IVec3 cc = chunk_of(l.voxel);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      if (k != ck) {
        ck = k;
        ch = grid_.chunk(cc);
        fc = nullptr;
        own = nullptr;
        nodes = nullptr;
        sid = -1;
      }
      const i32 li = chunk_index(l.voxel);
      if (!ch || !vox_free(ch->uniform ? ch->value : ch->v[size_t(li)])) continue;
      if (!fc) {
        fc = &frag_chunk(cc);
        const auto ot = owner_.find(k);
        own = ot == owner_.end() ? nullptr : &ot->second;
      }
      const i32 fi = fc->at(li);
      if (fi < 0) continue;
      const i64 id = own && fi < static_cast<i32>(own->size()) ? (*own)[size_t(fi)] : 0;
      if (id != sid) {
        sid = id;
        s = structure(id);
        nodes = nullptr;
        if (s) {
          const auto nt = s->nodemap.find(k);
          if (nt != s->nodemap.end()) nodes = &nt->second;
        }
      }
      if (!s || !nodes || fi >= static_cast<i32>(nodes->size())) continue;
      const i32 i = (*nodes)[size_t(fi)];
      if (i < 0) continue;
      const V3 p{grid_.h * l.voxel[0], grid_.h * l.voxel[1], grid_.h * l.voxel[2]};
      const V3 M = cross(p - s->P.nodes[size_t(i)].c, l.force);
      f64* a = &s->acc[6 * size_t(i)];
      a[0] += l.force.x;
      a[1] += l.force.y;
      a[2] += l.force.z;
      a[3] += M.x;
      a[4] += M.y;
      a[5] += M.z;
    }
}

void World::apply_force(i64 id, const V3& point, const V3& force) {
  Body* b = rigid_.find(id);
  if (!b || !in_range(point) || !finite3(force)) return;
  b->force += force;
  b->torque += cross(point - b->x, force);
}

void World::wake_piece(i64 id) {
  if (Body* b = rigid_.find(id)) rigid_.wake(*b);
}

// ---------------------------------------------------------------------------------------------
// Systems

void World::add_system(std::shared_ptr<WorldSystem> s) {
  if (!s) return;
  systems_.push_back(s);
  s->attach(*this);
}

void World::finish_tick_changes() {
  // voxel changes of this tick: to the systems now, to the host when it takes them
  std::vector<u64> keys = grid_.take_dirty();
  if (!host_dirty_all_) {
    host_dirty_.insert(keys.begin(), keys.end());
    if (host_dirty_.size() > std::max<size_t>(65536, 4 * grid_.chunks().size())) {
      host_dirty_all_ = true;
      std::unordered_set<u64>().swap(host_dirty_);
    }
  }
  sys_changed_.insert(sys_changed_.end(), keys.begin(), keys.end());
  std::sort(sys_changed_.begin(), sys_changed_.end());
  sys_changed_.erase(std::unique(sys_changed_.begin(), sys_changed_.end()), sys_changed_.end());
}

void World::step_systems(bool step) {
  finish_tick_changes();
  std::vector<u64> evicted, generated, changed;
  evicted.swap(sys_evicted_);
  generated.swap(sys_generated_);
  changed.swap(sys_changed_);
  // (what is resident now: a chunk generated and evicted again since is gone)
  auto sorted_unique = [](std::vector<u64>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  };
  auto gone = [&](u64 k) { return !chunk_resident(unkey3(k)); };
  sorted_unique(evicted);
  sorted_unique(generated);
  generated.erase(std::remove_if(generated.begin(), generated.end(), gone), generated.end());
  changed.erase(std::remove_if(changed.begin(), changed.end(), gone), changed.end());
  // (by index over the systems there are now: one added by a system starts next tick)
  const size_t n = systems_.size();
  for (size_t i = 0; i < n; ++i) {
    const std::shared_ptr<WorldSystem> s = systems_[i];
    if (!evicted.empty()) s->on_evicted(*this, evicted);
    if (!generated.empty()) s->on_generated(*this, generated);
    if (!changed.empty()) s->on_voxels_changed(*this, changed);
    if (step) s->step(*this, cfg_.dt);
  }
}

}  // namespace svx
