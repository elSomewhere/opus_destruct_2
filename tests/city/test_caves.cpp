// svx_city tests — natural caves and the caves' feature source (voxel_city nature/caves.js)
// against the reference (stage "caves"), over column tiles of the stage's own making.
#include <doctest.h>

#include "core/hash.hpp"
#include "nature/caves.hpp"
#include "nature/landcover.hpp"
#include "records.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// The worlds of lib/worlds.mjs the stage samples, and two of its own (stages/caves.mjs).
const char* const kCaveKeys[] = {"cities", "island:large", "nordicIsland:large", "desert", "wrapWorld:small", "planetNorth", "allMountains"};
const test::ExtraWorld kCaveWorlds[] = {
    {"cavesOff", R"({"seed":5,"caves":{"enabled":false}})"},
    {"cavesShallow", R"({"seed":6,"caves":{"depth":25}})"},
};

constexpr int32_t kNoWater = -2147483647 - 1;

// FNV-1a over the values (stages/chunk.mjs digest).
uint32_t digest(const std::vector<uint16_t>& a) {
  uint32_t h = 2166136261u;
  for (const uint16_t v : a) h = (h ^ v) * 16777619u;
  return h;
}

// A column tile of the stage's own (stages/caves.mjs makeTile): the fields caveSource reads.
struct Tile {
  std::vector<int32_t> z, water;
  std::vector<uint16_t> top;
  std::vector<uint8_t> kind;
  double z_min = js::kInf, z_max = -js::kInf;
  CaveColumns view() const { return {kind.data(), water.data(), top.data(), z.data(), z_min, z_max}; }
};

Tile make_tile(const World& w, int lod, double cx, double cy) {
  const double s = 1 << lod;
  const double half = (1 << lod) >> 1;
  const double bx = (cx * 32 - 1) * s;
  const double by = (cy * 32 - 1) * s;
  const double e = (kP - 1) * s;
  double c[4];
  constexpr double kCorners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  for (int q = 0; q < 4; ++q) c[q] = js::round(w.terrain->sample(bx + half + kCorners[q][0] * e, by + half + kCorners[q][1] * e).h);
  const int n = kP * kP;
  Tile t;
  t.z.assign(n, 0);
  t.top.assign(n, 0);
  t.water.assign(n, kNoWater);
  t.kind.assign(n, 0);
  for (int j = 0; j < kP; ++j)
    for (int i = 0; i < kP; ++i) {
      const int idx = i + j * kP;
      const double u = static_cast<double>(i) / (kP - 1);
      const double v = static_cast<double>(j) / (kP - 1);
      const uint32_t h = hash32(w.seed, cx * kP + i, cy * kP + j, lod);
      t.z[idx] = js::i32(js::round(c[0] * (1 - u) * (1 - v) + c[1] * u * (1 - v) + c[2] * (1 - u) * v + c[3] * u * v) + static_cast<double>(h & 7u) - 3);
      const uint32_t q = (h >> 3) & 63u;
      t.kind[idx] = q < 3 ? 1 : q < 5 ? 2 : q < 6 ? 3 : q < 8 ? 6 : 4;
      t.top[idx] = q == 8 ? MAT::SNOW : q == 9 ? MAT::SNOW_WIND : MAT::GRASS;
      if (q == 10 || q == 11) t.water[idx] = js::i32(t.z[idx] + 2.0);
      if (t.z[idx] < t.z_min) t.z_min = t.z[idx];
      if (t.z[idx] > t.z_max) t.z_max = t.z[idx];
    }
  return t;
}

// The ground of a chunk before the caves (stages/caves.mjs fill).
void fill(ChunkBuffer& ch, const Tile& tile, double seed) {
  for (int j = 0; j < kP; ++j)
    for (int i = 0; i < kP; ++i) {
      const int col = i + j * kP;
      const double gz = tile.z[col];
      for (int k = 0; k < kP; ++k) {
        const double z = ch.wz(k);
        if (z > gz) continue;
        const uint32_t h = hash32(seed, col, z, 77) & 63u;
        ch.data[col + k * kP2] = z == gz ? tile.top[col] : h == 0 ? MAT::WATER : h == 1 ? 0 : gz - z < 3 ? MAT::DIRT : MAT::STONE;
      }
    }
}

}  // namespace

