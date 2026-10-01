// svx_city — terrain/landforms.hpp (voxel_city terrain/landforms.js), landform for landform.
#include "terrain/landforms.hpp"

#include "core/js.hpp"
#include "core/math.hpp"
#include "terrain/terrain.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;  // Math.PI

// Distance (m) to the zero isoline of fn at the current point (fn over its gradient, by
// differences). The gradient is floored at gmin: near saddles of the noise it tends to zero and
// the estimate would jump by hundreds of metres over a few metres (cliffs in the terrain).
template <class Fn>
double iso_distance(const Fn& fn, double fx, double fy, double fz, double fw, double torus_r, double v, double e, double gmin) {
  double gx, gy;
  if (fw != fw) {
    gx = (fn(fx + e, fy, fz, js::kNaN) - v) / e;
    gy = (fn(fx, fy + e, fz, js::kNaN) - v) / e;
  } else {
    // torus chart: step e metres along the surface (x turns (fx, fy), y turns (fz, fw))
    const double R = torus_r;
    gx = (fn(fx - (e * fy) / R, fy + (e * fx) / R, fz, fw) - v) / e;
    gy = (fn(fx, fy, fz - (e * fw) / R, fw + (e * fz) / R) - v) / e;
  }
  return std::fabs(v) / js::max(js::hypot(gx, gy), gmin, 1e-9);
}

// Island shore profile (island mode, world/island): a lowland rising inland from the coast,
// beaches on gentle stretches, sea cliffs of 10-55 m where the coast is rocky; offshore a shelf
// sloping gently from beaches and steeply from cliffs, then the deep sea, with skerries on the
// shelf. The height is exactly 0 (sea level) on the coast line.
double island_base(TerrainCtx& ctx, const LandformState& st) {
  const double c = ctx.coast;
  const TerrainCfg& cfg = *ctx.cfg;
  if (c > 1600) return cfg.lowland_base * (1 - js::exp(-c / 1400));
  const double K = ctx.cliff();
  if (c >= 0) {
    const double low = cfg.lowland_base * (1 - js::exp(-c / 1400));
    if (K <= 0) return low;
    const double n = st.n[1].n2(ctx.fx / 500, ctx.fy / 500);
    const double Hc = (10 + 45 * K * (0.55 + 0.45 * n)) * K;
    return low + Hc * smoothstep(0, 12 + 22 * (1 - K), c);
  }
  const double d = -c;
  const double shelf = ctx.island->shelf;
  const double beach_d = d * 0.014;
  const double cliff_d = 12 * smoothstep(0, 10, d) + d * 0.05;
  double depth = lerp(beach_d, cliff_d, K) + js::max(0.0, d - shelf) * 0.06;
  depth = js::min(depth, 180.0);
  return -depth + ctx.island->skerry(ctx.x * 0.125, ctx.y * 0.125, c);
}

// U profile (0..1) of the glacial valleys at a field point for mountainness M: 1 on a valley
// floor, 0 outside (n, w: the "valley" and "valleyWarp" noises).
double valley_profile(const SimplexNoise& nn, const SimplexNoise& nw, double fx, double fy, double fz, double fw, double torus_r, double M,
                      double s) {
  auto fn = [&](double px, double py, double pz, double pw) {
    const double w = 0.3 * s * nw.fbmP(px / s, py / s, pz / s, pw / s, 2);
    return nn.fbmP((px + w) / s, (py - w) / s, pz / s, pw / s, 2);
  };
  const double v = fn(fx, fy, fz, fw);
  // cheap reject far outside any valley (the distance test below decides)
  if (std::fabs(v) > 0.8) return 0;
  const double d = iso_distance(fn, fx, fy, fz, fw, torus_r, v, 40, 0.7 / s);
  const double W = 700 + 1100 * M;
  if (d > W) return 0;
  const double t = d / W;
  // U profile: a flat floor over the inner quarter, walls rising outwards; faded out before the
  // cheap reject above so the terrain stays continuous
  return (t < 0.25 ? 1 : js::pow(1 - (t - 0.25) / 0.75, 1.3)) * (1 - smoothstep(0.7, 0.8, std::fabs(v)));
}

