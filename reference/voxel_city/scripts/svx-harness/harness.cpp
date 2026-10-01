// A district of voxel_city in structvox: the export's dump (dump.js) behind a ChunkSource, the
// game's materials then the city's registered, streamed around a focus, ticked. Checks what a
// merge relies on: the materials' ids, the chunks, the grids' frames, the world standing, the
// lanes on a surface. Run by run.sh; SVX_DEBUG=1 lists the grids and every lane point missed.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "svx/game/materials.hpp"
#include "svx/world/source.hpp"
#include "svx/world/world.hpp"

using namespace svx;

namespace {

struct Reader {
  std::vector<char> b;
  size_t p = 0;
  template <class T>
  T get() {
    T v;
    std::memcpy(&v, b.data() + p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  std::string str() {
    const u32 n = get<u32>();
    std::string s(b.data() + p, n);
    p += n;
    return s;
  }
  std::vector<Vox> chunk() {
    std::vector<Vox> v(kChunkVox);
    std::memcpy(v.data(), b.data() + p, kChunkVox);
    p += kChunkVox;
    return v;
  }
};

u64 k3(const IVec3& c) { return key3(c[0], c[1], c[2]); }

struct GridRec {
  SourceGrid g;
  IVec3 home;
  bool anchored = false;
  std::string kind;
  std::vector<std::pair<IVec3, std::vector<Vox>>> chunks;
};

struct Dump {
  f64 h = 0.125;
  struct Mat {
    i32 id;
    Material m;
  };
  std::vector<Mat> mats;
  IVec3 lo, hi;
  V3 focus;
  std::map<u64, std::vector<Vox>> chunks;
  std::vector<GridRec> grids;
  std::vector<std::pair<V3, V3>> lanes;
  std::vector<V3> touches;  // points loaded once streamed (a car's weight: the structures there are extracted and designed)
};

class DumpSource : public ChunkSource {
 public:
  explicit DumpSource(const Dump& d) : d_(d) {
    for (size_t i = 0; i < d_.grids.size(); ++i) by_home_[k3(d_.grids[i].home)].push_back(i);
  }
  bool generate(const IVec3& c, std::vector<Vox>& out) const override {
    auto it = d_.chunks.find(k3(c));
    if (it == d_.chunks.end()) return false;
    out = it->second;
    return true;
  }
  IVec3 chunk_lo() const override { return d_.lo; }
  IVec3 chunk_hi() const override { return d_.hi; }
  std::vector<SourceGrid> grids(const IVec3& c) const override {
    std::vector<SourceGrid> out;
    auto it = by_home_.find(k3(c));
    if (it != by_home_.end())
      for (size_t i : it->second) out.push_back(d_.grids[i].g);
    return out;
  }
  bool generate_grid(u32 id, VoxelGrid& out) const override {
    for (const GridRec& r : d_.grids) {
      if (r.g.id != id) continue;
      out.h = d_.h;
      i64 n = 0;
      for (const auto& [c, v] : r.chunks)
        for (i32 x = 0; x < kChunk; ++x)
          for (i32 y = 0; y < kChunk; ++y)
            for (i32 z = 0; z < kChunk; ++z) {
              const Vox q = v[size_t((x * kChunk + y) * kChunk + z)];
              if (q) {
                out.set(c[0] * kChunk + x, c[1] * kChunk + y, c[2] * kChunk + z, q);
                ++n;
              }
            }
      out.compact();
      return n > 0;
    }
    return false;
  }

 private:
  const Dump& d_;
  std::map<u64, std::vector<size_t>> by_home_;
};

bool read_dump(const char* path, Dump& d) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  Reader r;
  r.b.assign(std::istreambuf_iterator<char>(f), {});
  if (r.b.size() < 8 || std::memcmp(r.b.data(), "SVXD", 4) != 0) return false;
  r.p = 4;
  if (r.get<u32>() != 1) return false;
  d.h = r.get<f64>();
  const u32 nm = r.get<u32>();
  for (u32 i = 0; i < nm; ++i) {
    Dump::Mat m;
    m.id = r.get<i32>();
    m.m.name = r.str();
    m.m.E = r.get<f64>();
    m.m.G = r.get<f64>();
    m.m.rho = r.get<f64>();
    m.m.ft = r.get<f64>();
    m.m.fb = r.get<f64>();
    m.m.fc = r.get<f64>();
    m.m.cohesion = r.get<f64>();
    m.m.friction = r.get<f64>();
    m.m.Gf = r.get<f64>();
    m.m.frag_x = r.get<f64>();
    m.m.frag_y = r.get<f64>();
    m.m.frag_z = r.get<f64>();
    m.m.frag_noise = r.get<f64>();
    d.mats.push_back(m);
  }
  for (int a = 0; a < 3; ++a) d.lo[a] = r.get<i32>();
  for (int a = 0; a < 3; ++a) d.hi[a] = r.get<i32>();
  d.focus.x = r.get<f64>();
  d.focus.y = r.get<f64>();
  d.focus.z = r.get<f64>();
  const u32 nc = r.get<u32>();
  for (u32 i = 0; i < nc; ++i) {
    IVec3 c;
    for (int a = 0; a < 3; ++a) c[a] = r.get<i32>();
    d.chunks[k3(c)] = r.chunk();
  }
  const u32 ng = r.get<u32>();
  for (u32 i = 0; i < ng; ++i) {
    GridRec g;
    g.g.id = r.get<u32>();
    g.g.origin.x = r.get<f64>();
    g.g.origin.y = r.get<f64>();
    g.g.origin.z = r.get<f64>();
    g.g.rot.x = r.get<f64>();
    g.g.rot.y = r.get<f64>();
    g.g.rot.z = r.get<f64>();
    g.g.rot.w = r.get<f64>();
    g.g.voxel_size = r.get<f64>();
    g.g.priority = r.get<i32>();
    g.anchored = r.get<u8>() != 0;
    for (int a = 0; a < 3; ++a) g.home[a] = r.get<i32>();
    g.kind = r.str();
    const u32 n = r.get<u32>();
    for (u32 k = 0; k < n; ++k) {
      IVec3 c;
      for (int a = 0; a < 3; ++a) c[a] = r.get<i32>();
      g.chunks.push_back({c, r.chunk()});
    }
    d.grids.push_back(std::move(g));
  }
  const u32 nl = r.get<u32>();
  for (u32 i = 0; i < nl; ++i) {
    V3 a{r.get<f64>(), r.get<f64>(), r.get<f64>()};
    V3 b{r.get<f64>(), r.get<f64>(), r.get<f64>()};
    d.lanes.push_back({a, b});
  }
  const u32 nt = r.get<u32>();
  for (u32 i = 0; i < nt; ++i) d.touches.push_back(V3{r.get<f64>(), r.get<f64>(), r.get<f64>()});
  return r.p == r.b.size();
}

}  // namespace

int main(int argc, char** argv) {
  Dump d;
  if (argc < 2 || !read_dump(argv[1], d)) {
    std::fprintf(stderr, "harness: cannot read %s\n", argc > 1 ? argv[1] : "(no dump)");
    return 2;
  }
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
  };
  // 1. materials: the game's (12..20), then the city's own at the ids the export gave them
  register_game_materials();
  bool ids = true;
  for (const Dump::Mat& m : d.mats) {
    MaterialId id{};
    const bool ok = register_material(m.m, &id);
    if (!ok || static_cast<int>(id) != m.id) {
      std::printf("  %s: registered %d, the export says %d\n", m.m.name.c_str(), ok ? static_cast<int>(id) : -1, m.id);
      ids = false;
    }
  }
  check(ids, "the city's materials get the ids the export gave them (after the game's)");

