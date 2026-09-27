// svx_engine_demo — headless scenario runs of the v2 engine, with optional rendered frames.
//
// Loads a world (procedural or a Doom map), designs it (bake), runs a scripted scenario of blasts
// and carves at 60 Hz, prints per-interval statistics (pieces, breaks, solves, costs) and can
// render frames with a small CPU ray caster (world grid + rigid pieces) to PPM files, which
// ffmpeg turns into a video: the collapse can be judged without a browser.
//
// usage: svx_engine_demo [--world rooms|city|tower] [--seed N] [--wad F --map M] [--threads T]
//          [--seconds S] [--scenario pillars|side|rockets|core|none] [--fragility F] [--impact I]
//          [--dif D] [--frames DIR] [--fps F] [--res WxH] [--cam x,y,z] [--look x,y,z]
//          [--report S] [--debug-view N]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "svx/base/parallel.hpp"
#include "svx/doom/movers.hpp"
#include "svx/doom/world.hpp"
#include "svx/engine/engine.hpp"
#include "svx/world/procgen.hpp"
#include "svx/world/streaming.hpp"

using namespace svx;

namespace {

using Clock = std::chrono::steady_clock;

struct Shot {
  f64 t;
  bool blast;
  V3 pos;
  f64 radius, energy;
};

V3 parse3(const char* s) {
  V3 v;
  std::sscanf(s, "%lf,%lf,%lf", &v.x, &v.y, &v.z);
  return v;
}

// A simple renderer: pinhole camera, Engine::raycast per pixel (world and pieces), Lambert with a
// shadow ray + ambient, material colours, pieces tinted per piece, distance fog.
void render(const Engine& e, const V3& cam, const V3& look, int W, int H, const std::string& path) {
  const V3 fwd = normalized(look - cam);
  V3 right = normalized(cross(fwd, V3{0, 0, 1}));
  if (norm(right) < 0.5) right = V3{1, 0, 0};
  const V3 up = cross(right, fwd);
  const f64 fov = 0.9;
  std::vector<u8> img(size_t(W) * size_t(H) * 3);
  const V3 sun = normalized(V3{-0.45, -0.3, 0.84});
  static const f64 mat_col[7][3] = {{0.78, 0.74, 0.66}, {0.72, 0.7, 0.66}, {0.55, 0.57, 0.62}, {0.72, 0.45, 0.34},
                                    {0.45, 0.36, 0.25}, {0.5, 0.48, 0.45}, {0.35, 0.33, 0.3}};
  parallel_for(H, 4, [&](i64 y0, i64 y1) {
    for (i64 y = y0; y < y1; ++y)
      for (int x = 0; x < W; ++x) {
        const f64 u = (x + 0.5) / W - 0.5, v = 0.5 - (y + 0.5) / H;
        const V3 d = normalized(fwd + right * (u * fov * W / H) + up * (v * fov));
        const RayHit hit = e.raycast(to_arr(cam), to_arr(d), 400.0);
        f64 c[3];
        if (!hit.hit) {
          const f64 s = 0.5 + 0.5 * std::max(0.0, d.z);
          c[0] = 0.62 * s + 0.2;
          c[1] = 0.72 * s + 0.2;
          c[2] = 0.86 * s + 0.12;
        } else {
          const V3 n = to_v3(hit.normal);
          const int m = std::clamp(hit.material, 0, 6);
          f64 base[3] = {mat_col[m][0], mat_col[m][1], mat_col[m][2]};
          if (hit.body) {
            const u64 hsh = static_cast<u64>(hit.body) * 0x9E3779B97F4A7C15ull;
            const f64 t = 0.8 + 0.35 * static_cast<f64>((hsh >> 40) & 0xFF) / 255.0;
            for (f64& q : base) q *= t;
          }
          const V3 p = to_v3(hit.pos) + n * 0.02;
          const RayHit sh = e.raycast(to_arr(p), to_arr(sun), 120.0);
          const f64 lam = std::max(0.0, dot(n, sun)) * (sh.hit ? 0.0 : 1.0);
          const f64 sky = 0.35 + 0.15 * n.z;
          const f64 fog = std::exp(-hit.distance / 260.0);
          for (int q = 0; q < 3; ++q) {
            const f64 lit = base[q] * (0.75 * lam + sky);
            const f64 fogc = q == 0 ? 0.75 : (q == 1 ? 0.82 : 0.92);
            c[q] = lit * fog + fogc * (1.0 - fog);
          }
        }
        for (int q = 0; q < 3; ++q)
          img[(size_t(y) * size_t(W) + size_t(x)) * 3 + size_t(q)] =
              static_cast<u8>(std::lround(255.0 * std::pow(std::clamp(c[q], 0.0, 1.0), 1.0 / 1.6)));
      }
  });
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", W, H);
  std::fwrite(img.data(), 1, img.size(), f);
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string world = "tower", wad_path, map_name = "MAP01", scenario = "pillars", frames;
  u64 seed = 1;
  f64 seconds = 12.0, fps = 15.0, report = 1.0, frame_start = 0.0;
  int W = 640, H = 360, debug_view = 0;
  EngineParams par;
  i64 work = 0;
  bool cam_set = false, look_set = false;
  V3 cam, look;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--world") world = next();
    else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--wad") wad_path = next();
    else if (a == "--map") map_name = next();
    else if (a == "--threads") set_num_threads(std::atoi(next()));
    else if (a == "--seconds") seconds = std::atof(next());
    else if (a == "--scenario") scenario = next();
    else if (a == "--fragility") par.fragility = std::atof(next());
    else if (a == "--impact") par.impact = std::atof(next());
    else if (a == "--dif") par.dif = std::atof(next());
    else if (a == "--frames") frames = next();
    else if (a == "--fps") fps = std::atof(next());
    else if (a == "--frame-start") frame_start = std::atof(next());
    else if (a == "--report") report = std::atof(next());
    else if (a == "--debug-view") debug_view = std::atoi(next());
    else if (a == "--work") work = std::atoll(next());
    else if (a == "--res") std::sscanf(next(), "%dx%d", &W, &H);
    else if (a == "--cam") {
      cam = parse3(next());
      cam_set = true;
    } else if (a == "--look") {
      look = parse3(next());
      look_set = true;
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  par.debug_view = debug_view;
  Engine eng;
  if (work > 0 || std::getenv("SVX_NO_BODY_FRACTURE") || std::getenv("SVX_MIN_FRAC") || std::getenv("SVX_MIN_BODY") || std::getenv("SVX_RIGID") || std::getenv("SVX_ROUNDS")) {
    EngineConfig c = eng.config();
    if (const char* e = std::getenv("SVX_RIGID")) {
      // (experiments) iterations,position_iterations,manifold,manifold_per_m,substeps
      int it, pit, man, sub;
      double mpm;
      if (std::sscanf(e, "%d,%d,%d,%lf,%d", &it, &pit, &man, &mpm, &sub) == 5) {
        c.rigid.iterations = it;
        c.rigid.position_iterations = pit;
        c.rigid.manifold = man;
        c.rigid.manifold_per_m = mpm;
        c.rigid.substeps = sub;
      }
    }
    if (const char* e = std::getenv("SVX_MIN_BODY")) c.min_body_voxels = std::atoi(e);
    if (const char* e = std::getenv("SVX_ROUNDS")) {
      int r;
      double q;
      if (std::sscanf(e, "%d,%lf", &r, &q) == 2) {
        c.impact_rounds = r;
        c.impact_round_fraction = q;
      }
    }
    if (work > 0) c.stress_work = work;
    if (std::getenv("SVX_NO_BODY_FRACTURE")) c.min_fracture_frags = 1 << 30;
    if (const char* e = std::getenv("SVX_MIN_FRAC")) c.min_fracture_frags = std::atoi(e);
    eng.configure(c);
  }
  const f64 h = 0.125;
  std::unique_ptr<doom::DoomWorld> dw;
  if (!wad_path.empty()) {
    std::ifstream in(wad_path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    doom::Wad wad;
    std::string err;
    if (!wad.load_memory(bytes, &err)) {
      std::fprintf(stderr, "wad: %s\n", err.c_str());
      return 1;
    }
    dw = std::make_unique<doom::DoomWorld>();
    if (!doom::build_doom_world(wad, map_name, doom::VoxelizeOptions{}, h, false, dw.get(), &err)) {
      std::fprintf(stderr, "map: %s\n", err.c_str());
      return 1;
    }
    VoxelGrid g = std::move(dw->grid);
    eng.load(std::move(g), dw->spawn_pos, dw->spawn_dir);
    world = map_name;
  } else if (world == "city") {
    auto src = make_city_source(seed, 1000.0, h);
    VoxelGrid g;
    g.h = h;
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    eng.load(std::move(g), sp, sd);
    eng.enable_streaming(std::move(src), StreamConfig{});
  } else {
    ProcWorld w = make_procedural(world, seed, h);
    eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
  }
  eng.set_params(par);
  if (const char* wv = std::getenv("SVX_WATCH")) {
    int wx, wy, wz;
    if (std::sscanf(wv, "%d,%d,%d", &wx, &wy, &wz) == 3) {
      std::printf("[before bake] ");
      eng.debug_voxel({wx, wy, wz});
    }
  }
  f64 bake_ms = 0.0;
  eng.bake(&bake_ms);
  const auto& dr = eng.design_report();
  std::printf("world %s: %lld voxels; design: %lld structures, %lld nodes, max utilization %.3f, %lld voxels strengthened, "
              "%lld floating removed, %.0f ms\n",
              world.c_str(), static_cast<long long>(eng.grid().solid_count()), static_cast<long long>(dr.structures),
              static_cast<long long>(dr.nodes), dr.max_utilization, static_cast<long long>(dr.strengthened_voxels),
              static_cast<long long>(dr.floating_voxels), bake_ms);
  // scenario
  std::vector<Shot> shots;
  if (world == "tower") {
    // the tower: 3 x 3 bays of 26 voxels from (40, 40), columns 3 x 3, storeys of 24 voxels
    auto col = [&](int ix, int iy) { return V3{h * (40 + ix * 26 + 1), h * (40 + iy * 26 + 1), 1.0}; };
    if (scenario == "pillars" || scenario == "side") {
      // every ground column on the west side, then (pillars) the next row
      for (int iy = 0; iy <= 3; ++iy) shots.push_back({0.3 + 0.25 * iy, true, col(0, iy), 0.9, 1e6});
      if (scenario == "pillars")
        for (int iy = 0; iy <= 3; ++iy) shots.push_back({1.6 + 0.25 * iy, true, col(1, iy), 0.9, 1e6});
    } else if (scenario == "test") {
      // (the collapse test's schedule: both west rows, one blast every 15 ticks from the start)
      for (int ix = 0; ix <= 1; ++ix)
        for (int iy = 0; iy <= 3; ++iy) shots.push_back({0.25 * (ix * 4 + iy), true, col(ix, iy), 0.9, 1e6});
    } else if (scenario == "core") {
      for (int ix = 1; ix <= 2; ++ix)
        for (int iy = 1; iy <= 2; ++iy) shots.push_back({0.3 + 0.3 * (ix * 2 + iy), true, col(ix, iy), 0.9, 1e6});
    } else if (scenario == "all") {
      for (int ix = 0; ix <= 3; ++ix)
        for (int iy = 0; iy <= 3; ++iy) shots.push_back({0.3 + 0.1 * (ix * 4 + iy), true, col(ix, iy), 0.9, 1e6});
    } else if (scenario == "rockets") {
      for (int k = 0; k < 8; ++k) shots.push_back({0.3 + 0.4 * k, true, V3{h * 40, h * (46 + 9 * k), 1.0 + 3.0 * (k % 3)}, 1.0, 1e6});
    }
    if (!cam_set) cam = V3{-24.0, -18.0, 14.0};
    if (!look_set) look = V3{10.0, 10.0, 11.0};
  } else if (world == "slab") {
    // cut the four columns just under the slab: it falls 8 m flat onto the ground
    if (scenario != "none")
      for (int cx : {25, 70})
        for (int cy : {25, 70}) shots.push_back({0.3, false, V3{h * cx, h * cy, h * 60}, 0.45, 0.0});
    if (!cam_set) cam = V3{-6.0, -9.0, 9.0};
    if (!look_set) look = V3{6.0, 6.0, 3.0};
  } else if (world == "chimney") {
    // blast the base on one side: it topples, breaking in the air and on the ground
    if (scenario != "none") {
      shots.push_back({0.3, true, V3{h * 73, h * 80, 0.8}, 0.9, 1e6});
      shots.push_back({0.5, true, V3{h * 76, h * 73, 0.8}, 0.9, 1e6});
    }
    if (!cam_set) cam = V3{-18.0, -22.0, 12.0};
    if (!look_set) look = V3{10.0, 10.0, 9.0};
  } else if (world == "bridge") {
    // blast one pier: the deck hinges, falls and breaks
    if (scenario != "none") {
      shots.push_back({0.3, true, V3{h * 144, h * 32, 1.0}, 1.2, 1e6});
      shots.push_back({0.6, true, V3{h * 152, h * 32, 3.0}, 1.2, 1e6});
    }
    if (!cam_set) cam = V3{11.0, -16.0, 8.0};
    if (!look_set) look = V3{11.0, 4.0, 3.0};
  } else if (world == "city") {
    // the building on the central block (next to the spawn): its ground floor blown out along
    // one side (scenario "side") or everywhere ("demolish")
    const auto sp = eng.spawn_pos();
    const f64 ox = sp[0] + h * 48.0, oy = sp[1] - h * 72.0;  // the block's corner (from the spawn in the street)
    if (scenario == "side" || scenario == "pillars") {
      for (int k = 0; k <= 6; ++k) shots.push_back({0.3 + 0.1 * k, true, V3{ox + 0.2, oy + 0.2 + 3.0 * k, 1.0}, 1.0, 1e6});
    } else if (scenario == "poke1") {
      shots.push_back({0.3, false, V3{ox + 9.0, oy + 9.0, 3.1}, 0.06, 0.0});
    } else if (scenario == "stability") {
      // tiny carves on the first-floor slab of the 3 x 3 blocks around: each building is solved
      // under its own weight (does it stand?)
      for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j) shots.push_back({0.3, false, V3{ox + 24.0 * i + 9.0625, oy + 24.0 * j + 9.0625, 3.1}, std::getenv("SVX_POKE_R") ? std::atof(std::getenv("SVX_POKE_R")) : 0.06, 0.0});
    } else if (scenario == "demolish") {
      for (int i = 0; i <= 6; ++i)
        for (int j = 0; j <= 6; ++j) shots.push_back({0.3 + 0.02 * (i * 7 + j), true, V3{ox + 0.2 + 3.0 * i, oy + 0.2 + 3.0 * j, 1.0}, 1.0, 1e6});
    }
    // from the street crossing south-west of the block
    if (!cam_set) cam = V3{ox - 3.0, oy - 3.0, 14.0};
    if (!look_set) look = V3{ox + 9.0, oy + 9.0, 5.0};
  } else if (world == "rooms") {
    if (scenario != "none") {
      shots.push_back({0.3, true, V3{h * 51, h * 23, 1.5}, 1.0, 1e6});
      shots.push_back({0.8, true, V3{h * 102, h * 23, 1.5}, 1.0, 1e6});
      shots.push_back({1.3, true, V3{h * 51, h * 66, 1.5}, 1.0, 1e6});
    }
    if (!cam_set) cam = V3{-8.0, -10.0, 9.0};
    if (!look_set) look = V3{9.0, 5.0, 1.0};
  } else {
    const auto sp = eng.spawn_pos(), sd = eng.spawn_dir();
    if (!cam_set) cam = V3{sp[0], sp[1], sp[2] + 1.6};
    if (!look_set) look = cam + normalized(V3{sd[0], sd[1], 0.0}) * 10.0;
  }
  if (const char* e = std::getenv("SVX_POKE")) {
    // (debug) a tiny carve at x,y,z at time t
    double px, py, pz, pt;
    if (std::sscanf(e, "%lf,%lf,%lf,%lf", &px, &py, &pz, &pt) == 4) shots.push_back({pt, false, V3{px, py, pz}, 0.06, 0.0});
  }
  std::sort(shots.begin(), shots.end(), [](const Shot& a, const Shot& b) { return a.t < b.t; });
  const f64 dt = eng.config().dt;
  const i64 ticks = static_cast<i64>(std::llround(seconds / dt));
  size_t next_shot = 0;
  int frame = 0;
  f64 next_frame = frame_start, next_report = report;
  f64 max_tick = 0.0, sum_tick = 0.0;
  EngineStats prev = eng.stats();
  const auto wall0 = Clock::now();
  for (i64 t = 0; t < ticks; ++t) {
    const f64 time = t * dt;
    while (next_shot < shots.size() && shots[next_shot].t <= time) {
      const Shot& s = shots[next_shot++];
      if (s.blast) eng.blast(to_arr(s.pos), s.radius, s.energy);
      else eng.carve(to_arr(s.pos), s.radius);
    }
    if (world == "city") eng.set_viewer(to_arr(look));
    eng.tick();
    (void)eng.take_events();
    if (const char* wv = std::getenv("SVX_WATCH")) {
      int wx, wy, wz;
      if (std::sscanf(wv, "%d,%d,%d", &wx, &wy, &wz) == 3) {
        std::printf("[t%lld] ", static_cast<long long>(t));
        eng.debug_voxel({wx, wy, wz});
      }
    }
    if (std::getenv("SVX_TRACK_FAST"))
      for (const auto& bp : eng.rigid().bodies)
        if (norm(bp->v) > 14.0)
          std::printf("  [fast t%lld] id %lld m %.0f r %.2f x (%.1f %.1f %.1f) v (%.1f %.1f %.1f) |w| %.2f asleep %d age %.2f\n",
                      static_cast<long long>(t), static_cast<long long>(bp->id), bp->mass, bp->radius, bp->x.x, bp->x.y, bp->x.z,
                      bp->v.x, bp->v.y, bp->v.z, norm(bp->w), bp->asleep ? 1 : 0, bp->age);
    const EngineStats s = eng.stats();
    max_tick = std::max(max_tick, s.tick_ms);
    sum_tick += s.tick_ms;
    if (!frames.empty() && time + 1e-9 >= next_frame) {
      char path[512];
      std::snprintf(path, sizeof path, "%s/f_%05d.ppm", frames.c_str(), frame++);
      render(eng, cam, look, W, H, path);
      next_frame += 1.0 / fps;
    }
    if (time + dt >= next_report - 1e-9) {
      // fast pieces (ejections) and the highest piece
      int fast = 0;
      double vmax = 0.0, zmax = -1e9;
      for (const auto& bp : eng.rigid().bodies) {
        const double v = norm(bp->v);
        vmax = std::max(vmax, v);
        fast += v > 12.0 ? 1 : 0;
        zmax = std::max(zmax, bp->x.z);
      }
      {
        static double last[5] = {0, 0, 0, 0, 0};
        const double* pm = eng.rigid().prof_ms;
        const double n = std::max(1.0, report / dt);
        std::printf("        rigid ms/tick: collide %.1f solve %.1f fracture %.1f rollback %.1f integrate %.1f\n", (pm[0] - last[0]) / n,
                    (pm[1] - last[1]) / n, (pm[2] - last[2]) / n, (pm[3] - last[3]) / n, (pm[4] - last[4]) / n);
        for (int q = 0; q < 5; ++q) last[q] = pm[q];
      }
      {
        int hist[6] = {0, 0, 0, 0, 0, 0};
        for (const auto& bp : eng.rigid().bodies) {
          if (bp->asleep) continue;
          const double sp = norm(bp->v) + bp->radius * norm(bp->w);
          hist[sp < 0.15 ? 0 : sp < 0.45 ? 1 : sp < 1.0 ? 2 : sp < 3.0 ? 3 : sp < 10.0 ? 4 : 5]++;
        }
        std::printf("        awake speeds: <.15 %d <.45 %d <1 %d <3 %d <10 %d >10 %d\n", hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
      }
      std::printf("        fast %d vmax %.1f m/s zmax %.1f m | dust %lld vox | modes T %lld C %lld S %lld\n", fast, vmax, zmax,
                  static_cast<long long>(s.pulverized_voxels), static_cast<long long>(s.mode_breaks[1]),
                  static_cast<long long>(s.mode_breaks[2]), static_cast<long long>(s.mode_breaks[3]));
      std::printf("t=%5.1fs pieces %4d (awake %4d, contacts %5d) | broken %6lld (+%lld) detached %7lld vox in %5lld pieces | "
                  "structures %3d solving %2d (%lld nodes) | splits %4lld checks %5lld breaks imp %lld steady %lld | tick mean %.1f max %.1f ms "
                  "(rigid %.1f struct %.1f) | maxphi %.2f | chunks %lld\n",
                  time + dt, s.bodies, s.awake, s.contacts, static_cast<long long>(s.bonds_broken),
                  static_cast<long long>(s.bonds_broken - prev.bonds_broken), static_cast<long long>(s.detached_voxels),
                  static_cast<long long>(s.detached_pieces), s.structures, s.solving, static_cast<long long>(s.solve_nodes),
                  static_cast<long long>(s.body_splits), static_cast<long long>(s.body_checks), static_cast<long long>(s.impact_breaks),
                  static_cast<long long>(s.steady_breaks), sum_tick / std::max<f64>(1.0, report / dt), max_tick, s.rigid_ms, s.structural_ms, s.max_utilization, static_cast<long long>(s.chunks));
      prev = s;
      sum_tick = 0.0;
      max_tick = 0.0;
      next_report += report;
    }
  }
  if (std::getenv("SVX_SPEEDS")) {
    int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const f64 edges[7] = {0.05, 0.15, 0.3, 1.0, 3.0, 10.0, 25.0};
    int small = 0, big = 0, below = 0;
    for (const auto& b : eng.rigid().bodies) {
      if (b->asleep) continue;
      if (b->x.z < -0.5) ++below;
      const f64 sp = norm(b->v) + b->radius * norm(b->w);
      int k = 0;
      while (k < 7 && sp > edges[k]) ++k;
      ++hist[k];
      (b->shape.count < 100 ? small : big)++;
    }
    std::printf("awake speed histogram (m/s: <0.05 <0.15 <0.3 <1 <3 <10 <25 more): %d %d %d %d %d %d %d %d; small %d big %d, below ground %d\n",
                hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], small, big, below);
  }
  const f64 wall = std::chrono::duration<f64>(Clock::now() - wall0).count();
  if (std::getenv("SVX_HIGH")) {
    // world voxels high up (left behind?)
    const double zmin = std::atof(std::getenv("SVX_HIGH"));
    const auto& G = eng.grid();
    for (const auto& [k, c] : G.chunks()) {
      const IVec3 cc = unkey3(k);
      for (int i = 0; i < kChunk * kChunk * kChunk; ++i) {
        const Vox v = c.uniform ? c.value : c.v[size_t(i)];
        if (!vox_solid(v)) continue;
        const int x = i / (kChunk * kChunk), y = (i / kChunk) % kChunk, z = i % kChunk;
        const double wz = G.h * (cc[2] * kChunk + z);
        if (wz > zmin && std::getenv("SVX_HIGH_DETAIL")) {
          static int shown = 0;
          if (shown++ < 4) eng.debug_voxel({cc[0] * kChunk + x, cc[1] * kChunk + y, cc[2] * kChunk + z});
        }
        if (wz > zmin)
          std::printf("high voxel at (%.2f %.2f %.2f) mat %d free %d\n", G.h * (cc[0] * kChunk + x), G.h * (cc[1] * kChunk + y), wz, int(vox_mat(v)), vox_free(v) ? 1 : 0);
      }
    }
    // pieces resting high up (floating?)
    for (const auto& bp : eng.rigid().bodies)
      if (bp->x.z > std::atof(std::getenv("SVX_HIGH")))
        std::printf("high piece %lld: %d voxels at (%.2f %.2f %.2f) asleep %d v %.2f age %.1f\n", static_cast<long long>(bp->id), bp->shape.count,
                    bp->x.x, bp->x.y, bp->x.z, bp->asleep ? 1 : 0, norm(bp->v), bp->age);
  }
  {
    // piece sizes (voxels)
    int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    long long vox[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (const auto& bp : eng.rigid().bodies) {
      const int n = bp->shape.count;
      const int k = n < 8 ? 0 : n < 32 ? 1 : n < 64 ? 2 : n < 128 ? 3 : n < 512 ? 4 : n < 2048 ? 5 : n < 8192 ? 6 : 7;
      hist[k]++;
      vox[k] += n;
    }
    std::printf("piece sizes (voxels): <8 %d (%lld) <32 %d (%lld) <64 %d (%lld) <128 %d (%lld) <512 %d (%lld) <2k %d (%lld) <8k %d (%lld) >8k %d (%lld)\n",
                hist[0], vox[0], hist[1], vox[1], hist[2], vox[2], hist[3], vox[3], hist[4], vox[4], hist[5], vox[5], hist[6], vox[6], hist[7], vox[7]);
  }
  const EngineStats s = eng.stats();
  std::printf("done: %lld ticks in %.1f s wall; voxels %lld, pieces %d, broken %lld, detached %lld voxels, hash %016llx\n",
              static_cast<long long>(ticks), wall, static_cast<long long>(s.voxels), s.bodies,
              static_cast<long long>(s.bonds_broken), static_cast<long long>(s.detached_voxels),
              static_cast<unsigned long long>(eng.session_hash()));
  return 0;
}
