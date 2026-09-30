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

int World::Impl::add_layer(const LayerSpec& spec) {
  const int i = grid_.add_layer(spec);
  if (i >= 0) {
    ext_.layers = grid_.layers();
    for (size_t g = 1; g < grids_.size(); ++g)
      if (grids_[g]) grids_[g]->g.adopt_layers(ext_.layers);
  }
  return i;
}

std::vector<u64> World::Impl::take_layer_changes(GridId grid, int L) {
  const i32 g = slot_of(grid);
  return g < 0 ? std::vector<u64>{} : vg(static_cast<u16>(g)).take_layer_dirty(L);
}

u8 World::Impl::layer(GridId grid, int L, const IVec3& p) const {
  const i32 g = slot_of(grid);
  return g < 0 ? 0 : vg(static_cast<u16>(g)).layer(L, p);
}

i32 World::Impl::set_layer(int L, const std::vector<LayerEdit>& edits) { return world_set_layer(0, L, edits); }

i32 World::Impl::set_layer(GridId grid, int L, const std::vector<LayerEdit>& edits) {
  const i32 g = slot_of(grid);
  return g < 0 ? 0 : world_set_layer(static_cast<u16>(g), L, edits);
}

i32 World::Impl::world_set_layer(u16 g, int L, const std::vector<LayerEdit>& edits) {
  if (L < 0 || L >= static_cast<int>(ext_.layers.size())) return 0;
  VoxelGrid& G = vg(g);
  i32 changed = 0;
  std::vector<GVox> damaged;
  for (const LayerEdit& e : edits) {
    if (!in_voxel_range(e.p)) continue;
    // (a chunk of a streamed world not generated yet has no voxels to hold values: a chunk
    // made for them would stand in for the generated one)
    if (g == 0 && strm_.source && !chunk_resident(chunk_of(e.p))) continue;
    if (!G.set_layer(L, e.p, e.v)) continue;
    ++changed;
    if (L == kDamageLayer) damaged.push_back(GVox{e.p, g});
  }
  if (!damaged.empty()) {
    refresh_strengths(damaged);
    // structures nobody registered are extracted and judged: their sections are weaker (a seed
    // per fragment)
    std::unordered_set<u64> seeded;
    for (const GVox& p : damaged) {
      if (!vox_free(G.get(p.p))) continue;
      FragKey f;
      if (!frag_at(p, &f) || owner_of(f) != 0) continue;
      if (seeded.insert(f.chunk * 4099u + static_cast<u64>(f.idx)).second) seeds_.push_back(p);
    }
  }
  return changed;
}

u8 World::Impl::piece_layer(i64 id, i32 shape, int L, const IVec3& p) const {
  const Body* b = piece(id);
  if (!b || L < 0 || L >= kMaxLayers || shape < 0 || shape >= static_cast<i32>(b->shapes.size())) return 0;
  const BodyShape& S = b->shapes[size_t(shape)];
  return S.layer_at(L, S.index(p));
}

i32 World::Impl::set_piece_layer(i64 id, i32 shape, int L, const std::vector<LayerEdit>& edits) {
  Body* b = rigid_.find(id);
  if (!b || L < 0 || L >= static_cast<int>(ext_.layers.size()) || shape < 0 || shape >= static_cast<i32>(b->shapes.size())) return 0;
  BodyShape& S = b->shapes[size_t(shape)];
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

bool World::Impl::remove_piece_voxels(i64 id, i32 shape, const std::vector<IVec3>& voxels, bool dust) {
  Body* bp = rigid_.find(id);
  if (!bp || shape < 0 || shape >= static_cast<i32>(bp->shapes.size())) return false;
  Body& b = *bp;
  BodyShape& S = b.shapes[size_t(shape)];
  const f64 h = S.h;
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
    at += S.xf.to(V3{h * p[0], h * p[1], h * p[2]});
    ++removed;
  }
  if (!removed) return false;
  if (dust) {
    const V3 X = b.to_world(at * (1.0 / removed));
    dust_event(X, b.v + cross(b.w, X - b.x), removed, true);
  }
  refragment_body(b);
  rigid_.wake(b);
  if (b.count == 0) {
    remove_bodies({id}, PieceEnd::Split);
    return true;
  }
  b.graph_dirty = true;
  split_body(b, false, true);  // (what is left: new pieces, this one's parts)
  flush_body_changes();
  return true;
}

