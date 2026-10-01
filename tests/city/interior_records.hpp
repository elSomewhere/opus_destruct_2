// svx_city tests — records of building plans for the interior stages (interiors,
// interiorstreets): the C++ twin of tools/procgen_ref/lib/interiors.mjs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "building_records.hpp"
#include "buildings/interior/grid.hpp"
#include "buildings/interior/plan.hpp"
#include "records.hpp"

namespace svx::city::test {

// FNV-1a over a label grid's cells (16-bit values).
inline uint32_t cell_hash(const std::vector<uint16_t>& cells) {
  uint32_t h = 2166136261u;
  for (uint16_t c : cells) h = (h ^ c) * 16777619u;
  return h;
}

inline std::string rects_str(const std::vector<Rect>& v, const char* sep) {
  if (v.empty()) return "-";
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + rect_str(v[i]);
  return s;
}
// (a number JS may leave undefined: NaN here)
inline std::string fnan(double v) { return v == v ? js::num(v) : std::string("-"); }
inline std::string fo(const std::optional<char>& c) { return c ? std::string(1, *c) : std::string("-"); }
inline std::string fo(const std::optional<uint16_t>& v) { return v ? js::num(*v) : std::string("-"); }

// A room: every field a planner gives one, and its area.
inline std::string room_line(const FloorGrid& g, const Room& m) {
  std::string stalls = "-";
  if (!m.stalls.empty()) {
    stalls.clear();
    for (size_t i = 0; i < m.stalls.size(); ++i) {
      const GarageStall& s = m.stalls[i];
      stalls += js::cat(i ? ";" : "", s.x0, ",", s.x1, ",", s.y0, ",", s.y1, ",", s.nose_u, ",", s.car ? 1 : 0);
    }
  }
  rec::Line l;
  l << "rm" << m.id << m.type << rects_str(m.rects, "|") << fo(m.paint) << fo(m.floor_mat) << test::fo(m.stair) << test::fo(m.elevator) << fo(m.unit) << fo(m.template_)
    << test::fo(m.ceiling) << test::fo(m.deck_h) << fo(m.front) << m.shop << m.tall << m.no_windows << m.classical << m.fire << stalls << rects_str(m.pillars, ";")
    << (m.ramp_rect ? rect_str(*m.ramp_rect) : std::string("-")) << (m.link_to ? js::cat(m.link_to->floor, "/", m.link_to->room) : std::string("-")) << g.area(m);
  return l.str();
}

// A door: its cells, rooms, kind and leaf, what the street levels and the planners add, its leaf's
// resting rect.
inline std::string door_line(const FloorGrid& g, const Door& d) {
  const std::optional<Rect> leaf = g.door_leaf_rect(d);
  rec::Line l;
  l << "dr" << d.id << d.u0 << d.u1 << d.v0 << d.v1 << d.orient << d.a << d.b << d.kind << d.side_a << d.width << d.leaf << test::fo(d.height) << test::fo(d.street)
    << test::fo(d.sill) << fo(d.color) << d.misplaced << (leaf ? rect_str(*leaf) : std::string("-"));
  return l.str();
}

// A floor grid: its size, door extra, footprint, cut, a hash of its cells, its rooms and doors.
inline void interior_grid_records(rec::Out& out, const FloorGrid& g) {
  out << (rec::Line() << "gc" << g.U << g.V << g.door_extra << rects_str(g.footprint, "|") << static_cast<bool>(g.cut) << static_cast<double>(cell_hash(g.cells))
                      << static_cast<double>(g.rooms.size()) << static_cast<double>(g.doors.size()));
  for (const auto& m : g.rooms) out << (rec::Line() << room_line(g, *m));
  for (const auto& d : g.doors) out << (rec::Line() << door_line(g, *d));
}

// A plan: its stairs, elevators, ramps, links, its floors (each distinct grid once) and issues.
inline void plan_records(rec::Out& out, const BuildingPlan* plan) {
  if (!plan) {
    out << (rec::Line() << "plan" << "-");
    return;
  }
  out << (rec::Line() << "plan" << static_cast<double>(plan->floors.size()) << static_cast<double>(plan->stairs.size()) << static_cast<double>(plan->elevators.size())
                      << static_cast<double>(plan->ramps.size()) << static_cast<double>(plan->links.size()) << static_cast<double>(plan->issues.size()));
  for (const auto& s : plan->stairs) {
    std::string flights = "-";
    if (s->flights && !s->flights->empty()) {
      flights.clear();
      for (size_t k = 0; k < s->flights->size(); ++k) {
        const StairFlight& q = (*s->flights)[k];
        flights += js::cat(k ? ";" : "", q.f, "/", q.z0, "/", fnan(q.H));  // (H: floorHeight's undefined)
      }
    }
    out << (rec::Line() << "st" << test::fo(s->id) << rect_str(s->rect) << s->axis << s->dir << s->lane_low << s->lane << s->landing << s->f0 << s->f1 << s->L << s->W << s->open
                        << flights);
  }
  for (const Elevator& e : plan->elevators) out << (rec::Line() << "el" << e.id << rect_str(e.rect) << e.f0 << e.f1 << e.door_side);
  for (const Ramp& r : plan->ramps) out << (rec::Line() << "ra" << r.id << rect_str(r.rect) << r.f << fnan(r.H) << (r.pitch ? js::num(r.pitch) : std::string("-")));
  for (const PlanLink& l : plan->links) out << (rec::Line() << "ln" << l.fa << l.ra << l.fb << l.rb << l.ramp);
  std::vector<const FloorGrid*> grids;
  for (const PlanFloor& fl : plan->floors) {
    double gi = -1;
    for (size_t i = 0; i < grids.size(); ++i)
      if (grids[i] == fl.grid.get()) gi = static_cast<double>(i);
    const bool fresh = gi < 0;
    if (fresh) {
      gi = static_cast<double>(grids.size());
      grids.push_back(fl.grid.get());
    }
    std::string low = "-";
    if (!fl.low_regions.empty()) {
      low.clear();
      for (size_t i = 0; i < fl.low_regions.size(); ++i) low += js::cat(i ? ";" : "", rect_str(fl.low_regions[i].rect), "/", fl.low_regions[i].height);
    }
    std::string ramps = "-";
    if (!fl.ramps.empty()) {
      ramps.clear();
      for (size_t i = 0; i < fl.ramps.size(); ++i) ramps += js::cat(i ? "," : "", fl.ramps[i].id);
    }
    out << (rec::Line() << "fl" << fl.index << fl.z << fnan(fl.height) << fl.kind << gi << fl.mezzanine << low << ramps << (plan->floor_by_index(fl.index) == &fl));
    if (fresh) interior_grid_records(out, *fl.grid);
  }
  for (const PlanIssue& q : plan->issues) out << (rec::Line() << "is" << q.floor << q.room << q.type << q.msg);
}

}  // namespace svx::city::test
