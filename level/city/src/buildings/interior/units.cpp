// svx_city — voxel_city buildings/interior/units.js.
#include "buildings/interior/units.hpp"

#include <array>

#include "buildings/frame.hpp"

namespace svx::city {

namespace {

constexpr double M = 8;  // voxels per metre

using Span = std::array<double, 2>;

// Splits [a0, a1] into spans of the given widths separated by 1-cell walls; the last span absorbs
// the rest.
std::vector<Span> spans(double a0, double a1, const std::vector<double>& widths) {
  std::vector<Span> out;
  double a = a0;
  for (size_t k = 0; k < widths.size(); ++k) {
    const bool last = k == widths.size() - 1;
    const double e = last ? a1 : a + widths[k] - 1;
    out.push_back({a, e});
    a = e + 2;
  }
  return out;
}

// Splits [a0, a1] into n near-equal spans separated by walls.
std::vector<Span> even_spans(double a0, double a1, double n) {
  const double total = a1 - a0 + 1 - (n - 1);
  const double base = std::floor(total / n);
  const std::vector<double> widths(static_cast<size_t>(n), base);
  return spans(a0, a1, widths);
}

Rect rect_of(double u0, double v0, double u1, double v1) { return {u0, v0, u1, v1}; }

double bedrooms_for_area(double area_m2) {
  if (area_m2 < 33) return 0;
  if (area_m2 < 52) return 1;
  if (area_m2 < 80) return 2;
  return 3;
}

// A template's room (unit frame) and door ([a, b, opts]).
struct LRoom {
  std::string key, type;
  Rect rect;
  bool entry = false, facade = false;
  std::string merge;  // ("": none) the room it extends
};
struct LDoor {
  std::string a, b;
  DoorOpts o;
};
struct Layout {
  std::vector<LRoom> rooms;
  std::vector<LDoor> doors;
};

LRoom room(const char* key, const char* type, const Rect& r, bool entry = false, bool facade = false) { return {key, type, r, entry, facade, ""}; }
DoorOpts opening(double width, const char* place = nullptr) {
  DoorOpts o;
  o.kind = "opening";
  o.width = width;
  if (place) o.place = place;
  return o;
}
DoorOpts width_only(double width) {
  DoorOpts o;
  o.width = width;
  return o;
}
DoorOpts place_only(const char* place) {
  DoorOpts o;
  o.place = place;
  return o;
}

// --- templates (unit frame: u in [0, U - 1], v in [0, V - 1], entry at v = 0)

std::optional<Layout> t_band(double U, double V, double nb, Rng& rng) {
  Layout L;
  const double s1 = js::max(18.0, js::min(23.0, js::round(V * 0.3)));
  const double fw = rng.int_(11, 14);
  if (nb == 0) {
    const double bw = js::max(14.0, js::min(20.0, U - fw - 1));
    if (U < fw + 1 + 14 || V < s1 + 1 + 24) return std::nullopt;
    const std::vector<Span> sp = spans(0, U - 1, {bw, fw});
    const Span& ba = sp[0];
    const Span& f = sp[1];
    L.rooms.push_back(room("Ba", "bath", rect_of(ba[0], 0, ba[1], s1 - 1)));
    L.rooms.push_back(room("F", "foyer", rect_of(f[0], 0, f[1], s1 - 1), true));
    L.rooms.push_back(room("Main", "studio", rect_of(0, s1 + 1, U - 1, V - 1), false, true));
    L.doors.push_back({"F", "Main", opening(js::min(12.0, f[1] - f[0] - 1), "center")});
    L.doors.push_back({"F", "Ba", width_only(6)});
    return L;
  }
  const bool hall = nb >= 2;
  const double hd = hall ? rng.int_(9, 10) : 0;
  const double bed_top = hall ? s1 + hd + 2 : s1 + 1;
  if (V - bed_top < 24) return std::nullopt;
  const double kw = rng.int_(20, 26);
  double lw;
  if (nb == 1)
    lw = js::max(32.0, kw + 1 + fw);
  else
    lw = rng.int_(32, 38);
  const double beds_w = U - lw - 1;
  if (beds_w < nb * 22 + (nb - 1)) return std::nullopt;
  if (nb == 1) {
    const double ba_w = U - (kw + fw + 2);
    if (ba_w < 14) return std::nullopt;
    L.rooms.push_back(room("K", "kitchen", rect_of(0, 0, kw - 1, s1 - 1)));
    L.rooms.push_back(room("F", "foyer", rect_of(kw + 1, 0, kw + fw, s1 - 1), true));
    L.rooms.push_back(room("Ba", "bath", rect_of(kw + fw + 2, 0, U - 1, s1 - 1)));
    L.rooms.push_back(room("L", "living", rect_of(0, s1 + 1, lw - 1, V - 1), false, true));
    L.rooms.push_back(room("B1", "bedroom", rect_of(lw + 1, s1 + 1, U - 1, V - 1), false, true));
    L.doors.push_back({"F", "L", opening(js::min(10.0, fw - 2), "center")});
    L.doors.push_back({"K", "L", opening(js::min(14.0, kw - 4), "center")});
    L.doors.push_back({"F", "Ba", width_only(6)});
    L.doors.push_back({"L", "B1", DoorOpts{}});
    return L;
  }
  // 2-3 bedrooms with a hall
  const double svc_w = U - (lw + fw + 2);
  if (svc_w < 14) return std::nullopt;
  L.rooms.push_back(room("K", "kitchen", rect_of(0, 0, lw - 1, s1 - 1)));
  L.rooms.push_back(room("F", "foyer", rect_of(lw + 1, 0, lw + fw, s1 - 1), true));
  // service rooms right of the foyer: bath (+ wc / closet when wide)
  const double sv0 = lw + fw + 2;
  if (svc_w >= 14 + 1 + 10 + (nb >= 3 ? 11 : 0)) {
    const double bath = js::max(14.0, js::min(22.0, svc_w - 11 - (nb >= 3 ? 12 : 0)));
    const std::vector<double> parts = nb >= 3 && svc_w >= bath + 1 + 10 + 1 + 10 ? std::vector<double>{bath, 10, 10} : std::vector<double>{bath, 10};
    const std::vector<Span> sp = spans(sv0, U - 1, parts);
    L.rooms.push_back(room("Ba", "bath", rect_of(sp[0][0], 0, sp[0][1], s1 - 1)));
    L.rooms.push_back(room("C1", nb >= 3 ? "wc" : "closet", rect_of(sp[1][0], 0, sp[1][1], s1 - 1)));
    if (sp.size() > 2) L.rooms.push_back(room("C2", "closet", rect_of(sp[2][0], 0, sp[2][1], s1 - 1)));
  } else {
    L.rooms.push_back(room("Ba", "bath", rect_of(sv0, 0, U - 1, s1 - 1)));
  }
  L.rooms.push_back(room("H", "hall", rect_of(lw + 1, s1 + 1, U - 1, s1 + hd)));
  L.rooms.push_back(room("L", "living", rect_of(0, s1 + 1, lw - 1, V - 1), false, true));
  const std::vector<Span> beds = even_spans(lw + 1, U - 1, nb);
  for (size_t k = 0; k < beds.size(); ++k) {
    LRoom b{js::cat("B", static_cast<double>(k) + 1), "bedroom", rect_of(beds[k][0], bed_top, beds[k][1], V - 1), false, true, ""};
    L.rooms.push_back(b);
  }
  L.doors.push_back({"F", "H", opening(js::min(9.0, fw - 2), "center")});
  L.doors.push_back({"H", "L", place_only("center")});
  L.doors.push_back({"K", "L", opening(js::min(16.0, lw - 6), "center")});
  L.doors.push_back({"Ba", "H", width_only(6)});
  bool c1 = false, c2 = false;
  for (const LRoom& r : L.rooms) {
    if (r.key == "C1") c1 = true;
    if (r.key == "C2") c2 = true;
  }
  if (c1) L.doors.push_back({"C1", "H", width_only(6)});
  if (c2) L.doors.push_back({"C2", "H", width_only(6)});
  for (double k = 1; k <= nb; k += 1) L.doors.push_back({js::cat("B", k), "H", DoorOpts{}});
  return L;
}

std::optional<Layout> t_through(double U, double V, double nb, Rng& rng) {
  // facades at u = 0 (living) and u = U - 1 (bedrooms), entry at v = 0 in the middle
  Layout L;
  const double lw = rng.int_(30, 36);
  const double bw = rng.int_(24, 30);
  const double mid0 = lw + 1;
  const double mid1 = U - bw - 2;
  const double mid_w = mid1 - mid0 + 1;
  const double nbr = js::min(nb, V >= 2 * 24 + 1 ? 2.0 : 1.0);
  if (nbr < 1) return std::nullopt;
  const bool need_hall = nbr >= 2;
  const double hw = need_hall ? 10 : 0;
  if (mid_w < (need_hall ? hw + 1 + 14 : 14)) return std::nullopt;
  const double fd = rng.int_(12, 16);
  if (V < fd + 1 + 16 + 1 + 18) return std::nullopt;
  L.rooms.push_back(room("L", "living", rect_of(0, 0, lw - 1, V - 1), false, true));
  const std::vector<Span> beds = even_spans(0, V - 1, nbr);
  for (size_t k = 0; k < beds.size(); ++k) {
    LRoom b{js::cat("B", static_cast<double>(k) + 1), "bedroom", rect_of(U - bw, beds[k][0], U - 1, beds[k][1]), false, true, ""};
    L.rooms.push_back(b);
  }
  const double svc1 = need_hall ? mid1 - hw - 1 : mid1;
  L.rooms.push_back(room("F", "foyer", rect_of(mid0, 0, mid1, fd - 1), true));
  if (need_hall) L.rooms.push_back(room("H", "hall", rect_of(svc1 + 2, fd + 1, mid1, V - 1)));
  const double bd = js::max(16.0, js::min(22.0, js::round((V - fd - 1) * 0.45)));
  L.rooms.push_back(room("Ba", "bath", rect_of(mid0, fd + 1, svc1, fd + bd)));
  L.rooms.push_back(room("K", "kitchen", rect_of(mid0, fd + bd + 2, svc1, V - 1)));
  L.doors.push_back({"F", "L", place_only("center")});
  L.doors.push_back({"F", "Ba", width_only(6)});
  L.doors.push_back({"K", "L", opening(10, "center")});
  if (need_hall) {
    L.doors.push_back({"F", "H", opening(js::min(8.0, hw - 2), "center")});
    L.doors.push_back({"B1", "H", DoorOpts{}});
    L.doors.push_back({"B2", "H", DoorOpts{}});
  } else {
    L.doors.push_back({"F", "B1", DoorOpts{}});
  }
  return L;
}

std::optional<Layout> t_rail(double U, double V, double nb, Rng& rng) {
  Layout L;
  const double hw = rng.int_(9, 11);
  const double ld = rng.int_(30, 40);
  if (V < ld + 1 + 18 + 1 + 16 || U < hw + 1 + 18) return std::nullopt;
  const bool with_bed = nb >= 1 && U >= 32 + 1 + 22;
  const double col_end = V - ld - 2;
  L.rooms.push_back(room("H", "foyer", rect_of(0, 0, hw - 1, col_end), true));
  if (with_bed) {
    const std::vector<Span> lb = spans(0, U - 1, {U - 22 - 1 - js::max(0.0, U - 60), 22 + js::max(0.0, U - 60)});
    L.rooms.push_back(room("L", "living", rect_of(lb[0][0], V - ld, lb[0][1], V - 1), false, true));
    L.rooms.push_back(room("B1", "bedroom", rect_of(lb[1][0], V - ld, lb[1][1], V - 1), false, true));
  } else {
    L.rooms.push_back(room("L", "living", rect_of(0, V - ld, U - 1, V - 1), false, true));
  }
  const double col_len = col_end + 1;
  Span ba{0, col_end};
  std::optional<Span> k;
  if (col_len >= 16 + 1 + 18) {
    const std::vector<Span> sp = spans(0, col_end, {16, col_len});
    ba = sp[0];
    k = sp[1];
  }
  L.rooms.push_back(room("Ba", "bath", rect_of(hw + 1, ba[0], U - 1, ba[1])));
  if (k) L.rooms.push_back(room("K", "kitchen", rect_of(hw + 1, (*k)[0], U - 1, (*k)[1])));
  L.doors.push_back({"H", "L", place_only("center")});
  L.doors.push_back({"H", "Ba", width_only(6)});
  if (k) {
    DoorOpts o;
    o.kind = "opening";
    o.width = 9;
    L.doors.push_back({"H", "K", o});
  }
  if (with_bed) L.doors.push_back({"L", "B1", DoorOpts{}});
  return L;
}

std::optional<Layout> t_open(double U, double V) {
  if (U < 24 || V < 26) return std::nullopt;
  Layout L;
  const double bw = js::min(18.0, std::floor(U * 0.45));
  const double bd = js::min(18.0, std::floor(V * 0.4));
  L.rooms.push_back(room("Ba", "bath", rect_of(0, 0, bw - 1, bd - 1)));
  L.rooms.push_back(room("Main", "studio", rect_of(bw + 1, 0, U - 1, V - 1), true, true));
  LRoom m2 = room("Main2", "studio", rect_of(0, bd + 1, bw, V - 1));
  m2.merge = "Main";
  L.rooms.push_back(m2);
  L.doors.push_back({"Main", "Ba", width_only(6)});
  return L;
}

// TEMPLATES[name](U, V, nb, rng)
std::optional<Layout> run_template(const std::string& name, double U, double V, double nb, Rng& rng) {
  if (name == "band") return t_band(U, V, nb, rng);
  if (name == "through") return t_through(U, V, nb, rng);
  if (name == "rail") return t_rail(U, V, nb, rng);
  return t_open(U, V);
}

// u' coordinates (unit frame) along the entry wall that face the circulation room.
std::vector<double> entry_cells(const FloorGrid& grid, const Frame& frame, const Room& circ) {
  std::vector<double> out;
  const double lab = 16 + circ.id;
  for (double u = 0; u < frame.U; u += 1) {
    const XY w = frame.to_world(u, 0);
    const XY o = frame.to_world(u, -2);
    if (grid.get(o[0], o[1]) == lab && grid.get(w[0], w[1]) == 2) out.push_back(u);
  }
  return out;
}

double entry_overlap(const Layout& layout, const std::vector<double>& entry_range) {
  const LRoom* e = nullptr;
  for (const LRoom& r : layout.rooms)
    if (r.entry) {
      e = &r;
      break;
    }
  if (!e || e->rect.y0 != 0) return 0;
  double n = 0;
  for (double u : entry_range)
    if (u >= e->rect.x0 && u <= e->rect.x1) n += 1;
  return n;
}

Layout mirror_layout(const Layout& layout, double U) {
  Layout out;
  for (const LRoom& r : layout.rooms) {
    LRoom m = r;
    m.rect = {U - 1 - r.rect.x1, r.rect.y0, U - 1 - r.rect.x0, r.rect.y1};
    out.rooms.push_back(m);
  }
  out.doors = layout.doors;
  return out;
}

std::shared_ptr<Room> by_key(const std::vector<std::pair<std::string, std::shared_ptr<Room>>>& m, const std::string& key) {
  for (const auto& p : m)
    if (p.first == key) return p.second;
  return nullptr;
}

std::optional<UnitPlan> try_layout(FloorGrid& grid, const Frame& frame, const Layout& layout, const Room& circ, const UnitOpts& opts, const UnitStyle& unit_style,
                                   const std::string& template_name) {
  const FloorGrid::Snapshot snap = grid.snapshot();
  // (a Map: key -> room, in insertion order; keys are unique)
  std::vector<std::pair<std::string, std::shared_ptr<Room>>> keyed;
  for (const LRoom& r : layout.rooms) {
    const Rect w = frame.rect_to_world(r.rect);
    if (!r.merge.empty()) {
      const std::shared_ptr<Room> target = by_key(keyed, r.merge);
      if (!target || !grid.extend_room(*target, w)) {
        grid.restore(snap);
        return std::nullopt;
      }
      continue;
    }
    if (!grid.is_free(w)) {
      grid.restore(snap);
      return std::nullopt;
    }
    RoomProps props;
    props.unit = opts.unit;
    props.paint = r.type == "bath" || r.type == "wc" ? unit_style.tile : unit_style.paint;
    props.floor_mat = floor_for(r.type, unit_style);
    props.template_ = template_name;
    keyed.emplace_back(r.key, grid.add_room(r.type, {w}, props));
  }
  const LRoom* entry_room = nullptr;
  for (const LRoom& r : layout.rooms)
    if (r.entry) {
      entry_room = &r;
      break;
    }
  if (!entry_room) SVX_FAIL("units: a template without an entry room");
  const std::shared_ptr<Room> entry = by_key(keyed, entry_room->key);
  if (!entry) SVX_FAIL("units: the entry room merged into another");
  DoorOpts eo;
  eo.width = 8;
  eo.margin = 1;
  eo.kind = "entry";
  eo.place = opts.entry_near ? "near" : "center";
  eo.near = opts.entry_near;
  eo.leaf = "wood";
  std::shared_ptr<Door> entry_door = grid.add_door(*entry, &circ, eo);
  if (!entry_door) {
    grid.restore(snap);
    return std::nullopt;
  }
  for (const LDoor& d : layout.doors) {
    const std::shared_ptr<Room> ra = by_key(keyed, d.a);
    const std::shared_ptr<Room> rb = by_key(keyed, d.b);
    if (!ra || !rb) continue;
    const std::shared_ptr<Door> made = grid.add_door(*ra, rb.get(), d.o);
    if (!made) {
      // an opening that does not fit may fall back to a normal door
      std::shared_ptr<Door> d2;
      if (d.o.kind && *d.o.kind == "opening") d2 = grid.add_door(*ra, rb.get(), width_only(7));
      if (!d2) {
        grid.restore(snap);
        return std::nullopt;
      }
    }
  }
  UnitPlan out;
  for (const auto& p : keyed) out.rooms.push_back(p.second);
  out.entry_door = entry_door;
  out.template_ = template_name;
  return out;
}

const char* const kPaints[] = {"PAINT_WHITE", "PAINT_CREAM", "PAINT_GRAY", "PAINT_SAGE", "PAINT_BLUE", "PAINT_PEACH", "PAINT_MINT", "PAINT_LAVENDER"};
const char* const kTiles[] = {"WALL_TILE_WHITE", "WALL_TILE_BLUE", "WALL_TILE_GREEN"};
const char* const kWoods[] = {"FLOOR_OAK", "FLOOR_WALNUT", "FLOOR_PARQUET", "FLOOR_CARPET_BEIGE", "FLOOR_CARPET_GRAY"};
const char* const kWets[] = {"FLOOR_TILE_WHITE", "FLOOR_TILE_GRAY", "FLOOR_TILE_TERRA"};

}  // namespace

std::optional<UnitPlan> plan_unit(FloorGrid& grid, const Rect& rect, char entry_side, const Room& circ, const std::vector<char>& facades, const UnitOpts& opts) {
  Rng& rng = *opts.rng;
  const Frame frame(rect, entry_side);
  const double U = frame.U;
  const double V = frame.V;
  bool has_l = false, has_r = false, has_b = false;
  for (char s : facades) {
    const char c = frame.canon_side(s);
    if (c == 'L') has_l = true;
    if (c == 'R') has_r = true;
    if (c == 'B') has_b = true;
  }
  const double area_m2 = (U * V) / (M * M);
  const double nb_max = bedrooms_for_area(area_m2);
  std::vector<std::string> order;
  if (has_l && has_r && !has_b)
    order = {"through", "open"};
  else if (has_b)
    order = U < 56 && V > 64 ? std::vector<std::string>{"rail", "band", "open"} : std::vector<std::string>{"band", "rail", "open"};
  else if (has_l || has_r)
    order = {"through", "band", "open"};
  else
    order = {"band", "open"};

  const UnitStyle unit_style = pick_unit_style(rng);
  const std::vector<double> entry_range = entry_cells(grid, frame, circ);
  if (entry_range.empty()) return std::nullopt;
  for (const std::string& name : order) {
    for (double nb = nb_max; nb >= 0; nb -= 1) {
      const int variants = name == "open" ? 1 : 3;
      for (int attempt = 0; attempt < variants; ++attempt) {
        const std::optional<Layout> layout = run_template(name, U, V, nb, rng);
        if (!layout) continue;
        // try the layout and its mirror image; best entry overlap first
        struct Cand {
          Layout l;
          double ov;
        };
        std::vector<Cand> cands;
        for (Layout& l : std::vector<Layout>{*layout, mirror_layout(*layout, U)}) {
          const double ov = entry_overlap(l, entry_range);
          if (ov >= 10) cands.push_back({std::move(l), ov});
        }
        js::sort(cands, [](const Cand& a, const Cand& b) { return b.ov - a.ov; });
        for (const Cand& c : cands) {
          std::optional<UnitPlan> res = try_layout(grid, frame, c.l, circ, opts, unit_style, name);
          if (res) return res;
        }
      }
      if (name == "open") break;
    }
  }
  return std::nullopt;
}

UnitStyle pick_unit_style(Rng& rng) {
  UnitStyle s;
  s.paint = rng.pick(kPaints);
  s.tile = rng.pick(kTiles);
  s.wood = rng.pick(kWoods);
  s.wet = rng.pick(kWets);
  return s;
}

std::string floor_for(const std::string& type, const UnitStyle& st) {
  if (type == "bath" || type == "wc" || type == "kitchen" || type == "laundry") return st.wet;
  if (type == "closet" || type == "storage") return "FLOOR_OAK";
  return st.wood;
}

}  // namespace svx::city
