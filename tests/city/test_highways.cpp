// svx_city tests — the elevated highway network (voxel_city network/highways.js) against the
// reference (stage "highways"): the lattice, edges built in full (decks, ramps, piers, plateaus),
// queries and the feature source, on the reference's own roads and waters (tools/procgen_ref/data).
#include <doctest.h>

#include "config/presets.hpp"
#include "core/hash.hpp"
#include "network/highways.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "terrain/terrain.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"

using namespace svx::city;
using rec::Line;

namespace {

struct NetCase {
  const char* key;
  const char* id;  // a preset, or JSON overrides
  const char* size;
  double a0, b0, a1, b1;
  std::vector<std::array<double, 3>> built;
};
// (stages/highways.mjs NETS)
const std::vector<NetCase>& nets() {
  static const std::vector<NetCase> v = {
      {"seed99", R"({"seed":99})", "", -2, -2, 6, 2, {{0, -1, 0}, {0, 4, 0}}},
      {"cities", "cities", "", -4, -4, 4, 4, {}},
      {"infiniteCity1337", R"({"seed":1337,"world":{"mode":"infiniteCity"}})", "", -3, -3, 3, 3, {}},
      {"wrapWorld:small", "wrapWorld", "small", 11, -2, 16, 2, {}},
  };
  return v;
}

Value config_of(const char* id, const char* size) {
  if (id[0] != '{') return preset_config(id, size);
  Value v;
  if (!Value::parse_json(id, &v)) SVX_FAIL("highways: bad JSON");
  return v;
}

std::string n(double v) { return js::num(v); }
std::string b(bool v) { return v ? "1" : "0"; }

// stages/roadparts.mjs chunkDigest
void chunk_digest(Line& l, const ChunkBuffer& c) {
  uint32_t h = 0;
  double cnt = 0;
  for (size_t q = 0; q < c.data.size(); ++q) {
    h = hash32(h, c.data[q], static_cast<double>(q));
    if (c.data[q]) cnt += 1;
  }
  l << h << cnt;
}

void ramp_at_fields(Line& l, const std::optional<HighwayRampAt>& ra) {
  if (ra)
    l << std::vector<std::string>{n(ra->z), n(ra->t), b(ra->outer), b(ra->inner), n(ra->ramp->s_deck)};
  else
    l << rec::kUndef;
}

// stages/highways.mjs edgeRecords
void edge_records(rec::Out& out, const World& w, const HighwayEdge& e, rec::Samples& r) {
  const HighwayNetwork& hw = *w.highways;
  {
    Line l;
    std::vector<std::vector<double>> nodes{{e.nodes[0][0], e.nodes[0][1], e.nodes[0][2]}, {e.nodes[1][0], e.nodes[1][1], e.nodes[1][2]}};
    std::vector<std::vector<double>> js_;
    for (const HighwayJunction& j : e.junctions) js_.push_back({j.x, j.y, j.z, j.r, j.degree, j.node[0], j.node[1]});
    l << "edge" << e.id << e.axis << e.a << e.b << nodes << e.total << std::vector<double>{e.bb.x0, e.bb.y0, e.bb.x1, e.bb.y1} << js_
      << static_cast<double>(e.pts.size());
    out << l;
  }
  {
    Line l;
    l << "pts";
    for (const PPoint& p : e.pts) l << p.x << p.y << p.z;
    out << l;
  }
  {
    Line l;
    l << "lengths";
    for (const double v : e.lengths) l << v;
    out << l;
  }
  for (const HighwaySeg& s : e.segs)
    out << (Line() << "hseg" << s.ax << s.ay << s.az << s.bx << s.by << s.bz << s.len << s.dx << s.dy << s.s0 << std::vector<double>{s.bb.x0, s.bb.y0, s.bb.x1, s.bb.y1});
  const std::vector<HighwayRamp>& ramps = hw.ramps(e);
  for (const HighwayRamp& rp : ramps)
    out << (Line() << "ramp" << rp.side << rp.s_deck << rp.s_ground << rp.drop << rp.z_deck << rp.z_ground << rp.cross << rp.x << rp.y << rp.arterial
                   << hw.ramp_piers(e, rp) << hw.ramp_blocked(e, rp));
  for (const HighwayPier& p : hw.piers(e)) out << (Line() << "pier" << p.s << p.x << p.y << p.tx << p.ty << p.z << p.cols << p.cap);
  // queries beside the deck and its ramps
  const double R = hw.ramp_in + 56 + 40;
  for (int k = 0; k < 300; ++k) {
    const double s = r() * e.total;
    const double d = (r() - 0.5) * 2 * R;
    const double margin = r() < 0.5 ? 0 : 8;
    const HighwayPoint p = offset_at(e, s, d);
    const std::optional<HighwayNearest> nr = hw.nearest(p.x, p.y, hw.edges_near(Rect{p.x - 1, p.y - 1, p.x + 1, p.y + 1}), R);
    const std::optional<HighwayRampAt> ra = hw.ramp_at(e, s, d);
    Line l;
    l << "q" << s << d << p.x << p.y;
    if (nr)
      l << std::vector<std::string>{n(nr->d), n(nr->s), n(nr->z), nr->edge->id, n(nr->seg->s0)};
    else
      l << rec::kUndef;
    l << hw.covers(p.x, p.y, margin);
    const std::optional<double> under = hw.underside(std::floor(p.x), std::floor(p.y));
    if (under)
      l << *under;
    else
      l << rec::kUndef;
    l << hw.on_road(p.x, p.y);
    ramp_at_fields(l, ra);
    out << l;
  }
  for (const HighwayRamp& rp : ramps) {
    const double s0 = js::min(rp.s_deck, rp.s_ground) - 30;
    const double s1 = js::max(rp.s_deck, rp.s_ground) + 30;
    for (int k = 0; k < 40; ++k) {
      const double s = s0 + r() * (s1 - s0);
      const double d = rp.side * (hw.ramp_mid + (r() - 0.5) * 80);
      const std::optional<HighwayRampAt> ra = hw.ramp_at(e, s, d);
      Line l;
      l << "rq" << s << d << ramp_z(e, rp, s);
      ramp_at_fields(l, ra);
      out << l;
    }
  }
  for (int k = 0; k < 40; ++k) {
    const double s = -100 + r() * (e.total + 200);
    const HighwayPoint p = point_at(e, s);
    const double off = (r() - 0.5) * 200;
    const HighwayPoint q = offset_at(e, s, off);
    out << (Line() << "at" << s << p.x << p.y << p.z << p.tx << p.ty << q.x << q.y << q.z);
  }
  for (int k = 0; k < 60; ++k) {
    const double s = r() * e.total;
    const double d = (r() - 0.5) * 2 * (R + 60);
    const double w2 = 8 + r() * 200;
    const double h2 = 8 + r() * 200;
    const HighwayPoint p = offset_at(e, s, d);
    const Rect rect{std::floor(p.x), std::floor(p.y), std::floor(p.x + w2), std::floor(p.y + h2)};
    Line l;
    l << "corr" << rect.x0 << rect.y0 << rect.x1 << rect.y1;
    for (const HighwayCorridor& c : hw.corridors_near(rect)) l << js::cat(c.edge->id, ":", c.hits_rect(rect) ? 1 : 0);
    out << l;
  }
  const HighwayPoint mid = point_at(e, e.total / 2);
  for (const HighwayNetwork::MapEdge& m : hw.map_data(Rect{mid.x - 500, mid.y - 500, mid.x + 500, mid.y + 500}))
    out << (Line() << "map" << m.id << m.width << static_cast<double>(m.pts.size()) << std::vector<double>{m.pts.front()[0], m.pts.front()[1]}
                   << std::vector<double>{m.pts.back()[0], m.pts.back()[1]});
  // the feature source
  std::vector<std::array<double, 3>> spots;
  for (int k = 1; k <= 5; ++k) {
    const HighwayPoint p = point_at(e, (e.total * k) / 6);
    spots.push_back({p.x, p.y, p.z});
    spots.push_back({p.x, p.y, p.z - 32});
  }
  for (const HighwayRamp& rp : ramps) {
    const double sm = (rp.s_deck + rp.s_ground) / 2;
    const HighwayPoint p = offset_at(e, sm, rp.side * hw.ramp_mid);
    spots.push_back({p.x, p.y, ramp_z(e, rp, sm)});
    spots.push_back({rp.x, rp.y, rp.z_ground});
  }
  const std::vector<HighwayPier>& piers = hw.piers(e);
  for (size_t k = 0; k < piers.size() && k < 4; ++k) spots.push_back({piers[k].x, piers[k].y, piers[k].z - 24});
  for (const HighwayJunction& j : e.junctions) {
    spots.push_back({j.x, j.y, j.z});
    spots.push_back({j.x + j.r * 0.7, j.y - j.r * 0.7, j.z});
  }
  int nspot = 0;
  for (const auto& spot : spots) {
    for (const int lod : {0, 2, 5}) {
      if (lod > 0 && nspot % 3 != 0) continue;
      const double sz = static_cast<double>(32 << lod);
      ChunkBuffer c(lod, std::floor(spot[0] / sz), std::floor(spot[1] / sz), std::floor(spot[2] / sz));
      std::vector<int32_t> tile;
      if (nspot % 2 == 1) {
        tile.assign(static_cast<size_t>(kP * kP), 0);
        for (int j = 0; j < kP; ++j)
          for (int i = 0; i < kP; ++i)
            tile[static_cast<size_t>(i + j * kP)] = js::i32(js::round(w.terrain->sample(c.wx(i), c.wy(j)).h) + ((i * 7 + j * 3) % 9) - 4);
      }
      rasterize_highways(w, c, tile.empty() ? nullptr : tile.data());
      const Box3 box = c.world_box();
      Line l;
      l << "chunk" << lod << c.cx << c.cy << c.cz << !tile.empty();
      double z0 = 0, z1 = 0;
      if (highway_z_range(w, Rect{box.x0, box.y0, box.x1, box.y1}, &z0, &z1))
        l << std::vector<double>{z0, z1};
      else
        l << rec::kUndef;
      chunk_digest(l, c);
      out << l;
    }
    nspot += 1;
  }
}

}  // namespace