TEST_CASE("city caves: caves and their feature source conform to the reference (stage caves)") {
  rec::Samples r(71);
  rec::Out out;
  std::vector<test::WorldCase> worlds;
  for (const test::WorldCase& ws : test::all_worlds())
    for (const char* key : kCaveKeys)
      if (ws.key == key) worlds.push_back(ws);
  for (const test::ExtraWorld& e : kCaveWorlds) {
    Value v;
    REQUIRE(Value::parse_json(e.json, &v));
    worlds.push_back({e.key, v});
  }
  for (const test::WorldCase& ws : worlds) {
    World w(ws.overrides);
    w.caves = std::make_shared<Caves>(w);
    w.land_cover = std::make_shared<LandCover>(w);
    const Caves& cv = *w.caves;
    out << (Line() << "caves" << ws.key << cv.enabled << cv.depth);
    for (int k = 0; k < 400; ++k) {
      const double x = (r() - 0.5) * 400000;
      const double y = (r() - 0.5) * 400000;
      const double z = (r() - 0.3) * 4000;
      const double d = r() * 90;
      out << (Line() << "f" << x << y << z << d << cv.region(x, y) << cv.field(x, y, z, d));
    }
    for (int k = 0; k < 36; ++k) {
      const int lod = k % 3;
      const double s = 1 << lod;
      // a column in a cave region (most of the time), or anywhere
      double x = 0;
      double y = 0;
      for (int tries = 0; tries < 20; ++tries) {
        x = js::round((r() - 0.5) * 400000);
        y = js::round((r() - 0.5) * 400000);
        if (k % 6 == 5 || cv.region(x, y) > 0.2) break;
      }
      const double span = 32 * s;
      const double cx = std::floor(x / span);
      const double cy = std::floor(y / span);
      const Tile tile = make_tile(w, lod, cx, cy);
      const CaveColumns view = tile.view();
      const double bx = (cx * 32 - 1) * s;
      const double by = (cy * 32 - 1) * s;
      const Rect rect{bx, by, bx + kP * s - 1, by + kP * s - 1};
      double z0 = 0, z1 = 0;
      const bool zr = cave_z_range(w, rect, lod, &view, &z0, &z1);
      int ok = 0;
      for (int idx = 0; idx < kP * kP; ++idx)
        if (cv.column_ok(view, idx)) ok += 1;
      Line lt;
      lt << "tile" << lod << cx << cy << tile.z_min << tile.z_max << ok;
      if (zr)
        lt << std::vector<double>{z0, z1};
      else
        lt << rec::kUndef;
      out << lt;
      // chunks: by the cave band's top, at the ground, two drawn in the band, one above the ground
      const double lo = zr ? z0 : tile.z_min - 200;
      const double cz0 = std::floor((lo + 24) / span);
      const double cz1 = std::floor(tile.z_min / span);
      const double cz2 = std::floor((lo + r() * (tile.z_max - lo)) / span);
      const double cz3 = std::floor((tile.z_min - r() * cv.depth * 8) / span);
      const double cz4 = std::floor(tile.z_max / span) + 2;
      for (const double cz : {cz0, cz1, cz2, cz3, cz4}) {
        ChunkBuffer ch(lod, cx, cy, cz);
        fill(ch, tile, w.seed);
        const std::vector<uint16_t> before = ch.data;
        cave_rasterize(w, ch, &view);
        int changed = 0;
        int carved = 0;
        for (int i = 0; i < kP3; ++i)
          if (ch.data[i] != before[i]) {
            changed += 1;
            if (ch.data[i] == 0) carved += 1;
          }
        out << (Line() << "ch" << lod << cx << cy << cz << digest(before) << digest(ch.data) << changed << carved);
      }
    }
  }
  CHECK(rec::record("caves", out.text()) == rec::recorded_digest("caves"));
}