void World::Impl::refresh_strengths(const std::vector<GVox>& voxels) {
  // The bonds with a face at a voxel whose damage changed are measured again: every face of a
  // free voxel is in a bond of its node, and those of an anchored one in bonds of its free
  // neighbours' nodes (the other bonds' sections did not change); junction samples likewise
  // (a node's bonds are all measured again: its junctions too).
  std::map<i64, std::vector<i32>> marked;  // structure -> nodes
  auto mark = [&](const GVox& q) {
    FragKey f;
    if (!frag_at(q, &f)) return;
    const i64 id = owner_of(f);
    const Structure* s = id ? structure(id) : nullptr;
    const i32 i = s ? s->node(f) : -1;
    if (i >= 0) marked[id].push_back(i);
  };
  for (const GVox& p : voxels) {
    const VoxelGrid& G = vg(p.grid);
    const Vox v = G.get(p.p);
    if (vox_free(v)) {
      mark(p);
    } else if (vox_anchored(v)) {
      for (int d = 0; d < 6; ++d) {
        IVec3 q = p.p;
        q[d / 2] += (d & 1) ? -1 : 1;
        if (vox_free(G.get(q))) mark(GVox{q, p.grid});
      }
      // (free voxels of other grids around it: its junctions)
      if (oriented_ > 0) {
        const f64 hp = G.h;
        const V3 X = xf_of(p.grid).to(V3{hp * p.p[0], hp * p.p[1], hp * p.p[2]});
        const IVec3 cc = chunk_of(p.p);
        for (u16 o : near_grids(p.grid, key3(cc[0], cc[1], cc[2]))) {
          const f64 ho = h_of(o), r = (0.5 + std::clamp(cfg_.junction_reach, 0.0, 2.0)) * std::max(hp, ho);
          const V3 L = o == 0 ? X : xf_of(o).from(X);
          // (every voxel of o in the box of half side r around it)
          const IVec3 vlo = voxel_of(L - V3{r, r, r}, ho), vhi = voxel_of(L + V3{r, r, r}, ho);
          for (i32 x = vlo[0]; x <= vhi[0]; ++x)
            for (i32 y = vlo[1]; y <= vhi[1]; ++y)
              for (i32 z = vlo[2]; z <= vhi[2]; ++z)
                if (vox_free(vg(o).get(x, y, z))) mark(GVox{{x, y, z}, o});
        }
      }
    }
  }
  auto at = [this](u16 g, const IVec3& q) { return voxel_at(GVox{q, g}); };
  for (const auto& [id, nodes] : marked) {
    Structure* s = structure(id);
    if (!s) continue;
    std::vector<u8> m(s->P.nodes.size(), 0);
    for (i32 i : nodes) m[size_t(i)] = 1;
    for (i32 b = 0; b < static_cast<i32>(s->P.bonds.size()); ++b) {
      SBond& B = s->P.bonds[size_t(b)];
      if (B.broken || !(m[size_t(B.a)] || (B.b >= 0 && m[size_t(B.b)]))) continue;
      const i32 f0 = s->face_start[size_t(b)], f1 = s->face_start[size_t(b) + 1];
      const i32 j0 = s->jstart[size_t(b)], j1 = s->jstart[size_t(b) + 1];
      const f32 ft = B.ft, fb = B.fb, fc = B.fc, coh = B.coh;
      const u16 g = s->bgrid[size_t(b)];
      if (j0 == j1)
        section_strengths(mats(), &s->face_p[size_t(f0)], &s->face_axis[size_t(f0)], size_t(f1 - f0), [&](const IVec3& q) { return voxel_at(GVox{q, g}); }, B);
      else
        section_strengths_general(mats(), g, s->face_p.data() + f0, s->face_axis.data() + f0, size_t(f1 - f0), s->jref.data() + j0, size_t(j1 - j0), at, B,
                                  std::clamp(cfg_.junction_samples, 1, 7));
      if (B.ft != ft || B.fb != fb || B.fc != fc || B.coh != coh) s->rejudge = true;
    }
  }
}

