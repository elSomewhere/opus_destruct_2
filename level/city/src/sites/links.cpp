// svx_city — voxel_city sites/links.js.
#include "sites/links.hpp"

#include <array>

#include "core/hash.hpp"
#include "core/math.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kHalf = 32;      // HALF = vx(4): the carriageway's half width
constexpr double kH = 52;         // H = vx(6.5): the clear height
constexpr double kCover = 240;    // COVER = vx(30)
constexpr double kGrade = 0.06;
constexpr double kStep = 128;     // STEP = vx(16)
constexpr double kMaxLen = 96000;  // MAX_LEN = vx(12000)
constexpr double kChance = 0.75;

std::array<double, 2> point_at(const std::vector<LinkSeg>& segs, double s) {
  for (size_t k = 0; k < segs.size(); ++k) {
    const LinkSeg& g = segs[k];
    if (s <= g.s0 + g.len || k == segs.size() - 1) {
      const double t = js::max(0, js::min(g.len, s - g.s0));
      return g.along_x ? std::array<double, 2>{g.p.x + g.dir_sign * t, g.p.y} : std::array<double, 2>{g.p.x, g.p.y + g.dir_sign * t};
    }
  }
  return {segs[0].p.x, segs[0].p.y};
}

// Along / across coordinates of a column in a segment (false outside its band).
bool local(const LinkSeg& g, double x, double y, double band, double* along, double* across) {
  const double al = g.along_x ? (x - g.p.x) * g.dir_sign : (y - g.p.y) * g.dir_sign;
  const double ac = g.along_x ? y - g.p.y : x - g.p.x;
  if (al < -band || al > g.len + band || std::fabs(ac) > band) return false;
  *along = al;
  *across = ac;
  return true;
}

}  // namespace

size_t SiteLinks::KeyHash::operator()(const Key& k) const {
  const uint64_t a = static_cast<uint64_t>(static_cast<int64_t>(k.a));
  const uint64_t b = static_cast<uint64_t>(static_cast<int64_t>(k.b));
  const uint64_t d = static_cast<uint64_t>(static_cast<int64_t>(k.dir));
  return static_cast<size_t>((a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull));
}

SiteLinks::SiteLinks(const World& world) : world_(world) {}

std::shared_ptr<const SiteLink> SiteLinks::link(double a, double b, double dir) const {
  return cache_.get(Key{a, b, dir}, [&] { return build(a, b, dir); });
}

