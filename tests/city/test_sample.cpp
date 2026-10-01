// svx_city tests — sample buildings staged on a world's lots (voxel_city buildings/sample.js)
// against the reference (stage "sample" of tools/procgen_ref): scripted worlds of cell plans,
// the reference's nordic stagings and every archetype staged, the plans dropped and envelopesIn.
#include <doctest.h>

#include <map>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/sample.hpp"
#include "buildings/styles.hpp"
#include "core/math.hpp"
#include "records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

constexpr double kCell = 4096;

// A cabin-sized square plot (14-22 m) at the front of a larger lot.
std::optional<Rect> cabin_plot(const Lot& lot, const Frame& f) {
  const double side = vx(14 + std::fmod(static_cast<double>(js::to_int32(lot.rect.x0 * 7 + lot.rect.y0 * 3) & 0xffff), 9));
  if (side <= f.U && side <= f.V) return Rect{0, 0, side - 1, side - 1};
  return std::nullopt;
}

template <class Next>
Lot sample_lot(Next& next, double i, double j, double k, const std::string& district_id) {
  const double U = 60 + std::floor(next() * 260);
  const double V = 80 + std::floor(next() * 260);
  const bool turned = next() < 0.2;
  const double x0 = i * kCell + std::floor(next() * 3000);
  const double y0 = j * kCell + std::floor(next() * 3000);
  Lot lot;
  lot.id = js::cat("C", i, "_", j, "/b", std::fmod(k, 3), "/l", k);
  lot.district = district_id;
  lot.corner = next() < 0.3;
  lot.micro = false;
  if (turned) {
    const int yaw = static_cast<int>(std::floor(next() * 132));
    lot.front = nominal_front(yaw);
    Turn t;
    t.yaw = yaw;
    t.origin = {x0, y0};
    t.U = U;
    t.V = V;
    lot.turn = t;
    lot.rect = lot_frame_of(lot.turn, Rect{}, lot.front).R;
  } else {
    lot.front = "NESW"[static_cast<int>(std::floor(next() * 4))];
    const bool ns = lot.front == 'N' || lot.front == 'S';
    lot.rect = {x0, y0, x0 + (ns ? U : V) - 1, y0 + (ns ? V : U) - 1};
  }
  lot.u = next();
  lot.core = next();
  lot.ground_z = std::floor(next() * 900);
  return lot;
}

// A scripted world: its plans made on first use (a pure function of (seed, i, j)), the plans it
// drops logged.
struct ScriptedWorld {
  const Value* config;
  const std::vector<District>* DS;
  std::map<std::pair<double, double>, std::shared_ptr<const StagePlan>> plans;
  std::vector<std::string> dropped;
  StagedEnvelopes staged;

  std::shared_ptr<const StagePlan> plan(double i, double j) {
    auto it = plans.find({i, j});
    if (it != plans.end()) return it->second;
    const double seed = (*config)["seed"].to_number();
    Rng rng = Rng::from(seed, "plan", i, j);
    auto next = [&] { return rng.next(); };
    auto p = std::make_shared<StagePlan>();
    const double n = 3 + std::floor(next() * 6);
    for (double k = 0; k < n; k += 1) {
      const District& d = (*DS)[static_cast<size_t>(std::floor(next() * static_cast<double>(DS->size())))];
      Lot lot = sample_lot(next, i, j, k, d.id);
      Rng brng = Rng::from(seed, lot.id, "building");
      EnvelopeExtra extra;
      extra.u = lot.u;
      extra.core = lot.core;
      extra.ground_z = lot.ground_z;
      extra.config = config;
      std::optional<Envelope> env = plan_building_envelope(lot, d, brng, extra);
      if (env) {
        lot.building = env->id;
        p->buildings.push_back(std::make_shared<const Envelope>(std::move(*env)));
      }
      p->lots.push_back(std::move(lot));
    }
    plans[{i, j}] = p;
    return p;
  }
  std::vector<std::shared_ptr<const Envelope>> base_envelopes_in(const Rect& rect) {
    std::vector<std::shared_ptr<const Envelope>> out;
    for (double j = -6; j <= 6; j += 1)
      for (double i = -6; i <= 6; i += 1)
        for (const auto& b : plan(i, j)->buildings)
          if (r_overlaps(b->bounds, rect)) out.push_back(b);
    return out;
  }
};

struct Run {
  std::string id, style;
  StageOpts opts;
  std::string district_id;  // ("": null)
};

}  // namespace