VoxelAt World::Impl::voxel_at(const GVox& v) const { return {vg(v.grid).get(v.p), vg(v.grid).layer(kDamageLayer, v.p)}; }

VoxelAt World::Impl::piece_voxel_at(const Body& b, i32 shape, const IVec3& p) const {
  const BodyShape& S = b.shapes[size_t(shape)];
  const i32 i = S.index(p);
  return {i < 0 ? kAir : S.vox[size_t(i)], S.layer_at(kDamageLayer, i)};
}

// ---------------------------------------------------------------------------------------------
// Loads and forces

namespace {

u64 load_hash(const VoxelLoad& l) {
  u64 h = key3(l.voxel[0], l.voxel[1], l.voxel[2]) * 0x9E3779B97F4A7C15ull;
  if (l.grid != kWorldGrid) h = (h ^ l.grid) * 0x100000001B3ull;
  for (f64 v : {l.force.x, l.force.y, l.force.z}) {
    u64 b;
    std::memcpy(&b, &v, sizeof b);
    h = (h ^ b) * 0x100000001B3ull;
  }
  return h;
}

}  // namespace

void World::Impl::set_loads(u64 group, std::vector<VoxelLoad> loads) {
  std::vector<VoxelLoad> ok;
  ok.reserve(loads.size());
  for (const VoxelLoad& l : loads)
    if (finite3(l.force) && in_voxel_range(l.voxel)) ok.push_back(l);
  // (the structures under loads that are new or changed are extracted, if they are not
  // registered: loads on a structure nobody touched would go unnoticed; registered structures
  // see the current loads every tick)
  std::unordered_set<u64> before;
  if (const auto it = ext_.loads.find(group); it != ext_.loads.end())
    for (const VoxelLoad& l : it->second) before.insert(load_hash(l));
  u64 last = ~0ull;
  for (const VoxelLoad& l : ok) {
    if (before.count(load_hash(l))) continue;
    const i32 g = slot_of(l.grid);
    if (g < 0) continue;
    const IVec3 cc = chunk_of(l.voxel);
    const u64 k = (key3(cc[0], cc[1], cc[2]) ^ (static_cast<u64>(chunk_index(l.voxel)) << 1)) + static_cast<u64>(l.grid);
    if (k == last) continue;
    last = k;
    seeds_.push_back(GVox{l.voxel, static_cast<u16>(g)});
  }
  if (ok.empty()) ext_.loads.erase(group);
  else ext_.loads[group] = std::move(ok);
}

void World::Impl::add_external_loads() {
  // (in the loads' own order - the same sums on every platform - with what a load's chunk
  // resolves to kept for the next load: producers write their loads chunk by chunk)
  u64 ck = ~0ull;
  GridId cg = ~0u;
  i32 g = -1;
  const Chunk* ch = nullptr;
  FragChunk* fc = nullptr;
  const std::vector<i64>* own = nullptr;
  i64 sid = -1;
  Structure* s = nullptr;
  const std::vector<i32>* nodes = nullptr;
  for (const auto& [group, list] : ext_.loads)
    for (const VoxelLoad& l : list) {
      const IVec3 cc = chunk_of(l.voxel);
      const u64 k = key3(cc[0], cc[1], cc[2]);
      if (k != ck || l.grid != cg) {
        ck = k;
        cg = l.grid;
        g = slot_of(l.grid);
        ch = g >= 0 ? vg(static_cast<u16>(g)).chunk(cc) : nullptr;
        fc = nullptr;
        own = nullptr;
        nodes = nullptr;
        sid = -1;
      }
      const i32 li = chunk_index(l.voxel);
      if (!ch || !vox_free(ch->uniform ? ch->value : ch->v[size_t(li)])) continue;
      if (!fc) {
        fc = &frag_chunk(static_cast<u16>(g), cc);
        const auto& owner = gs(static_cast<u16>(g)).owner;
        const auto ot = owner.find(k);
        own = ot == owner.end() ? nullptr : &ot->second;
      }
      const i32 fi = fc->at(li);
      if (fi < 0) continue;
      const i64 id = own && fi < static_cast<i32>(own->size()) ? (*own)[size_t(fi)] : 0;
      if (id != sid) {
        sid = id;
        s = structure(id);
        nodes = nullptr;
        if (s) {
          const auto nt = s->nodemap.find(GKey{static_cast<u16>(g), k});
          if (nt != s->nodemap.end()) nodes = &nt->second;
        }
      }
      if (!s || !nodes || fi >= static_cast<i32>(nodes->size())) continue;
      const i32 i = (*nodes)[size_t(fi)];
      if (i < 0) continue;
      const V3 p = voxel_centre(GVox{l.voxel, static_cast<u16>(g)}), F = l.force;
      const V3 M = cross(p - s->P.nodes[size_t(i)].c, F);
      f64* a = &s->acc[6 * size_t(i)];
      a[0] += F.x;
      a[1] += F.y;
      a[2] += F.z;
      a[3] += M.x;
      a[4] += M.y;
      a[5] += M.z;
    }
}

