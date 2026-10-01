// svx_city — voxel_city city/skybridges.js.
#include "city/skybridges.hpp"

#include <cmath>
#include <map>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kInner = 20;  // clear width 2.5 m
constexpr double kH = 22;      // clear height 2.75 m
constexpr double kMinGap = 64;
constexpr double kMaxGap = 240;

char opp(char f) { return f == 'N' ? 'S' : f == 'S' ? 'N' : f == 'E' ? 'W' : f == 'W' ? 'E' : 0; }

bool office_like(const Envelope& env) {
  if (env.archetype == "office") return true;
  return env.archetype == "tower" && env.program.upper != "apartments";
}

// Floors where the front facade is the lot front (whole footprint).
std::vector<double> bridge_floors(const Envelope& env) {
  const double top = env.archetype == "tower" ? env.podium_floors - 1 : env.floors - 2;
  std::vector<double> out;
  for (double f = 3; f <= top; f += 1) out.push_back(f);
  return out;
}

bool has_door_at(const Envelope& env, double f) {
  for (const SkyDoor& d : env.sky_doors)
    if (d.floor == f) return true;
  return false;
}

}  // namespace

std::vector<Skybridge> plan_skybridges(const World& world, const std::string& plan_id, const std::vector<Envelope*>& buildings,
                                       const std::function<bool(const Rect&)>& corridor_hits) {
  const double seed = world.seed;
  // (between buildings square to the grid: a turned one, the angled world's, has no facade to meet)
  std::vector<Envelope*> cand;
  for (Envelope* b : buildings)
    if (office_like(*b) && !b->turn) cand.push_back(b);
  std::vector<Skybridge> bridges;
  std::map<std::string, double> used;  // (JS: a Map never iterated)
  auto used_of = [&](const std::string& id) {
    auto it = used.find(id);
    return it == used.end() ? 0.0 : it->second;
  };
  for (size_t a = 0; a < cand.size(); ++a) {
    for (size_t b = 0; b < cand.size(); ++b) {
      Envelope* A = cand[a];
      Envelope* B = cand[b];
      if (A == B || opp(A->front) != B->front) continue;
      // A faces south / east towards B across the street (each pair once)
      if (A->front != 'S' && A->front != 'E') continue;
      const bool along_x = A->front == 'S';
      const double gap = along_x ? B->R.y0 - A->R.y1 - 1 : B->R.x0 - A->R.x1 - 1;
      if (gap < kMinGap || gap > kMaxGap) continue;
      const double lo = along_x ? js::max(A->R.x0, B->R.x0) : js::max(A->R.y0, B->R.y0);
      const double hi = along_x ? js::min(A->R.x1, B->R.x1) : js::min(A->R.y1, B->R.y1);
      // stay in the middle half of both facades (open office, not end rooms)
      const double ma0 = along_x ? A->R.x0 + (A->R.x1 - A->R.x0) / 4 : A->R.y0 + (A->R.y1 - A->R.y0) / 4;
      const double ma1 = along_x ? A->R.x1 - (A->R.x1 - A->R.x0) / 4 : A->R.y1 - (A->R.y1 - A->R.y0) / 4;
      const double mb0 = along_x ? B->R.x0 + (B->R.x1 - B->R.x0) / 4 : B->R.y0 + (B->R.y1 - B->R.y0) / 4;
      const double mb1 = along_x ? B->R.x1 - (B->R.x1 - B->R.x0) / 4 : B->R.y1 - (B->R.y1 - B->R.y0) / 4;
      const double s0 = std::ceil(js::max(lo, ma0, mb0));
      const double s1 = std::floor(js::min(hi, ma1, mb1));
      if (s1 - s0 + 1 < kInner + 6) continue;
      const bool futuristic = A->flavor == "futuristic" || B->flavor == "futuristic";
      const double chance = futuristic ? 0.75 : A->district == "downtown" ? 0.15 : 0;
      Rng rng = Rng::from(seed, A->id, B->id, "skybridge");
      if (!rng.chance(chance)) continue;
      if (used_of(A->id) >= 2 || used_of(B->id) >= 2) continue;
      // matching floors (level within 2 voxels so the walk is a gentle slope)
      double fa = -1;
      double fb = -1;
      const std::vector<double> floors_b = bridge_floors(*B);
      for (const double f : bridge_floors(*A)) {
        const double za = floor_z(*A, f);
        const double* g = nullptr;
        for (const double& k : floors_b)
          if (std::fabs(floor_z(*B, k) - za) <= 2) {
            g = &k;
            break;
          }
        if (g && !has_door_at(*A, f) && !has_door_at(*B, *g)) {
          fa = f;
          fb = *g;
          break;
        }
      }
      if (fa < 0) continue;
      const double c = js::round((s0 + s1) / 2);
      const double w0 = c - kInner / 2 - 1;
      const double w1 = c + kInner / 2;
      const Rect rect = along_x ? Rect{w0, A->R.y1 + 1, w1, B->R.y0 - 1} : Rect{A->R.x1 + 1, w0, B->R.x0 - 1, w1};
      if (corridor_hits && corridor_hits(rect)) continue;
      const double za = floor_z(*A, fa);
      const double zb = floor_z(*B, fb);
      Skybridge bridge;
      bridge.id = js::cat(plan_id, "/sky", static_cast<double>(bridges.size()));
      bridge.a = A->id;
      bridge.b = B->id;
      bridge.along_x = along_x;
      bridge.rect = rect;
      bridge.za = za;
      bridge.zb = zb;
      bridge.bb = {rect.x0, rect.y0, js::min(za, zb) - 2, rect.x1, rect.y1, js::max(za, zb) + kH + 6};
      bridges.push_back(bridge);
      // door spans: the clear width on each facade (world coordinates)
      SkyDoor door;
      door.span_x = along_x;
      door.s0 = c - 8;
      door.s1 = c + 7;
      door.bridge = bridge.id;
      door.floor = fa;
      A->sky_doors.push_back(door);
      door.floor = fb;
      B->sky_doors.push_back(door);
      used[A->id] = used_of(A->id) + 1;
      used[B->id] = used_of(B->id) + 1;
    }
  }
  return bridges;
}

