// svx_city — voxel_city world/landmarks.js.
#include "world/landmarks.hpp"

#include <cmath>
#include <optional>

#include "buildings/interior/prefabs.hpp"
#include "city/industry.hpp"
#include "city/propPrefabs.hpp"
#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "core/obb.hpp"
#include "network/roadSurface.hpp"
#include "network/roadView.hpp"
#include "svx/base/types.hpp"
#include "terrain/terrain.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;

// A prop (city/propPrefabs) placed in the world at (x, y, z), its local a axis along (ax, ay), b
// along (bx, by): boxes, bounds and footprint (none without boxes). A plinth: a granite plinth down
// into uneven ground.
std::optional<Landmark> make_prop(double seed, const std::string& kind, double x, double y, double z, double ax, double ay, double bx, double by, bool plinth) {
  Rng rng = Rng::from(seed, "landmark", kind, x, y);
  PropOpts o;
  if (plinth) o.plinth = true;
  std::vector<PrefabBox> boxes = prop(kind)->build(rng, o);
  if (plinth) boxes.push_back(PrefabBox{-16, 16, -16, 16, -12, -1, MAT::GRANITE});
  Landmark it;
  it.kind = kind;
  for (const PrefabBox& q : boxes) {
    const double xa = x + q.a0 * ax + q.b0 * bx;
    const double ya = y + q.a0 * ay + q.b0 * by;
    const double xb = x + q.a1 * ax + q.b1 * bx;
    const double yb = y + q.a1 * ay + q.b1 * by;
    it.boxes.push_back({js::round(js::min(xa, xb)), js::round(js::min(ya, yb)), js::round(js::max(xa, xb)), js::round(js::max(ya, yb)), z + q.z0, z + q.z1, q.m});
  }
  if (it.boxes.empty()) return std::nullopt;
  const LandmarkBox& f = it.boxes[0];
  Box3 bb{f.x0, f.y0, f.z0, f.x1, f.y1, f.z1};
  for (size_t k = 1; k < it.boxes.size(); ++k) {
    const LandmarkBox& q = it.boxes[k];
    bb = {js::min(bb.x0, q.x0), js::min(bb.y0, q.y0), js::min(bb.z0, q.z0), js::max(bb.x1, q.x1), js::max(bb.y1, q.y1), js::max(bb.z1, q.z1)};
  }
  it.bb = bb;
  it.foot = {bb.x0, bb.y0, bb.x1, bb.y1};
  return it;
}

}  // namespace

Landmarks::Landmarks(const World& world) : world_(world) {}

const Landmarks::Plan& Landmarks::plan() const {
  return plan_.get([&] { return make_plan(); });
}

const std::vector<Landmark>& Landmarks::all() const { return plan().items; }

std::vector<const Landmark*> Landmarks::near(const Rect& rect) const {
  const Plan& P = plan();
  std::vector<uint32_t> found;
  P.grid.query(rect, found);
  std::vector<const Landmark*> out;
  out.reserve(found.size());
  for (const uint32_t k : found) out.push_back(&P.items[k]);
  return out;
}

bool Landmarks::blocks(double x, double y, double margin) const {
  const Plan& P = plan();
  std::vector<uint32_t> found;
  P.grid.query(Rect{x - margin, y - margin, x + margin, y + margin}, found);
  for (const uint32_t k : found) {
    const Rect& b = P.items[k].foot;
    if (x >= b.x0 - margin && x <= b.x1 + margin && y >= b.y0 - margin && y <= b.y1 + margin) return true;
  }
  return false;
}

bool Landmarks::free(double x, double y) const {
  if (free_source) return free_source(x, y);
  const World& w = world_;
  const CellIJ c = w.cell_at(x, y);
  const std::shared_ptr<const RoadView> view = w.road_view(c.i, c.j);
  RoadSample rs = make_road_sample();
  sample_road_surface(view->near(Rect{x - 2, y - 2, x + 2, y + 2}), x + 0.5, y + 0.5, rs, w.seed);
  if (rs.kind != RoadKind::NONE || rs.sdf_r < vx(3)) return false;
  // (plan.lotAt(x, y) || plan.spaceAt(x, y) of world.cellPlan(c.i, c.j))
  if (!plan_occupied) SVX_FAIL("landmarks: the open-ground test needs the cell plan's lots and spaces (Landmarks::plan_occupied)");
  return !plan_occupied(x, y);
}