void World::Impl::apply_force(i64 id, const V3& point, const V3& force) {
  Body* b = rigid_.find(id);
  if (!b || !in_range(point) || !finite3(force)) return;
  b->force += force;
  b->torque += cross(point - b->x, force);
}

void World::Impl::wake_piece(i64 id) {
  if (Body* b = rigid_.find(id)) rigid_.wake(*b);
}

// ---------------------------------------------------------------------------------------------
// Systems

void World::Impl::add_system(std::shared_ptr<WorldSystem> s) {
  if (!s) return;
  ext_.systems.push_back(s);
  s->attach(*self_);
}

void World::Impl::finish_tick_changes() {
  // voxel changes of this tick: to the systems now, to the host when it takes them (the
  // oriented grids' to the host only)
  for (size_t g = 1; g < grids_.size(); ++g) {
    if (!grids_[g]) continue;
    for (u64 k : grids_[g]->g.take_dirty()) {
      grid_dirty_.push_back(GridChunk{grids_[g]->id, unkey3(k)});
      // (the world's lattice sees a grid's voxels too: the systems of it hear of them)
      if (!ext_.systems.empty()) {
        const GridState& st = *grids_[g];
        const f64 h = st.g.h;
        const IVec3 c = unkey3(k);
        const V3 lo{h * (c[0] * kChunk - 0.5), h * (c[1] * kChunk - 0.5), h * (c[2] * kChunk - 0.5)};
        const V3 hi{lo.x + h * kChunk, lo.y + h * kChunk, lo.z + h * kChunk};
        world_chunks_of(st.xf, lo, hi, ext_.sys_changed);
      }
    }
  }
  if (grid_dirty_.size() > 65536) {
    // (nobody takes them: every chunk of every grid is reported instead)
    grid_dirty_.clear();
    for (size_t g = 1; g < grids_.size(); ++g)
      if (grids_[g]) grids_[g]->g.mark_all_dirty();
  }
  std::vector<u64> keys = grid_.take_dirty();
  if (!ext_.host_dirty_all) {
    ext_.host_dirty.insert(keys.begin(), keys.end());
    if (ext_.host_dirty.size() > std::max<size_t>(65536, 4 * grid_.chunks().size())) {
      ext_.host_dirty_all = true;
      std::unordered_set<u64>().swap(ext_.host_dirty);
    }
  }
  ext_.sys_changed.insert(ext_.sys_changed.end(), keys.begin(), keys.end());
  std::sort(ext_.sys_changed.begin(), ext_.sys_changed.end());
  ext_.sys_changed.erase(std::unique(ext_.sys_changed.begin(), ext_.sys_changed.end()), ext_.sys_changed.end());
}

void World::Impl::step_systems(bool step) {
  finish_tick_changes();
  std::vector<u64> evicted, generated, changed;
  evicted.swap(ext_.sys_evicted);
  generated.swap(ext_.sys_generated);
  changed.swap(ext_.sys_changed);
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
  const size_t n = ext_.systems.size();
  systems_phase_ = true;
  for (size_t i = 0; i < n; ++i) {
    const std::shared_ptr<WorldSystem> s = ext_.systems[i];
    if (!evicted.empty()) s->on_evicted(*self_, evicted);
    if (!generated.empty()) s->on_generated(*self_, generated);
    if (!changed.empty()) s->on_voxels_changed(*self_, changed);
    if (step) s->step(*self_, cfg_.dt);
  }
  systems_phase_ = false;
}

}  // namespace svx
