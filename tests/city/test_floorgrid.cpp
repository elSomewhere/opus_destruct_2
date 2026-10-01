// svx_city tests — the floor grid (voxel_city buildings/interior/grid.js) and the planners' grid
// helpers (common.js) against the reference (stage "floorgrid" of tools/procgen_ref): the same
// sample floors planned the same way.
#include <doctest.h>

#include <string>
#include <vector>

#include "buildings/chamfer.hpp"
#include "buildings/interior/common.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/stairs.hpp"
#include "records.hpp"

using namespace svx::city;
using rec::Line;

namespace {

std::string rle(const std::vector<uint16_t>& a) {
  std::string out;
  size_t i = 0;
  while (i < a.size()) {
    size_t j = i;
    while (j < a.size() && a[j] == a[i]) ++j;
    if (!out.empty()) out += ',';
    out += js::cat(a[i], ":", static_cast<double>(j - i));
    i = j;
  }
  return out;
}
std::string fo(const std::optional<double>& v) { return v ? js::num(*v) : "-"; }
std::string fo(const std::optional<std::string>& s) { return s ? (s->empty() ? "\"\"" : *s) : "-"; }
std::string fstr(const std::string& s) { return s.empty() ? "\"\"" : s; }
std::string frect(const Rect& q) { return js::cat(q.x0, ",", q.y0, ",", q.x1, ",", q.y1); }
std::string fdoor(const Door& d) {
  return js::cat(d.id, ",", d.u0, ",", d.u1, ",", d.v0, ",", d.v1, ",", d.orient, ",", d.a, ",", d.b, ",", fstr(d.kind), ",", fstr(d.side_a), ",", d.width, ",",
                 fstr(d.leaf), ",", fo(d.height));
}
std::string frun(const WallRun& w) { return js::cat(w.orient, ",", w.fixed, ",", w.t0, ",", w.t1, ",", w.thick, ",", fstr(w.side_a)); }
std::string froom(const Room& m) {
  std::string rects;
  for (size_t k = 0; k < m.rects.size(); ++k) rects += js::cat(k ? "|" : "", frect(m.rects[k]));
  return js::cat(m.id, ",", fstr(m.type), ",", fstr(rects), ",", fo(m.paint), ",", fo(m.floor_mat), ",", fo(m.stair));
}
template <class T, class F>
std::string join(const std::vector<T>& v, const char* sep, F&& fmt) {
  std::string s;
  for (size_t k = 0; k < v.size(); ++k) {
    if (k) s += sep;
    s += fmt(v[k]);
  }
  return s;
}

const char* const kKinds[6] = {nullptr, "interior", "opening", "closet", "elevator", "entry"};
const char* const kLeaves[6] = {nullptr, "wood", "none", "glass", "metal", "rollup"};
const char* const kPlaces[6] = {nullptr, "auto", "center", "start", "end", "near"};
const char* const kTypes[6] = {"office", "bedroom", "living", "kitchen", "bath", "storage"};

DoorOpts door_opts(rec::Samples& r, double b0, double b1) {
  double v[7];
  for (double& x : v) x = r();
  DoorOpts o;
  if (v[0] < 0.6) o.width = 6 + std::floor(v[1] * 6);
  if (v[1] < 0.3) o.margin = std::floor(v[2] * 3);
  const char* place = kPlaces[static_cast<size_t>(std::floor(v[2] * 6))];
  if (place) o.place = place;
  if ((place && std::string(place) == "near") || v[3] < 0.2) o.near = DoorNear{b0 + std::floor(v[3] * (b1 - b0 + 1)), std::floor(v[4] * 80) - 20};
  const char* kind = kKinds[static_cast<size_t>(std::floor(v[4] * 6))];
  if (kind) o.kind = kind;
  const char* leaf = kLeaves[static_cast<size_t>(std::floor(v[5] * 6))];
  if (leaf) o.leaf = leaf;
  if (v[6] < 0.3) o.height = std::floor(v[6] * 70) - 1;
  return o;
}

std::string id_or(const std::shared_ptr<Door>& d) { return d ? js::num(d->id) : "-"; }

}  // namespace