Landmarks::Plan Landmarks::make_plan() const {
  Plan P;
  const World& w = world_;
  const IslandPlan* isl = w.fields->island.get();
  if (!isl) return P;
  const double seed = w.seed;
  Rng rng = Rng::from(seed, "landmarks");
  auto coast = [&](double x, double y) { return isl->coast(x / 8, y / 8); };
  struct Taken {
    double x, y, r;
  };
  std::vector<Taken> taken;
  auto clash = [&](double x, double y, double r) {
    for (const Taken& t : taken)
      if (js::hypot(t.x - x, t.y - y) < t.r + r) return true;
    return false;
  };
  // unit vector towards the water from a shore point
  auto seaward = [&](double x, double y) -> XY {
    const double c = coast(x, y);
    const double gx = coast(x + 48, y) - c;
    const double gy = coast(x, y + 48) - c;
    const double g = js::or_(js::hypot(gx, gy), 1);
    return {-gx / g, -gy / g};
  };
  auto ground = [&](double x, double y) { return js::round(w.terrain->sample(x, y).h); };
  // first point seaward of (x, y) along (nx, ny) where the ground meets the sea (1 m steps)
  auto waterline = [&](double x, double y, double nx, double ny) -> std::optional<XY> {
    for (double t = 0; t <= 45 * 8; t += 8)
      if (w.terrain->sample(x + nx * t, y + ny * t).h < 3) return XY{x + nx * t, y + ny * t};
    return std::nullopt;
  };
  auto add = [&](const char* kind, double x, double y, double z, double ax, double ay, double bx, double by, double r, bool plinth) {
    std::optional<Landmark> it = make_prop(seed, kind, x, y, z, ax, ay, bx, by, plinth);
    if (!it) return;
    P.grid.insert(static_cast<uint32_t>(P.items.size()), Rect{it->bb.x0, it->bb.y0, it->bb.x1, it->bb.y1});
    P.items.push_back(std::move(*it));
    taken.push_back({x, y, r});
  };

  // the lighthouse: the most exposed low headland 300-1400 m from the harbour
  const std::optional<Harbour>& h = isl->harbour();
  if (h) {
    bool has_best = false;
    double best_x = 0, best_y = 0, best_score = 0;
    for (double r = 300; r <= 1400; r += 50)
      for (double a = 0; a < 72; a += 1) {
        const double t = (a / 72) * kPi * 2;
        const double x = (h->x + js::cos(t) * r) * 8;
        const double y = (h->y + js::sin(t) * r) * 8;
        const double c = coast(x, y);
        if (c < 8 || c > 40) continue;
        double sea = 0;
        for (double k = 0; k < 16; k += 1)
          if (coast(x + js::cos((k / 16) * kPi * 2) * 1000, y + js::sin((k / 16) * kPi * 2) * 1000) < 0) sea += 1;
        const double score = sea / 16 - std::fabs(r - 650) / 3000 + hash_float(seed, a, r, 71) * 0.05;
        if (score > 0.4 && (!has_best || score > best_score) && free(x, y)) {
          has_best = true;
          best_x = x;
          best_y = y;
          best_score = score;
        }
      }
    if (has_best) add("lighthouse", js::round(best_x), js::round(best_y), ground(best_x, best_y) + 1, 1, 0, 0, 1, vx(30), true);
  }

  // boathouses in rows along the shore near each place; fish racks by the hamlets
  const IslandSettlements& S = w.fields->island_settlements();
  std::vector<const Settlement*> places;
  for (const auto& s : S.towns) places.push_back(s.get());
  for (const auto& s : S.villages) places.push_back(s.get());
  for (const Settlement* p : places) {
    const double want = p->hamlet ? 4 : p->village ? 5 : 7;
    const double R = p->radius / 8 + 450;
    double placed = 0;
    for (double r = 60; r <= R && placed < want; r += 35)
      for (double a = 0; a < 90 && placed < want; a += 1) {
        const double t = (a / 90) * kPi * 2 + hash_float(seed, js::to_int32(p->x), a, 72);
        const double x = js::round(p->x + js::cos(t) * r * 8);
        const double y = js::round(p->y + js::sin(t) * r * 8);
        const double c = coast(x, y);
        if (c < 2 || c > 24 || isl->cliff(x / 8, y / 8) > 0.6) continue;
        if (clash(x, y, vx(6)) || !free(x, y)) continue;
        const XY n = seaward(x, y);
        const double nx = n[0];
        const double ny = n[1];
        // the real waterline (the ground under a town is graded above the sea)
        const std::optional<XY> wl = waterline(x, y, nx, ny);
        if (!wl) continue;
        // a row of two to four sheds side by side, their doors at the water
        const double nn = 2 + static_cast<double>(hash32(seed, x, y, 73) % 3u);
        for (double k = 0; k < nn && placed < want; k += 1) {
          const double off = (k - (nn - 1) / 2) * vx(5.5);
          const double qx = (*wl)[0] - ny * off;
          const double qy = (*wl)[1] + nx * off;
          const std::optional<XY> w2o = waterline(qx - nx * vx(4), qy - ny * vx(4), nx, ny);
          const XY w2 = w2o ? *w2o : XY{qx, qy};
          const double bx = js::round(w2[0] - nx * vx(7.5));
          const double by = js::round(w2[1] - ny * vx(7.5));
          const double z = ground(bx, by);
          // low shore only (no sheds up on a bank), dry land behind
          if (z > 3 * 8 || z < 2 || !free(bx, by) || clash(bx, by, vx(2.5))) continue;
          add("boathouse", bx, by, z + 1, -ny, nx, nx, ny, vx(3), false);
          placed += 1;
        }
      }
    if (!p->hamlet && !p->village) continue;
    // stockfish racks on open ground near the shore
    for (double k = 0, racks = 0; k < 40 && racks < 2; k += 1) {
      const double t = rng.float_(0, kPi * 2);
      const double r = rng.float_(40, 220);
      const double x = js::round(p->x + js::cos(t) * r * 8);
      const double y = js::round(p->y + js::sin(t) * r * 8);
      const double c = coast(x, y);
      if (c < 12 || c > 160 || clash(x, y, vx(14)) || !free(x, y) || !free(x + vx(10), y)) continue;
      add("fishRack", x, y, ground(x, y) + 1, 1, 0, 0, 1, vx(14), false);
      racks += 1;
    }
  }

  // cairns: the highest point of each 1.2 km square of the fells (the high ones, half a dozen at
  // most, the highest first)
  const Rect b = isl->bounds();
  const double cell = 1200;
  struct Top {
    double x, y, h;
  };
  std::vector<Top> tops;
  for (double cy = b.y0; cy < b.y1; cy += cell)
    for (double cx = b.x0; cx < b.x1; cx += cell) {
      bool has = false;
      Top top{0, 0, 0};
      for (double y = cy; y < cy + cell; y += 60)
        for (double x = cx; x < cx + cell; x += 60) {
          if (isl->coast(x, y) < 150) continue;
          const double hz = w.terrain->sample(x * 8, y * 8).h;
          if (!has || hz > top.h) {
            has = true;
            top = {x, y, hz};
          }
        }
      if (has && top.h >= js::max(90.0, 0.45 * isl->cfg["peak"].num(400)) * 8) tops.push_back(top);
    }
  js::sort(tops, [](const Top& p, const Top& q) { return q.h - p.h; });
  for (size_t i = 0; i < tops.size() && i < 6; ++i) {
    // climb to the very top in 6 m steps
    double x = tops[i].x;
    double y = tops[i].y;
    for (int k = 0; k < 20; ++k) {
      bool has_nb = false;
      Top nb{0, 0, 0};
      const double dirs[4][2] = {{6, 0}, {-6, 0}, {0, 6}, {0, -6}};
      for (const auto& d : dirs) {
        const double hz = w.terrain->sample((x + d[0]) * 8, (y + d[1]) * 8).h;
        if (hz > (has_nb ? nb.h : w.terrain->sample(x * 8, y * 8).h)) {
          has_nb = true;
          nb = {x + d[0], y + d[1], hz};
        }
      }
      if (!has_nb) break;
      x = nb.x;
      y = nb.y;
    }
    const double px = js::round(x * 8);
    const double py = js::round(y * 8);
    if (free(px, py)) add("cairn", px, py, ground(px, py) + 1, 1, 0, 0, 1, vx(3), false);
  }
  return P;
}

bool landmark_z_range(const World& world, const Rect& rect, double* z0, double* z1) {
  if (!world.landmarks) return false;
  double lo = js::kInf;
  double hi = -js::kInf;
  for (const Landmark* it : world.landmarks->near(rect)) {
    lo = js::min(lo, it->bb.z0);
    hi = js::max(hi, it->bb.z1);
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void rasterize_landmarks(const World& world, ChunkBuffer& chunk) {
  if (!world.landmarks) return;
  const Box3 box = chunk.world_box();
  for (const Landmark* it : world.landmarks->near(Rect{box.x0, box.y0, box.x1, box.y1})) {
    // small things drop out at a distance, the lighthouse stays
    if (chunk.lod >= 3 && it->kind != "lighthouse" && it->kind != "boathouse") continue;
    if (it->bb.z1 < box.z0 || it->bb.z0 > box.z1) continue;
    for (const LandmarkBox& q : it->boxes)
      if (q.m) chunk.fill_box(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, 0);
  }
}

}  // namespace svx::city
