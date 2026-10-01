// svx_city tests — the furniture prefab tables (voxel_city buildings/interior/prefabs.js,
// civicPrefabs.js, civicRules.js's RULE_PREFABS) against the reference (stage "prefabs" of
// tools/procgen_ref).
#include <doctest.h>

#include <string>
#include <vector>

#include "buildings/interior/civicPrefabs.hpp"
#include "buildings/interior/civicRules.hpp"
#include "buildings/interior/prefabs.hpp"
#include "prefab_records.hpp"
#include "records.hpp"

using namespace svx::city;
using irec::fo;
using rec::Line;

namespace {

std::optional<bool> tri(double x) {
  if (x < 1.0 / 3) return std::nullopt;
  return x < 2.0 / 3;
}

PrefabOpts sample_opts(rec::Samples& r, const Prefab& pf, const std::string& key) {
  double v[28];
  for (double& x : v) x = r();
  PrefabOpts o;
  if (pf.w == 0 || v[0] < 0.5) o.w = 3 + std::floor(v[1] * 45);
  if (pf.d == 0 || v[2] < 0.5) o.d = 3 + std::floor(v[3] * 25);
  std::optional<bool>* flags[9] = {&o.monitor, &o.hood, &o.upper, &o.screen, &o.metal, &o.steel, &o.double_, &o.band, &o.piano};
  for (int k = 0; k < 9; ++k) *flags[k] = tri(v[4 + k]);
  if (v[13] < 0.5) {
    if (key == "goodsShelf") {
      std::vector<uint16_t> g = {js::u16(1 + std::floor(v[14] * 399))};
      if (v[15] < 0.5) g.push_back(js::u16(1 + std::floor(v[16] * 399)));
      o.goods_list = g;
    } else {
      o.goods = js::u16(1 + std::floor(v[14] * 399));
    }
  }
  if (v[17] < 0.5) o.h = 6 + std::floor(v[18] * 50);
  if (v[19] < 0.4) o.top = js::u16(1 + std::floor(v[20] * 399));
  if (v[21] < 0.4) o.curtain = js::u16(1 + std::floor(v[22] * 399));
  if (v[23] < 0.4) o.seat = js::u16(1 + std::floor(v[24] * 399));
  if (v[25] < 0.4) o.frame = js::u16(1 + std::floor(v[26] * 399));
  return o;
}

void boxes_to(Line& l, const std::vector<PrefabBox>& boxes) {
  for (const PrefabBox& q : boxes) l << q.a0 << q.a1 << q.b0 << q.b1 << q.z0 << q.z1 << q.m;
}

}  // namespace

TEST_CASE("city prefabs: furniture prefabs conform to the reference (stage prefabs)") {
  rec::Samples r(9);
  rec::Out out;
  const std::pair<const char*, const std::vector<Prefab>*> tables[] = {{"P", &prefabs()}, {"C", &civic_prefabs()}, {"R", &rule_prefabs()}};
  for (const auto& [t, table] : tables) {
    for (const Prefab& pf : *table) {
      out << (Line() << "pf" << t << pf.id << pf.w << pf.d << pf.tall << pf.free << pf.flat << fo(pf.pad) << fo(pf.d_extra));
      for (int i = 0; i < 24; ++i) {
        const PrefabOpts o = sample_opts(r, pf, pf.id);
        Rng rng(std::floor(r() * 4294967296.0));
        const std::vector<PrefabBox> boxes = pf.build(rng, o);
        Line l;
        l << "b" << pf.id << i << irec::prefab_opts(o) << static_cast<double>(boxes.size());
        boxes_to(l, boxes);
        l << rng.next();
        out << l;
      }
    }
  }
  for (int i = 0; i < 80; ++i) {
    Rng rng(std::floor(r() * 4294967296.0));
    Line l;
    l << "car" << i;
    boxes_to(l, car_boxes(rng));
    l << rng.next();
    out << l;
  }
  CHECK(rec::record("prefabs", out.text()) == rec::recorded_digest("prefabs"));
}

TEST_CASE("city prefabs: lookups by key") {
  REQUIRE(prefab("sofa"));
  CHECK(prefab("sofa")->w == 16);
  CHECK(prefab("hospitalBed") == nullptr);
  REQUIRE(civic_prefab("hospitalBed"));
  CHECK(civic_prefab("hospitalBed")->d_extra == 0.0);
  CHECK(furnish_prefab("hospitalBed") == civic_prefab("hospitalBed"));
  CHECK(furnish_prefab("marketCounter") == rule_prefab("marketCounter"));
  CHECK(furnish_prefab("desk")->d_extra == 5.0);
  CHECK(furnish_prefab("nothing") == nullptr);
  CHECK(prefabs().size() == 46);
  CHECK(civic_prefabs().size() == 41);
  CHECK(rule_prefabs().size() == 2);
}