TEST_CASE("city floor grid: plans conform to the reference (stage floorgrid)") {
  rec::Samples r(23);
  rec::Out out;
  for (int i = 0; i < 90; ++i) {
    const double U = 40 + std::floor(r() * 100);
    const double V = 34 + std::floor(r() * 80);
    const double shape = std::floor(r() * 4);
    const double s1 = r();
    const double s2 = r();
    std::vector<Rect> fp;
    if (shape == 0)
      fp = {{0, 0, U - 1, V - 1}};
    else if (shape == 1) {
      const double v1 = 16 + std::floor(s1 * (V - 30));
      const double u1 = 16 + std::floor(s2 * (U - 30));
      fp = {{0, 0, U - 1, v1}, {0, v1 + 1, u1, V - 1}};
    } else if (shape == 2) {
      fp = {{0, 0, U - 1, std::floor(V / 2)}, {std::floor(U / 4), 0, std::floor((3 * U) / 4), V - 1}};
    } else
      fp = {{-3, 2, U + 4, V - 3}};
    const double cr = r();
    const double cs = r();
    const double ck = r();
    std::optional<Chamfer> ch;
    if (cr < 0.3) ch = Chamfer{cs < 0.5 ? 'L' : 'R', 20 * (1 + std::floor(ck * 2)), 21 * (1 + std::floor(ck * 2))};
    FloorGrid::Cut cut = nullptr;
    if (ch) cut = [c = *ch, U](double u, double v) { return chamfer_cut(c, U, u, v); };
    FloorGrid g(U, V, fp, cut);
    const double dr = r();
    const double dy = r();
    g.door_extra = dr < 0.25 ? door_extra_of(static_cast<int>(std::floor(dy * 132))) : 0;
    out << (Line() << "grid" << i << U << V << shape << join(fp, "|", frect) << (ch ? js::cat(ch->side, ch->a, "/", ch->b) : std::string("-")) << g.door_extra
                   << rle(g.cells));
    out << (Line() << "inner" << i << join(g.inner_rects(), "|", frect));

    const Rect inner = g.inner_rects()[0];
    const Rect bx{js::max(inner.x0, 2), js::max(inner.y0, 2), js::min(inner.x1, U - 3), js::min(inner.y1, V - 3)};
    const double story_h = 20 + std::floor(r() * 16);
    const bool lane_low = r() < 0.5;
    const StairDims dims = stair_dims(story_h);
    const bool want_stair = r() < 0.8;
    std::optional<Stair> st;
    std::shared_ptr<Room> s_room;
    if (want_stair && bx.x1 - bx.x0 + 1 >= dims.W + 20 && bx.y1 - bx.y0 + 1 >= dims.L + 6) {
      MakeStairOpts so;
      so.rect = {bx.x0, bx.y0, bx.x0 + dims.W - 1, bx.y0 + dims.L - 1};
      so.axis = 'v';
      so.dir = 1;
      so.lane_low = lane_low;
      so.f0 = 0;
      so.f1 = 2;
      st = make_stair(so);
      st->id = 0;
      s_room = add_stair_room(g, *st);
    }
    const double cw = 6 + std::floor(r() * 6);
    const double cx0 = st ? st->rect.x1 + 2 : bx.x0;
    RoomProps cp;
    cp.paint = "PAINT_CREAM";
    cp.floor_mat = "FLOOR_TERRAZZO";
    const auto corr = g.add_room("corridor", {{cx0, bx.y0, bx.x1, bx.y0 + cw - 1}}, cp);
    // rooms below the corridor, one or two rows
    const Rect band{cx0, bx.y0 + cw + 1, bx.x1, bx.y1};
    const bool two_rows = band.y1 - band.y0 + 1 >= 30 && r() < 0.6;
    const double mid = two_rows ? band.y0 + std::floor((band.y1 - band.y0) * (0.35 + 0.3 * r())) : band.y1;
    const double target = 10 + std::floor(r() * 30);
    Rng rng(std::floor(r() * 4294967296.0));
    std::vector<std::shared_ptr<Room>> front, back;
    if (band.x1 - band.x0 + 1 >= 8 && mid - band.y0 + 1 >= 5) {
      for (const auto& p : split_length(band.x0, band.x1, target, 7, rng)) {
        const char* type = kTypes[static_cast<size_t>(std::floor(r() * 6))];
        front.push_back(g.add_room(type, {{p[0], band.y0, p[1], mid}}));
      }
      if (two_rows && band.y1 - (mid + 2) + 1 >= 5) {
        for (const auto& p : split_length(band.x0, band.x1, target + 6, 8, rng)) {
          const char* type = kTypes[static_cast<size_t>(std::floor(r() * 6))];
          back.push_back(g.add_room(type, {{p[0], mid + 2, p[1], band.y1}, {p[0], band.y1 + 1, p[0] - 1, band.y1}}));
        }
      }
    }
    // doors
    if (st) {
      DoorOpts so;
      const double sw = r();
      const double sk = r();
      if (sw < 0.5) so.width = 6 + std::floor(sw * 8);
      if (sk < 0.3) so.kind = "opening";
      if (sk > 0.8) so.leaf = "glass";
      const auto d = stair_door(g, *s_room, *st, corr.get(), so);
      out << (Line() << "sd" << i << id_or(d));
    }
    {
      const DoorOpts o = door_opts(r, bx.x0, bx.x1);
      out << (Line() << "ed" << i << id_or(g.add_door(*corr, nullptr, o)));
    }
    for (const auto& m : front) {
      const DoorOpts o = door_opts(r, m->rects[0].x0, m->rects[0].x1);
      const auto d = g.add_door(*m, corr.get(), o);
      DoorOpts o2;
      o2.width = 6;
      o2.margin = 1;
      const auto d2 = d ? nullptr : g.add_door(*m, corr.get(), o2);
      out << (Line() << "fd" << i << m->id << id_or(d) << id_or(d2));
    }
    for (const auto& m : back) {
      const Room* best = nullptr;
      double best_ov = 0;
      for (const auto& q : front) {
        const double ov = js::min(q->rects[0].x1, m->rects[0].x1) - js::max(q->rects[0].x0, m->rects[0].x0);
        if (!best || ov > best_ov) {
          best = q.get();
          best_ov = ov;
        }
      }
      const DoorOpts o = door_opts(r, m->rects[0].x0, m->rects[0].x1);
      const auto d = best ? g.add_door(*m, best, o) : nullptr;
      std::shared_ptr<Door> outd;
      if (r() < 0.4) {
        const DoorOpts oo = door_opts(r, m->rects[0].x0, m->rects[0].x1);
        outd = g.add_door(*m, nullptr, oo);
      }
      out << (Line() << "bd" << i << m->id << id_or(d) << id_or(outd));
    }
    // snapshot, a probe room and door, restore
    const FloorGrid::Snapshot snap = g.snapshot();
    const std::string before = rle(g.cells);
    const double px = bx.x0 + std::floor(r() * 10);
    const double py = bx.y1 - std::floor(r() * 10);
    const auto probe = g.add_room("probe", {{px, py - 6, px + 6, py}});
    DoorOpts po;
    po.width = 6;
    po.margin = 1;
    const auto pd = g.add_door(*probe, front.empty() ? corr.get() : front[0].get(), po);
    out << (Line() << "probe" << i << static_cast<double>(g.rooms.size()) << static_cast<double>(g.doors.size()) << id_or(pd) << (rle(g.cells) == before));
    g.restore(snap);
    out << (Line() << "restored" << i << static_cast<double>(g.rooms.size()) << static_cast<double>(g.doors.size()) << (rle(g.cells) == before));
    // extend a room into free cells below the stair
    if (st && !front.empty()) {
      const Rect ext{st->rect.x0, st->rect.y1 + 2, st->rect.x1, st->rect.y1 + 2 + std::floor(r() * 8)};
      const bool is_free = g.is_free(ext);
      const bool extended = g.extend_room(*front[0], ext);
      out << (Line() << "ext" << i << frect(ext) << is_free << extended << static_cast<double>(front[0]->rects.size()));
    }
    out << (Line() << "cells" << i << rle(g.cells));
    for (const auto& m : g.rooms) {
      std::string sides;
      for (char c : facade_sides_of(g, m->rects.empty() ? Rect{0, 0, 0, 0} : m->rects[0])) sides += c;
      out << (Line() << "room" << i << froom(*m) << g.area(*m) << sides);
    }
    for (const auto& d : g.doors) {
      const auto leaf = g.door_leaf_rect(*d);
      out << (Line() << "door" << i << fdoor(*d) << (leaf ? frect(*leaf) : std::string("-")));
    }
    for (const auto& m : g.rooms)
      out << (Line() << "runs" << i << m->id << "c" << join(g.wall_runs(*m, corr.get()), ";", frun) << "x" << join(g.wall_runs(*m, nullptr), ";", frun));
    for (const auto& m : back)
      for (const auto& q : front) out << (Line() << "runs2" << i << m->id << q->id << join(g.wall_runs(*m, q.get()), ";", frun));
    const DoorGraph adj = g.door_graph();
    std::string graph;
    for (size_t k = 0; k < adj.keys.size(); ++k) {
      if (k) graph += ' ';
      graph += js::cat(adj.keys[k], ">");
      for (size_t s = 0; s < adj.sets[k].size(); ++s) graph += js::cat(s ? "/" : "", adj.sets[k][s]);
    }
    out << (Line() << "graph" << i << graph);
    std::string at, free;
    for (int k = 0; k < 40; ++k) {
      const double u = std::floor(r() * (U + 4)) - 2;
      const double v = std::floor(r() * (V + 4)) - 2;
      const Room* m = g.room_at(u, v);
      if (k) at += ' ';
      at += js::cat(g.get(u, v), "/", m ? js::num(m->id) : std::string("-"));
      const double x1 = u + std::floor(r() * 8) - 1;
      const double y1 = v + std::floor(r() * 8) - 1;
      free += g.is_free({u, v, x1, y1}) ? '1' : '0';
    }
    out << (Line() << "at" << i << at << free);
    g.set(-1, 3, 7);
    g.set(U, 3, 7);
    g.set(1, 1, 65536 + 5);
    g.fill({U - 3, V - 3, U + 5, V + 5}, 70000);
    g.paint_inside({-2, -2, 4, 4}, 99);
    out << (Line() << "edit" << i << g.get(1, 1) << g.get(U - 1, V - 1) << rle(g.cells));
  }
  CHECK(rec::record("floorgrid", out.text()) == rec::recorded_digest("floorgrid"));
}

TEST_CASE("city floor grid: rooms and doors are shared records") {
  FloorGrid g(20, 20, {{0, 0, 19, 19}});
  CHECK(g.get(0, 0) == FloorGrid::EXT);
  CHECK(g.get(5, 5) == FloorGrid::WALL);
  CHECK(g.get(-1, 5) == FloorGrid::OUT);
  const auto a = g.add_room("a", {{2, 2, 8, 17}});
  const auto b = g.add_room("b", {{10, 2, 17, 17}});
  const auto snap = g.snapshot();
  const auto d = g.add_door(*a, b.get());
  REQUIRE(d);
  CHECK(d->kind == "interior");
  CHECK(g.room_at(3, 3) == a.get());
  g.restore(snap);
  CHECK(g.doors.empty());
  CHECK(d->a == 0);  // (the door a planner kept lives on)
  a->type = "shaft";
  CHECK(g.rooms[0]->type == "shaft");
}
