// structvox — movers: doors, lifts, floors, ceilings (plan Phase 7). See MoverDef in engine.hpp.
#include <algorithm>
#include <cmath>

#include "svx/engine/engine.hpp"
#include "svx/engine/replay.hpp"

namespace svx {

namespace {

inline u64 col_key(i32 x, i32 y) { return key3(x, y, 0); }

// The player's box from the viewer (eye) position: 0.35 m around, 1.7 m below to 0.2 m above.
constexpr f64 kHalfWidth = 0.35, kBelowEye = 1.7, kAboveEye = 0.2;

}  // namespace

i32 Engine::add_mover(const MoverDef& d) {
  Mover m;
  m.def = d;
  m.def.vox = static_cast<Vox>(d.vox | kAnchorBit);
  const i32 H = d.z1 - d.z0;
  if (m.def.moves.empty()) {  // Door / Lift: open / lower fully, wait, return (or stay open)
    MoverMove mv;
    mv.type = d.repeat ? MoverMove::Type::Return : MoverMove::Type::To;
    mv.target = 0;
    mv.back = H;
    mv.speed = d.speed * H;
    mv.wait = d.wait;
    mv.reverse = d.kind == MoverDef::Kind::Door;
    m.def.moves.push_back(mv);
  }
  const i32 id = static_cast<i32>(movers_.size());
  for (const auto& c : d.cols) mover_cols_[col_key(c[0], c[1])].push_back(id);
  const i32 rows = d.rows < 0 ? H : std::clamp(d.rows, 0, H);
  m.level = rows;
  movers_.push_back(m);
  set_mover_rows(movers_.back(), rows);
  return id;
}

const MoverDef* Engine::mover_def(i32 id) const {
  return id >= 0 && id < mover_count() ? &movers_[static_cast<size_t>(id)].def : nullptr;
}

i32 Engine::mover_rows(i32 id) const { return id >= 0 && id < mover_count() ? movers_[static_cast<size_t>(id)].rows : 0; }

bool Engine::mover_busy(i32 id) const {
  if (id < 0 || id >= mover_count()) return false;
  const Mover& m = movers_[static_cast<size_t>(id)];
  return m.move >= 0 && !m.stopped && !m.disabled;
}

f64 Engine::mover_position(i32 id) const {
  if (id < 0 || id >= mover_count()) return 0.0;
  const Mover& m = movers_[static_cast<size_t>(id)];
  const i32 H = m.def.z1 - m.def.z0;
  return H > 0 ? 1.0 - m.level / H : 0.0;
}

i32 Engine::mover_at(const IVec3& v) const {
  const auto it = mover_cols_.find(col_key(v[0], v[1]));
  if (it == mover_cols_.end()) return -1;
  for (i32 id : it->second) {
    const MoverDef& d = movers_[static_cast<size_t>(id)].def;
    if (v[2] >= d.z0 && v[2] < d.z1) return id;
  }
  return -1;
}

bool Engine::activate_mover(i32 id, i32 k) {
  if (id < 0 || id >= mover_count()) return false;
  Mover& m = movers_[static_cast<size_t>(id)];
  if (m.disabled || k < 0 || k >= static_cast<i32>(m.def.moves.size())) return false;
  using T = MoverMove::Type;
  const MoverMove& mv = m.def.moves[static_cast<size_t>(k)];
  if (mv.type == T::Stop) {
    if (m.move < 0 || m.stopped || m.def.moves[static_cast<size_t>(m.move)].type != T::Cycle) return false;
    m.stopped = true;
    return true;
  }
  if (m.move >= 0) {
    if (m.move != k) return false;  // busy with another move (Doom: one mover per plane)
    if (mv.type == T::Cycle) {
      if (!m.stopped) return false;
      m.stopped = false;  // resumed
      return true;
    }
    if (mv.type == T::Return) {
      if (m.leg == 1) {  // Doom: using an open door closes it early
        m.leg = 2;
        return true;
      }
      if (m.leg == 2) {  // coming back: go again
        m.leg = 0;
        return true;
      }
    }
    return false;
  }
  const i32 H = m.def.z1 - m.def.z0;
  const i32 target = std::clamp(mv.target, 0, H);
  if (mv.type == T::To && target == m.rows) return false;  // already there
  m.move = k;
  m.leg = 0;
  m.timer = 0.0;
  m.stopped = false;
  m.back = mv.back >= 0 ? std::clamp(mv.back, 0, H) : m.rows;
  return true;
}

bool Engine::use(const std::array<f64, 3>& eye, const std::array<f64, 3>& dir, f64 reach) {
  if (log_) log_->push({st_.ticks, Command::Type::Use, {eye[0], eye[1], eye[2], dir[0], dir[1], dir[2]}});
  const RayHit hit = raycast(eye, dir, reach);
  if (!hit.hit) return false;
  auto usable = [&](i32 id) { return id >= 0 && movers_[static_cast<size_t>(id)].def.usable ? id : -1; };
  i32 id = usable(mover_at(hit.voxel));
  if (id < 0) {
    // the voxel just in front of the hit face may be the (open) mover span: a door jamb
    IVec3 q = hit.voxel;
    for (int a = 0; a < 3; ++a) q[a] += static_cast<i32>(std::lround(hit.normal[a]));
    id = usable(mover_at(q));
  }
  if (id >= 0) return activate_mover(id, 0);
  if (!use_resolver) return false;
  int face = 0;
  for (int a = 0; a < 3; ++a)
    if (hit.normal[a] != 0.0) face = 2 * a + (hit.normal[a] > 0.0 ? 1 : 0);
  bool any = false;
  for (const MoverTrigger& t : use_resolver(hit.voxel, face)) any = activate_mover(t.mover, t.move) || any;
  return any;
}

void Engine::set_mover_rows(Mover& m, i32 rows) {
  const MoverDef& d = m.def;
  const i32 H = d.z1 - d.z0;
  rows = std::clamp(rows, 0, H);
  // solid levels: a ceiling's part hangs from z1 (a door retracts upward), a floor's stands on z0
  const bool top = d.ceiling();
  auto solid = [&](i32 z, i32 r) { return top ? z >= d.z1 - r : z < d.z0 + r; };
  const bool tracked = grid_.tracking();
  grid_.track_changes(false);  // movers reset at load: never part of the persistence delta
  const f32 zero[3] = {0, 0, 0};
  for (const auto& c : d.cols)
    for (i32 z = d.z0; z < d.z1; ++z) {
      const bool want = solid(z, rows);
      const IVec3 p{c[0], c[1], z};
      const Vox cur = grid_.get(p);
      if (want == (cur == d.vox)) continue;
      if (!want) {
        if (cur != d.vox) continue;  // something else is there (never clear foreign voxels)
        grid_.set(p, kAir);
        continue;
      }
      if (vox_solid(cur)) continue;  // occupied (debris landed, etc.): leave it
      grid_.set(p, d.vox);
      grid_.clear_baseline(p);
      grid_.set_offset(p, zero);
      for (int a = 0; a < 3; ++a) {
        grid_.break_bond(p, a);
        IVec3 q = p;
        q[a] -= 1;
        grid_.break_bond(q, a);
      }
    }
  grid_.track_changes(tracked);
  m.rows = rows;
}

void Engine::step_movers() {
  using T = MoverMove::Type;
  const f64 dt = cfg_.dt;
  for (Mover& m : movers_) {
    if (m.disabled || m.move < 0 || m.stopped) continue;
    const MoverMove& mv = m.def.moves[static_cast<size_t>(m.move)];
    if (m.leg == 1 || m.leg == 3) {  // waiting at an end
      m.timer += dt;
      if (m.timer >= mv.wait) {
        m.leg = (m.leg + 1) & 3;
        m.timer = 0.0;
      }
      continue;
    }
    const i32 H = m.def.z1 - m.def.z0;
    const i32 goal = m.leg == 0 ? std::clamp(mv.target, 0, H) : m.back;
    const f64 step = dt * mv.speed;
    const f64 next = goal > m.level ? std::min<f64>(goal, m.level + step) : std::max<f64>(goal, m.level - step);
    const i32 rows_next = static_cast<i32>(std::lround(next));
    if (rows_next > m.rows && blocks_player(m, rows_next)) {
      // a door coming back goes up again (Doom); everything else waits for the player
      if (mv.type == T::Return && mv.reverse && m.leg == 2) m.leg = 0;
      continue;
    }
    m.level = next;
    if (rows_next != m.rows) set_mover_rows(m, rows_next);
    if (m.level != goal) continue;
    if (m.leg == 0) {
      if (mv.type == T::To) m.move = -1;
      else {
        m.leg = 1;
        m.timer = 0.0;
      }
    } else if (mv.type == T::Cycle) {
      m.leg = 3;
      m.timer = 0.0;
    } else {
      m.move = -1;
    }
  }
}

bool Engine::blocks_player(const Mover& m, i32 rows) const {
  const f64 h = grid_.h;
  const f64 x0 = viewer_[0] - kHalfWidth, x1 = viewer_[0] + kHalfWidth;
  const f64 y0 = viewer_[1] - kHalfWidth, y1 = viewer_[1] + kHalfWidth;
  const f64 zb = viewer_[2] - kBelowEye, zt = viewer_[2] + kAboveEye;
  const MoverDef& d = m.def;
  bool over = false;
  for (const auto& c : d.cols) {
    const f64 cx = h * c[0], cy = h * c[1];
    if (cx + 0.5 * h > x0 && cx - 0.5 * h < x1 && cy + 0.5 * h > y0 && cy - 0.5 * h < y1) {
      over = true;
      break;
    }
  }
  if (!over) return false;
  if (d.ceiling()) {
    // levels that become solid: [z1 - rows, z1 - m.rows)
    const f64 lo = h * (d.z1 - rows - 0.5), hi = h * (d.z1 - m.rows - 0.5);
    return hi > zb && lo < zt;
  }
  // A rising floor carries the player (the client lifts the box out of it) unless the player,
  // standing on the new top, would not fit under what is above.
  const f64 top = h * (d.z0 + rows - 0.5);
  if (zb >= top) return false;  // not pushed
  const f64 need = zt - zb;
  const i32 i0 = static_cast<i32>(std::floor(x0 / h + 0.5)), i1 = static_cast<i32>(std::floor(x1 / h + 0.5));
  const i32 j0 = static_cast<i32>(std::floor(y0 / h + 0.5)), j1 = static_cast<i32>(std::floor(y1 / h + 0.5));
  const i32 k0 = d.z0 + rows, k1 = static_cast<i32>(std::floor((top + need) / h + 0.5));
  for (i32 i = i0; i <= i1; ++i)
    for (i32 j = j0; j <= j1; ++j)
      for (i32 k = k0; k < k1; ++k)
        if (vox_solid(grid_.get(i, j, k))) return true;
  return false;
}

void Engine::disable_movers_at(const std::vector<IVec3>& removed) {
  for (const IVec3& p : removed) {
    const i32 id = mover_at(p);
    if (id >= 0) movers_[static_cast<size_t>(id)].disabled = true;
  }
}

}  // namespace svx