TEST_CASE("city sample: archetypes staged on scripted worlds are the reference's (stage sample)") {
  register_all();
  rec::Samples r(47);
  const std::vector<District> DS = district_list();
  const std::vector<Value> CFG = config_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  District oldtown;
  oldtown.id = "oldtown";
  oldtown.floors = {2, 3};
  District mixed = oldtown;
  mixed.id = "mixed";
  District forest;
  forest.id = "forest";
  forest.floors = {1, 1};
  rec::Out out;
  for (int w : {0, 3, 17, 21, 27}) {
    ScriptedWorld sw;
    sw.config = &CFG[static_cast<size_t>(w)];
    sw.DS = &DS;
    StageWorld world;
    world.seed = (*sw.config)["seed"].to_number();
    world.config = sw.config;
    world.cell_plan = [&](double i, double j) { return sw.plan(i, j); };
    world.staged = &sw.staged;
    world.drop_plan = [&](const std::string& id) { sw.dropped.push_back(id); };
    std::vector<Run> runs;
    auto run = [&](const char* id, const char* style, double n, double radius, std::optional<std::vector<std::string>> from, const District* d, bool plot) {
      Run x;
      x.id = id;
      x.style = style;
      x.opts.n = n;
      x.opts.radius = radius;
      x.opts.from = std::move(from);
      x.opts.district = d;
      if (plot) x.opts.lot_rect = cabin_plot;
      runs.push_back(std::move(x));
    };
    run("townhouse", "nordicWood", 5, 3, std::vector<std::string>{"walkup", "rowhouse"}, &oldtown, false);
    run("townhouse", "nordicPlaster", 2, 3, std::vector<std::string>{"rowhouse", "walkup"}, &mixed, false);
    run("cabin", "cabin", 4, 5, std::vector<std::string>{"house"}, &forest, true);
    run("church", "nordicChurch", 3, 3, std::vector<std::string>{"walkup", "midrise", "house"}, &oldtown, false);
    run("house", "siding", 3, 3, std::vector<std::string>{}, nullptr, false);
    run("office", "glass", 0, 3, std::nullopt, nullptr, false);
    for (const Archetype& a : archetype_registry().all()) {
      const double n = 1 + std::floor(r() * 3);
      const double radius = std::floor(r() * 4);
      const bool plot = r() < 0.3;
      const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
      run(a.id.c_str(), style.c_str(), n, radius, std::nullopt, nullptr, plot);
    }
    std::vector<std::shared_ptr<const Envelope>> staged;
    for (const Run& x : runs) {
      const std::vector<std::shared_ptr<const Envelope>> envs = stage_archetype(world, x.id, x.style, x.opts);
      std::string from = "-";
      if (x.opts.from) {
        from.clear();
        for (size_t k = 0; k < x.opts.from->size(); ++k) from += (k ? "," : "") + (*x.opts.from)[k];
        if (from.empty()) from = "[]";
      }
      out << (Line() << "stage" << w << x.id << x.style << x.opts.n << x.opts.radius << from << (x.opts.district ? x.opts.district->id : std::string("-"))
                     << static_cast<bool>(x.opts.lot_rect) << static_cast<double>(envs.size()) << static_cast<double>(sw.dropped.size()));
      for (const auto& e : envs) {
        out << (Line() << env_line(*e));
        staged.push_back(e);
      }
    }
    std::string dropped;
    for (size_t k = 0; k < sw.dropped.size(); ++k) dropped += (k ? "," : "") + sw.dropped[k];
    out << (Line() << "dropped" << dropped);
    std::vector<Rect> rects;
    for (const auto& e : staged) rects.push_back(e->bounds);
    for (int k = 0; k < 20; ++k) {
      const double x0 = std::floor((r() - 0.5) * 6 * kCell);
      const double y0 = std::floor((r() - 0.5) * 6 * kCell);
      const double x1 = x0 + std::floor(r() * 3000);
      const double y1 = y0 + std::floor(r() * 3000);
      rects.push_back({x0, y0, x1, y1});
    }
    for (const Rect& q : rects) {
      const std::vector<std::shared_ptr<const Envelope>> in = sw.staged.envelopes_in(q, sw.base_envelopes_in(q));
      std::string ids;
      for (size_t k = 0; k < in.size(); ++k) ids += (k ? "," : "") + in[k]->id;
      out << (Line() << "in" << rect_str(q) << (ids.empty() ? std::string("-") : ids));
    }
  }
  CHECK(rec::record("sample", out.text()) == rec::recorded_digest("sample"));
}
