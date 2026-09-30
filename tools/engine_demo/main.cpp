// svx_engine_demo — headless scenario runs of the v2 engine, with optional rendered frames.
//
// Loads a world (procedural or a Doom map), designs it (bake), runs a scripted scenario of blasts
// and carves at 60 Hz, prints per-interval statistics (pieces, breaks, solves, costs) and can
// render frames with a small CPU ray caster (world grid + rigid pieces) to PPM files, which
// ffmpeg turns into a video: the collapse can be judged without a browser.
//
// usage: svx_engine_demo [--world rooms|city|tower|yard|slab|chimney|bridge|angles|machines] [--seed N] [--wad F --map M] [--threads T]
//          [--seconds S] [--scenario pillars|side|rockets|core|none] [--fragility F] [--impact I]
//          [--dif D] [--frames DIR] [--fps F] [--res WxH] [--cam x,y,z] [--look x,y,z]
//          [--report S] [--debug-view N] [--turn DEG] [--turned-city]
//
// --turned-city: the streamed city with some of its buildings turned in grids of their own.
// --turn DEG: a procedural world's structure (everything above the ground) stands in a grid of
// its own turned DEG degrees about the vertical through its centre (docs/GRIDS.md), on the world
// grid's ground (bonded to it by junctions); the scenario's blasts and the camera turn with it.
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
#include "svx/game/doom/movers.hpp"
#include "svx/game/doom/world.hpp"
#include "svx/game/game.hpp"
#include "svx/game/procgen.hpp"
#include "svx/game/city.hpp"
#include "svx/game/drive_city.hpp"
#include "svx/anim/system.hpp"

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

