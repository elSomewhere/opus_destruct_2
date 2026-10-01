// svx_city tests — pitched road pieces (voxel_city network/roadParts.js) against the reference
// (stage "roadparts"): runs, parts, their content in their own lattices and the slab's foot, on the
// reference's own roads and waters (tools/procgen_ref/data).
#include <doctest.h>

#include <unordered_map>

#include "config/presets.hpp"
#include "core/hash.hpp"
#include "network/arterials.hpp"
#include "network/roadParts.hpp"
#include "network/roadView.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

using namespace svx::city;
using rec::Line;

namespace {

struct PartCase {
  const char* key;
  const char* id;
  const char* size;
  double i, j;
};
// (stages/roadparts.mjs PARTS)
const std::vector<PartCase>& cases() {
  static const std::vector<PartCase> v = {
      {"angledOldHarbourTown", "angledOldHarbourTown", "", 0, 0},
      {"angledNordicTown:fjord", "angledNordicTown", "fjord", 0, 0},
      {"angledNordicTown:fjord:wet", "angledNordicTown", "fjord", 0, 0},
  };
  return v;
}

// stages/roadparts.mjs partFields
void part_fields(Line& l, const Part& p) {
  const Placement& pl = p.placement;
  const LocalBox& e = p.extent;
  const Box3& a = p.aabb;
  l << p.key << p.id << std::vector<double>{p.cell[0], p.cell[1]} << p.kind << std::vector<double>{pl.origin.x, pl.origin.y, pl.origin.z} << pl.yaw << pl.yaw2
    << pl.pitch << pl.roll << pl.anchored << pl.priority << std::vector<double>(pl.m.begin(), pl.m.end()) << pl.d
    << std::vector<double>{e.u0, e.v0, e.w0, e.u1, e.v1, e.w1} << std::vector<double>{a.x0, a.y0, a.z0, a.x1, a.y1, a.z1}
    << std::vector<double>{p.home.cx, p.home.cy, p.home.cz} << std::vector<double>{p.base.x, p.base.y, p.base.z} << p.reach << p.anchored << p.priority << p.road
    << p.seg << p.s0 << p.s1 << p.grade << p.hr;
}

// stages/roadparts.mjs chunkDigest
void chunk_digest(Line& l, const ChunkBuffer& c) {
  uint32_t h = 0;
  double n = 0;
  for (size_t q = 0; q < c.data.size(); ++q) {
    h = hash32(h, c.data[q], static_cast<double>(q));
    if (c.data[q]) n += 1;
  }
  l << h << n;
}

}  // namespace

TEST_CASE("city roadparts: pitched road pieces conform to the reference (stage roadparts)") {
  rec::Samples r(53);
  rec::Out out;
  const auto recorded = test::load_recorded("roadparts");
  for (const PartCase& pc : cases()) {
    World w(preset_config(pc.id, pc.size));
    test::use_recorded(w, recorded.at(pc.key));
    const ArterialGrid& A = *w.arterials;
    const std::string net_id = js::cat("C", A.canon(pc.i), "_", A.canon(pc.j));
    const std::shared_ptr<const RoadView> view = w.road_view(pc.i, pc.j);
    PartCell cell;
    cell.i = pc.i;
    cell.j = pc.j;
    cell.ci = A.canon(pc.i);
    cell.cj = A.canon(pc.j);
    cell.rect = A.cell_rect(pc.i, pc.j);
    // (the cell plan's own roads: its segments in this view, by road)
    std::vector<const Road*> order;
    std::unordered_map<const Road*, std::vector<const RoadSeg*>> own;
    for (const RoadSeg& s : view->segs) {
      if (s.road->cell != net_id) continue;
      auto it = own.find(s.road.get());
      if (it == own.end()) {
        order.push_back(s.road.get());
        it = own.emplace(s.road.get(), std::vector<const RoadSeg*>{}).first;
      }
      it->second.push_back(&s);
    }
    out << (Line() << "world" << pc.key << net_id << static_cast<double>(order.size()));
    std::vector<Part> parts;
    for (const Road* road : order) {
      const std::vector<const RoadSeg*>& segs = own[road];
      for (const RoadSeg* s : segs) {
        Line l;
        l << "runs" << test::seg_ref(s);
        for (const PitchedRun& q : pitched_runs(w, *s)) l << std::vector<double>{q.a0, q.a1, static_cast<double>(q.pitch), static_cast<double>(q.yaw)};
        out << l;
      }
      for (Part& p : road_run_parts(w, *road, segs, cell)) {
        Line l;
        l << "part";
        part_fields(l, p);
        out << l;
        parts.push_back(std::move(p));
      }
    }
    // (their content: the steepest pieces first, then by key)
    js::sort(parts, [](const Part& p, const Part& q) {
      return js::or_(js::abs(q.grade) - js::abs(p.grade), static_cast<double>(js::compare(p.key, q.key)));
    });
    for (size_t pi = 0; pi < parts.size() && pi < 6; ++pi) {
      const Part& p = parts[pi];
      const LocalBox& e = p.extent;
      for (const int lod : {0, 1, 2}) {
        const double s = static_cast<double>(32 << lod);
        for (double cz = std::floor((e.w0 - 1) / s); cz <= std::floor((e.w1 + 1) / s); cz += 1)
          for (double cy = std::floor((e.v0 - 1) / s); cy <= std::floor((e.v1 + 1) / s); cy += 1)
            for (double cx = std::floor((e.u0 - 1) / s); cx <= std::floor((e.u1 + 1) / s); cx += 1) {
              ChunkBuffer c(lod, cx, cy, cz);
              rasterize_road_part(w, p, c);
              Line l;
              l << "chunk" << p.key << lod << cx << cy << cz;
              chunk_digest(l, c);
              out << l;
            }
      }
      const Box3& a = p.aabb;
      for (int k = 0; k < 40; ++k) {
        const double x = a.x0 + r() * (a.x1 - a.x0);
        const double y = a.y0 + r() * (a.y1 - a.y0);
        out << (Line() << "foot" << p.key << x << y << slab_foot(p, x, y));
      }
    }
  }
  CHECK(rec::record("roadparts", out.text()) == rec::recorded_digest("roadparts"));
}
