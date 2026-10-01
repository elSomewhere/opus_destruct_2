// svx_city tests — shared inputs and records of the building stages (archetypes, houses, facade,
// sample): the C++ twin of tools/procgen_ref/lib/buildings.mjs. The districts an envelope may be
// planned in, the configs of every world, scripted lots (plain and turned) and the envelope
// records.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "buildings/archetypes.hpp"
#include "buildings/interior/grid.hpp"
#include "city/districts.hpp"
#include "city/flavors.hpp"
#include "config/defaults.hpp"
#include "records.hpp"
#include "world/register_all.hpp"
#include "worlds.hpp"

namespace svx::city::test {

// Every district, then each one as every flavor sees it (village flavors too).
inline std::vector<District> district_list() {
  register_all();
  std::vector<District> out(district_registry().all().begin(), district_registry().all().end());
  std::vector<const Flavor*> fls;
  for (const Flavor& fl : flavor_registry().all()) fls.push_back(&fl);
  for (const Flavor& fl : flavor_registry().all()) {
    FlavorSettlement s;
    s.village = true;
    s.flavor = fl.id;
    fls.push_back(&flavor_of(&s));
  }
  for (const District& d : district_registry().all())
    for (const Flavor* fl : fls) out.push_back(flavored_district(d, *fl));
  return out;
}

// The merged config of every world (all_worlds), in its order.
inline std::vector<Value> config_list() {
  std::vector<Value> out;
  for (const WorldCase& w : all_worlds()) out.push_back(make_config(w.overrides));
  return out;
}

// A scripted lot whose frame is U x V cells (lib/buildings.mjs scriptedLot).
inline Lot scripted_lot(rec::Samples& r, double U, double V, double k, const std::string& district_id) {
  const bool turned = r() < 0.25;
  const double fi = std::floor(r() * 4);
  const bool corner = r() < 0.3;
  const bool micro = r() < 0.12;
  const double x0 = std::floor((r() - 0.5) * 400000);
  const double y0 = std::floor((r() - 0.5) * 400000);
  Lot lot;
  lot.id = js::cat("C", std::fmod(k, 9) - 4, "_", std::fmod(k, 7) - 3, "/b", std::fmod(k, 13), "/l", k);
  lot.district = district_id;
  lot.corner = corner;
  lot.micro = micro;
  if (turned) {
    const int yaw = static_cast<int>(std::floor(r() * 132));
    const double ou = std::floor(r() * 41) - 20;
    const double ov = std::floor(r() * 41) - 20;
    lot.front = nominal_front(yaw);
    Turn t;
    t.yaw = yaw;
    t.origin = {x0, y0};
    t.ou = ou;
    t.ov = ov;
    t.U = U;
    t.V = V;
    lot.turn = t;
    lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
  } else {
    lot.front = "NESW"[static_cast<int>(fi)];
    const bool ns = lot.front == 'N' || lot.front == 'S';
    lot.rect = {x0, y0, x0 + (ns ? U : V) - 1, y0 + (ns ? V : U) - 1};
  }
  return lot;
}

// ---- record fields (rec.mjs's f: undefined "-")

inline std::string fnum(double v) { return js::num(v); }
inline std::string fopt(double v) { return v == v ? js::num(v) : std::string("-"); }  // (NaN: undefined)
inline std::string fopt(const std::optional<double>& v) { return v ? js::num(*v) : std::string("-"); }
inline std::string fopt(const std::optional<bool>& v) { return v ? std::string(*v ? "1" : "0") : std::string("-"); }
inline std::string fstr(const std::string& s) { return s.empty() ? std::string("-") : s; }  // ("": undefined or null)
inline std::string fbool(bool b) { return b ? "1" : "0"; }
inline std::string rect_str(const Rect& q) { return js::cat(q.x0, ",", q.y0, ",", q.x1, ",", q.y1); }
inline std::string nums(const std::vector<double>& v) { return rec::f(v); }

inline std::string tiers_str(const std::vector<EnvTier>& tiers) {
  if (tiers.empty()) return "-";
  std::string s;
  for (size_t i = 0; i < tiers.size(); ++i) {
    if (i) s += '|';
    s += js::cat(tiers[i].f0, "/", tiers[i].f1, "/");
    for (size_t k = 0; k < tiers[i].rects.size(); ++k) s += (k ? ";" : "") + rect_str(tiers[i].rects[k]);
  }
  return s;
}

inline std::string roof_str(const EnvRoof& o) {
  std::string overhang = "-";
  if (o.overhang_kind == EnvRoof::Overhang::Number)
    overhang = js::num(o.overhang);
  else if (o.overhang_kind == EnvRoof::Overhang::Sides)
    overhang = "{F:" + fopt(o.overhang_f) + ",B:" + fopt(o.overhang_b) + ",L:" + fopt(o.overhang_l) + ",R:" + fopt(o.overhang_r) + "}";
  return o.type + "/" + fopt(o.pitch) + "/" + fstr(o.ridge) + "/" + fopt(o.slope) + "/" + overhang + "/" + fopt(o.crown) + "/" + fopt(o.chimney);
}

inline std::string program_str(const EnvProgram& p) { return fstr(p.ground) + "/" + fstr(p.podium) + "/" + fstr(p.upper); }

inline std::string steeple_str(const std::optional<EnvSteeple>& s) {
  if (!s) return "-";
  return js::cat(s->rect, "/", s->shaft, "/", s->spire, "/", s->dome ? js::num(*s->dome) : std::string("-"), "/", fopt(s->tent));
}

inline std::string domes_str(const std::vector<EnvDome>& d) {
  if (d.empty()) return "-";
  std::string s;
  for (size_t i = 0; i < d.size(); ++i) s += js::cat(i ? ";" : "", d[i].rect, "/", d[i].fv, "/", d[i].r, "/", d[i].drum, "/", d[i].h, "/", d[i].m);
  return s;
}

inline std::string civic_str(const std::optional<CivicExtra>& x) {
  if (!x) return "-";
  return js::cat(fstr(x->civic), "/", fbool(x->storefront), "/", fstr(x->sign), "/", x->sign_color, "/", fbool(x->portico), "/", fbool(x->parking), "/", x->set_f);
}

inline std::string turn_str(const std::optional<Turn>& t) {
  if (!t) return "-";
  return js::cat(t->yaw, "/", t->yaw2 ? js::num(t->yaw2) : std::string("-"), "/", t->origin.x, "/", t->origin.y, "/", t->ou, "/", t->ov);
}

// An archetype's raw envelope as a record (specLine).
inline std::string spec_line(const std::optional<EnvSpec>& env) {
  if (!env) return "spec -";
  std::string annexes = "-";
  if (!env->annexes.empty()) {
    annexes.clear();
    for (size_t i = 0; i < env->annexes.size(); ++i) annexes += js::cat(i ? "|" : "", env->annexes[i].kind, "/", rect_str(env->annexes[i].rect), "/", env->annexes[i].height);
  }
  std::string s = "spec";
  for (const std::string& x : {tiers_str(env->tiers), annexes, fnum(env->floors), nums(env->story_h), fnum(env->basements), roof_str(env->roof), program_str(env->program),
                               fopt(env->podium_floors), fstr(env->entrance_side), fbool(env->stoop), fbool(env->yard), fbool(env->plinth), fbool(env->porch),
                               steeple_str(env->steeple), domes_str(env->domes), fstr(env->force_style), civic_str(env->extra)})
    s += " " + x;
  return s;
}

// A building's envelope as a record (envLine).
inline std::string env_line(const std::optional<Envelope>& e) {
  if (!e) return "env -";
  std::string annexes = "-";
  if (!e->annexes.empty()) {
    annexes.clear();
    for (size_t i = 0; i < e->annexes.size(); ++i) {
      const EnvelopeAnnex& a = e->annexes[i];
      annexes += js::cat(i ? "|" : "", a.kind, "/", rect_str(a.rect), "/", a.height, "/", rect_str(a.world), "/", a.canon ? rect_str(*a.canon) : std::string("-"));
    }
  }
  std::string s = "env";
  for (const std::string& x :
       {fbool(e->mirror), fnum(e->entrance_u), e->id, e->lot, e->archetype, e->style, fstr(e->district), std::string(1, e->front), rect_str(e->R), fnum(e->U), fnum(e->V),
        tiers_str(e->tiers), annexes, fnum(e->floors), nums(e->story_h), fnum(e->basements), fnum(e->basement_h), fnum(e->base_z), fnum(e->ground_z), roof_str(e->roof),
        program_str(e->program), fnum(e->podium_floors), fbool(e->stoop), fbool(e->yard), std::string(e->plinth ? "1" : "-"), std::string(e->porch ? "1" : "-"),
        steeple_str(e->steeple), domes_str(e->domes), civic_str(e->extra), fnum(e->top_z), fnum(e->bottom_z), rect_str(e->bounds), turn_str(e->turn)})
    s += " " + x;
  return s;
}

// ---- floor grids (gridRecords)

// Run lengths of a label grid: value:count,...
inline std::string grid_rle(const std::vector<uint16_t>& a) {
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
inline std::string fo(const std::optional<double>& v) { return v ? js::num(*v) : "-"; }
inline std::string fo(const std::optional<std::string>& s) { return s ? (s->empty() ? "\"\"" : *s) : "-"; }
inline std::string fs(const std::string& s) { return s.empty() ? "\"\"" : s; }
inline std::string door_str(const Door& d) {
  return js::cat(d.id, ",", d.u0, ",", d.u1, ",", d.v0, ",", d.v1, ",", d.orient, ",", d.a, ",", d.b, ",", fs(d.kind), ",", fs(d.side_a), ",", d.width, ",", fs(d.leaf), ",",
                 fo(d.height));
}
inline std::string room_str(const Room& m) {
  std::string rects;
  for (size_t k = 0; k < m.rects.size(); ++k) rects += js::cat(k ? "|" : "", rect_str(m.rects[k]));
  return js::cat(m.id, ",", fs(m.type), ",", fs(rects), ",", fo(m.paint), ",", fo(m.floor_mat), ",", fo(m.stair), ",", fo(m.unit), ",", fo(m.template_));
}
// A floor grid's records: its cells, rooms, doors with their leaves, the door graph.
inline void grid_records(rec::Out& out, const std::string& tag, const FloorGrid& g) {
  const std::string cells = grid_rle(g.cells);
  out << (rec::Line() << tag + "c" << g.U << g.V << g.door_extra << (cells.empty() ? std::string("-") : cells));
  for (const auto& m : g.rooms) out << (rec::Line() << tag + "r" << room_str(*m) << g.area(*m));
  for (const auto& d : g.doors) {
    const std::optional<Rect> leaf = g.door_leaf_rect(*d);
    out << (rec::Line() << tag + "d" << door_str(*d) << (leaf ? rect_str(*leaf) : std::string("-")));
  }
  const DoorGraph adj = g.door_graph();
  std::string graph;
  for (size_t k = 0; k < adj.keys.size(); ++k) {
    if (k) graph += ' ';
    graph += js::cat(adj.keys[k], ">");
    for (size_t s = 0; s < adj.sets[k].size(); ++s) graph += js::cat(s ? "/" : "", adj.sets[k][s]);
  }
  out << (rec::Line() << tag + "g" << (graph.empty() ? std::string("-") : graph));
}

}  // namespace svx::city::test
