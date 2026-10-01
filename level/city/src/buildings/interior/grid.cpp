// svx_city — voxel_city buildings/interior/grid.js.
#include "buildings/interior/grid.hpp"

#include "core/placement.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

// ---- DoorGraph

const std::vector<double>* DoorGraph::get(double key) const {
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) return &sets[i];
  return nullptr;
}

std::vector<double>* DoorGraph::get(double key) {
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key) return &sets[i];
  return nullptr;
}

void DoorGraph::add(double x, double y) {
  std::vector<double>* s = get(x);
  if (!s) {
    keys.push_back(x);
    sets.emplace_back();
    s = &sets.back();
  }
  for (double v : *s)
    if (v == y) return;
  s->push_back(y);
}

bool DoorGraph::erase(double x, double y) {
  std::vector<double>* s = get(x);
  if (!s) return false;
  for (size_t i = 0; i < s->size(); ++i)
    if ((*s)[i] == y) {
      s->erase(s->begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  return false;
}

// ---- FloorGrid

FloorGrid::FloorGrid(double U_, double V_, const std::vector<Rect>& footprint_, Cut cut_)
    : U(U_), V(V_), footprint(footprint_), cut(std::move(cut_)) {
  cells.assign(static_cast<size_t>(U * V), 0);
  for (const Rect& r : footprint) fill(r, WALL);
  // (the exterior wall follows a cut: its cells are outside before the ring is found; rooms keep off them)
  if (cut)
    for (double v = 0; v < V; v += 1)
      for (double u = 0; u < U; u += 1)
        if (cut(u, v)) cells[static_cast<size_t>(u + v * U)] = OUT;
  // exterior wall ring: interior cells within EXT_T of the outside
  std::vector<size_t> ext;
  for (double v = 0; v < V; v += 1) {
    for (double u = 0; u < U; u += 1) {
      if (cells[static_cast<size_t>(u + v * U)] == OUT) continue;
      bool edge = false;
      for (double dv = -EXT_T; dv <= EXT_T && !edge; dv += 1) {
        for (double du = -EXT_T; du <= EXT_T; du += 1) {
          if (get(u + du, v + dv) == OUT) {
            edge = true;
            break;
          }
        }
      }
      if (edge) ext.push_back(static_cast<size_t>(u + v * U));
    }
  }
  for (size_t k : ext) cells[k] = EXT;
}

void FloorGrid::fill(const Rect& r, double val) {
  const double x0 = js::max(0.0, r.x0);
  const double y0 = js::max(0.0, r.y0);
  const double x1 = js::min(U - 1, r.x1);
  const double y1 = js::min(V - 1, r.y1);
  const uint16_t lab = js::u16(val);
  // (TypedArray.fill(val, start, end): nothing when end <= start)
  for (double v = y0; v <= y1; v += 1) {
    const double start = x0 + v * U, end = x1 + v * U + 1;
    for (double k = start; k < end; k += 1) cells[static_cast<size_t>(k)] = lab;
  }
}

std::vector<Rect> FloorGrid::inner_rects() const {
  std::vector<Rect> out;
  out.reserve(footprint.size());
  for (const Rect& r : footprint) out.push_back({r.x0 + EXT_T, r.y0 + EXT_T, r.x1 - EXT_T, r.y1 - EXT_T});
  return out;
}

bool FloorGrid::is_free(const Rect& r) const {
  if (r.x0 < 0 || r.y0 < 0 || r.x1 >= U || r.y1 >= V || r.x1 < r.x0 || r.y1 < r.y0) return false;
  for (double v = r.y0; v <= r.y1; v += 1) {
    for (double u = r.x0; u <= r.x1; u += 1)
      if (cells[static_cast<size_t>(u + v * U)] != WALL) return false;
  }
  return true;
}

std::shared_ptr<Room> FloorGrid::add_room(const std::string& type, const std::vector<Rect>& rects, const RoomProps& props) {
  const double id = static_cast<double>(rooms.size());
  auto room = std::make_shared<Room>();
  static_cast<RoomProps&>(*room) = props;
  room->id = id;
  room->type = type;
  for (const Rect& r : rects)
    if (r.x1 >= r.x0 && r.y1 >= r.y0) room->rects.push_back(r);
  rooms.push_back(room);
  for (const Rect& r : room->rects) {
    if (cut)
      paint_inside(r, ROOM0 + id);
    else
      fill(r, ROOM0 + id);
  }
  return room;
}

void FloorGrid::paint_inside(const Rect& r, double val) {
  const uint16_t lab = js::u16(val);
  for (double v = js::max(0.0, r.y0); v <= js::min(V - 1, r.y1); v += 1)
    for (double u = js::max(0.0, r.x0); u <= js::min(U - 1, r.x1); u += 1) {
      const size_t k = static_cast<size_t>(u + v * U);
      if (cells[k] != OUT && cells[k] != EXT) cells[k] = lab;
    }
}

bool FloorGrid::extend_room(Room& room, const Rect& rect) {
  if (!is_free(rect)) return false;
  room.rects.push_back(rect);
  fill(rect, ROOM0 + room.id);
  return true;
}

Room* FloorGrid::room_at(double u, double v) const {
  const int l = get(u, v);
  if (l < ROOM0) return nullptr;
  const size_t i = static_cast<size_t>(l - ROOM0);
  return i < rooms.size() ? rooms[i].get() : nullptr;
}

double FloorGrid::area(const Room& room) const {
  double a = 0;
  for (const Rect& r : room.rects) a += (r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1);
  return a;
}

FloorGrid::Snapshot FloorGrid::snapshot() const { return {cells, rooms.size(), doors.size()}; }

void FloorGrid::restore(const Snapshot& s) {
  cells = s.cells;
  rooms.resize(s.rooms);
  doors.resize(s.doors);
}

std::vector<WallRun> FloorGrid::wall_runs(const Room& a, const Room* b) const {
  const double la = ROOM0 + a.id;
  const double lb = b ? ROOM0 + b->id : OUT;
  const double thick = b ? 1 : EXT_T;
  const int wall_lab = b ? WALL : EXT;
  std::vector<WallRun> runs;
  auto push = [&](char orient, double fixed, double t, const char* side_a) {
    if (!runs.empty()) {
      WallRun& last = runs.back();
      if (last.orient == orient && last.fixed == fixed && last.side_a == side_a && last.t1 == t - 1) {
        last.t1 = t;
        return;
      }
    }
    runs.push_back({orient, fixed, t, t, thick, side_a});
  };
  auto is_wall = [&](double u, double v) { return get(u, v) == wall_lab; };
  for (const Rect& r : a.rects) {
    // east side
    for (double v = r.y0; v <= r.y1; v += 1) {
      if (get(r.x1, v) != la) continue;
      bool ok = true;
      for (double k = 1; k <= thick; k += 1)
        if (!is_wall(r.x1 + k, v)) ok = false;
      if (ok && get(r.x1 + thick + 1, v) == lb) push('v', r.x1 + 1, v, "low");
    }
    // west side
    for (double v = r.y0; v <= r.y1; v += 1) {
      if (get(r.x0, v) != la) continue;
      bool ok = true;
      for (double k = 1; k <= thick; k += 1)
        if (!is_wall(r.x0 - k, v)) ok = false;
      if (ok && get(r.x0 - thick - 1, v) == lb) push('v', r.x0 - thick, v, "high");
    }
    // south side (+v)
    for (double u = r.x0; u <= r.x1; u += 1) {
      if (get(u, r.y1) != la) continue;
      bool ok = true;
      for (double k = 1; k <= thick; k += 1)
        if (!is_wall(u, r.y1 + k)) ok = false;
      if (ok && get(u, r.y1 + thick + 1) == lb) push('h', r.y1 + 1, u, "low");
    }
    // north side (-v)
    for (double u = r.x0; u <= r.x1; u += 1) {
      if (get(u, r.y0) != la) continue;
      bool ok = true;
      for (double k = 1; k <= thick; k += 1)
        if (!is_wall(u, r.y0 - k)) ok = false;
      if (ok && get(u, r.y0 - thick - 1) == lb) push('h', r.y0 - thick, u, "high");
    }
  }
  return runs;
}

std::shared_ptr<Door> FloorGrid::add_door(const Room& a, const Room* b, DoorOpts opts) {
  if (!b && opts.place.value_or("auto") != "near") {
    // exterior doors face the street unless told otherwise
    if (a.rects.empty()) SVX_FAIL("grid: an exterior door of a room without rects");
    const Rect& r = a.rects[0];
    opts.place = "near";
    opts.near = DoorNear{(r.x0 + r.x1) / 2, -12};
  }
  const double width = opts.width.value_or(7) + door_extra;
  const double margin = opts.margin.value_or(2);
  std::vector<WallRun> runs;
  for (WallRun& r : wall_runs(a, b))
    if (r.t1 - r.t0 + 1 >= width + 2 * margin) runs.push_back(std::move(r));
  if (runs.empty()) return nullptr;
  const std::string place = opts.place.value_or("auto");
  if (place == "near" && !opts.near) SVX_FAIL("grid: a door placed near no point");
  const WallRun* best = nullptr;
  double best_t = 0;
  double best_score = js::kInf;
  std::vector<double> candidates;
  for (const WallRun& run : runs) {
    const double lo = run.t0 + margin;
    const double hi = run.t1 - margin - width + 1;
    candidates.clear();
    if (place == "center" || place == "auto") candidates.push_back(js::round((lo + hi) / 2));
    if (place == "start" || place == "auto") candidates.push_back(lo);
    if (place == "end" || place == "auto") candidates.push_back(hi);
    if (place == "near") {
      const double target = run.orient == 'v' ? opts.near->v : opts.near->u;
      candidates.push_back(js::max(lo, js::min(hi, js::round(target - width / 2))));
    }
    for (const double t : candidates) {
      double score = 0;
      if (place == "near") {
        const double cu = run.orient == 'v' ? run.fixed : t + width / 2;
        const double cv = run.orient == 'v' ? t + width / 2 : run.fixed;
        score = std::fabs(cu - opts.near->u) + std::fabs(cv - opts.near->v);
      } else if (place == "auto") {
        // prefer near a corner (leaves wall for furniture), long runs
        score = js::min(t - lo, hi - t) - (run.t1 - run.t0) * 0.01;
      }
      // avoid crowding existing doors
      for (const auto& d : doors) {
        const double du = std::fabs((d->u0 + d->u1) / 2 - (run.orient == 'v' ? run.fixed : t + width / 2));
        const double dv = std::fabs((d->v0 + d->v1) / 2 - (run.orient == 'v' ? t + width / 2 : run.fixed));
        if (du + dv < width + 4) score += 50;
      }
      if (score < best_score) {
        best_score = score;
        best = &run;
        best_t = t;
      }
    }
  }
  if (!best) return nullptr;
  const WallRun& run = *best;
  const double t = best_t;
  auto door = std::make_shared<Door>();
  if (run.orient == 'v') {
    door->u0 = run.fixed, door->u1 = run.fixed + run.thick - 1, door->v0 = t, door->v1 = t + width - 1, door->orient = 'v';
  } else {
    door->u0 = t, door->u1 = t + width - 1, door->v0 = run.fixed, door->v1 = run.fixed + run.thick - 1, door->orient = 'h';
  }
  door->a = a.id;
  door->b = b ? b->id : -1;
  door->kind = opts.kind ? *opts.kind : (b ? "interior" : "entrance");
  door->side_a = run.side_a;
  door->width = width;
  door->leaf = opts.leaf ? *opts.leaf : (door->kind == "opening" ? "none" : "wood");
  if (opts.height && js::truthy(*opts.height)) door->height = *opts.height;
  door->id = static_cast<double>(doors.size());
  for (double v = door->v0; v <= door->v1; v += 1)
    for (double u = door->u0; u <= door->u1; u += 1) set(u, v, DOOR);
  doors.push_back(door);
  return door;
}

std::optional<Rect> FloorGrid::door_leaf_rect(const Door& d) const {
  if (d.leaf == "none" || d.kind == "opening" || d.kind == "elevator" || d.leaf == "rollup") return std::nullopt;
  const double len = d.width - 1;
  const double room_lab = ROOM0 + d.a;  // (inside room a, an exterior door's too)
  const int tries[2][2] = {{0, 1}, {1, 0}};
  const int* order = (js::to_int32(d.id) & 1) == 0 ? tries[0] : tries[1];
  for (int k = 0; k < 2; ++k) {
    const int which = order[k];
    Rect r;
    if (d.orient == 'h') {
      const bool low = get(d.u0, d.v0 - 1) == room_lab;
      const double v_face = low ? d.v0 - 1 : d.v1 + 1;
      r = which == 0 ? Rect{d.u0 - len, v_face, d.u0 - 1, v_face} : Rect{d.u1 + 1, v_face, d.u1 + len, v_face};
    } else {
      const bool low = get(d.u0 - 1, d.v0) == room_lab;
      const double u_face = low ? d.u0 - 1 : d.u1 + 1;
      r = which == 0 ? Rect{u_face, d.v0 - len, u_face, d.v0 - 1} : Rect{u_face, d.v1 + 1, u_face, d.v1 + len};
    }
    bool ok = true;
    for (double v = r.y0; v <= r.y1 && ok; v += 1)
      for (double u = r.x0; u <= r.x1 && ok; u += 1)
        if (get(u, v) != room_lab) ok = false;
    if (ok) return r;
  }
  return std::nullopt;
}

DoorGraph FloorGrid::door_graph() const {
  DoorGraph adj;
  for (const auto& d : doors) {
    adj.add(d->a, d->b);
    adj.add(d->b, d->a);
  }
  return adj;
}

double door_extra_of(int turn_yaw) {
  const Yaw& Y = yaws()[static_cast<size_t>(turn_yaw)];
  return std::ceil((4 * (std::fabs(Y.c) + std::fabs(Y.s) - Y.r)) / Y.r) + 1;
}

}  // namespace svx::city