// The gullies' kernels: spacing (m) and window radius (m).
constexpr double GC = 80;
constexpr double GR = 120;

Landform lf(const char* id, double order, std::vector<std::string> noises, std::function<void(TerrainCtx&, const LandformState&)> apply) {
  Landform l;
  l.id = id;
  l.order = order;
  l.noises = std::move(noises);
  l.apply = std::move(apply);
  return l;
}

}  // namespace

Registry<Landform>& landforms_mut() {
  static Registry<Landform> r("landform");
  return r;
}
const Registry<Landform>& landforms() { return landforms_mut(); }

double ruggedness(TerrainCtx& ctx, const LandformState& st) {
  if (ctx.rugged_ >= 0) return ctx.rugged_;
  double t, m;
  ctx.climate(&t, &m);
  const double glacial = smoothstep(0.56, 0.38, t) * smoothstep(0.35, 0.6, m);
  const double n = st.n[3].fbmP(ctx.fx / 6000, ctx.fy / 6000, ctx.fz / 6000, ctx.fw / 6000, 2);
  const double r = 0.22 + 0.5 * glacial + 0.35 * n + ctx.cfg->rugged + 0.45 * smoothstep(0.02, 0.25, ctx.mountain);
  ctx.rugged_ = js::max(0.0, js::min(1.0, r)) * (1 - 0.8 * ctx.desert());
  return ctx.rugged_;
}

