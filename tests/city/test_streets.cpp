// svx_city tests — the street patterns (voxel_city city/streets.js) and diagonal boulevards
// (city/diagonals.js) on their own, against the reference (stage "streets").
#include <doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "city/diagonals.hpp"
#include "city/districts.hpp"
#include "city/streets.hpp"
#include "city_records.hpp"
#include "config/defaults.hpp"
#include "core/hash.hpp"
#include "records.hpp"
#include "world/register_all.hpp"
#include "worlds.hpp"

using namespace svx::city;
using rec::Line;

namespace {

const char* const kCls[5] = {"local", "collector", "arterial", "village", "rural"};
const char* const kPatterns[3] = {"grid", "subdivide", "organic"};

RoadSide side(rec::Samples& r) {
  const double k = std::floor(r() * 4);
  const double hr = 8 + std::floor(r() * 60);
  const std::string id = js::cat("s", std::floor(r() * 1000));
  if (k == 0) return RoadSide{};
  RoadSide s;
  s.cls = kCls[static_cast<int>(k)];
  s.hr = hr;
  if (k != 3) s.id = id;
  return s;
}

// The districts the patterns are run with (stages/streets.mjs districtVariants).
std::vector<District> district_variants() {
  std::vector<District> out;
  for (const District& d : district_registry().all()) {
    out.push_back(d);
    if (d.streets.pattern == "none") continue;
    for (const char* p : kPatterns) {
      if (p == d.streets.pattern) continue;
      District v = d;
      v.id = d.id + "~" + p;
      v.streets.pattern = p;
      out.push_back(v);
    }
  }
  for (const char* id : {"downtown", "mixed", "residential", "oldtown", "industrial"}) {
    District v = district_registry().get(id);
    v.id = std::string(id) + "~busy";
    v.streets.merge_chance = 0.6;
    v.streets.pedestrian_chance = 0.4;
    v.streets.lane_chance = 0.7;
    v.streets.lane_class = "lane";
    v.streets.paving = "cobble";
    out.push_back(v);
  }
  return out;
}

const std::vector<const char*>& diagonal_configs() {
  static const std::vector<const char*> c = {
      R"({"world":{"angles":{"enabled":true}}})",
      R"({"world":{"angles":{"enabled":true,"features":{"roads":false}}}})",
      R"({"world":{"angles":{"enabled":true,"features":{"roads":0}}}})",
      R"({"world":{"angles":{"enabled":true},"chart":"torus","size":30000}})",
      R"({"world":{"angles":{"enabled":true},"chart":"cube","planet":{"radius":120000,"face":1}}})",
      R"({"world":{"angles":{"enabled":1,"features":{"roads":true}},"chart":"flat"}})",
      R"({"world":{"angles":{"enabled":false,"features":{"roads":true}}}})",
  };
  return c;
}

Value pieces_config(double seed, const std::optional<double>& spacing) {
  Value angles = Value::object({{"enabled", true}});
  if (spacing) angles.set("diagonalSpacing", *spacing);
  return Value::object({{"seed", seed}, {"world", Value::object({{"angles", angles}})}});
}

void piece_lines(rec::Out& out, const std::vector<DiagonalPiece>& pieces) {
  for (const DiagonalPiece& p : pieces) out << (Line() << "pc" << p.fam->f << p.k << p.D << p.a.x << p.a.y << p.b.x << p.b.y);
}

}  // namespace

