// svx_city tests — the worlds and records of the underground stages (subway, sewers): stations,
// tunnels and sewer plans as lines, column tiles and chunk fills of the stages' own
// (tools/procgen_ref/lib/underground.mjs is the Node twin).
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "nature/rivers.hpp"
#include "records.hpp"
#include "terrain/terrain.hpp"
#include "underground/sewers.hpp"
#include "underground/subway.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"
#include "world/World.hpp"

namespace svx::city::test {

// The street level of the stages' worlds: the terrain's height (the road levels are a stage of
// their own).
inline StreetLevel terrain_street_level(const World& w) {
  const World* wp = &w;
  return [wp](double x, double y) { return wp->terrain->sample(x, y).h; };
}

// lib/underground.mjs undergroundWorld: a World of World.js with the rivers, the subway (unless
// config.subway.enabled is false) and the sewers, the street level the terrain's height. (Its sea
// tests are the World's; the cache sizes are any: results never depend on them.)
inline std::unique_ptr<World> underground_world(const Value& overrides, size_t plan_cache = 256, size_t station_cache = 512) {
  auto w = std::make_unique<World>(overrides);
  w->rivers = std::make_shared<Rivers>(*w);
  const StreetLevel street_level = terrain_street_level(*w);
  const Value& on = w->config["subway"]["enabled"];
  if (!(on.is_bool() && !on.truthy())) w->subway = std::make_shared<Subway>(*w, street_level, station_cache);
  w->sewers = std::make_shared<Sewers>(*w, street_level, plan_cache);
  return w;
}

// FNV-1a over a string's bytes (the records are ASCII).
inline uint32_t fnv(const std::string& s) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

// FNV-1a over a chunk's values (stages/chunk.mjs digest).
inline uint32_t digest(const std::vector<uint16_t>& a) {
  uint32_t h = 2166136261u;
  for (const uint16_t v : a) h = (h ^ v) * 16777619u;
  return h;
}

inline std::string fbox(const UndergroundBox& q) {
  return js::cat(q.x0, ",", q.x1, ",", q.y0, ",", q.y1, ",", q.z0, ",", q.z1, ",", static_cast<int>(q.m), ",", q.mode);
}
inline std::string fbb(const Box3& b) { return js::cat(b.x0, ",", b.y0, ",", b.z0, ",", b.x1, ",", b.y1, ",", b.z1); }
inline std::string fbb(const std::optional<Box3>& b) { return b ? fbb(*b) : std::string("-"); }
// Boxes: their count and the digest of their fields.
inline std::string fboxes(const std::vector<UndergroundBox>& boxes) {
  std::string s;
  for (size_t i = 0; i < boxes.size(); ++i) {
    if (i) s += ';';
    s += fbox(boxes[i]);
  }
  return js::cat(static_cast<double>(boxes.size()), ":", static_cast<double>(fnv(s)));
}

inline void station_lines(rec::Out& out, const Station& s, bool full) {
  out << (rec::Line() << "st" << s.axis << s.i << s.j << s.x << s.y << s.zp << s.zs << fbb(s.bb) << fboxes(s.boxes));
  if (full)
    for (const UndergroundBox& q : s.boxes) out << (rec::Line() << "b" << fbox(q));
}

inline rec::Line tunnel_line(const Tunnel& t) {
  rec::Line l;
  l << "t" << t.axis << t.fixed << t.l0 << t.l1 << t.s0 << t.s1 << t.z0 << t.z1;
  return l;
}

inline rec::Line node_line(const SewerNode& n) {
  rec::Line l;
  l << "n" << n.key() << n.x << n.y << n.z << n.zr << n.arms << n.hall << n.blocked << n.qx << n.qy << n.shaft << n.open << n.R << n.H;
  if (n.stair_top)
    l << js::cat(n.stair_top->x, ",", n.stair_top->y, ",", n.stair_top->z);
  else
    l << rec::kUndef;
  return l;
}

inline void plan_lines(rec::Out& out, const SewerPlan& p, bool full) {
  out << (rec::Line() << "plan" << p.runs.size() << p.nodes.size() << fboxes(p.boxes) << p.openings.size() << fbb(p.bb));
  for (const SewerRun& r : p.runs) out << (rec::Line() << "r" << r.axis << r.fixed << r.l0 << r.l1 << r.z0 << r.z1 << r.cls);
  for (const SewerNode& n : p.nodes) out << node_line(n);
  if (full)
    for (const UndergroundBox& q : p.boxes) out << (rec::Line() << "b" << fbox(q));
  for (const SewerOpening& o : p.openings) out << (rec::Line() << "o" << o.x0 << o.y0 << o.x1 << o.y1 << o.top.x << o.top.y << o.top.z);
}

// A column tile of the stages' own: the ground's z of each padded column - the street level at
// the tile's corners, bilinear, with a hashed jitter of -1 .. 2 (lib/underground.mjs makeTile).
struct GroundZ {
  std::vector<int32_t> z;
};
inline GroundZ make_tile(const World& w, const StreetLevel& street_level, int lod, double cx, double cy) {
  const double s = 1 << lod;
  const double half = (1 << lod) >> 1;
  const double bx = (cx * 32 - 1) * s;
  const double by = (cy * 32 - 1) * s;
  const double e = (kP - 1) * s;
  double c[4];
  constexpr double kCorners[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  for (int q = 0; q < 4; ++q) c[q] = js::round(street_level(bx + half + kCorners[q][0] * e, by + half + kCorners[q][1] * e));
  GroundZ t;
  t.z.assign(kP * kP, 0);
  for (int j = 0; j < kP; ++j)
    for (int i = 0; i < kP; ++i) {
      const double u = static_cast<double>(i) / (kP - 1);
      const double v = static_cast<double>(j) / (kP - 1);
      const uint32_t h = hash32(w.seed, cx * kP + i, cy * kP + j, lod);
      t.z[i + j * kP] = js::i32(js::round(c[0] * (1 - u) * (1 - v) + c[1] * u * (1 - v) + c[2] * (1 - u) * v + c[3] * u * v) + static_cast<double>(h & 3u) - 1);
    }
  return t;
}

// The ground of a chunk before the underground (lib/underground.mjs fill).
inline void fill(ChunkBuffer& ch, const GroundZ& tile, double seed) {
  for (int j = 0; j < kP; ++j)
    for (int i = 0; i < kP; ++i) {
      const int col = i + j * kP;
      const double gz = tile.z[col];
      for (int k = 0; k < kP; ++k) {
        const double z = ch.wz(k);
        if (z > gz) continue;
        const uint32_t h = hash32(seed, col, z, 77) & 63u;
        ch.data[col + k * kP2] = z == gz ? MAT::ASPHALT : h == 0 ? MAT::WATER : h == 1 ? 0 : gz - z < 3 ? MAT::DIRT : MAT::STONE;
      }
    }
}

// The chunk at `lod` holding the LOD 0 voxel (x, y, z), its ground filled from its column tile,
// then drawn by raster(chunk, tile) - no tile when `no_tile`: its record (lib/underground.mjs
// chunkLine).
template <class Raster>
rec::Line chunk_line(const World& w, const StreetLevel& street_level, int lod, double x, double y, double z, Raster&& raster, bool no_tile = false) {
  const double span = 32 << lod;
  const double cx = std::floor(x / span);
  const double cy = std::floor(y / span);
  const double cz = std::floor(z / span);
  const GroundZ tile = make_tile(w, street_level, lod, cx, cy);
  ChunkBuffer ch(lod, cx, cy, cz);
  fill(ch, tile, w.seed);
  const std::vector<uint16_t> before = ch.data;
  const SewerColumns cols = sewer_columns(tile);
  raster(ch, no_tile ? nullptr : &cols);
  int changed = 0;
  int carved = 0;
  for (int i = 0; i < kP3; ++i)
    if (ch.data[i] != before[i]) {
      changed += 1;
      if (ch.data[i] == 0) carved += 1;
    }
  rec::Line l;
  l << "ch" << lod << cx << cy << cz << no_tile << digest(before) << digest(ch.data) << changed << carved;
  return l;
}

// The padded rect (voxels) of column tile (lod, cx, cy).
inline Rect tile_rect(int lod, double cx, double cy) {
  const double s = 1 << lod;
  const double bx = (cx * 32 - 1) * s;
  const double by = (cy * 32 - 1) * s;
  return {bx, by, bx + kP * s - 1, by + kP * s - 1};
}

// A z range as a record field: "[lo,hi]", or "-".
inline std::string fzr(bool ok, double z0, double z1) { return ok ? rec::f(std::vector<double>{z0, z1}) : std::string("-"); }

}  // namespace svx::city::test