void register_landforms() {
  Registry<Landform>& R = landforms_mut();

  R.add(lf("continent", 0, {"continent", "plains"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.island) {
      ctx.h = island_base(ctx, st);
      return;
    }
    const TerrainCfg& c = *ctx.cfg;
    const double s = c.continent_scale;
    const double k = st.n[0].fbmP(ctx.fx / s, ctx.fy / s, ctx.fz / s, ctx.fw / s, 3);
    const double p = st.n[1].fbmP(ctx.fx / 9000, ctx.fy / 9000, ctx.fz / 9000, ctx.fw / 9000, 2);
    ctx.h = c.lowland_base + c.continent_amplitude * k + 25 * p;
  }));

  R.add(lf("hills", 10, {"hill", "detail"}, [](TerrainCtx& ctx, const LandformState& st) {
    const TerrainCfg& c = *ctx.cfg;
    const double hs = c.hill_scale;
    const double ds = c.detail_scale;
    // island: the coast stays at sea level; hills grow inland (sooner above cliffs)
    const double fade = ctx.island ? (ctx.coast <= 0 ? 0 : smoothstep(0, 520 - 400 * ctx.cliff(), ctx.coast)) : 1;
    if (fade <= 0) {
      ctx.lowland = ctx.h;
      return;
    }
    const double hills = c.hill_amplitude * st.n[0].fbmP(ctx.fx / hs, ctx.fy / hs, ctx.fz / hs, ctx.fw / hs, 4) * (1 - 0.5 * ctx.mountain);
    const double detail = c.detail_amplitude * st.n[1].fbmP(ctx.fx / ds, ctx.fy / ds, ctx.fz / ds, ctx.fw / ds, 3);
    // (hills never sink an island's inland below sea level)
    ctx.h += (hills + detail) * fade;
    if (ctx.island && ctx.coast > 0) ctx.h = js::max(ctx.h, js::min(2.5, 0.3 + ctx.coast * 0.02));
    ctx.lowland = ctx.h;
  }));

  // Mountain ranges: a broad uplift under the belt plus ridged multifractal peaks of up to ~6 km.
  // Detail concentrates on ridges; valleys stay smooth.
  R.add(lf("mountains", 20, {"mountain", "mountainWarp"}, [](TerrainCtx& ctx, const LandformState& st) {
    const double M = ctx.mountain;
    if (M <= 0) return;
    const TerrainCfg& c = *ctx.cfg;
    const double s = c.mountain_scale;
    const double base = c.mountain_base;
    const double wx = 0.25 * s * st.n[1].fbmP(ctx.fx / s, ctx.fy / s, ctx.fz / s, ctx.fw / s, 2);
    const double Rr = st.n[0].ridgedP((ctx.fx + wx) / s, (ctx.fy - wx) / s, ctx.fz / s, ctx.fw / s, 6, 2.05, c.mountain_gain);
    // broad massif under the ridges; foothills where the belt fades
    const double Mh = M * std::sqrt(M);
    const double uplift = base * M + c.mountain_uplift * Mh;
    const double peaks = c.mountain_height * Mh * js::pow(Rr, 1.2);
    // arêtes and couloirs: sharper mid-scale relief that grows with height
    const double ds = c.mountain_detail_scale;
    const double dr = st.n[0].ridgedP(ctx.fx / ds + 13.1, ctx.fy / ds - 7.7, ctx.fz / ds, ctx.fw / ds, 3, 2.1, 0.45);
    const double detail = c.mountain_detail * Mh * (0.35 + Rr) * (dr - 0.45);
    ctx.h += uplift + peaks + detail;
    ctx.ridge = Rr;
  }));

  // Dry plateaus: arid country stands high, which gives canyons and mesas room to cut; towns sit
  // in shallow basins (the plateau fades over their foothill buffer like the mountains do).
  R.add(lf("plateau", 25, {"plateau"}, [](TerrainCtx& ctx, const LandformState& st) {
    const double D = ctx.desert();
    if (D < 0.25) return;
    const double k = smoothstep(0.25, 0.7, D) * (1 - ctx.mountain) * (1 - ctx.prox);
    if (k <= 0) return;
    const double vary = 0.75 + 0.25 * st.n[0].fbmP(ctx.fx / 14000, ctx.fy / 14000, ctx.fz / 14000, ctx.fw / 14000, 2);
    ctx.h += ctx.cfg->plateau_height * k * vary;
  }));

  // Glacial valleys: U-shaped trunk valleys along the isolines of a warped noise, cut into the
  // massifs (flat floors, steep walls), which break the ranges into ridges, passes and valleys.
  R.add(lf("glacialValleys", 22, {"valley", "valleyWarp"}, [](TerrainCtx& ctx, const LandformState& st) {
    const double M = ctx.mountain;
    if (M < 0.12) return;
    const double u = valley_profile(st.n[0], st.n[1], ctx.fx, ctx.fy, ctx.fz, ctx.fw, ctx.torus_r, M, ctx.cfg->valley_scale);
    if (u <= 0) return;
    // (faded in over the mountainness threshold: no wall where the cut starts)
    const double depth = ctx.cfg->valley_depth * js::pow(M, 1.2) * smoothstep(0.12, 0.3, M);
    // never below the foothills
    const double cut = js::min(depth * u, js::max(0.0, ctx.h - ctx.lowland - 150));
    ctx.h -= cut;
    if (cut > 50) ctx.valley = u;
  }));

  // Gullies and couloirs: grooves running down the fall line of mountain flanks, between sharp
  // ribs. Built from kernels on an 80 m lattice (Gabor style): each kernel carries stripes across
  // its own downslope direction (from a cheap copy of the range's relief at the kernel, cached),
  // weighted by how steep it is there and blended with compact windows (no seams). Two scales:
  // couloirs ~140 m apart and gullies ~45 m apart.
  R.add(lf("gullies", 23, {"mountain", "mountainWarp", "gullyPhase", "valley", "valleyWarp"}, [](TerrainCtx& ctx, const LandformState& st) {
    const double M = ctx.mountain;
    if (M < 0.1) return;
    const TerrainCfg& c = *ctx.cfg;
    const double s = c.mountain_scale;
    // the range's ridged relief (3 octaves, metres) less its glacial valleys, at a field point
    // (a fixed mountainness: a kernel is a pure function of its position, whoever asks first)
    const double Mv = 0.7;
    const double v_depth = c.valley_depth * js::pow(Mv, 1.2) * smoothstep(0.12, 0.3, Mv);
    auto relief = [&](const FieldPoint& f) {
      const double wx = 0.25 * s * st.n[1].fbmP(f.x / s, f.y / s, f.z / s, f.w / s, 2);
      const double r = c.mountain_height * js::pow(st.n[0].ridgedP((f.x + wx) / s, (f.y - wx) / s, f.z / s, f.w / s, 3, 2.05, c.mountain_gain), 1.2);
      return r - v_depth * valley_profile(st.n[3], st.n[4], f.x, f.y, f.z, f.w, ctx.torus_r, Mv, c.valley_scale);
    };
    auto kernel = [&](double i, double j) -> const TerrainCtx::GullyKernel& {
      const double key = i * 1000003 + j;
      auto it = ctx.gully_cache.find(key);
      if (it != ctx.gully_cache.end()) return it->second;
      const double xm = (i + 0.5) * GC;
      const double ym = (j + 0.5) * GC;
      const double e = 30;
      const double gx = (relief(ctx.to_field(xm + e, ym)) - relief(ctx.to_field(xm - e, ym))) / (2 * e);
      const double gy = (relief(ctx.to_field(xm, ym + e)) - relief(ctx.to_field(xm, ym - e))) / (2 * e);
      const double g = js::hypot(gx, gy);
      const FieldPoint f = ctx.to_field(xm, ym);
      // (phases vary slowly, so grooves carry on from kernel to kernel)
      const double p1 = kPi * 2 * st.n[2].fbmP(f.x / 700, f.y / 700, f.z / 700, f.w / 700, 2);
      const double p2 = kPi * 2 * st.n[2].fbmP(f.x / 260 + 9.7, f.y / 260, f.z / 260, f.w / 260, 2);
      const TerrainCtx::GullyKernel k{xm, ym, g > 1e-6 ? -gy / g : 0, g > 1e-6 ? gx / g : 0, smoothstep(0.15, 0.55, g), p1, p2};
      if (ctx.gully_cache.size() > 60000) ctx.gully_cache.clear();
      return ctx.gully_cache.emplace(key, k).first->second;
    };
    const double xm = ctx.x * 0.125;
    const double ym = ctx.y * 0.125;
    const double i0 = std::floor((xm - GR) / GC - 0.5);
    const double j0 = std::floor((ym - GR) / GC - 0.5);
    const double i1 = std::floor((xm + GR) / GC - 0.5) + 1;
    const double j1 = std::floor((ym + GR) / GC - 0.5) + 1;
    double sw = 0, sa = 0, s1 = 0, s2 = 0;
    for (double j = j0; j <= j1; j += 1)
      for (double i = i0; i <= i1; i += 1) {
        const TerrainCtx::GullyKernel k = kernel(i, j);
        const double dx = xm - k.x;
        const double dy = ym - k.y;
        const double d2 = (dx * dx + dy * dy) / (GR * GR);
        if (d2 >= 1) continue;
        const double w = (1 - d2) * (1 - d2);
        const double q = dx * k.nx + dy * k.ny;
        sw += w;
        sa += w * k.a;
        s1 += w * k.a * js::cos((kPi * 2 * q) / 140 + k.p1);
        s2 += w * k.a * js::cos((kPi * 2 * q) / 45 + k.p2);
      }
    if (sw <= 0 || sa <= 1e-4) return;
    const double a = sa / sw;
    // grooves (V-shaped) between sharp ribs; the mean level stays put
    const double c1 = s1 / sa;
    const double c2 = s2 / sa;
    auto groove = [](double v) { return 0.3 - js::pow((1 - v) / 2, 1.6); };
    // (forested mid-slopes are furrowed too, the high rock faces most)
    const double A = c.gully_depth * smoothstep(0.1, 0.45, M) * (0.45 + 0.55 * M) * a;
    ctx.h += A * (groove(c1) + 0.35 * groove(c2));
  }));

  // Desert mesas: the land steps up in flat benches with steep risers.
  R.add(lf("mesas", 30, {}, [](TerrainCtx& ctx, const LandformState&) {
    if (ctx.h < 25 || ctx.u > 0.3) return;
    const double D = ctx.desert();
    if (D < 0.25) return;
    const double step = ctx.cfg->mesa_step;
    const double f = ctx.h / step;
    const double k = std::floor(f);
    const double stepped = (k + smoothstep(0.62, 0.96, f - k)) * step;
    // faded near the gates above so the benches do not start with a wall
    const double fade = smoothstep(25, 45, ctx.h) * (1 - smoothstep(0.2, 0.3, ctx.u));
    ctx.h = lerp(ctx.h, stepped, smoothstep(0.25, 0.6, D) * (1 - ctx.mountain) * fade);
  }));

  // Wind-aligned dunes in sandy deserts.
  R.add(lf("dunes", 31, {"dune"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.3) return;
    const double D = ctx.desert();
    if (D <= 0) return;
    const SimplexNoise& n = st.n[0];
    const double warp = 40 * n.nP(ctx.fx / 400, ctx.fy / 400, ctx.fz / 400, ctx.fw / 400);
    const double ridge = ctx.fw != ctx.fw ? n.ridged2((ctx.fx + warp) / 140, (ctx.fy - warp) / 55 + 17.3, 2)
                                          : n.ridged4((ctx.fx + warp) / 140, ctx.fy / 140, (ctx.fz - warp) / 55 + 17.3, ctx.fw / 55, 2, 2.1, 0.5);
    ctx.h += D * D * 9 * ridge * ridge * (1 - ctx.mountain) * (1 - smoothstep(0.2, 0.3, ctx.u));
  }));

  // Canyons: along the zero isolines of a warped noise in dry country, cut down to ~260 m. The
  // walls climb in bands of hard and soft rock (a bench, a concave apron of scree, a near-vertical
  // cliff, the next bench); the floor is flat with a stream (or a dry wash in true desert).
  R.add(lf("canyons", 40, {"canyon", "canyonWarp", "canyonBands", "canyonGully"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.2 || ctx.h < 8) return;
    const double D = ctx.desert();
    if (D < 0.2) return;
    const double s = ctx.cfg->canyon_scale;
    auto fn = [&](double px, double py, double pz, double pw) {
      const double w = 0.3 * s * st.n[1].fbmP(px / s, py / s, pz / s, pw / s, 2);
      return st.n[0].fbmP((px + w) / s, (py - w) / s, pz / s, pw / s, 3);
    };
    const double v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (std::fabs(v) > 0.07) return;
    const double d0 = iso_distance(fn, ctx.fx, ctx.fy, ctx.fz, ctx.fw, ctx.torus_r, v, 15, 0.7 / s);
    double d = d0;
    // (faded out before the |v| reject so no wall appears where it cuts in)
    const double mask =
        smoothstep(0.2, 0.55, D) * (1 - ctx.mountain) * (1 - smoothstep(0.05, 0.07, std::fabs(v))) * (1 - smoothstep(0.1, 0.2, ctx.u));
    const double fx = ctx.fx, fy = ctx.fy, fz = ctx.fz, fw = ctx.fw;
    // buttresses and alcoves; side gullies (a ridged noise) bite deep into the walls
    const double wander = st.n[2].fbmP(fx / 70, fy / 70, fz / 70, fw / 70, 2);
    const double gully = 1 - std::fabs(st.n[3].fbmP(fx / 260, fy / 260, fz / 260, fw / 260, 2));
    d = js::max(0.0, d + 16 * mask * wander - 55 * mask * js::pow(gully, 6));
    const double floor = 14 + 20 * mask;
    const double rim = floor + 60 + 170 * mask;
    if (d > rim) return;
    const double t = js::max(0.0, (d - floor) / (rim - floor));
    // the bands: their number and proportions change slowly along the canyon
    const double vb = st.n[2].fbmP(fx / 900 + 7.3, fy / 900, fz / 900, fw / 900, 2);
    const double steps = 4 + 2.5 * (vb + 1);
    const double q = t * steps;
    const double u = q - std::floor(q);
    const double bench = 0.3 + 0.2 * st.n[2].nP(fx / 400 - 3.1, fy / 400, fz / 400, fw / 400);
    const double cliff = js::min(0.92, bench + 0.28);
    // bench (flat), scree apron (concave, a quarter of the rise), cliff (the rest, steep)
    const double y = u < bench   ? 0
                     : u < cliff ? 0.25 * js::pow((u - bench) / (cliff - bench), 1.6)
                                 : 0.25 + 0.75 * smoothstep(0, 1, (u - cliff) / (1 - cliff));
    const double stepped = js::min(1.0, (std::floor(q) + y) / steps);
    // never below sea level: a canyon cuts at most 90% of the land's height
    const double depth = js::max(0.0, js::min(ctx.cfg->canyon_depth * mask, 0.9 * (ctx.h - 8)));
    const double cut = depth * (1 - stepped);
    ctx.h -= cut;
    if (cut > 2) ctx.canyon = 1 - stepped;
    ctx.channel = js::max(ctx.channel, 1 - smoothstep(floor * 0.5, floor + 10, d));
    // a stream winds along the flat floor (a dry sandy wash in true desert), on the canyon's own line
    const double sh = js::min(floor * 0.35, 3 + 3 * mask);
    if (d0 < sh + 2 && d < floor && depth > 40) ctx.stream = Stream{d0, sh, 0, D < 0.72};
  }));

  // Ravines: narrow V-shaped gorges in wet, wooded hill country, with a creek at the bottom.
  R.add(lf("ravines", 41, {"ravine", "ravineWarp"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.12 || ctx.mountain > 0.5) return;
    double t, m;
    ctx.climate(&t, &m);
    const double mask = smoothstep(0.52, 0.72, m) * (1 - ctx.desert()) * (1 - 2 * ctx.mountain);
    if (mask < 0.1) return;
    const double s = ctx.cfg->ravine_scale;
    auto fn = [&](double px, double py, double pz, double pw) {
      const double w = 0.35 * s * st.n[1].fbmP(px / s, py / s, pz / s, pw / s, 2);
      return st.n[0].fbmP((px + w) / s, (py - w) / s, pz / s, pw / s, 2);
    };
    const double v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (std::fabs(v) > 0.05) return;
    const double d = iso_distance(fn, ctx.fx, ctx.fy, ctx.fz, ctx.fw, ctx.torus_r, v, 6, 0.7 / s);
    const double half = 9 + 20 * mask;
    if (d > half) return;
    const double k = (1 - d / half) * (1 - smoothstep(0.038, 0.05, std::fabs(v)));
    // (on an island ravines fade out before the shore: no gorge below sea level)
    const double shore = ctx.island ? smoothstep(80, 420, ctx.coast) : 1;
    const double depth = ctx.cfg->ravine_depth * mask * smoothstep(0.1, 0.2, mask) * (1 - smoothstep(0.06, 0.12, ctx.u)) * shore;
    // (never down to the sea: a low island's ravine stays a dry gorge above it)
    ctx.h -= js::min(depth * js::pow(k, 1.25), js::max(0.0, ctx.h - 1.5));
    ctx.ravine = k;
    ctx.channel = js::max(ctx.channel, 1 - smoothstep(3, 9, d));
    // a creek at the bottom of the V
    if (d < 4.5) ctx.stream = Stream{d, 2.2 + mask, depth * (1 - js::pow(k, 1.25)), true};
  }));

  // Relief of open country at walking scale: knolls and hollows (~170 m), hummocks (~22 m) and
  // small bumps (~4 m), all growing with the country's ruggedness; it fades out towards towns, on
  // beaches and along the channels of streams, ravines and canyons. Hummocks and bumps count as
  // roughness (land cover reads slopes without them).
  R.add(lf("relief", 43, {"reliefKnoll", "reliefHummock", "reliefBump", "reliefRugged"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.08) return;
    const TerrainCfg& cfg = *ctx.cfg;
    double mask = (1 - smoothstep(0.02, 0.07, ctx.u)) * (1 - ctx.channel);
    if (ctx.island) mask *= ctx.coast <= 0 ? 0 : smoothstep(4, 60, ctx.coast);
    if (mask <= 0.01) return;
    const double R = ruggedness(ctx, st);
    const double fx = ctx.fx, fy = ctx.fy, fz = ctx.fz, fw = ctx.fw;
    const double ks = cfg.relief_knoll_scale;
    const double hs = cfg.relief_hummock_scale;
    const double knoll = st.n[0].fbmP(fx / ks, fy / ks, fz / ks, fw / ks, 3);
    const double hum = st.n[1].fbmP(fx / hs, fy / hs, fz / hs, fw / hs, 2);
    const double bump = st.n[2].nP(fx / 4.2, fy / 4.2, fz / 4.2, fw / 4.2);
    // (high mountains carry their own relief)
    const double low = 1 - 0.7 * smoothstep(0.1, 0.5, ctx.mountain);
    const double kn = cfg.relief_knoll * (0.15 + 0.85 * R) * knoll * low;
    // rugged ground: sharper hummocks (rounded tops, narrow hollows)
    const double hm = cfg.relief_hummock * (0.3 + 0.7 * R) * (hum + 0.35 * R * (hum * hum - 0.3));
    const double bp = cfg.relief_bump * (0.4 + 0.6 * R) * bump;
    const double rough = (hm + bp) * mask;
    ctx.h += kn * mask + rough;
    ctx.rough += rough;
  }));

  // Roughness: hummocks, knolls and crags on mountain ground (a couple of metres at 2-6 m
  // wavelengths), strongest on high ridges.
  R.add(lf("roughness", 60, {"rough"}, [](TerrainCtx& ctx, const LandformState& st) {
    const double M = ctx.mountain;
    if (M < 0.08 || ctx.u > 0.05) return;
    const SimplexNoise& n = st.n[0];
    const double a = n.nP(ctx.fx / 6, ctx.fy / 6, ctx.fz / 6, ctx.fw / 6);
    const double b = n.nP(ctx.fx / 2.3 + 17.2, ctx.fy / 2.3, ctx.fz / 2.3, ctx.fw / 2.3);
    double r = (0.55 * a + 0.3 * b) * 0.9 * M;
    // crags: blocky knobs on the high ridges
    const double c = n.nP(ctx.fx / 11 - 5.1, ctx.fy / 11, ctx.fz / 11, ctx.fw / 11);
    if (c > 0.25) r += (c - 0.25) * 4.5 * M * (0.4 + ctx.ridge);
    r *= smoothstep(0.08, 0.16, M) * (1 - smoothstep(0.02, 0.05, ctx.u));
    ctx.h += r;
    ctx.rough += r;
  }));

  // Granite outcrops: glacially smoothed rock knolls a few metres high breaking through the forest
  // and meadows of cool, wet country, and bare rock slabs sloping into the sea on low island shores.
  R.add(lf("outcrops", 45, {"outcrop", "outcropDetail"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.12) return;
    double t, m;
    ctx.climate(&t, &m);
    const double cool = smoothstep(0.5, 0.4, t) * smoothstep(0.45, 0.6, m) * (1 - smoothstep(0.35, 0.6, ctx.mountain));
    // low island shores: rock slabs down to the water (not on beaches of sand)
    double shore = 0;
    if (ctx.island && ctx.coast > -25 && ctx.coast < 70)
      shore = smoothstep(-25, 0, ctx.coast) * (1 - smoothstep(30, 70, ctx.coast)) * smoothstep(0.1, 0.45, ctx.cliff() + 0.25);
    if (cool <= 0 && shore <= 0) return;
    const double fx = ctx.fx, fy = ctx.fy, fz = ctx.fz, fw = ctx.fw;
    const double n = st.n[0].fbmP(fx / 130, fy / 130, fz / 130, fw / 130, 2);
    // (a ragged outline: the rock edge frays into tongues and islets)
    const double edge = 0.14 * st.n[1].fbmP(fx / 22 + 3.3, fy / 22, fz / 22, fw / 22, 2);
    const double k = smoothstep(0.4, 0.72, n + 0.35 * shore + edge) * js::max(cool, shore) * (1 - smoothstep(0.04, 0.12, ctx.u));
    if (k <= 0.01) return;
    // a dome (whaleback), bumpier on top, broken by joints into slabs and low ledges
    const double bump = 0.6 * st.n[1].nP(fx / 9, fy / 9, fz / 9, fw / 9);
    const double dome = 5.5 * js::pow(k, 1.6) + bump * k;
    const double f = dome / 1.1;
    const double ledged = (std::floor(f) + smoothstep(0.55, 0.9, f - std::floor(f))) * 1.1;
    const double joint = 1 - std::fabs(st.n[1].nP(fx / 16 - 11.9, fy / 16, fz / 16, fw / 16));
    const double crack = smoothstep(0.9, 0.98, joint) * js::min(0.9, dome * 0.35);
    const double rise = (0.55 * dome + 0.45 * ledged - crack) * (shore > 0 ? 1 - 0.7 * shore : 1);
    ctx.h += ctx.island && ctx.coast < 0 ? js::min(rise, 1.5) * smoothstep(-25, 0, ctx.coast) : rise;
    ctx.outcrop = k;
  }));

  // Creeks: small winding streams through the woods and meadows of wet country, along the
  // isolines of a warped noise (a few hundred metres apart), cut a metre or two into the ground;
  // they run out into the sea on an island.
  R.add(lf("creeks", 42, {"creek", "creekWarp", "creekMask"}, [](TerrainCtx& ctx, const LandformState& st) {
    if (ctx.u > 0.1 || ctx.mountain > 0.6 || ctx.stream) return;
    double t, m;
    ctx.climate(&t, &m);
    const double wet = smoothstep(0.5, 0.64, m) * (1 - ctx.desert());
    if (wet <= 0.05) return;
    const double s = ctx.cfg->creek_scale;
    // (one octave under a warp: long winding lines, no little closed loops)
    auto fn = [&](double px, double py, double pz, double pw) {
      const double w = 0.4 * s * st.n[1].fbmP(px / s, py / s, pz / s, pw / s, 2);
      return st.n[0].fbmP((px + w) / s, (py - w) / s, pz / s, pw / s, 1);
    };
    const double v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (std::fabs(v) > 0.035) return;
    // creeks come and go (springs, soaks) over kilometres
    const double on = smoothstep(-0.15, 0.3, st.n[2].fbmP(ctx.fx / 2400, ctx.fy / 2400, ctx.fz / 2400, ctx.fw / 2400, 2));
    if (on <= 0) return;
    const double d = iso_distance(fn, ctx.fx, ctx.fy, ctx.fz, ctx.fw, ctx.torus_r, v, 5, 0.7 / s);
    const double half = (0.7 + 1.0 * on) * wet;
    // a shallow valley with gentle banks, the channel in its floor
    const double bank = half + 4 + 5 * on;
    if (d > bank) return;
    const double shore_k = ctx.island ? smoothstep(0, 60, ctx.coast) : 1;
    const double fade = (1 - smoothstep(0.024, 0.035, std::fabs(v))) * (1 - smoothstep(0.05, 0.1, ctx.u)) * (1 - smoothstep(0.4, 0.6, ctx.mountain));
    const double depth = (0.5 + 0.9 * on) * wet * fade * shore_k;
    if (depth <= 0.05) return;
    const double q = 1 - smoothstep(half * 0.3, bank, d);
    const double k = q * q * (3 - 2 * q);
    ctx.h -= js::min(depth * k, js::max(0.0, ctx.h - 1.2));
    ctx.channel = js::max(ctx.channel, 1 - smoothstep(half, bank + 2, d));
    if (d < half + 1 && ctx.h > 1.4 && on > 0.2) ctx.stream = Stream{d, half, 0, true};
  }));
}

}  // namespace svx::city
