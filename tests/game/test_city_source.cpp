// The city generator as a game's world (svx/procgen/city_source.hpp): the adapter gives the
// engine exactly what svx_city's export gives - chunks, layers, seams, column ranges, regions,
// oriented grids, spawn. (Until the export is ported, docs/CITY.md §5, it is a placeholder: an
// extent and nothing in it; the cases check the plumbing and then the real thing.)
#include <set>

#include "doctest.h"
#include "svx/city/materials.hpp"
#include "svx/city/source.hpp"
#include "svx/material/material.hpp"
#include "svx/procgen/city_source.hpp"
#include "svx/procgen/presets.hpp"

using namespace svx;

namespace {

std::shared_ptr<CityWorldSource> angled() {
  const Preset* p = find_preset("city/angledInfiniteCity");
  REQUIRE(p != nullptr);
  std::string err;
  std::shared_ptr<CityWorldSource> s = make_city_world(p->params_json, p->seed, &err);
  REQUIRE_MESSAGE(s != nullptr, err);
  return s;
}

IVec3 spawn_chunk(const CityWorldSource& s) {
  const V3 p = s.spawn_pos();
  const f64 h = 0.125;
  return IVec3{static_cast<i32>(std::floor(p.x / h + 0.5)) >> 5, static_cast<i32>(std::floor(p.y / h + 0.5)) >> 5,
               static_cast<i32>(std::floor(p.z / h + 0.5)) >> 5};
}

}  // namespace

TEST_CASE("city source: a preset's parameters make a world - its extent, materials, looks and roads") {
  const std::shared_ptr<CityWorldSource> s = angled();
  std::array<int, 3> lo{}, hi{};
  s->city().extent(lo, hi);
  for (int a = 0; a < 3; ++a) {
    CHECK(s->chunk_lo()[a] == lo[a]);
    CHECK(s->chunk_hi()[a] == hi[a]);
    CHECK(lo[a] < hi[a]);
  }
  // (the city reaches as far as structvox's voxel keys do)
  CHECK(static_cast<i64>(hi[0]) * 32 < kVoxelLimit);
  CHECK(static_cast<i64>(-lo[0]) * 32 < kVoxelLimit);
  CHECK(s->roads() != nullptr);
  REQUIRE(s->appearances() != nullptr);
  CHECK(s->appearances()->size() >= 400);
  CHECK(default_materials().registered(static_cast<MaterialId>(city::kCityMaterialBase + 5)));  // (foliage)
  const V3 sp = s->spawn_pos(), sd = s->spawn_dir();
  CHECK(std::isfinite(sp.x));
  CHECK(std::isfinite(sp.z));
  CHECK(sd.x == doctest::Approx(1.0));
  // (malformed parameters)
  std::string err;
  CHECK(make_city_world("[1]", 1, &err) == nullptr);
  CHECK(!err.empty());
  err.clear();
  CHECK(make_city_world(R"({"preset":"noSuchPreset"})", 1, &err) == nullptr);
  CHECK(!err.empty());
  err.clear();
  CHECK(make_city_world(R"({"preset":"infiniteCity","config":3})", 1, &err) == nullptr);
  CHECK(!err.empty());
}

TEST_CASE("city source: its chunks, layers, seams, columns and regions are the export's, the same every time") {
  const std::shared_ptr<CityWorldSource> s = angled(), t = angled();
  const city::Export& e = s->city();
  const IVec3 c0 = spawn_chunk(*s);
  int seen = 0;
  for (i32 dx = -1; dx <= 1; ++dx)
    for (i32 dy = -1; dy <= 1; ++dy) {
      i32 zl = 0, zh = 0, zl2 = 0, zh2 = 0;
      Vox below = kAir, below2 = kAir;
      s->column_range(c0[0] + dx, c0[1] + dy, &zl, &zh, &below);
      int ezl = 0, ezh = 0;
      uint8_t eb = 0;
      e.column_range(c0[0] + dx, c0[1] + dy, &ezl, &ezh, &eb);
      CHECK(zl == ezl);
      CHECK(zh == ezh);
      CHECK(below == eb);
      t->column_range(c0[0] + dx, c0[1] + dy, &zl2, &zh2, &below2);
      CHECK(zl == zl2);
      CHECK(zh == zh2);
      CHECK(s->region(IVec3{c0[0] + dx, c0[1] + dy, c0[2]}) == e.region(c0[0] + dx, c0[1] + dy));
      for (i32 dz = -1; dz <= 1; ++dz) {
        const IVec3 c{c0[0] + dx, c0[1] + dy, c0[2] + dz};
        city::ChunkData d;
        const bool any = e.chunk(c[0], c[1], c[2], d);
        std::vector<Vox> v, v2;
        const bool got = s->generate(c, v);
        CHECK(got == (any && d.any));
        CHECK(got == t->generate(c, v2));
        if (got) {
          CHECK(v == std::vector<Vox>(d.vox.begin(), d.vox.end()));
          CHECK(v == v2);
          ++seen;
        }
        for (const char* layer : {"look", "water"}) {
          std::vector<u8> l;
          const std::vector<uint8_t>& want = std::string(layer) == "look" ? d.look : d.water;
          CHECK(s->generate_layer(c, layer, l) == !want.empty());
          if (!want.empty()) CHECK(l == std::vector<u8>(want.begin(), want.end()));
        }
        std::vector<u8> seams;
        CHECK(s->generate_seams(c, seams) == !d.seams.empty());
        if (!d.seams.empty()) CHECK(seams == std::vector<u8>(d.seams.begin(), d.seams.end()));
        std::vector<u8> none;
        CHECK(!s->generate_layer(c, "flora", none));
      }
    }
  MESSAGE("solid chunks round the spawn: " << seen);
  CHECK(s->memory_bytes() >= 0);
}

TEST_CASE("city source: the parts at home round the spawn are its oriented grids, their voxels and looks the export's") {
  const std::shared_ptr<CityWorldSource> s = angled();
  const city::Export& e = s->city();
  const IVec3 c0 = spawn_chunk(*s);
  std::set<u32> ids;
  for (i32 dx = -2; dx <= 2; ++dx)
    for (i32 dy = -2; dy <= 2; ++dy)
      for (i32 dz = -1; dz <= 1; ++dz) {
        const IVec3 c{c0[0] + dx, c0[1] + dy, c0[2] + dz};
        std::vector<city::GridInfo> want;
        e.grids(c[0], c[1], c[2], want);
        const std::vector<SourceGrid> got = s->grids(c);
        REQUIRE(got.size() == want.size());
        for (size_t k = 0; k < got.size(); ++k) {
          CHECK(got[k].id == want[k].id);
          CHECK(got[k].id != 0);
          CHECK(got[k].id < (1u << 30));
          CHECK(got[k].voxel_size == doctest::Approx(want[k].voxel_size));
          CHECK(got[k].origin.x == doctest::Approx(want[k].origin[0]));
          CHECK(got[k].rot.w == doctest::Approx(want[k].rot[3]));
          ids.insert(got[k].id);
        }
      }
  MESSAGE("parts round the spawn: " << ids.size());
  int made = 0;
  for (u32 id : ids) {
    if (made >= 6) break;
    VoxelGrid g;
    if (!s->generate_grid(id, g)) continue;
    ++made;
    CHECK(g.h == doctest::Approx(0.125));
    CHECK(g.layer_index("look") >= 0);
  }
  // (an id no part has)
  VoxelGrid none;
  CHECK(!s->generate_grid(0x3FFFFFFF, none));
}
