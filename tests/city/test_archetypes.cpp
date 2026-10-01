// svx_city tests — the building archetypes and their envelopes (voxel_city buildings/archetypes.js)
// against the reference (stage "archetypes" of tools/procgen_ref): fits over every lot size, every
// archetype's envelope on scripted contexts, planBuildingEnvelope and planBuildingEnvelopeAs on
// scripted lots, and each envelope's queries.
#include <doctest.h>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/styles.hpp"
#include "records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

std::string rle(const std::vector<char>& bits) {
  std::string out;
  size_t i = 0;
  while (i < bits.size()) {
    size_t j = i;
    while (j < bits.size() && bits[j] == bits[i]) ++j;
    if (!out.empty()) out += ',';
    out += js::cat(bits[i] ? "1" : "0", ":", static_cast<double>(j - i));
    i = j;
  }
  return out;
}

const std::vector<std::vector<std::string>>& domes() {
  static const std::vector<std::vector<std::string>> d = {{"GOLD", "DOME_GREEN", "DOME_BLUE", "GOLD"}, {"DOME_SHINGLE", "DOME_SHINGLE", "DOME_GREEN"}, {"NOPE"}, {}};
  return d;
}
const double kPitched[4] = {0.08, 0.35, 0.65, 1};

// The context envelope(ctx) gets: { lot, frame, district, rng, ...extra }.
ArchetypeCtx ctx_of(const Lot& lot, const Frame& lot_frame, const District& d, Rng& rng, const EnvelopeExtra& extra) {
  ArchetypeCtx ctx;
  ctx.lot = &lot;
  ctx.frame = &lot_frame;
  ctx.district = &d;
  ctx.rng = &rng;
  ctx.u = extra.u;
  ctx.core = extra.core;
  ctx.ground_z = extra.ground_z;
  ctx.config = extra.config;
  ctx.chapel = extra.chapel;
  ctx.dome = extra.dome;
  ctx.pitched_civic = extra.pitched_civic;
  return ctx;
}

void queries(rec::Out& out, const Envelope& e) {
  std::string tiers;
  for (double fl = -3; fl <= e.floors + 1; fl += 1) {
    if (fl != -3) tiers += '|';
    const std::vector<Rect>& rs = tier_rects(e, fl);
    if (rs.empty()) tiers += '-';
    for (size_t k = 0; k < rs.size(); ++k) tiers += (k ? ";" : "") + rect_str(rs[k]);
  }
  std::vector<double> zs, hs;
  for (double fl = -e.basements - 1; fl <= e.floors; fl += 1) zs.push_back(floor_z(e, fl));
  for (double fl = -1; fl < e.floors; fl += 1) hs.push_back(floor_height(e, fl));
  const Frame& fr = envelope_frame(e);
  Line l;
  l << "q" << tiers << zs << hs << rect_str(fr.R) << fr.U << fr.V << fr.turned;
  const XY a = fr.to_world(0, 0);
  const XY b = fr.to_world(e.U - 1, e.V - 1);
  const XY c = fr.from_world(e.R.x0, e.R.y0);
  l << a[0] << a[1] << b[0] << b[1] << c[0] << c[1];
  out << l;
}

}  // namespace