std::shared_ptr<const SiteLink> SiteLinks::build(double a, double b, double dir) const {
  const World& w = world_;
  const SiteLayer& S = *w.sites;
  // (a wrapping world hashes the canonical lattice cell)
  const double ca = Wrap::canon(a, S.n);
  const double cb = Wrap::canon(b, S.n);
  // (JS's hashFloat(w.seed, ca, cb, dir, 971) and (..., 972): hashFloat takes four numbers, so
  // both are the same hash of (seed, ca, cb, dir))
  const double h = hash_float(w.seed, ca, cb, dir);
  if (h > kChance) return nullptr;
  std::shared_ptr<const Site> sa = S.site_at(a, b);
  std::shared_ptr<const Site> sb = dir == 0 ? S.site_at(a + 1, b) : S.site_at(a, b + 1);
  if (!sa || !sb || !sa->def->port || !sb->def->port) return nullptr;
  // cheap rejections before planning either complex
  const double dc = std::fabs(sa->center.x - sb->center.x) + std::fabs(sa->center.y - sb->center.y);
  if (dc > kMaxLen || std::fabs(sa->pad_z - sb->pad_z) > kGrade * dc + vx(200)) return nullptr;
  const std::optional<SitePort> A = sa->def->port(w, *sa, &sb->center);
  const std::optional<SitePort> B = sb->def->port(w, *sb, &sa->center);
  if (!A || !B) return nullptr;
  // L route: along x then y, or y then x
  const bool x_first = h < 0.5;
  const Point2 corner = x_first ? Point2{B->x, A->y} : Point2{A->x, B->y};
  const Point2 pts[3] = {{A->x, A->y}, corner, {B->x, B->y}};
  auto L = std::make_shared<SiteLink>();
  double len = 0;
  for (int k = 0; k < 2; ++k) {
    const Point2& p = pts[k];
    const Point2& q = pts[k + 1];
    const double l = std::fabs(q.x - p.x) + std::fabs(q.y - p.y);
    if (l == 0) continue;
    const bool along_x = q.y == p.y;
    LinkSeg g;
    g.p = p;
    g.q = q;
    g.along_x = along_x;
    g.s0 = len;
    g.len = l;
    g.dir_sign = along_x ? js::sign(q.x - p.x) : js::sign(q.y - p.y);
    L->segs.push_back(g);
    len += l;
  }
  if (L->segs.empty() || len > kMaxLen) return nullptr;
  if (std::fabs(B->z - A->z) > kGrade * len) return nullptr;
  // profile: a straight grade between the ports, kept COVER under the terrain
  const size_t n = static_cast<size_t>(std::ceil(len / kStep) + 1);
  std::vector<double> up(n);
  for (size_t i = 0; i < n; ++i) {
    const auto xy = point_at(L->segs, js::min(len, static_cast<double>(i) * kStep));
    up[i] = w.terrain->sample(xy[0], xy[1]).h - kCover;
  }
  // grade-limited lower envelope of the cover limit
  for (size_t i = 1; i < n; ++i) up[i] = js::min(up[i], up[i - 1] + kGrade * kStep);
  for (size_t i = n - 1; i-- > 0;) up[i] = js::min(up[i], up[i + 1] + kGrade * kStep);
  std::vector<int32_t>& prof = L->prof;
  prof.assign(n, 0);
  double zlo = js::kInf;
  double zhi = -js::kInf;
  for (size_t i = 0; i < n; ++i) {
    const double t = js::min(1, (static_cast<double>(i) * kStep) / len);
    prof[i] = js::i32(js::round(js::min(A->z + (B->z - A->z) * t, up[i])));
    zlo = js::min(zlo, prof[i]);
    zhi = js::max(zhi, prof[i]);
  }
  // the ports themselves must lie under enough rock
  if (prof[0] < A->z - 2 || prof[n - 1] < B->z - 2) return nullptr;
  prof[0] = js::i32(A->z);
  prof[n - 1] = js::i32(B->z);
  const double pad = kHalf + 4;
  L->bb = {js::min(A->x, B->x, corner.x) - pad, js::min(A->y, B->y, corner.y) - pad, js::max(A->x, B->x, corner.x) + pad, js::max(A->y, B->y, corner.y) + pad};
  for (LinkSeg& s : L->segs) s.rect = {js::min(s.p.x, s.q.x) - pad, js::min(s.p.y, s.q.y) - pad, js::max(s.p.x, s.q.x) + pad, js::max(s.p.y, s.q.y) + pad};
  L->id = js::cat("link:", sa->id, ">", sb->id);
  L->a = std::move(sa);
  L->b = std::move(sb);
  L->A = *A;
  L->B = *B;
  L->len = len;
  L->zlo = zlo;
  L->zhi = zhi;
  return L;
}

std::vector<std::shared_ptr<const SiteLink>> SiteLinks::near(const Rect& rect) const {
  const double cell = world_.sites->cell;
  // a link lies within the bounding box of its two ports, i.e. within the two neighbouring
  // lattice cells (plus site overhang): one cell of reach
  const double reach = 1;
  std::vector<std::shared_ptr<const SiteLink>> out;
  const double a0 = std::floor(rect.x0 / cell) - reach;
  const double a1 = std::floor(rect.x1 / cell) + reach;
  const double b0 = std::floor(rect.y0 / cell) - reach;
  const double b1 = std::floor(rect.y1 / cell) + reach;
  for (double b = b0; b <= b1; b += 1)
    for (double a = a0; a <= a1; a += 1)
      for (double dir = 0; dir < 2; dir += 1) {
        std::shared_ptr<const SiteLink> L = link(a, b, dir);
        if (!L) continue;
        const Rect& r = L->bb;
        if (r.x0 <= rect.x1 && rect.x0 <= r.x1 && r.y0 <= rect.y1 && rect.y0 <= r.y1) out.push_back(std::move(L));
      }
  return out;
}

double SiteLinks::z_at(const SiteLink& L, double s) {
  const double last = static_cast<double>(L.prof.size()) - 1;
  const double f = js::max(0, js::min(static_cast<double>(L.prof.size()) - 1.0001, s / kStep));
  const double i = std::floor(f);
  const double t = f - i;
  return js::round(L.prof[static_cast<size_t>(i)] * (1 - t) + L.prof[static_cast<size_t>(js::min(i + 1, last))] * t);
}

std::vector<SiteLinks::MapItem> SiteLinks::map_data(const Rect& rect) const {
  std::vector<MapItem> out;
  for (const std::shared_ptr<const SiteLink>& L : near(rect)) {
    MapItem m;
    m.id = L->id;
    m.pts.push_back({L->A.x, L->A.y});
    for (const LinkSeg& s : L->segs) m.pts.push_back(s.q);
    out.push_back(std::move(m));
  }
  return out;
}

