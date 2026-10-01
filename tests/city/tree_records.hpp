// svx_city tests — trees for the stages of nature/trees (trees, treemodels, treevox): the C++
// twin of tools/procgen_ref/lib/trees.mjs - sampled tree records of every kind, their fields and
// their models' parts as record fields, and the seasonal looks the emitters give them.
#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/placement.hpp"
#include "core/value.hpp"
#include "nature/trees.hpp"
#include "records.hpp"
#include "world/season.hpp"

namespace svx::city::trec {

// Every kind of TREE_KINDS, in its key order, then a kind the tables do not know.
inline std::vector<std::string> kinds() {
  std::vector<std::string> v;
  for (const TreeKindSpec& k : tree_kinds()) v.emplace_back(k.name);
  v.emplace_back("unknownKind");
  return v;
}

// The understory's kinds (the forest gives them the smaller reach).
inline bool under(std::string_view k) {
  return k == "fern" || k == "berry" || k == "log" || k == "stump" || k == "shrub" || k == "shrubDry" || k == "hazel" || k == "juniper";
}

// The tilts the forest lays a wild log at (forest.js LOG_TILTS): level, the grades, the yaw
// table's first angles.
inline std::vector<Yaw> log_tilts() {
  std::vector<Yaw> v = pitches();
  for (size_t i = 1; i < 9; ++i) v.push_back(yaws()[i]);
  return v;
}

// A tree of `kind` at (x, y, z) from 24 draws of r (lib/trees.mjs sampleTree).
inline Tree sample_tree(rec::Samples& r, const std::string& kind, double x, double y, double z) {
  double u[24];
  for (double& v : u) v = r();
  const TreeKindSpec* spec = tree_kind_spec(kind);
  if (!spec) spec = tree_kind_spec("oak");
  const double g = u[0] < 0.1 ? 0.12 + 0.2 * u[1] : u[0] < 0.3 ? 0.4 + 0.45 * u[1] : 0.9 + 0.2 * u[1];
  const double h = js::max(2, js::round(js::round((spec->h[0] + (spec->h[1] - spec->h[0]) * u[2]) * 8) * g));
  const double rr = js::round((spec->r[0] + (spec->r[1] - spec->r[0]) * u[3]) * 8) * (u[4] < 0.3 ? 1 : 0.4 + 0.95 * u[4]);
  const double seed = u[5] < 0.06 ? std::floor((u[6] - 0.5) * 68719476736.0) : std::floor(u[6] * 4294967296.0);
  Tree t;
  t.x = x;
  t.y = y;
  t.z = z;
  t.h = h;
  t.r = rr;
  t.kind = tree_kind(kind);
  t.seed = seed;
  t.open = u[7] < 0.4;
  if (u[8] < 0.5) {
    t.wild = true;
    if (u[9] < 0.35) t.amp = u[10] < 0.25 ? 0 : u[10] < 0.4 ? 0.5 : u[10];
    if (u[11] < 0.5)
      t.reach = under(kind) ? 27 : 57;
    else if (u[11] < 0.75)
      t.reach = 4 + std::floor(u[12] * 40);
  }
  if (kind == "log") {
    if (u[13] < 0.5) t.yaw = static_cast<int>(std::floor(u[14] * 132));
    if (u[15] < 0.6) {
      static const std::vector<Yaw> tilts = log_tilts();
      const Yaw T = tilts[static_cast<size_t>(std::floor(u[16] * static_cast<double>(tilts.size())))];
      t.tilt = u[17] < 0.5 ? Yaw{T.c, -T.s, T.r, 0} : T;
    }
    if (u[18] < 0.6) t.plate = u[19] < 0.5;
  }
  if (u[20] < 0.05) t.r = u[21] * 1.2;
  return t;
}

template <class T>
void opt(rec::Line& l, const std::optional<T>& v) {
  if (v)
    l << *v;
  else
    l << rec::kUndef;
}

// A tree's fields (lib/trees.mjs treeFields): kind, x, y, z, h, r, seed, open, wild, amp, reach,
// yaw, tilt (c,s,r), plate ("-": undefined).
inline void tree_fields(rec::Line& l, const std::string& kind, const Tree& t) {
  l << kind << t.x << t.y << t.z << t.h << t.r << t.seed << t.open;
  if (t.wild)
    l << true;
  else
    l << rec::kUndef;
  opt(l, t.amp);
  opt(l, t.reach);
  opt(l, t.yaw);
  if (t.tilt)
    l << js::cat(t.tilt->c, ",", t.tilt->s, ",", t.tilt->r);
  else
    l << rec::kUndef;
  opt(l, t.plate);
}

inline std::vector<double> pal_fields(const LeafPalette* p) { return {double((*p)[0]), double((*p)[1]), double((*p)[2])}; }

// A model part's fields (lib/trees.mjs partFields).
inline rec::Line part_line(const TreePart& p) {
  rec::Line l;
  switch (p.k) {
    case TreePartKind::Blob: {
      const TreeBlob& b = p.blob;
      l << "b" << b.x << b.y << b.z << b.rx << b.rz << b.mix;
      break;
    }
    case TreePartKind::Limb: {
      const TreeLimb& b = p.limb;
      l << "l" << b.ax << b.ay << b.az << b.dx << b.dy << b.dz << b.L2 << b.r0 << b.r1 << p.mat << p.trunk << p.twig << p.birch;
      if (p.has_foot)
        l << b.foot;
      else
        l << rec::kUndef;
      l << p.moss << p.stump;
      break;
    }
    case TreePartKind::Cone: {
      const TreeCone& c = p.cone;
      l << "c" << c.x << c.y << c.z0 << c.z1 << c.z_live << c.r << c.tier << c.lobes << c.phase << p.lean;
      if (p.lean)
        l << c.lx << c.ly << c.zf << c.hh << c.ax << c.ay << c.asym;
      else
        for (int k = 0; k < 7; ++k) l << rec::kUndef;
      break;
    }
    case TreePartKind::Curtain: {
      const TreeCurtain& c = p.curtain;
      l << "u" << c.x << c.y << c.z << c.rx << c.rz << c.drop << c.mix;
      break;
    }
    case TreePartKind::Fern: {
      const TreeFern& f = p.fern;
      l << "f" << f.x << f.y << f.z << f.r << f.h << f.n << f.phase;
      break;
    }
    case TreePartKind::Plate: {
      const TreePlate& q = p.plate;
      l << "p" << q.x << q.y << q.z << q.nx << q.ny << q.nz << q.r << q.th;
      break;
    }
  }
  l << p.bb.x0 << p.bb.y0 << p.bb.z0 << p.bb.x1 << p.bb.y1 << p.bb.z1;
  return l;
}

// A model's bounds ("-": none).
inline void model_bounds(rec::Line& l, const TreeModel& m) {
  if (m.has_bb)
    l << m.bb.x0 << m.bb.y0 << m.bb.z0 << m.bb.x1 << m.bb.y1 << m.bb.z1;
  else
    l << rec::kUndef;
}

inline void box_fields(rec::Line& l, const Box3& b) { l << b.x0 << b.y0 << b.z0 << b.x1 << b.y1 << b.z1; }

// The seasons a tree's look is taken from: every season under four climates (lib/trees.mjs
// SEASONS: none, explicit snow covers).
inline std::vector<Season> seasons() {
  auto obj = [](std::vector<Value::Member> m) { return Value::object(std::move(m)); };
  const std::vector<Value> climates = {
      Value(nullptr),
      obj({{"snowCover", 0.5}}),
      obj({{"temperature", 0.3}, {"temperatureVar", 0.04}, {"snowCover", 0.2}}),
      obj({{"snowCover", 1}}),
  };
  std::vector<Season> out;
  for (const Value& cl : climates)
    for (const char* id : {"spring", "summer", "autumn", "winter"}) {
      Value w = Value::object();
      w.set("season", id);
      w.set("climate", cl);
      out.emplace_back(obj({{"world", w}}));
    }
  return out;
}

// FNV-1a over a string's characters (lib/trees.mjs fnv).
inline uint32_t fnv(std::string_view s, uint32_t h = 2166136261u) {
  for (unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

}  // namespace svx::city::trec