TEST_CASE("city streets: street patterns and diagonal boulevards conform to the reference (stage streets)") {
  register_all();
  rec::Samples r(61);
  rec::Out out;
  const std::vector<District>& base = district_registry().all();
  const std::vector<District> variants = district_variants();
  for (const District& d : variants) {
    Line l;
    l << "district" << d.id << d.streets.pattern << d.streets.merge_chance << d.streets.pedestrian_chance;
    if (d.streets.lane_chance)
      l << *d.streets.lane_chance;
    else
      l << rec::kUndef;
    l << test::fo(d.streets.lane_class) << test::fo(d.streets.paving);
    out << l;
  }
  for (int it = 0; it < 900; ++it) {
    const District& d = variants[static_cast<size_t>(std::floor(r() * static_cast<double>(variants.size())))];
    const double W = 120 + std::floor(r() * 6000);
    const double H = 120 + std::floor(r() * 6000);
    const double snap = r() < 0.7 ? 8 : 1;
    const double x0 = js::round(((r() - 0.5) * 40000) / snap) * snap;
    const double y0 = js::round(((r() - 0.5) * 40000) / snap) * snap;
    const Rect rect{x0, y0, x0 + W, y0 + H};
    BlockSides sides;
    sides.N = side(r);
    sides.E = side(r);
    sides.S = side(r);
    sides.W = side(r);
    const double seed = std::floor(r() * 4294967296.0);
    const int mode = static_cast<int>(std::floor(r() * 6));
    const bool use_local = r() < 0.35;
    const double local_seed = std::floor(r() * 1e6);
    const StreetOpts opt_table[6] = {{}, {true, 0}, {true, 0.35}, {true, 1}, {true, 0.35}, {false, 1}};
    const StreetOpts* opts = mode == 0 ? nullptr : &opt_table[mode];
    out << (Line() << "call" << it << d.id << test::frect(rect) << test::fsides(sides) << seed << mode << use_local << local_seed);
    double n = 0;
    std::vector<Line> lines;
    StreetEmit emit;
    emit.road = [&](const std::string& cls, double ax, double ay, double bx, double by, const std::string* pav) {
      const std::string id = js::cat("t", n);
      n += 1;
      lines.push_back(Line() << "r" << id << cls << ax << ay << bx << by << (pav ? test::fo(*pav) : std::string("dflt")));
      return RoadSide{cls, 8.0 + hash_string(cls) % 41, id};
    };
    emit.block = [&](const Rect& rr, const BlockSides& ss, const Poly* poly) {
      lines.push_back(Line() << "b" << test::frect(rr) << test::fsides(ss) << test::fpoly(poly));
    };
    if (use_local)
      emit.local = [&](const Rect& rr) -> const District* {
        const double h = hash_float(local_seed, rr.x0, rr.y0, rr.x1 * 3 + rr.y1);
        if (h < 0.2) return nullptr;
        return &base[static_cast<size_t>(std::fmod(std::floor(h * 1000), static_cast<double>(base.size())))];
      };
    Rng rng(seed);
    const StreetPattern pattern = street_pattern(d.streets.pattern);
    REQUIRE(pattern != nullptr);
    pattern(StreetSub{rect, sides}, d, rng, emit, opts);
    for (const Line& l : lines) out << l;
    out << (Line() << "end" << n << rng.next());
  }
  for (const char* p : {"grid", "organic", "subdivide", "none", "nope"}) out << (Line() << "family" << p << test::fo(std::string(pattern_family(p))));

  // ---- diagonals
  for (const DiagonalFamily& fam : diagonal_families()) out << (Line() << "diag" << fam.f << fam.yaw << fam.c << fam.s << fam.r);
  for (int k = 0; k < 300; ++k) {
    const double seed = std::floor((r() - 0.5) * 4e9);
    const DiagonalFamily& fam = diagonal_families()[static_cast<size_t>(std::floor(r() * 2))];
    const double kk = std::floor((r() - 0.5) * 4000);
    const double spacing = 800 + std::floor(r() * 30000);
    out << (Line() << "off" << seed << fam.f << kk << spacing << diagonal_offset(seed, fam, kk, spacing));
  }
  for (const test::WorldCase& ws : test::city_worlds()) out << (Line() << "on" << ws.key << diagonals_on(make_config(ws.overrides)));
  for (const char* json : diagonal_configs()) {
    Value v;
    REQUIRE(Value::parse_json(json, &v));
    out << (Line() << "on" << json << diagonals_on(make_config(v)));
  }
  for (int k = 0; k < 500; ++k) {
    const double seed = std::floor((r() - 0.5) * 4e9);
    std::optional<double> spacing;
    if (!(r() < 0.2)) spacing = 200 + std::floor(r() * 3000);
    const double x0 = std::floor((r() - 0.5) * 400000);
    const double y0 = std::floor((r() - 0.5) * 400000);
    const double w = r() < 0.1 ? 1 + std::floor(r() * 4) : 200 + std::floor(r() * 9000);
    const double h = r() < 0.1 ? 1 + std::floor(r() * 4) : 200 + std::floor(r() * 9000);
    const std::vector<DiagonalPiece> pieces = diagonal_pieces(pieces_config(seed, spacing), Rect{x0, y0, x0 + w, y0 + h});
    Line l;
    l << "pieces" << k << seed;
    if (spacing)
      l << *spacing;
    else
      l << rec::kUndef;
    out << (l << x0 << y0 << w << h << pieces.size());
    piece_lines(out, pieces);
  }
  for (int k = 0; k < 300; ++k) {
    const double seed = std::floor((r() - 0.5) * 4e9);
    const double spacing = 200 + std::floor(r() * 3000);
    const DiagonalFamily& fam = diagonal_families()[static_cast<size_t>(std::floor(r() * 2))];
    const double kk = std::floor((r() - 0.5) * 40);
    const double Dk = diagonal_offset(seed, fam, kk, spacing * 8);
    const double x = std::floor((r() - 0.5) * 100000);
    const double y = std::floor((Dk + fam.s * x) / fam.c);
    const double x0 = x - std::floor(r() * 3);
    const double y0 = y - std::floor(r() * 3);
    const double w = 1 + std::floor(r() * 3);
    const double h = 1 + std::floor(r() * 3);
    const std::vector<DiagonalPiece> pieces = diagonal_pieces(pieces_config(seed, spacing), Rect{x0, y0, x0 + w, y0 + h});
    out << (Line() << "tiny" << k << seed << spacing << fam.f << kk << x0 << y0 << w << h << pieces.size());
    piece_lines(out, pieces);
  }
  CHECK(rec::record("streets", out.text()) == rec::recorded_digest("streets"));
}