bool site_links_z_range(const World& w, const Rect& rect, double* z0, double* z1) {
  if (!w.site_links) return false;
  double lo = js::kInf;
  double hi = -js::kInf;
  for (const std::shared_ptr<const SiteLink>& L : w.site_links->near(rect)) {
    for (const LinkSeg& g : L->segs) {
      const Rect& r = g.rect;
      if (r.x0 > rect.x1 || r.x1 < rect.x0 || r.y0 > rect.y1 || r.y1 < rect.y0) continue;
      // arc-length window of the rect on this segment
      const double a0 = g.along_x ? (g.dir_sign > 0 ? rect.x0 - g.p.x : g.p.x - rect.x1) : g.dir_sign > 0 ? rect.y0 - g.p.y : g.p.y - rect.y1;
      const double a1 = a0 + (g.along_x ? rect.x1 - rect.x0 : rect.y1 - rect.y0);
      for (double s = js::max(0, a0) - kStep; s <= js::min(g.len, a1) + kStep; s += kStep / 2) {
        const double z = SiteLinks::z_at(*L, g.s0 + js::max(0, js::min(g.len, s)));
        lo = js::min(lo, z - 3);
        hi = js::max(hi, z + kH + 4);
      }
    }
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void site_links_rasterize(const World& w, ChunkBuffer& chunk) {
  if (!w.site_links) return;
  const Box3 box = chunk.world_box();
  const std::vector<std::shared_ptr<const SiteLink>> links = w.site_links->near({box.x0, box.y0, box.x1, box.y1});
  if (links.empty()) return;
  uint16_t* data = chunk.data.data();
  const double band = kHalf + 2;
  for (const std::shared_ptr<const SiteLink>& L : links)
    for (size_t gi = 0; gi < L->segs.size(); ++gi) {
      const LinkSeg& g = L->segs[gi];
      const Rect& r = g.rect;
      if (r.x0 > box.x1 || r.x1 < box.x0 || r.y0 > box.y1 || r.y1 < box.y0) continue;
      for (int j = 0; j < kP; ++j)
        for (int i = 0; i < kP; ++i) {
          const double x = chunk.wx(i);
          const double y = chunk.wy(j);
          double along, across;
          if (!local(g, x, y, band, &along, &across)) continue;
          // the other segment owns the corner beyond this segment's end
          if (along > g.len + 1 && gi != L->segs.size() - 1) continue;
          if (along < -1 && gi != 0) continue;
          const double z = SiteLinks::z_at(*L, g.s0 + js::max(0, js::min(g.len, along)));
          if (z + kH + 2 < box.z0 || z - 2 > box.z1) continue;
          const double aa = std::fabs(across);
          const int col = i + j * kP;
          const bool wall = aa > kHalf;
          const double chamfer = aa > kHalf - 6 ? aa - (kHalf - 6) : 0;
          const bool walk = aa > kHalf - 8 && aa <= kHalf;
          const double seg = std::fmod(std::fmod(js::round(along), 64) + 64, 64);
          for (int k = 0; k < kP; ++k) {
            const double zz = chunk.wz(k);
            const double dz = zz - z;
            if (dz < -1 || dz > kH + 2) continue;
            const int idx = col + k * kP2;
            const bool lining = dz == -1 || wall || dz > kH - chamfer;
            if (lining && data[idx] == 0) continue;
            uint16_t m;
            if (dz == -1)
              m = MAT::CONCRETE;
            else if (wall || dz > kH - chamfer)
              m = dz >= kH + 1 || wall ? MAT::TUNNEL_WALL : MAT::CONCRETE;
            else if (dz == 0)
              m = walk ? MAT::SIDEWALK : aa < 1 && seg < 24 ? MAT::LINE_YELLOW : MAT::ASPHALT;
            else if (walk && dz <= 2)
              m = MAT::SIDEWALK;
            else if (dz == kH - chamfer && aa < 2 && seg < 8)
              m = MAT::LIGHT_STRIP;
            else if (!wall && aa == kHalf && dz == vx(2.5) && seg < 2)
              m = MAT::EMERGENCY_RED;
            else
              m = 0;
            data[idx] = m;
          }
        }
    }
}

std::shared_ptr<const FeatureSource> site_link_source() {
  auto src = std::make_shared<FeatureSource>();
  src->id = "siteLinks";
  // after the sites: the bore always clears a passage through any room it crosses, while its
  // lining only fills solid rock (existing spaces stay open)
  src->order = 4.5;
  src->max_lod = 2;
  src->z_range = [](const World& w, const Rect& rect, int, const GroundTile&, double* z0, double* z1) { return site_links_z_range(w, rect, z0, z1); };
  src->rasterize = [](const World& w, ChunkBuffer& chunk, const GroundTile&) { site_links_rasterize(w, chunk); };
  return src;
}

}  // namespace svx::city