TEST_CASE("city archetypes: fits, envelopes and their finalizing are the reference's (stage archetypes)") {
  register_all();
  rec::Samples r(31);
  const std::vector<District> DS = district_list();
  const std::vector<Value> CFG = config_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const std::vector<Archetype>& all = archetype_registry().all();
  rec::Out out;
  // ---- fits over every size
  for (const Archetype& a : all) {
    std::vector<char> bits;
    bits.reserve(721 * 721);
    for (double U = 0; U <= 720; U += 1)
      for (double V = 0; V <= 720; V += 1) bits.push_back(a.fits(U, V) ? 1 : 0);
    out << (Line() << "fits" << a.id << static_cast<bool>(a.entrance_u) << rle(bits));
  }
  double k = 0;
  auto extra_of = [&] {
    EnvelopeExtra extra;
    extra.u = r();
    extra.core = r() * 1.2;
    extra.ground_z = std::floor(r() * 4000) - 200;
    extra.config = &CFG[static_cast<size_t>(std::floor(r() * static_cast<double>(CFG.size())))];
    if (r() < 0.3) extra.chapel = true;
    if (r() < 0.5) extra.dome = domes()[static_cast<size_t>(std::floor(r() * 4))];
    if (r() < 0.5) extra.pitched_civic = kPitched[static_cast<int>(std::floor(r() * 4))];
    return extra;
  };
  auto pick_district = [&]() -> const District& { return DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))]; };
  // ---- every archetype's envelope on scripted contexts
  for (const Archetype& a : all) {
    const int n = a.civic ? 30 : 240;
    for (int s = 0; s < n; ++s) {
      double U = 0, V = 0;
      for (int t = 0; t < 40; ++t) {
        U = 40 + std::floor(r() * 680);
        V = 40 + std::floor(r() * 680);
        if (a.fits(U, V) && r() < 0.95) break;
      }
      const District& d = pick_district();
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      Rng rng(std::floor(r() * 4294967296.0));
      const EnvelopeExtra extra = extra_of();
      const Frame lot_frame = lot_frame_of(lot.turn, lot.rect, lot.front);
      const ArchetypeCtx ctx = ctx_of(lot, lot_frame, d, rng, extra);
      const std::optional<EnvSpec> env = a.envelope(ctx);
      out << (Line() << "a" << a.id << U << V << d.id << lot.id << static_cast<bool>(lot.turn) << spec_line(env) << rng.next());
    }
  }
  // ---- tall towers
  std::vector<const District*> downtown;
  for (const District& d : DS)
    if (d.id == "downtown") downtown.push_back(&d);
  const Archetype& tower = archetype_registry().get("tower");
  for (int s = 0; s < 80; ++s) {
    const double U = 240 + std::floor(r() * 400);
    const double V = 240 + std::floor(r() * 400);
    const District& d = *downtown[static_cast<size_t>(std::floor(r() * static_cast<double>(downtown.size())))];
    const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
    Rng rng(std::floor(r() * 4294967296.0));
    EnvelopeExtra extra = extra_of();
    extra.core = 0.75 + r() * 0.5;
    const Frame lot_frame = lot_frame_of(lot.turn, lot.rect, lot.front);
    const ArchetypeCtx ctx = ctx_of(lot, lot_frame, d, rng, extra);
    const std::optional<EnvSpec> env = tower.envelope(ctx);
    out << (Line() << "t" << U << V << d.id << lot.id << static_cast<bool>(lot.turn) << spec_line(env) << rng.next());
  }
  // ---- planBuildingEnvelope
  for (int s = 0; s < 4000; ++s) {
    const double U = 30 + std::floor(r() * 640);
    const double V = 30 + std::floor(r() * 640);
    const District& d = pick_district();
    const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
    Rng rng(std::floor(r() * 4294967296.0));
    const EnvelopeExtra extra = extra_of();
    const std::optional<Envelope> env = plan_building_envelope(lot, d, rng, extra);
    out << (Line() << "p" << U << V << d.id << lot.id << static_cast<bool>(lot.turn) << env_line(env) << rng.next());
    if (env) queries(out, *env);
  }
  // ---- planBuildingEnvelopeAs
  for (const Archetype& a : all) {
    for (int s = 0; s < 50; ++s) {
      double U = 0, V = 0;
      for (int t = 0; t < 40; ++t) {
        U = 40 + std::floor(r() * 680);
        V = 40 + std::floor(r() * 680);
        if (a.fits(U, V) && r() < 0.9) break;
      }
      const District& d = pick_district();
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      const std::string& style = styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
      Rng rng(std::floor(r() * 4294967296.0));
      const EnvelopeExtra extra = extra_of();
      const std::optional<Envelope> env = plan_building_envelope_as(lot, a.id, style, d, rng, extra);
      out << (Line() << "as" << a.id << style << U << V << d.id << lot.id << static_cast<bool>(lot.turn) << env_line(env) << rng.next());
      if (env) queries(out, *env);
    }
  }
  CHECK(rec::record("archetypes", out.text()) == rec::recorded_digest("archetypes"));
}

TEST_CASE("city archetypes: the registry's order and a house's envelope") {
  register_all();
  const std::vector<Archetype>& all = archetype_registry().all();
  REQUIRE(all.size() == 34);
  const char* const mine[17] = {"house", "rowhouse", "walkup", "midrise", "panelSlab", "panelTower", "office", "tower", "warehouse",
                                "barn", "factory", "garage", "school", "townhouse", "wharfhouse", "cabin", "church"};
  for (int i = 0; i < 17; ++i) CHECK(all[static_cast<size_t>(17 + i)].id == mine[i]);
  Value config = make_config(Value::object());
  Lot lot;
  lot.id = "C0_0/b1/l2";
  lot.rect = {0, 0, 119, 199};
  lot.front = 'N';
  lot.district = "suburban";
  District d = district_registry().get("suburban");
  d.archetypes = {{"house", 1}};
  EnvelopeExtra extra;
  extra.config = &config;
  extra.ground_z = 100;
  Rng rng(7);
  const std::optional<Envelope> env = plan_building_envelope(lot, d, rng, extra);
  REQUIRE(env);
  CHECK(env->archetype == "house");
  CHECK(env->id == "C0_0/b1/l2/B");
  CHECK(env->base_z == 101);
  CHECK(floor_z(*env, 0) == 101);
  CHECK(floor_z(*env, 1) == 101 + env->story_h[0]);
  CHECK(tier_rects(*env, -1).size() == 1);
  CHECK(tier_rects(*env, 5).empty());
  const Frame& f = envelope_frame(*env);
  CHECK(&f == &envelope_frame(*env));  // (made once)
  const Envelope copy = *env;           // (a copy makes its own)
  CHECK(envelope_frame(copy).U == env->U);
}