std::vector<const Skybridge*> skybridges_in(const std::vector<Skybridge>& bridges, const Rect& rect) {
  std::vector<const Skybridge*> out;
  for (const Skybridge& br : bridges) {
    const Rect& r = br.rect;
    if (r.x1 < rect.x0 || r.x0 > rect.x1 || r.y1 < rect.y0 || r.y0 > rect.y1) continue;
    out.push_back(&br);
  }
  return out;
}

bool skybridge_z_range(const std::vector<const Skybridge*>& near, double* z0, double* z1) {
  double lo = js::kInf;
  double hi = -js::kInf;
  for (const Skybridge* br : near) {
    lo = js::min(lo, br->bb.z0);
    hi = js::max(hi, br->bb.z1);
  }
  if (lo == js::kInf) return false;
  *z0 = lo;
  *z1 = hi;
  return true;
}

void rasterize_skybridges(const std::vector<const Skybridge*>& near, ChunkBuffer& chunk) {
  const Box3 box = chunk.world_box();
  for (const Skybridge* br : near) {
    const Rect& r = br->rect;
    if (br->bb.z1 < box.z0 || br->bb.z0 > box.z1) continue;
    const IdxRange ri = chunk.range_x(js::max(r.x0, box.x0), js::min(r.x1, box.x1));
    const IdxRange rj = chunk.range_y(js::max(r.y0, box.y0), js::min(r.y1, box.y1));
    const double span = br->along_x ? r.y1 - r.y0 : r.x1 - r.x0;
    for (int j = rj.lo; j <= rj.hi; ++j) {
      const double y = chunk.wy(j);
      for (int i = ri.lo; i <= ri.hi; ++i) {
        const double x = chunk.wx(i);
        const double along = br->along_x ? y - r.y0 : x - r.x0;
        const double across = br->along_x ? x - r.x0 : y - r.y0;
        const double width = br->along_x ? r.x1 - r.x0 : r.y1 - r.y0;
        const double t = along / js::max(1.0, span);
        const double zf = js::round(br->za + (br->zb - br->za) * t);
        const bool side = across == 0 || across == width;
        const bool post = side && std::fmod(along, 16) == 0;
        const IdxRange rk = chunk.range_z(zf - 1, zf + kH + 4);
        for (int k = rk.lo; k <= rk.hi; ++k) {
          const double zr = chunk.wz(k) - zf;
          uint16_t m = 0;
          if (zr <= 0) {
            m = MAT::PANEL_GRAPHITE;  // underside
          } else if (zr == 1) {
            m = side ? MAT::PANEL_GRAPHITE : MAT::FLOOR_TERRAZZO;
          } else if (zr <= kH + 1) {
            if (side) m = post || zr == 2 || zr == kH + 1 ? MAT::MULLION : MAT::GLASS_TINT;
          } else {
            m = zr == kH + 2 && !side && across == std::floor(width / 2) ? MAT::LIGHT_STRIP : MAT::PANEL_WHITE;
          }
          chunk.data[static_cast<size_t>(ChunkBuffer::index(i, j, k))] = m;
        }
      }
    }
  }
}

}  // namespace svx::city