  // 2. the world: streamed from the dump around its focus
  World w;
  VoxelGrid g0;
  g0.h = d.h;
  w.load(std::move(g0));
  StreamConfig sc;
  sc.load_radius = 40.0;
  sc.evict_radius = 60.0;
  sc.chunks_per_tick = 256;
  sc.archive_mb = 16.0;
  auto src = std::make_shared<DumpSource>(d);
  w.enable_streaming(src, sc);
  w.set_focus(d.focus);
  int pieces = 0;
  for (int t = 0; t < 240; ++t) {
    w.tick();
    for (const WorldEvent& e : w.take_events())
      if (e.kind == WorldEvent::Kind::PieceAdded) ++pieces;
    w.take_changed_chunks();
  }
  const WorldStats s = w.stats();
  std::printf("  resident chunks %lld, voxels %lld, grids %d, structures %d, bodies %d (awake %d), pieces made %d, detached voxels %lld, max utilization %.2f\n",
              static_cast<long long>(s.resident_chunks), static_cast<long long>(s.voxels), s.grids, s.structures, s.bodies, s.awake,
              pieces, static_cast<long long>(s.detached_voxels), s.max_utilization);
  check(s.resident_chunks > 100, "the district streams in");

  // 3. the grids: every grid at home in a resident chunk is in the world, where the export put it
  int placed = 0, expected = 0, frames = 0;
  for (const GridRec& r : d.grids) {
    if (!w.chunk_resident(r.home)) continue;
    ++expected;
    GridFrame f;
    if (!w.grid_frame(r.g.id, &f)) continue;
    ++placed;
    const f64 dq = std::fabs(f.rot.x - r.g.rot.x) + std::fabs(f.rot.y - r.g.rot.y) + std::fabs(f.rot.z - r.g.rot.z) + std::fabs(f.rot.w - r.g.rot.w);
    const f64 dp = std::fabs(f.origin.x - r.g.origin.x) + std::fabs(f.origin.y - r.g.origin.y) + std::fabs(f.origin.z - r.g.origin.z);
    if (dq < 1e-12 && dp < 1e-9) ++frames;
  }
  std::printf("  grids at home in resident chunks %d, in the world %d, frames as exported %d\n", expected, placed, frames);
  if (std::getenv("SVX_DEBUG"))
    for (GridId id : w.grids()) {
      const VoxelGrid* vg = w.grid(id);
      i64 n = 0;
      if (vg) n = vg->solid_count();
      std::printf("  grid %u: priority %d, %lld solid voxels in the world\n", static_cast<unsigned>(id), w.grid_priority(id), static_cast<long long>(n));
    }
  // (a district of no parts, the plain world grid: nothing to place)
  check((expected > 0 || d.grids.empty()) && placed == expected && frames == placed, "every part is a grid of the world, at its exported frame");

