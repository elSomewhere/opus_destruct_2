// svx_city tests — wings, corner bays, canted bays and chamfers (voxel_city buildings/wings.js)
// against the reference (stage "wings"): planWings on scripted sites, the wings' chunks in the
// world grid and in their own lattices; the building source (buildings/source.js) over them.
#include <doctest.h>

#include "buildings/archetypes.hpp"
#include "buildings/source.hpp"
#include "buildings/styles.hpp"
#include "buildings/wings.hpp"
#include "records.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const std::vector<int> kLods = {0, 1, 2, 3};

std::string box_str(const Box3& b) { return js::cat(b.x0, ",", b.y0, ",", b.z0, ",", b.x1, ",", b.y1, ",", b.z1); }

// stages/wings.mjs wingLine
std::string wing_line(const Wing& w) {
  const Turn& t = w.turn;
  const std::string turn = js::cat(t.yaw, "/", t.yaw2 ? js::num(t.yaw2) : std::string("-"), "/", t.origin.x, "/", t.origin.y, "/", t.ou, "/", t.ov);
  const Placement p = wing_placement(w);
  std::string pl = js::cat(p.origin.x, ",", p.origin.y, ",", p.origin.z, ",", p.yaw, ",", p.yaw2, ",", p.q);
  for (double m : p.m) pl += "," + js::num(m);
  pl += "," + js::num(p.d);
  const std::string chamfer = w.chamfer ? js::cat(std::string(1, w.chamfer->side), w.chamfer->a, "/", w.chamfer->b) : std::string("-");
  return (Line() << "wing" << w.kind << turn << w.U << w.V << w.f0 << w.f1 << w.z0 << w.z1 << box_str(w.bounds) << rect_str(w.canon) << chamfer << pl).str();
}

}  // namespace

TEST_CASE("city wings: planned and drawn as the reference's (stage wings)") {
  register_all();
  rec::Out out;
  rec::Samples r(67);
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  for (const size_t wk : {size_t{0}, size_t{1}, size_t{6}}) {
    Value cfg;
    REQUIRE(Value::parse_json(shell_worlds()[wk], &cfg));
    World w(cfg);
    CellRoads cell_roads;
    serve_cell_roads(w, cell_roads);
    const RoadSpecs specs(w.config);
    // (the building source's worlds: grid mode with a stub interior, parts mode)
    Value pcfg = cfg;
    pcfg.at_mut("world").at_mut("angles").set("partsMode", "separate");
    const World w_parts(pcfg);
    const VoxelizeBuildingFn stub = [](const Envelope& e, ChunkBuffer& c) { c.fill_box(e.R.x0, e.R.y0, e.base_z, e.R.x1, e.R.y1, e.base_z + 3, 7); };
    for (double k = 0; k < 242; k += 1) {
      const double i = 3 * std::fmod(k, 11) - 15;
      const double j = 3 * std::floor(k / 11) - 33;
      WingSite site = wing_site(r, w, cell_roads, i, j, k, DS, styles, specs);
      const Block* block = site.block ? &*site.block : nullptr;
      out << Line().operator<<(lot_line("site", site.lot, block));
      out << Line().operator<<(env_line(site.env));
      if (!site.env) continue;
      Envelope& env = *site.env;
      const bool nolot = r() < 0.04;
      Rng rng = Rng::from(w.seed, env.id, "wings");
      const std::vector<Wing> ws = plan_wings(w, env, nolot ? nullptr : &site.lot, block, *site.view, site.cell_id, rng);
      out << (Line() << "wings" << nolot << static_cast<double>(ws.size()) << rng.next());
      for (const Wing& q : ws) out << Line().operator<<(wing_line(q));
      if (ws.empty()) continue;
      // (granted, as the cell plan does: the envelope keeps them, a chamfer cuts its corner; a
      // turned building is a part of its own)
      env.wings = ws;
      for (const Wing& q : ws)
        if (q.chamfer) env.chamfer = *q.chamfer;
      if (env.turn) env.part = "1";
      const EnvelopeList envs = {std::shared_ptr<const Envelope>(std::shared_ptr<const Envelope>(), &env)};
      for (int src = 0; src < 2; ++src) {
        const World& ww = src ? w_parts : w;
        double z0 = 0, z1 = 0;
        const bool has = building_z_range(ww, envs, &z0, &z1);
        const Box3& b = ws[0].bounds;
        const std::vector<std::array<double, 3>> pts = {
            {(env.R.x0 + env.R.x1) / 2, (env.R.y0 + env.R.y1) / 2, env.base_z + 10},
            {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2},
        };
        out << (Line() << "bz" << (src == 1) << (has ? js::cat(z0, ",", z1) : std::string("-")));
        for (const ChunkAt& c : chunks_at({0, 1, 2}, pts)) {
          ChunkBuffer chunk(static_cast<int>(c[0]), c[1], c[2], c[3]);
          rasterize_buildings(ww, envs, chunk, src ? VoxelizeBuildingFn() : stub);
          Line l;
          l << "b" << c[0] << c[1] << c[2] << c[3];
          chunk_digest(l, chunk);
          out << l;
        }
      }
      for (const Wing& q : env.wings) {
        const Box3& b = q.bounds;
        const double zr = floor_z(env, q.f1 + 1);
        const std::vector<std::array<double, 3>> pts = {
            {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, (b.z0 + b.z1) / 2},
            {b.x0, b.y0, q.z0 + 2},
            {b.x1, b.y1, q.z1 - 1},
            {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, zr + 1},
            {b.x1 + 100, b.y1 + 100, q.z0},
        };
        for (const ChunkAt& c : chunks_at(kLods, pts)) {
          ChunkBuffer chunk(static_cast<int>(c[0]), c[1], c[2], c[3]);
          rasterize_wing(w, env, q, chunk);
          Line l;
          l << "g" << c[0] << c[1] << c[2] << c[3];
          chunk_digest(l, chunk);
          out << l;
        }
        const std::vector<std::array<double, 3>> lp = {
            {0, 0, q.z0},
            {q.U - 1, q.V - 1, q.z1},
            {q.U / 2, q.V / 2, zr + 1},
            {q.U + 200, q.V / 2, zr},
        };
        for (const ChunkAt& c : chunks_at(kLods, lp)) {
          ChunkBuffer chunk(static_cast<int>(c[0]), c[1], c[2], c[3]);
          rasterize_wing_part(w, env, q, chunk);
          Line l;
          l << "q" << c[0] << c[1] << c[2] << c[3];
          chunk_digest(l, chunk);
          out << l;
        }
      }
    }
  }
  CHECK(rec::record("wings", out.text()) == rec::recorded_digest("wings"));
}
