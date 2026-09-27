// structvox — the world's extension points: layers (and the damage layer's strengths), loads on
// the static world, forces on pieces, systems (docs/CORE.md §5).
#include <algorithm>
#include <cmath>

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
  constexpr i32 kLim = (1 << 20) - 4096;
  i32 changed = 0;
  std::vector<u64> chunks;
  for (const LayerEdit& e : edits) {
    if (std::abs(e.p[0]) >= kLim || std::abs(e.p[1]) >= kLim || std::abs(e.p[2]) >= kLim) continue;
    if (!grid_.set_layer(L, e.p, e.v)) continue;
    ++changed;
    if (L == kDamageLayer) {
      const IVec3 cc = chunk_of(e.p);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      if (chunks.empty() || chunks.back() != k) {
        chunks.push_back(k);
        // (a structure nobody registered is extracted and judged: its sections are weaker)
        if (!owner_.count(k) && vox_free(grid_.get(e.p))) seeds_.push_back(e.p);
      }
    }
  }
  if (!chunks.empty()) refresh_strengths(chunks);
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
  for (const LayerEdit& e : edits) {
    const i32 i = S.index(e.p);
    if (i < 0 || !vox_solid(S.vox[size_t(i)])) continue;
    if (a.empty()) {
      if (e.v == 0) continue;
      a.assign(S.vox.size(), 0);
    }
    if (a[size_t(i)] == e.v) continue;
    a[size_t(i)] = e.v;
    ++changed;
  }
  if (changed && std::all_of(a.begin(), a.end(), [](u8 v) { return v == 0; })) std::vector<u8>().swap(a);
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

void World::refresh_strengths(const std::vector<u64>& chunks) {
  // Damage changed in these chunks: the sections of the bonds with a face there, in the
  // structures holding fragments there or next to them, are measured again.
  std::unordered_set<u64> changed(chunks.begin(), chunks.end());
  std::vector<i64> ids;
  for (u64 k : chunks) {
    const IVec3 cc = unkey3(k);
    for (int d = -1; d < 6; ++d) {
      IVec3 q = cc;
      if (d >= 0) q[d / 2] += (d & 1) ? 1 : -1;
      const auto ot = owner_.find(key3(q[0], q[1], q[2]));
      if (ot == owner_.end()) continue;
      for (i64 id : ot->second)
        if (id) ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  auto face_in = [&](const IVec3& p, int axis) {
    IVec3 q = p;
    q[axis] += 1;
    const IVec3 a = chunk_of(p), b = chunk_of(q);
    return changed.count(key3(a[0], a[1], a[2])) > 0 || changed.count(key3(b[0], b[1], b[2])) > 0;
  };
  for (i64 id : ids) {
    Structure* s = structure(id);
    if (!s) continue;
    for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
      SBond& B = s->P.bonds[size_t(b)];
      if (B.broken) continue;
      const i32 f0 = s->face_start[size_t(b)], f1 = s->face_start[size_t(b) + 1];
      bool hit = false;
      for (i32 f = f0; f < f1 && !hit; ++f) hit = face_in(s->face_p[size_t(f)], s->face_axis[size_t(f)]);
      if (!hit) continue;
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

void World::set_loads(u64 group, std::vector<VoxelLoad> loads) {
  constexpr i32 kLim = (1 << 20) - 4096;
  std::vector<VoxelLoad> ok;
  ok.reserve(loads.size());
  for (const VoxelLoad& l : loads)
    if (finite3(l.force) && std::abs(l.voxel[0]) < kLim && std::abs(l.voxel[1]) < kLim && std::abs(l.voxel[2]) < kLim)
      ok.push_back(l);
  // (the structures under them are extracted, if they are not registered: loads on a structure
  // nobody touched would go unnoticed)
  for (const VoxelLoad& l : ok) seeds_.push_back(l.voxel);
  if (ok.empty()) loads_.erase(group);
  else loads_[group] = std::move(ok);
}

void World::add_external_loads() {
  for (const auto& [group, list] : loads_)
    for (const VoxelLoad& l : list) {
      FragKey f;
      if (!frag_at(l.voxel, &f)) continue;
      Structure* s = structure(owner_of(f));
      if (!s) continue;
      const i32 i = s->node(f);
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

void World::step_systems() {
  finish_tick_changes();
  std::vector<u64> evicted, generated, changed;
  evicted.swap(sys_evicted_);
  generated.swap(sys_generated_);
  changed.swap(sys_changed_);
  for (auto& s : systems_) {
    if (!evicted.empty()) s->on_evicted(*this, evicted);
    if (!generated.empty()) s->on_generated(*this, generated);
    if (!changed.empty()) s->on_voxels_changed(*this, changed);
    s->step(*this, cfg_.dt);
  }
}

}  // namespace svx
