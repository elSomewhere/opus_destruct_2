// svx_city — voxel_city world/sites.js.
#include "world/sites.hpp"

#include "core/hash.hpp"
#include "core/math.hpp"
#include "network/arterials.hpp"
#include "network/highways.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "world/caches.hpp"
#include "world/chart.hpp"
#include "world/fields.hpp"

namespace svx::city {

const Registry<SiteDef>& site_registry() { return site_registry_mut(); }

Registry<SiteDef>& site_registry_mut() {
  static Registry<SiteDef> r("site");
  return r;
}

// Path pads: a ribbon of half width `half` along a polyline of points { x, y, z } (a road on a
// slope): the distance beyond the ribbon's edge, and the interpolated level, the arc length and
// the distance to the centre line at the nearest point.
PathHit path_distance(const SitePad& p, double x, double y) {
  const std::vector<SitePathPoint>& pts = p.path;
  double best = js::kInf;
  double s0 = 0;
  PathHit hit;
  for (size_t k = 0; k + 1 < pts.size(); ++k) {
    const SitePathPoint& a = pts[k];
    const SitePathPoint& b = pts[k + 1];
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double L2 = js::or_(dx * dx + dy * dy, 1);
    const double t = js::max(0, js::min(1, ((x - a.x) * dx + (y - a.y) * dy) / L2));
    const double d = js::hypot(x - (a.x + dx * t), y - (a.y + dy * t));
    const double L = std::sqrt(L2);
    if (d < best) {
      best = d;
      hit.z = a.z + (b.z - a.z) * t;
      hit.s = s0 + L * t;
      hit.c = d;
    }
    s0 += L;
  }
  hit.d = js::max(0, best - p.half);
  return hit;
}

double pad_distance(const SitePad& p, double x, double y) {
  if (!p.path.empty()) return path_distance(p, x, y).d;
  const Rect& r = p.rect;
  return js::hypot(js::max(r.x0 - x, 0, x - r.x1), js::max(r.y0 - y, 0, y - r.y1));
}

namespace {

// Level of a pad at (x, y): flat (z), a ramp rising linearly along ramp.axis from z0 at a0 to z1
// at a1 (roads on slopes), or a path pad's level at its nearest point (hit).
double pad_z(const SitePad& p, double x, double y, const PathHit& hit) {
  if (!p.path.empty()) return hit.z;
  if (!p.ramp) return p.z;
  const SiteRamp& r = *p.ramp;
  const double a = r.axis == 'x' ? x : y;
  const double t = js::max(0, js::min(1, (a - r.a0) / js::or_(r.a1 - r.a0, 1)));
  return r.z0 + (r.z1 - r.z0) * t;
}

}  // namespace

const SiteStructure& Site::structure(const World& w) const {
  return structure_.get([&] {
    if (!def->structure) SVX_FAIL("sites: a site kind without a structure");
    return def->structure(w, *this);
  });
}

SiteLayer::SiteLayer(const World& world) : SiteLayer(world, {}) {
  for (const SiteDef& d : site_registry().all()) defs_.push_back(&d);
}

SiteLayer::SiteLayer(const World& world, std::vector<const SiteDef*> defs) : world_(world), defs_(std::move(defs)) {
  const Value& wc = world.config["world"];
  const double cell_m = wc["siteCell"].num(5200);
  cell = vx(cell_m);
  // a wrapping world has n site cells round it
  wrap = wrap_of(world.config);
  n = wrap.count(cell_m);
  face_margin_ = wc["faceMargin"].to_number();
  sea_level_ = wc["seaLevel"].to_number();
}

std::shared_ptr<const Site> SiteLayer::site_at(double a, double b) const {
  return cache_.get(cell_key(a, b), [&] { return build(a, b); });
}

std::shared_ptr<const Site> SiteLayer::build(double a, double b) const {
  const World& w = world_;
  const double seed = w.seed;
  if (defs_.empty()) return nullptr;
  // a wrapping world: canonical seeds, the anchor moved by whole laps (the site itself is planned
  // again in place, so its geometry is local)
  const double ca = Wrap::canon(a, n);
  const double cb = Wrap::canon(b, n);
  const double lap_x = Wrap::lap(a, n) * wrap.size_v;
  const double lap_y = Wrap::lap(b, n) * wrap.size_v;
  const double r = hash_float(seed, ca, cb, 881);
  double acc = 0;
  const SiteDef* def = nullptr;
  for (const SiteDef* d : defs_) {
    acc += d->frequency;
    if (r < acc) {
      def = d;
      break;
    }
  }
  if (!def) return nullptr;
  // anchor: a jittered point -> its arterial cell -> a centred rect inside it
  const double px = (ca + 0.5 + (hash_float(seed, ca, cb, 882) - 0.5) * 0.6) * cell + lap_x;
  const double py = (cb + 0.5 + (hash_float(seed, ca, cb, 883) - 0.5) * 0.6) * cell + lap_y;
  if (w.chart->edge_distance(px / 8, py / 8) < face_margin_) return nullptr;
  const CellIJ c = w.cell_at(px, py);
  const Rect cr = w.arterials->cell_rect(c.i, c.j);
  const Urban ur = w.fields->urban((cr.x0 + cr.x1) / 2, (cr.y0 + cr.y1) / 2);
  if (ur.u < def->min_u || ur.u > def->max_u) return nullptr;
  const double sw = vx(def->size[0]);
  const double sh = vx(def->size[1]);
  const double margin = vx(def->margin.value_or(60));
  if (cr.x1 - cr.x0 < sw + 2 * margin + vx(40) || cr.y1 - cr.y0 < sh + 2 * margin + vx(40)) return nullptr;
  const double x0 = js::round((cr.x0 + cr.x1 - sw) / 2);
  const double y0 = js::round((cr.y0 + cr.y1 - sh) / 2);
  const Rect rect{x0, y0, x0 + sw - 1, y0 + sh - 1};
  const double cx = (rect.x0 + rect.x1) / 2;
  const double cy = (rect.y0 + rect.y1) / 2;
  // on an island: the whole site well inside the shore
  if (w.fields->island) {
    const double corners[5][2] = {{cx, cy}, {rect.x0, rect.y0}, {rect.x1, rect.y0}, {rect.x0, rect.y1}, {rect.x1, rect.y1}};
    for (const auto& q : corners)
      if (w.fields->coast_distance(q[0], q[1]) < 150) return nullptr;
  }
  // placement: custom (def.place) or a level pad over the whole rect
  std::shared_ptr<const SitePlaced> placed;
  if (def->place) {
    placed = def->place(w, {rect, cr, margin, static_cast<double>(hash32(seed, ca, cb, 885))});
    if (!placed) return nullptr;
  } else {
    double h_sum = 0;
    double h_min = js::kInf;
    double h_max = -js::kInf;
    const double fs[5][2] = {{0.5, 0.5}, {0.1, 0.1}, {0.9, 0.1}, {0.1, 0.9}, {0.9, 0.9}};
    for (const auto& f : fs) {
      const double h = w.terrain->sample(rect.x0 + (rect.x1 - rect.x0) * f[0], rect.y0 + (rect.y1 - rect.y0) * f[1]).h;
      h_sum += h;
      h_min = js::min(h_min, h);
      h_max = js::max(h_max, h);
    }
    if (h_max - h_min > vx(def->max_relief.value_or(25))) return nullptr;
    const double pz = js::max(js::round(h_sum / 5), js::round(sea_level_ * 8) + 16);
    auto p = std::make_shared<SitePlaced>();
    p->pad_z = pz;
    SitePad pad;
    pad.rect = rect;
    pad.z = pz;
    pad.margin = margin;
    p->pads.push_back(pad);
    p->footprint = rect;
    placed = std::move(p);
  }
  // createWorld's world keeps sites off its highways' corridors and its water within the margin
  // round the footprint (a World of World.js has neither: no highways, and no waterHitsRect - its
  // rivers and lakes, which create_world installs with it)
  const Rect& fp = placed->footprint;
  const Rect fpm{fp.x0 - margin, fp.y0 - margin, fp.x1 + margin, fp.y1 + margin};
  if (w.highways)
    for (const HighwayCorridor& hc : w.highways->corridors_near(fpm))
      if (hc.hits_rect(fpm)) return nullptr;
  if (w.rivers && w.lakes && w.water_hits_rect(fpm, 10)) return nullptr;
  auto site = std::make_shared<Site>();
  site->id = js::cat("site:", def->id, ":", ca, "_", cb);
  site->type = def->id;
  site->def = def;
  site->a = a;
  site->b = b;
  site->cell = c;
  site->cell_rect = cr;
  site->rect = rect;
  site->blend = {rect.x0 - margin, rect.y0 - margin, rect.x1 + margin, rect.y1 + margin};
  site->margin = margin;
  site->pad_z = placed->pad_z;
  site->placed = std::move(placed);
  site->seed = static_cast<double>(hash32(seed, ca, cb, 884));
  site->center = {cx, cy};
  if (!def->plan) SVX_FAIL("sites: a site kind without a plan");
  site->plan = def->plan(w, *site);
  return site;
}

std::vector<std::shared_ptr<const Site>> SiteLayer::sites_near(const Rect& rect) const {
  std::vector<std::shared_ptr<const Site>> out;
  const double a0 = std::floor(rect.x0 / cell) - 1;
  const double a1 = std::floor(rect.x1 / cell) + 1;
  const double b0 = std::floor(rect.y0 / cell) - 1;
  const double b1 = std::floor(rect.y1 / cell) + 1;
  for (double b = b0; b <= b1; b += 1)
    for (double a = a0; a <= a1; a += 1) {
      std::shared_ptr<const Site> s = site_at(a, b);
      if (!s) continue;
      const Rect r = s->plan && s->plan->bounds ? *s->plan->bounds : s->blend;
      if (r.x0 <= rect.x1 && rect.x0 <= r.x1 && r.y0 <= rect.y1 && rect.y0 <= r.y1) out.push_back(std::move(s));
    }
  return out;
}

std::vector<std::shared_ptr<const Site>> SiteLayer::sites_in_cell(double i, double j) const {
  std::vector<std::shared_ptr<const Site>> out;
  for (std::shared_ptr<const Site>& s : sites_near(world_.arterials->cell_rect(i, j)))
    if (s->cell.i == i && s->cell.j == j) out.push_back(std::move(s));
  return out;
}

std::optional<SiteGround> SiteLayer::ground(double x, double y, double natural_z) const {
  for (const std::shared_ptr<const Site>& s : sites_near({x, y, x, y})) {
    // the nearest pad wins where blend zones overlap (a road's switchbacks)
    const SitePad* best = nullptr;
    double best_k = js::kInf;
    double best_d = 0;
    PathHit best_hit;
    for (const SitePad& p : s->pads()) {
      const Rect& r = p.rect;
      const double m = p.margin;
      if (x < r.x0 - m || x > r.x1 + m || y < r.y0 - m || y > r.y1 + m) continue;
      PathHit hit;
      double d;
      if (!p.path.empty()) {
        hit = path_distance(p, x, y);
        d = hit.d;
      } else {
        d = js::hypot(js::max(r.x0 - x, 0, x - r.x1), js::max(r.y0 - y, 0, y - r.y1));
      }
      if (p.round && d >= m) continue;
      const double k = d / m;
      if (k < best_k) {
        best_k = k;
        best = &p;
        best_d = d;
        best_hit = hit;
      }
    }
    if (!best) continue;
    const SitePad& p = *best;
    const double t = smoothstep(0, p.margin, best_d);
    SiteGround out;
    out.z = js::round(pad_z(p, x, y, best_hit) * (1 - t) + natural_z * t);
    out.mat = 0;
    out.sub = 0;
    out.site = s;
    out.pad = &p;
    out.natural = natural_z;
    out.inside = best_d == 0;
    if (!p.path.empty()) {
      out.path_z = best_hit.z;
      out.path_s = best_hit.s;
      out.path_c = best_hit.c;
    }
    if (s->def->ground) s->def->ground(*s, x, y, out);
    return out;
  }
  return std::nullopt;
}

std::vector<SiteLayer::MapItem> SiteLayer::map_data(const Rect& rect) const {
  std::vector<MapItem> out;
  for (const std::shared_ptr<const Site>& s : sites_near(rect)) out.push_back({s->id, s->type, s->rect, s->center});
  return out;
}

std::vector<SiteLayer::Nearest> SiteLayer::nearest(double x, double y, double n_max, double radius_cells) const {
  const double a = std::floor(x / cell);
  const double b = std::floor(y / cell);
  std::vector<Nearest> out;
  for (double db = -radius_cells; db <= radius_cells; db += 1)
    for (double da = -radius_cells; da <= radius_cells; da += 1) {
      const std::shared_ptr<const Site> s = site_at(a + da, b + db);
      if (s) out.push_back({s->id, s->type, s->center.x, s->center.y, js::hypot(s->center.x - x, s->center.y - y), s->pad_z});
    }
  js::sort(out, [](const Nearest& p, const Nearest& q) { return p.d - q.d; });
  // (slice(0, n): n truncated, a negative n counting from the end)
  const double size = static_cast<double>(out.size());
  double end = n_max == n_max ? std::trunc(n_max) : 0;
  end = end < 0 ? js::max(size + end, 0) : js::min(end, size);
  out.resize(static_cast<size_t>(end));
  return out;
}

bool site_source_z_range(const World& w, const Rect& rect, double* z0, double* z1) {
  double lo = js::kInf;
  double hi = -js::kInf;
  auto hit = [&](double x0, double y0, double x1, double y1) { return !(x1 < rect.x0 || x0 > rect.x1 || y1 < rect.y0 || y0 > rect.y1); };
  std::vector<uint32_t> qs;
  for (const std::shared_ptr<const Site>& s : w.sites->sites_near(rect)) {
    const SiteStructure& st = s->structure(w);
    if (!st.bb || !hit(st.bb->x0, st.bb->y0, st.bb->x1, st.bb->y1)) continue;
    qs.clear();
    st.grid.query(rect, qs);
    for (const uint32_t i : qs) {
      const SiteBox& q = st.boxes[i];
      if (!hit(q.x0, q.y0, q.x1, q.y1)) continue;
      if (q.z0 < lo) lo = q.z0;
      if (q.z1 > hi) hi = q.z1;
    }
    for (const SiteVolume& c : st.custom) {
      if (!hit(c.bb.x0, c.bb.y0, c.bb.x1, c.bb.y1)) continue;
      lo = js::min(lo, c.bb.z0);
      hi = js::max(hi, c.bb.z1);
    }
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void site_source_rasterize(const World& w, ChunkBuffer& chunk, double tile_z_min) {
  const Box3 box = chunk.world_box();
  // coarse LODs: rooms and tunnels sealed in the rock below the ground are invisible
  const double deep = chunk.lod >= 3 ? tile_z_min - 16 : -js::kInf;
  for (const std::shared_ptr<const Site>& s : w.sites->sites_near({box.x0, box.y0, box.x1, box.y1})) rasterize_structure(s->structure(w), chunk, deep);
}

}  // namespace svx::city