  // 4. it stands: the design pass holds what was generated (nothing comes down by itself)
  check(pieces == 0 && s.detached_voxels == 0, "the district stands under its own weight (no piece comes down)");
  // 4b. touched: a car's weight (15 kN) on each touch point (a deck's surface) loads the structure
  // there, which is extracted and designed as it is first touched (pristine: a load damages
  // nothing); it must still stand
  if (!d.touches.empty()) {
    std::vector<VoxelLoad> loads;
    for (const V3& p : d.touches) {
      VoxelLoad l;
      l.voxel = IVec3{static_cast<i32>(std::floor(p.x / d.h + 0.5)), static_cast<i32>(std::floor(p.y / d.h + 0.5)), static_cast<i32>(std::floor(p.z / d.h + 0.5))};
      l.force = V3{0, 0, -15000.0};
      loads.push_back(l);
    }
    w.set_loads(1, loads);
    int after = 0;
    for (int t = 0; t < 240; ++t) {
      w.tick();
      for (const WorldEvent& e : w.take_events())
        if (e.kind == WorldEvent::Kind::PieceAdded) ++after;
      w.take_changed_chunks();
    }
    const WorldStats s2 = w.stats();
    std::printf("  touched %zu points: structures %d, extracted nodes %lld, design utilization %.2f, strengthened voxels %lld, pieces made %d, detached voxels %lld, max utilization %.2f\n", d.touches.size(), s2.structures,
                static_cast<long long>(s2.extracted_nodes), s2.design_max_utilization, static_cast<long long>(s2.strengthened_voxels), after, static_cast<long long>(s2.detached_voxels - s.detached_voxels), s2.max_utilization);
    check(s2.structures > 0 && after == 0 && s2.detached_voxels == s.detached_voxels, "touched, its structures stand (designed under their own weight)");
  }

  // (the dumped columns: the district proper)
  std::set<u64> columns;
  for (const auto& [k, v] : d.chunks) {
    (void)v;
    const IVec3 c = unkey3(k);
    columns.insert(key3(c[0], c[1], 0));
  }
  // 5. lanes: straight down from points every 2 m along each, a surface within a voxel or two (world grid or road slab)
  int hit = 0, tried = 0;
  for (const auto& [a, b] : d.lanes) {
    const f64 len = std::hypot(b.x - a.x, b.y - a.y);
    for (f64 t = 0.5; t < len - 0.5; t += 2.0) {
      const f64 u = t / len;
      const V3 m{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
      const IVec3 c{static_cast<i32>(std::floor((m.x / d.h + 0.5) / kChunk)), static_cast<i32>(std::floor((m.y / d.h + 0.5) / kChunk)), static_cast<i32>(std::floor((m.z / d.h + 0.5) / kChunk))};
      // (over the dumped district only: beyond it the harness's source is air)
      if (!columns.count(key3(c[0], c[1], 0)) || !w.chunk_resident(c)) continue;
      ++tried;
      const RayHit h = w.raycast(V3{m.x, m.y, m.z + 1.0}, V3{0, 0, -1}, 3.0);
      if (h.hit && std::fabs(h.pos.z - m.z) < 0.3) ++hit;
      else if (std::getenv("SVX_DEBUG")) {
        GridId gid = 0;
        IVec3 gv{0, 0, 0};
        const bool in = w.grid_voxel_at(V3{m.x, m.y, m.z - 0.06}, &gid, &gv);
        std::printf("  miss at (%.2f %.2f %.2f): %s %.3f (hit grid %u) | just under: %s grid %u voxel %d %d %d\n", m.x, m.y, m.z, h.hit ? "surface" : "none", h.hit ? h.pos.z - m.z : 0.0,
                    static_cast<unsigned>(h.grid), in ? "solid" : "nothing", static_cast<unsigned>(gid), gv[0], gv[1], gv[2]);
      }
    }
  }
  std::printf("  lane points checked %d, on a surface %d\n", tried, hit);
  check(tried > 40 && hit >= tried * 9 / 10, "the lanes run on the road's surface");

  std::printf("%s\n", fails ? "FAILED" : "all checks passed");
  return fails ? 1 : 0;
}
