// svx_city tests — shared inputs of the building shell stages (massing, wings, garageramps,
// skybridges, grading): the C++ twin of tools/procgen_ref/lib/shells.mjs. The worlds a shell is
// drawn in (every season, explicit snow covers), scripted envelopes of every archetype, the
// chunks round a box and their digests.
#pragma once

#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "building_records.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

namespace svx::city::test {

// lib/shells.mjs SHELL_WORLDS
inline const std::vector<const char*>& shell_worlds() {
  static const std::vector<const char*> w = {
      R"({"seed":1})",
      R"({"seed":2,"world":{"season":"winter"}})",
      R"({"seed":3,"world":{"season":"spring","climate":{"temperature":0.2}}})",
      R"({"seed":4,"world":{"season":"autumn","climate":{"temperature":0.1,"temperatureVar":0.02}}})",
      R"({"seed":5,"world":{"climate":{"snowCover":0.6}}})",
      R"({"seed":6,"world":{"season":"winter","climate":{"snowCover":1.4}}})",
      R"({"seed":7,"world":{"season":"winter","climate":{"temperature":0.05}}})",
  };
  return w;
}

// World k of shell_worlds() (a World of World.js, made once).
inline const World& shell_world(size_t k) {
  static std::vector<std::unique_ptr<World>> ws(shell_worlds().size());
  if (!ws[k]) {
    Value v;
    if (!Value::parse_json(shell_worlds()[k], &v)) SVX_FAIL("shells: a world's JSON does not parse");
    ws[k] = std::make_unique<World>(v);
  }
  return *ws[k];
}

// a flavor's church dome materials (and a list with a name the palette lacks)
inline const std::vector<std::vector<std::string>>& shell_domes() {
  static const std::vector<std::vector<std::string>> d = {{"GOLD", "DOME_GREEN", "DOME_BLUE"}, {"DOME_SHINGLE", "DOME_GREEN"}, {"NOPE"}};
  return d;
}

struct ShellCase {
  Lot lot;
  std::optional<Envelope> env;
};

// shellEnvelope(r, a, style, d, w, k): an envelope of archetype a in style (null: the district's
// pick) on a scripted lot of a size it fits.
inline ShellCase shell_envelope(rec::Samples& r, const Archetype& a, const std::string* style, const District& d, const World& w, double k) {
  double U = 0, V = 0;
  for (int t = 0; t < 40; ++t) {
    U = 40 + std::floor(r() * 560);
    V = 40 + std::floor(r() * 560);
    if (a.fits(U, V)) break;
  }
  ShellCase out;
  out.lot = scripted_lot(r, U, V, k, d.id);
  EnvelopeExtra extra;
  extra.u = r();
  extra.core = r() * 1.2;
  extra.ground_z = std::floor(r() * 3000) - 200;
  extra.config = &w.config;
  if (r() < 0.2) extra.chapel = true;
  if (r() < 0.5) extra.dome = shell_domes()[static_cast<size_t>(std::floor(r() * static_cast<double>(shell_domes().size())))];
  if (r() < 0.4) extra.pitched_civic = 0.6;
  Rng rng(std::floor(r() * 4294967296.0));
  out.env = style ? plan_building_envelope_as(out.lot, a.id, *style, d, rng, extra) : plan_building_envelope(out.lot, d, rng, extra);
  return out;
}

// chunkDigest: FNV-1a over a chunk's voxels, and the count of the solid ones.
inline void chunk_digest(rec::Line& l, const ChunkBuffer& c) {
  uint32_t h = 2166136261u;
  double n = 0;
  for (const uint16_t v : c.data) {
    h = (h ^ v) * 16777619u;
    if (v) n += 1;
  }
  l << h << n;
}

// chunksAt(lods, pts): the chunks holding the points, each once per LOD, in the points' order.
using ChunkAt = std::array<double, 4>;  // lod, cx, cy, cz
inline std::vector<ChunkAt> chunks_at(const std::vector<int>& lods, const std::vector<std::array<double, 3>>& pts) {
  std::vector<ChunkAt> out;
  for (int lod : lods) {
    const double E = static_cast<double>(32 << lod);
    std::vector<ChunkAt> seen;
    for (const auto& p : pts) {
      const ChunkAt c{static_cast<double>(lod), std::floor(p[0] / E), std::floor(p[1] / E), std::floor(p[2] / E)};
      bool dup = false;
      for (const ChunkAt& s : seen) dup = dup || s == c;
      if (dup) continue;
      seen.push_back(c);
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace svx::city::test
