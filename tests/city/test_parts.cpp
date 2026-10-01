// svx_city tests — oriented parts (voxel_city world/parts.js) against the reference (stage
// "parts" of tools/procgen_ref).
#include <doctest.h>

#include "core/geom2d.hpp"
#include "records.hpp"
#include "world/parts.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// (stages/parts.mjs's lattice)
PartLattice test_lattice() {
  PartLattice l;
  l.index_at = [](int axis, double v) { return std::floor((v - (axis ? 64 : 0)) / 2048); };
  l.cell_rect = [](double i, double j) { return Rect{i * 2048, j * 2048 + 64, (i + 1) * 2048, (j + 1) * 2048 + 64}; };
  return l;
}

Value obj(std::vector<Value::Member> m) { return Value::object(std::move(m)); }

}  // namespace

TEST_CASE("city parts: oriented parts and their budget are the reference's (stage parts)") {
  rec::Samples r(19);
  auto big = [&](double n) { return std::floor((r() - 0.5) * n); };
  rec::Out out;
  out << (Line() << "reach" << kPartReach << resident_d(Value::object()) << resident_d(obj({{"world", obj({{"angles", obj({{"residentRadius", 64}})}})}}))
                 << resident_d(Value(nullptr)) << resident_d(obj({{"world", Value::object()}})));
  for (int i = 0; i < 2000; ++i) {
    const double ci = big(1e7);
    const double cj = big(1099511627776.0);
    const double k = std::floor(r() * 256);
    out << (Line() << "id" << part_id(ci, cj, k) << part_id(cj, ci, 255 - k) << part_id(-ci, 3, 0) << part_id(ci + 0.5, -cj, k));
  }
  for (const char* kind : {"road", "ramp", "wing", "building", "tree", ""})
    for (int i = 0; i < 50; ++i) {
      const double id = std::floor(r() * 1073741824.0);
      out << (Line() << "prio" << kind << part_priority(kind, id) << part_priority(kind, id, true) << part_priority(kind, id, false));
    }
  for (int i = 0; i < 1000; ++i) {
    const double x0 = big(1e5);
    const double y0 = big(1e5);
    const double w = 200 + std::floor(r() * 4000);
    const double h = 200 + std::floor(r() * 4000);
    const Rect rect{x0, y0, x0 + w, y0 + h};
    double x = x0 + big(6000) + 1000;
    if (!(i % 3)) x += r();
    const double y = y0 + big(6000) + 1000;
    const double z = big(2000);
    const ChunkXYZ home = home_chunk(x, y, z, rect);
    const double ax0 = x - std::floor(r() * 300);
    const double ay0 = y - std::floor(r() * 300);
    const double ax1 = x + std::floor(r() * 300);
    const double ay1 = y + std::floor(r() * 300);
    out << (Line() << "home" << home.cx << home.cy << home.cz << reach_of(Rect{ax0, ay0, ax1, ay1}, home));
  }
  for (int i = 0; i < 300; ++i) {
    PlacementOpts po;
    po.yaw = static_cast<int>(std::floor(r() * 132));
    po.pitch = i % 3 ? 0 : static_cast<int>(std::floor(r() * 13)) - 6;
    po.roll = i % 7 ? 0 : static_cast<int>(std::floor(r() * 132));
    po.origin.x = big(8000);
    po.origin.y = big(8000);
    po.origin.z = std::floor(r() * 200);
    po.priority = std::floor(r() * 100);
    const Placement pl(po);
    LocalBox extent;
    extent.u0 = -std::floor(r() * 20);
    extent.v0 = -std::floor(r() * 20);
    const double w0 = -std::floor(r() * 10);
    extent.u1 = std::floor(r() * 120);
    extent.v1 = std::floor(r() * 80);
    const double w1 = std::floor(r() * 90);
    if (i % 4 != 1) {
      extent.w0 = w0;
      extent.w1 = w1;
    }
    PartCell cell;
    cell.i = big(100);
    cell.j = big(100);
    cell.ci = big(5000);
    cell.cj = big(5000);
    cell.rect.x0 = po.origin.x - std::floor(r() * 3000);
    cell.rect.y0 = po.origin.y - std::floor(r() * 3000);
    cell.rect.x1 = po.origin.x + 1 + std::floor(r() * 3000);
    cell.rect.y1 = po.origin.y + 1 + std::floor(r() * 3000);
    const double index = std::floor(r() * 256);
    static const char* const kKinds[4] = {"road", "building", "wing", "ramp"};
    const Part p = make_part(cell, index, js::cat("k", i), kKinds[i % 4], pl, extent, i % 5 == 0);
    const Box3& a = p.aabb;
    out << (Line() << "part" << p.id << std::vector<double>{p.cell[0], p.cell[1]} << p.key << p.kind << a.x0 << a.y0 << a.z0 << a.x1 << a.y1 << a.z1 << p.home.cx
                   << p.home.cy << p.home.cz << p.base.x << p.base.y << p.base.z << p.reach << p.anchored << p.priority << p.placement.yaw << p.placement.pitch
                   << p.placement.roll);
  }
  const PartLattice lattice = test_lattice();
  for (int c = 0; c < 60; ++c) {
    Value angles = obj({{"enabled", r() < 0.9}});
    if (r() < 0.7) {
      const double areas[4] = {900, 1800, 3600, 6400};
      angles.set("partArea", areas[static_cast<int>(std::floor(r() * 4))]);
    }
    if (r() < 0.5) angles.set("partCluster", 1 + std::floor(r() * 6));
    if (r() < 0.5) angles.set("maxPartsPerChunk", 1 + std::floor(r() * 3));
    if (r() < 0.5) angles.set("maxResident", 2 + std::floor(r() * 12));
    if (r() < 0.5) angles.set("residentRadius", 32 + std::floor(r() * 96));
    const Value config = obj({{"world", obj({{"angles", angles}})}});
    const double ci = big(20);
    const double cj = big(20);
    const Rect cell_rect = lattice.cell_rect(ci, cj);
    PartBudget b(config, cell_rect, c % 4 == 3 ? PartLattice{} : lattice);
    out << (Line() << "budget" << b.enabled << b.max_per_chunk << b.cluster << b.nx << b.ny << b.limit << b.max_resident << b.R << b.used.size());
    Line g;
    g << "grants";
    std::vector<std::string> grants;
    for (int k = 0; k < 120; ++k) {
      const double x = cell_rect.x0 + std::floor(r() * 2048);
      const double y = cell_rect.y0 + std::floor(r() * 2048);
      const ChunkXYZ home = home_chunk(x, y, std::floor(r() * 10), cell_rect);
      const bool fit = b.fits(x, y, home);
      grants.push_back(js::cat(fit ? 1 : 0, ":", b.grant(x, y, home)));
    }
    g << b.count;
    for (const std::string& s : grants) g << s;
    out << g;
    std::vector<double> homes;
    for (const Point2& q : b.homes) homes.insert(homes.end(), {q.x, q.y});
    out << (Line() << "used" << b.used << homes);
  }
  for (int i = 0; i < 100; ++i) {
    const int n = 2 + static_cast<int>(std::floor(r() * 5));
    std::vector<PPoint> pts;
    for (int k = 0; k < n; ++k) {
      PPoint q;
      q.x = big(3000);
      q.y = big(3000);
      if (k % 2) q.y += r();
      pts.push_back(q);
    }
    if (i % 10 == 0) pts.push_back(pts.back());
    const double hr = 8 + std::floor(r() * 40) / 2;
    for (const RoadPiece& pc : road_pieces(js::cat("R", i), pts, hr, Rect{-1500, -1500, 1500, 1500}))
      out << (Line() << "piece" << pc.key << pc.seg << pc.k << pc.s0 << pc.s1 << pc.a.x << pc.a.y << pc.b.x << pc.b.y << pc.aabb.x0 << pc.aabb.y0 << pc.aabb.x1 << pc.aabb.y1
                     << pc.home.cx << pc.home.cy << pc.home.cz << pc.reach);
  }
  CHECK(rec::record("parts", out.text()) == rec::recorded_digest("parts"));
}

TEST_CASE("city parts: ids and priorities") {
  CHECK(part_id(0, 0, 0) == 1);
  CHECK(part_id(2047, 2047, 255) == 1073741824.0);
  CHECK(part_priority("building", 5) == 3 * 16777216.0 + 5);
  CHECK(part_priority("wing", 5, true) < 0);
  const ChunkXYZ h = home_chunk(100, 100, 40, Rect{0, 0, 1024, 1024});
  CHECK(h.cx == 3);
  CHECK(h.cz == 1);
}