// A simple renderer: pinhole camera, Game::raycast per pixel (world and pieces), Lambert with a
// shadow ray + ambient, material colours, pieces tinted per piece, distance fog.
void render(const Game& e, const V3& cam, const V3& look, int W, int H, const std::string& path) {
  const V3 fwd = normalized(look - cam);
  V3 right = normalized(cross(fwd, V3{0, 0, 1}));
  if (norm(right) < 0.5) right = V3{1, 0, 0};
  const V3 up = cross(right, fwd);
  const f64 fov = 0.9;
  std::vector<u8> img(size_t(W) * size_t(H) * 3);
  const V3 sun = normalized(V3{-0.45, -0.3, 0.84});
  static const f64 mat_col[11][3] = {{0.78, 0.74, 0.66}, {0.72, 0.7, 0.66}, {0.55, 0.57, 0.62}, {0.72, 0.45, 0.34},
                                    {0.45, 0.36, 0.25}, {0.5, 0.48, 0.45}, {0.35, 0.33, 0.3}, {0.62, 0.42, 0.22},
                                    {0.68, 0.66, 0.6},  {0.7, 0.85, 0.9},  {0.3, 0.26, 0.22}};
  parallel_for(H, 4, [&](i64 y0, i64 y1) {
    for (i64 y = y0; y < y1; ++y)
      for (int x = 0; x < W; ++x) {
        const f64 u = (x + 0.5) / W - 0.5, v = 0.5 - (y + 0.5) / H;
        const V3 d = normalized(fwd + right * (u * fov * W / H) + up * (v * fov));
        const Game::ShotHit hit = e.raycast_shot(cam, d, 400.0);
        f64 c[3];
        if (!hit.hit) {
          const f64 s = 0.5 + 0.5 * std::max(0.0, d.z);
          c[0] = 0.62 * s + 0.2;
          c[1] = 0.72 * s + 0.2;
          c[2] = 0.86 * s + 0.12;
        } else {
          const V3 n = hit.normal;
          const int m = std::clamp(hit.material, 0, 10);
          f64 base[3] = {mat_col[m][0], mat_col[m][1], mat_col[m][2]};
          // (a character: its voxel's colour, from its palette)
          const anim::Character* ch = hit.character && e.characters() ? e.characters()->get(hit.character) : nullptr;
          if (ch && hit.slot < ch->palette.size())
            for (int q = 0; q < 3; ++q) base[q] = std::pow(std::clamp(static_cast<f64>(ch->palette[hit.slot][size_t(q)]), 0.0, 1.0), 1.0 / 1.6);
          const V3 p = hit.pos + n * 0.02;
          const Game::ShotHit sh = e.raycast_shot(p, sun, 120.0);
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
  GameParams par;
  i64 work = 0;
  bool cam_set = false, look_set = false;
  V3 cam, look;
  f64 turn = 0.0;
  bool turned = false, turned_city = false;
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
    else if (a == "--turned-city") turned_city = true;
    else if (a == "--turn") {
      turn = std::atof(next());
      turned = true;
    }
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
  Game eng;
  if (work > 0 || std::getenv("SVX_NO_BODY_FRACTURE") || std::getenv("SVX_MIN_FRAC") || std::getenv("SVX_MIN_BODY") || std::getenv("SVX_RIGID") || std::getenv("SVX_ROUNDS") || std::getenv("SVX_REST") ||
      std::getenv("SVX_NO_CCD")) {
    WorldConfig c = eng.config();
    if (std::getenv("SVX_NO_CCD")) c.rigid.speculative = false;  // (experiments: no continuous collision)
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
    if (const char* e = std::getenv("SVX_REST")) std::sscanf(e, "%lf,%lf", &c.rigid.rest_speed, &c.rigid.rest_damping);
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
  IVec3 turn_pivot{0, 0, 0};
  Quat turn_rot;
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
  } else if (world == "drive") {
    // (the endless city to drive through: its traffic and its people)
    auto src = make_drive_city(seed, h);
    VoxelGrid g;
    g.h = h;
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    eng.load(std::move(g), sp, sd);
    eng.load_streaming(std::move(src), eng.grid().h);
  } else if (world == "city") {
    auto src = make_city_source(seed, 1000.0, h, turned_city);
    VoxelGrid g;
    g.h = h;
    const auto sp = src->spawn_pos(), sd = src->spawn_dir();
    eng.load(std::move(g), sp, sd);
    eng.load_streaming(std::move(src), eng.grid().h);
  } else {
    ProcWorld w = make_procedural(world, seed, h);
    if (turned) {
      // the structure (z >= 0) into a grid of its own, turned about the vertical through its
      // centre; the ground (z < 0) stays, grown to reach under it
      VoxelGrid& g = w.grid;
      IVec3 lo{INT32_MAX, INT32_MAX, 0}, hi{INT32_MIN, INT32_MIN, 0};
      for (const auto& [k, c] : g.chunks()) {
        const IVec3 cc = unkey3(k);
        if (cc[2] < 0) continue;
        for (int i = 0; i < kChunkVox; ++i) {
          if (!vox_solid(c.uniform ? c.value : c.v[size_t(i)])) continue;
          const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
          for (int a = 0; a < 2; ++a) {
            lo[a] = std::min(lo[a], cc[a] * kChunk + l[a]);
            hi[a] = std::max(hi[a], cc[a] * kChunk + l[a]);
          }
        }
      }
      turn_pivot = IVec3{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, 0};
      VoxelGrid t;
      t.h = h;
      std::vector<std::pair<IVec3, Vox>> moved;
      for (const auto& [k, c] : g.chunks()) {
        const IVec3 cc = unkey3(k);
        if (cc[2] < 0) continue;
        for (int i = 0; i < kChunkVox; ++i) {
          const Vox v = c.uniform ? c.value : c.v[size_t(i)];
          if (!vox_solid(v)) continue;
          const IVec3 l{i / (kChunk * kChunk), (i / kChunk) % kChunk, i % kChunk};
          moved.push_back({{cc[0] * kChunk + l[0], cc[1] * kChunk + l[1], cc[2] * kChunk + l[2]}, v});
        }
      }
      for (const auto& [q, v] : moved) {
        g.set(q, kAir);
        t.set(q[0] - turn_pivot[0], q[1] - turn_pivot[1], q[2], v);
      }
      const i32 r = static_cast<i32>(std::ceil(0.75 * std::max(hi[0] - lo[0], hi[1] - lo[1]))) + 16;
      for (i32 x = turn_pivot[0] - r; x < turn_pivot[0] + r; ++x)
        for (i32 y = turn_pivot[1] - r; y < turn_pivot[1] + r; ++y)
          if (!vox_solid(g.get(x, y, -1))) g.fill_column(x, y, -4, 0, make_vox(MaterialId::Rock, true));
      g.compact();
      t.compact();
      eng.load(std::move(w.grid), w.spawn_pos, w.spawn_dir);
      const f64 th = 0.5 * turn * 3.14159265358979323846 / 180.0;
      turn_rot = Quat{0.0, 0.0, std::sin(th), std::cos(th)};
      const GridId id = eng.world().add_grid(GridFrame{V3{h * turn_pivot[0], h * turn_pivot[1], 0.0}, turn_rot}, std::move(t));
      std::printf("turned %.1f degrees: %zu voxels in grid %u about (%d %d)\n", turn, moved.size(), id, turn_pivot[0], turn_pivot[1]);
      add_grids(eng.world(), std::move(w.grids));
    } else {
      load_procedural(eng, std::move(w));
    }
  }
  eng.set_params(par);
  if (const char* wv = std::getenv("SVX_WATCH")) {
    int wx, wy, wz;
    if (std::sscanf(wv, "%d,%d,%d", &wx, &wy, &wz) == 3) {
      std::printf("[before bake] ");
      eng.world().debug_voxel({wx, wy, wz});
    }
  }
  f64 bake_ms = 0.0;
  eng.bake(&bake_ms);
  const auto& dr = eng.world().design_report();
  std::printf("world %s: %lld voxels; design: %lld structures, %lld nodes, max utilization %.3f, %lld voxels strengthened, "
              "%lld floating removed, %.0f ms\n",
              world.c_str(), static_cast<long long>(eng.stats().voxels), static_cast<long long>(dr.structures),
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
  } else if (world == "yard") {
    // a blast at each construction: a house post, the tower's foot, the greenhouse, a shed
    // column, the reinforced wall
    if (scenario != "none") {
      shots.push_back({0.3, true, V3{h * 26, h * 26, 0.6}, 0.8, 5e5});
      shots.push_back({0.6, true, V3{h * 150, h * 40, 0.8}, 1.0, 1e6});
      shots.push_back({0.9, true, V3{h * 68, h * 139, 1.0}, 0.6, 3e5});
      shots.push_back({1.2, true, V3{h * 151, h * 141, 1.0}, 0.8, 1e6});
      shots.push_back({1.5, true, V3{h * 270, h * 39, 1.2}, 0.7, 1e6});
    }
    if (!cam_set) cam = V3{20.0, -12.0, 16.0};
    if (!look_set) look = V3{20.0, 12.0, 2.0};
  } else if (world == "angles") {
    // structures in oriented grids: a pier of the diagonal bridge, the portal's left column (it
    // hangs on the braces), the ramp's block, the turned tower's west columns, the 20 degree
    // wall, the stacked crates and the monolith's foot
    if (scenario != "none") {
      const World& W = eng.world();
      shots.push_back({0.3, true, V3{h * 224, h * 168, 1.0}, 1.2, 1e6});
      shots.push_back({0.5, true, V3{h * 224, h * 168, 3.5}, 1.2, 1e6});
      shots.push_back({0.7, true, V3{h * 161.5, h * 41, h * 14}, 0.7, 1e6});
      shots.push_back({0.9, true, V3{h * 106, h * 60, 1.0}, 1.2, 1e6});
      for (int k = 0; k < 3; ++k) shots.push_back({1.2 + 0.15 * k, true, W.grid_to_world(1, V3{h * -25.5, h * (-25.5 + 26 * k), 0.8}), 0.9, 1e6});
      shots.push_back({1.8, true, W.grid_to_world(6, V3{h * 12, 0.0, 1.5}), 0.8, 5e5});
      shots.push_back({2.0, true, V3{h * 170, h * 120, 1.2}, 0.6, 2e5});
      shots.push_back({2.2, true, V3{h * 350, h * 140, 0.4}, 0.5, 5e5});
    }
    if (!cam_set) cam = V3{-6.0, -10.0, 18.0};
    if (!look_set) look = V3{24.0, 16.0, 2.0};
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
  if (turned) {
    // (the scenario turns with the structure)
    const V3 pv{h * turn_pivot[0], h * turn_pivot[1], 0.0};
    auto turned = [&](const V3& p) { return pv + rotate(turn_rot, p - pv); };
    for (Shot& s : shots) s.pos = turned(s.pos);
    cam = turned(cam);
    look = turned(look);
  }
  std::sort(shots.begin(), shots.end(), [](const Shot& a, const Shot& b) { return a.t < b.t; });
  const f64 dt = eng.config().dt;
  const i64 ticks = static_cast<i64>(std::llround(seconds / dt));
  size_t next_shot = 0;
  int frame = 0;
  f64 next_frame = frame_start, next_report = report;
  f64 max_tick = 0.0, sum_tick = 0.0;
  GameStats prev = eng.stats();
  const auto wall0 = Clock::now();
  for (i64 t = 0; t < ticks; ++t) {
    const f64 time = t * dt;
    while (next_shot < shots.size() && shots[next_shot].t <= time) {
      const Shot& s = shots[next_shot++];
      if (s.blast) eng.blast(s.pos, s.radius, s.energy);
      else eng.carve(s.pos, s.radius);
    }
    if (world == "city") eng.set_viewer(look);
    eng.tick();
    (void)eng.take_events();
    if (const char* wv = std::getenv("SVX_WATCH")) {
      int wx, wy, wz;
      if (std::sscanf(wv, "%d,%d,%d", &wx, &wy, &wz) == 3) {
        std::printf("[t%lld] ", static_cast<long long>(t));
        eng.world().debug_voxel({wx, wy, wz});
      }
    }
    if (std::getenv("SVX_TRACK_FAST"))
      for (const auto& bp : eng.world().rigid().bodies)
        if (norm(bp->v) > 14.0)
          std::printf("  [fast t%lld] id %lld m %.0f r %.2f x (%.1f %.1f %.1f) v (%.1f %.1f %.1f) |w| %.2f asleep %d age %.2f\n",
                      static_cast<long long>(t), static_cast<long long>(bp->id), bp->mass, bp->radius, bp->x.x, bp->x.y, bp->x.z,
                      bp->v.x, bp->v.y, bp->v.z, norm(bp->w), bp->asleep ? 1 : 0, bp->age);
    const GameStats s = eng.stats();
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
      for (const auto& bp : eng.world().rigid().bodies) {
        const double v = norm(bp->v);
        vmax = std::max(vmax, v);
        fast += v > 12.0 ? 1 : 0;
        zmax = std::max(zmax, bp->x.z);
      }
      {
        static double last[5] = {0, 0, 0, 0, 0};
        const double* pm = eng.world().rigid().prof_ms;
        const double n = std::max(1.0, report / dt);
        std::printf("        rigid ms/tick: collide %.1f solve %.1f fracture %.1f rollback %.1f integrate %.1f\n", (pm[0] - last[0]) / n,
                    (pm[1] - last[1]) / n, (pm[2] - last[2]) / n, (pm[3] - last[3]) / n, (pm[4] - last[4]) / n);
        for (int q = 0; q < 5; ++q) last[q] = pm[q];
      }
      {
        int hist[6] = {0, 0, 0, 0, 0, 0};
        for (const auto& bp : eng.world().rigid().bodies) {
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
    for (const auto& b : eng.world().rigid().bodies) {
      if (b->asleep) continue;
      if (b->x.z < -0.5) ++below;
      const f64 sp = norm(b->v) + b->radius * norm(b->w);
      int k = 0;
      while (k < 7 && sp > edges[k]) ++k;
      ++hist[k];
      (b->count < 100 ? small : big)++;
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
          if (shown++ < 4) eng.world().debug_voxel({cc[0] * kChunk + x, cc[1] * kChunk + y, cc[2] * kChunk + z});
        }
        if (wz > zmin)
          std::printf("high voxel at (%.2f %.2f %.2f) mat %d free %d\n", G.h * (cc[0] * kChunk + x), G.h * (cc[1] * kChunk + y), wz, int(vox_mat(v)), vox_free(v) ? 1 : 0);
      }
    }
    // pieces resting high up (floating?)
    for (const auto& bp : eng.world().rigid().bodies)
      if (bp->x.z > std::atof(std::getenv("SVX_HIGH")))
        std::printf("high piece %lld: %d voxels at (%.2f %.2f %.2f) asleep %d v %.2f age %.1f\n", static_cast<long long>(bp->id), bp->count,
                    bp->x.x, bp->x.y, bp->x.z, bp->asleep ? 1 : 0, norm(bp->v), bp->age);
  }
  {
    // piece sizes (voxels)
    int hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    long long vox[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (const auto& bp : eng.world().rigid().bodies) {
      const int n = bp->count;
      const int k = n < 8 ? 0 : n < 32 ? 1 : n < 64 ? 2 : n < 128 ? 3 : n < 512 ? 4 : n < 2048 ? 5 : n < 8192 ? 6 : 7;
      hist[k]++;
      vox[k] += n;
    }
    std::printf("piece sizes (voxels): <8 %d (%lld) <32 %d (%lld) <64 %d (%lld) <128 %d (%lld) <512 %d (%lld) <2k %d (%lld) <8k %d (%lld) >8k %d (%lld)\n",
                hist[0], vox[0], hist[1], vox[1], hist[2], vox[2], hist[3], vox[3], hist[4], vox[4], hist[5], vox[5], hist[6], vox[6], hist[7], vox[7]);
  }
  const GameStats s = eng.stats();
  std::printf("done: %lld ticks in %.1f s wall; voxels %lld, pieces %d, broken %lld, detached %lld voxels, hash %016llx\n",
              static_cast<long long>(ticks), wall, static_cast<long long>(s.voxels), s.bodies,
              static_cast<long long>(s.bonds_broken), static_cast<long long>(s.detached_voxels),
              static_cast<unsigned long long>(eng.session_hash()));
  return 0;
}
