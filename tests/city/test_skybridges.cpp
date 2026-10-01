// svx_city tests — skybridges (voxel_city city/skybridges.js) against the reference (stage
// "skybridges"): bridges between scripted rows of facing buildings, their sky doors, and the
// feature source's z ranges and chunks.
#include <doctest.h>

#include <deque>

#include "buildings/archetypes.hpp"
#include "buildings/styles.hpp"
#include "city/skybridges.hpp"
#include "records.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const char* const kKinds[] = {"office", "office", "office", "tower", "tower", "walkup"};
const char* const kFlavors[] = {"futuristic", "futuristic", "modern", ""};

std::string door_str(const SkyDoor& d) { return js::cat(d.floor, "/", d.span_x ? "x" : "y", d.s0, ",", d.s1, "/", d.bridge); }

}  // namespace

TEST_CASE("city skybridges: bridges, doors and the feature source are the reference's (stage skybridges)") {
  register_all();
  rec::Out out;
  rec::Samples r(79);
  const std::vector<District> DS = district_list();
  std::vector<const District*> downtown;
  for (const District& d : DS)
    if (d.id == "downtown") downtown.push_back(&d);
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  for (int cell = 0; cell < 100; ++cell) {
    const World& w = shell_world(static_cast<size_t>(cell % 3));
    const bool along_x = r() < 0.5;
    const double n = 2 + std::floor(r() * 3);
    const double ox = std::floor((r() - 0.5) * 100000);
    const double oy = std::floor((r() - 0.5) * 100000);
    const double gap = 50 + std::floor(r() * 210);
    const bool wide = r() < 0.4;
    const int wide_side = r() < 0.5 ? 0 : 1;
    std::deque<Envelope> envs;
    std::vector<Envelope*> buildings;
    for (int side = 0; side < 2; ++side) {
      double pos = (along_x ? ox : oy) + std::floor(r() * 80) - 40;
      const bool big = wide && side == wide_side;
      for (double q = 0; q < (big ? 1 : wide ? n + 2 : n); q += 1) {
        const char* kd = kKinds[static_cast<size_t>(std::floor(r() * 6))];
        const std::string kind = wide ? "office" : kd;
        const double u = r();
        const double U = big ? 1200 + std::floor(u * 600) : wide ? 160 + std::floor(u * 40) : 160 + std::floor(u * 300);
        const double V = 160 + std::floor(r() * 300);
        const double shift = std::floor(r() * 60);
        const bool turned = r() < 0.08;
        const int yaw = static_cast<int>(std::floor(r() * 132));
        const bool dt = r() < 0.7;
        const District& d = wide || dt ? *downtown[static_cast<size_t>(std::floor(r() * static_cast<double>(downtown.size())))]
                                       : DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
        const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
        const std::string fl = kFlavors[static_cast<size_t>(std::floor(r() * 4))];
        const std::string flavor = wide ? "futuristic" : fl;
        const double z = 400 + std::floor(r() * 4);
        Lot lot;
        lot.id = js::cat("C0_", cell, "/b", side, "/l", q);
        lot.district = d.id;
        lot.corner = false;
        lot.micro = false;
        if (along_x) {
          lot.front = side ? 'N' : 'S';
          const double y0 = side ? oy + 1 + gap : oy - V + 1;
          lot.rect = {pos, y0, pos + U - 1, y0 + V - 1};
        } else {
          lot.front = side ? 'W' : 'E';
          const double x0 = side ? ox + 1 + gap : ox - V + 1;
          lot.rect = {x0, pos, x0 + V - 1, pos + U - 1};
        }
        if (turned && !big) {
          lot.front = nominal_front(yaw);
          Turn t;
          t.yaw = yaw;
          t.origin = {lot.rect.x0, lot.rect.y0};
          t.ou = 0;
          t.ov = 0;
          t.U = U;
          t.V = V;
          lot.turn = t;
          lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
        }
        pos += U + shift;
        EnvelopeExtra extra;
        extra.u = r();
        extra.core = r();
        extra.ground_z = z;
        extra.config = &w.config;
        Rng rng(std::floor(r() * 4294967296.0));
        std::optional<Envelope> env = plan_building_envelope_as(lot, kind, style, d, rng, extra);
        out << (Line() << "b" << kind << (flavor.empty() ? std::string("-") : flavor) << env_line(env));
        if (!env) continue;
        if (!flavor.empty()) env->flavor = flavor;
        envs.push_back(std::move(*env));
        buildings.push_back(&envs.back());
      }
    }
    // a highway corridor across the street now and then
    const bool hc = r() < 0.4;
    const double ha = std::floor(r() * 1500);
    const double hs = 100 + std::floor(r() * 600);
    const Rect c = along_x ? Rect{ox + ha, oy - 40, ox + ha + hs, oy + gap + 40} : Rect{ox - 40, oy + ha, ox + gap + 40, oy + ha + hs};
    std::function<bool(const Rect&)> corridor_hits;
    if (hc) corridor_hits = [c](const Rect& q) { return !(q.x1 < c.x0 || q.x0 > c.x1 || q.y1 < c.y0 || q.y0 > c.y1); };
    const std::vector<Skybridge> bridges = plan_skybridges(w, js::cat("C0_", cell), buildings, corridor_hits);
    out << (Line() << "cell" << cell << along_x << gap << (hc ? rect_str(c) : std::string("-")) << static_cast<double>(bridges.size()));
    for (const Skybridge& br : bridges) out << (Line() << "bridge" << br.id << br.a << br.b << br.along_x << rect_str(br.rect) << br.za << br.zb << br.bb.z0 << br.bb.z1);
    for (const Envelope* env : buildings) {
      std::string doors;
      for (size_t k = 0; k < env->sky_doors.size(); ++k) doors += (k ? ";" : "") + door_str(env->sky_doors[k]);
      out << (Line() << "doors" << env->id << (doors.empty() ? std::string("-") : doors));
    }
    // the feature source: the cell's bridges whose rect meets the chunk (and a cell without any)
    const std::vector<Skybridge> none;
    for (const Skybridge& br : bridges) {
      const Rect& q = br.rect;
      const std::vector<std::array<double, 3>> pts = {
          {(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, (br.za + br.zb) / 2 + 10},
          {q.x0, q.y0, br.za},
          {q.x1, q.y1, br.zb + 24},
          {q.x1 + 300, q.y1, br.za},
          {(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, br.zb + 400},
      };
      for (const ChunkAt& ca : chunks_at({0, 1, 2, 3, 4}, pts)) {
        ChunkBuffer ch(static_cast<int>(ca[0]), ca[1], ca[2], ca[3]);
        const Box3 b = ch.world_box();
        const Rect box{b.x0, b.y0, b.x1, b.y1};
        std::vector<const Skybridge*> near = skybridges_in(bridges, box);
        for (const Skybridge* o : skybridges_in(none, box)) near.push_back(o);
        double z0 = 0, z1 = 0;
        const bool has = skybridge_z_range(near, &z0, &z1);
        rasterize_skybridges(near, ch);
        Line l;
        l << "c" << ca[0] << ca[1] << ca[2] << ca[3] << (has ? js::cat(z0, ",", z1) : std::string("-"));
        chunk_digest(l, ch);
        out << l;
      }
    }
  }
  CHECK(rec::record("skybridges", out.text()) == rec::recorded_digest("skybridges"));
}