TEST_CASE("city highways: the highway network conforms to the reference (stage highways)") {
  rec::Samples r(59);
  rec::Out out;
  const auto recorded = test::load_recorded("highways");
  for (const NetCase& nc : nets()) {
    World w(config_of(nc.id, nc.size));
    test::use_recorded(w, recorded.at(nc.key));
    w.highways = std::make_shared<HighwayNetwork>(w);
    const HighwayNetwork& hw = *w.highways;
    const HighwayNetwork::Cfg& c = hw.cfg;
    out << (Line() << "net" << nc.key << hw.spacing << hw.hw << hw.clear << hw.n << hw.plateau << hw.ramp_in << hw.ramp_mid << c.jitter << c.edge_chance
                   << c.min_urbanization << c.lanes_per_side << c.lane_width << c.pier_spacing);
    for (double bb = nc.b0; bb <= nc.b1; bb += 1)
      for (double a = nc.a0; a <= nc.a1; a += 1) {
        const HighwayNode nd = hw.node(a, bb);
        std::vector<std::string> at;
        for (const auto& q : hw.edges_at(a, bb)) at.push_back(js::cat(q[0], ":", q[1], ":", q[2]));
        out << (Line() << "node" << a << bb << nd.x << nd.y << nd.z << at << hw.node_z(a, bb));
        for (const int axis : {0, 1})
          out << (Line() << "lat" << axis << a << bb << hw.grade_ok(axis, a, bb) << hw.on_intercity_route(axis, a, bb) << hw.chance_edge(axis, a, bb)
                         << hw.base_edge(axis, a, bb, 0) << hw.base_edge(axis, a, bb, 1) << hw.edge_exists(axis, a, bb));
      }
    for (const auto& e : nc.built) {
      const HighwayEdgePtr edge = hw.edge(static_cast<int>(e[0]), e[1], e[2]);
      REQUIRE(edge);
      edge_records(out, w, *edge, r);
    }
  }
  CHECK(rec::record("highways", out.text()) == rec::recorded_digest("highways"));
}
