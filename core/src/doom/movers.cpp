#include "svx/doom/movers.hpp"

#include <map>
#include <memory>
#include <set>

#include "svx/doom/specials.hpp"

namespace svx::doom {

namespace {

inline bool segments_cross(f64 ax, f64 ay, f64 bx, f64 by, f64 cx, f64 cy, f64 dx, f64 dy) {
  auto orient = [](f64 px, f64 py, f64 qx, f64 qy, f64 rx, f64 ry) {
    return (qx - px) * (ry - py) - (qy - py) * (rx - px);
  };
  const f64 o1 = orient(ax, ay, bx, by, cx, cy), o2 = orient(ax, ay, bx, by, dx, dy);
  const f64 o3 = orient(cx, cy, dx, dy, ax, ay), o4 = orient(cx, cy, dx, dy, bx, by);
  return ((o1 > 0) != (o2 > 0)) && ((o3 > 0) != (o4 > 0));
}

// squared distance from (px, py) to the segment a-b
inline f64 seg_dist2(f64 px, f64 py, f64 ax, f64 ay, f64 bx, f64 by) {
  const f64 ex = bx - ax, ey = by - ay, l2 = ex * ex + ey * ey;
  f64 t = l2 > 0.0 ? ((px - ax) * ex + (py - ay) * ey) / l2 : 0.0;
  t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  const f64 dx = ax + t * ex - px, dy = ay + t * ey - py;
  return dx * dx + dy * dy;
}

inline u32 line_key(u16 special, u16 tag) { return (u32(special) << 16) | tag; }

MoverMove::Type move_type(MoveKind k) {
  switch (k) {
    case MoveKind::To: return MoverMove::Type::To;
    case MoveKind::Return: return MoverMove::Type::Return;
    case MoveKind::Cycle: return MoverMove::Type::Cycle;
    default: return MoverMove::Type::Stop;
  }
}

}  // namespace

int attach_doom_movers(Engine& eng, const DoomWorld& w) {
  constexpr f64 kTic = 35.0;  // Doom tics per second
  const i32 upv = w.vstats.units_per_voxel;
  const size_t nsec = w.map.sectors.size();
  std::vector<std::vector<std::array<i32, 2>>> cols(nsec);
  for (i32 j = 0; j < w.ny; ++j)
    for (i32 i = 0; i < w.nx; ++i) {
      const i32 s = w.sector[size_t(j) * w.nx + i];
      if (s >= 0 && static_cast<size_t>(s) < nsec) cols[static_cast<size_t>(s)].push_back({i, j});
    }
  auto triggers = std::make_shared<std::map<u32, std::vector<MoverTrigger>>>();  // (special, tag) -> moves
  for (const MoverInfo& mi : w.movers) {
    // (every plane becomes a mover, so mover ids are indices into w.movers; a sector too small
    // to own a column moves nothing)
    MoverDef d;
    switch (mi.kind) {
      case MoverInfo::Kind::Door: d.kind = MoverDef::Kind::Door; break;
      case MoverInfo::Kind::Lift: d.kind = MoverDef::Kind::Lift; break;
      case MoverInfo::Kind::Floor: d.kind = MoverDef::Kind::Floor; break;
      case MoverInfo::Kind::Ceiling: d.kind = MoverDef::Kind::Ceiling; break;
    }
    if (mi.sector >= 0 && static_cast<size_t>(mi.sector) < nsec) d.cols = cols[static_cast<size_t>(mi.sector)];
    d.z0 = mi.z0;
    d.z1 = mi.z1;
    d.rows = mi.rows;
    d.vox = make_vox(mi.kind == MoverInfo::Kind::Door ? MaterialId::Steel : MaterialId::Concrete, true);
    // a door opens on use when a manual door line opens it (tagged doors: from their switch);
    // lifts on use of the lift itself
    d.usable = mi.kind == MoverInfo::Kind::Lift || (mi.kind == MoverInfo::Kind::Door && mi.manual);
    for (const PlaneMove& pm : mi.moves) {
      SpecialInfo si;
      special_info(pm.special, &si);
      const SpecialAction* a = nullptr;
      for (int k = 0; k < si.n; ++k)
        if (si.a[k].ceiling == mi.ceiling()) a = &si.a[k];
      MoverMove m;
      if (a) {
        m.type = move_type(a->kind);
        m.speed = a->speed * kTic / upv;
        m.wait = a->wait / kTic;
        m.reverse = (a->door || a->lift) && a->kind == MoveKind::Return;
      }
      m.target = pm.target;
      m.back = pm.back;
      d.moves.push_back(m);
    }
    const i32 id = eng.add_mover(d);
    for (size_t k = 0; k < mi.moves.size(); ++k)
      if (mi.moves[k].tag != 0)
        (*triggers)[line_key(mi.moves[k].special, mi.moves[k].tag)].push_back({id, static_cast<i32>(k)});
  }
  // lines that work once (S1 / W1 / G1) stop after their first activation
  auto fired = std::make_shared<std::set<i32>>();
  auto run = [triggers, fired](i32 li, const Linedef& L, bool once, std::vector<MoverTrigger>* out) {
    const auto it = triggers->find(line_key(L.special, L.tag));
    if (it == triggers->end()) return;
    if (once && !fired->insert(li).second) return;
    out->insert(out->end(), it->second.begin(), it->second.end());
  };
  const DoomWorld* wp = &w;
  eng.use_resolver = [wp, run](const IVec3& voxel, int face) {
    std::vector<MoverTrigger> out;
    const i32 li = wp->face_linedef(voxel, face);
    if (li < 0) return out;
    const Linedef& L = wp->map.linedefs[static_cast<size_t>(li)];
    SpecialInfo si;
    if (L.tag == 0 || !special_info(L.special, &si) || si.trigger != LineTrigger::Use) return out;
    run(li, L, si.once, &out);
    return out;
  };
  // walk-over and gun-shot lines, in world metres (voxel (i, j) is centred at (i h, j h); map
  // x = origin_x + (i + 1/2) upv)
  struct Line {
    i32 index;
    f64 ax, ay, bx, by;
    bool once;
  };
  std::vector<Line> walk, shot;
  const f64 h = w.live ? w.live->h : w.grid.h;
  auto wx = [&](f64 mx) { return h * ((mx - w.vstats.origin_x) / upv - 0.5); };
  auto wy = [&](f64 my) { return h * ((my - w.vstats.origin_y) / upv - 0.5); };
  for (size_t li = 0; li < w.map.linedefs.size(); ++li) {
    const Linedef& L = w.map.linedefs[li];
    SpecialInfo si;
    if (L.tag == 0 || !special_info(L.special, &si)) continue;
    if (si.trigger != LineTrigger::Walk && si.trigger != LineTrigger::Shoot) continue;
    if (!triggers->count(line_key(L.special, L.tag))) continue;
    if (L.v1 >= w.map.vertices.size() || L.v2 >= w.map.vertices.size()) continue;
    const Vertex& A = w.map.vertices[L.v1];
    const Vertex& B = w.map.vertices[L.v2];
    (si.trigger == LineTrigger::Walk ? walk : shot)
        .push_back({static_cast<i32>(li), wx(A.x), wy(A.y), wx(B.x), wy(B.y), si.once});
  }
  if (!walk.empty())
    eng.walk_resolver = [wp, walk, run](const std::array<f64, 3>& from, const std::array<f64, 3>& to) {
      std::vector<MoverTrigger> out;
      for (const Line& l : walk)
        if (segments_cross(from[0], from[1], to[0], to[1], l.ax, l.ay, l.bx, l.by))
          run(l.index, wp->map.linedefs[static_cast<size_t>(l.index)], l.once, &out);
      return out;
    };
  // a hitscan impact within a voxel of a gun line's wall (vanilla: the shot crosses the line)
  if (!shot.empty())
    eng.shot_resolver = [wp, shot, run, h](const std::array<f64, 3>& at) {
      std::vector<MoverTrigger> out;
      for (const Line& l : shot)
        if (seg_dist2(at[0], at[1], l.ax, l.ay, l.bx, l.by) <= h * h)
          run(l.index, wp->map.linedefs[static_cast<size_t>(l.index)], l.once, &out);
      return out;
    };
  return eng.mover_count();
}

}  // namespace svx::doom
