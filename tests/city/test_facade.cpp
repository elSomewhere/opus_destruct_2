// svx_city tests — facade rules (voxel_city buildings/facade.js) against the reference (stage
// "facade" of tools/procgen_ref): building looks of every style, facade classes and materials.
#include <doctest.h>

#include <atomic>
#include <thread>

#include "building_records.hpp"
#include "buildings/archetypes.hpp"
#include "buildings/facade.hpp"
#include "buildings/styles.hpp"
#include "records.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const double kLens[] = {5, 13, 24, 37, 61, 157};
const double kFloors[] = {0, 1, 3};
const double kZrs[] = {0, 1, 2, 3, 5, 6, 7, 99};
const char* const kCode = "0123456789abcdef";
constexpr int kRaw = 60;

uint32_t fnv(const std::string& s) {
  uint32_t h = 0x811c9dc5u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

std::string mat(int m) { return m < 0 ? std::string("-") : js::num(m); }
std::string opt_int(int m) { return m < 0 ? std::string("-") : js::num(m); }

}  // namespace

TEST_CASE("city facade: looks, classes and materials are the reference's (stage facade)") {
  register_all();
  rec::Out out;
  out << (Line() << "win" << "WALL:0" << "GLASS:1" << "FRAME:2" << "SILL:3" << "LINTEL:4" << "STOREFRONT:5" << "SPANDREL:6" << "JOINT:7" << "SEAM:8" << "CASING:9"
                 << "CORNER:10");
  rec::Samples r(43);
  const std::vector<District> DS = district_list();
  const std::vector<Value> CFG = config_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const std::vector<Archetype>& all = archetype_registry().all();
  double k = 0;
  for (int s = 0; s < 300; ++s) {
    const std::string& style = styles[static_cast<size_t>(s) % styles.size()];
    std::optional<Envelope> env;
    const Value* config = nullptr;
    for (int tries = 0; !env && tries < 20; ++tries) {
      const Archetype& a = all[static_cast<size_t>(std::floor(r() * static_cast<double>(all.size())))];
      double U = 0, V = 0;
      for (int t = 0; t < 40; ++t) {
        U = 40 + std::floor(r() * 680);
        V = 40 + std::floor(r() * 680);
        if (a.fits(U, V)) break;
      }
      const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
      const Lot lot = scripted_lot(r, U, V, (k += 1), d.id);
      config = &CFG[static_cast<size_t>(std::floor(r() * static_cast<double>(CFG.size())))];
      Rng rng(std::floor(r() * 4294967296.0));
      EnvelopeExtra extra;
      extra.u = r();
      extra.core = r() * 1.2;
      extra.ground_z = 0;
      extra.config = config;
      env = plan_building_envelope_as(lot, a.id, style, d, rng, extra);
    }
    if (!env) {
      out << (Line() << "look" << "-");
      continue;
    }
    const double seed = (*config)["seed"].to_number();
    const BuildingLook& look = building_look(*env, seed);
    const ResolvedStyle& st = look.style;
    out << (Line() << "look" << env->id << env->archetype << env->style << env->program.ground << (env->extra && env->extra->storefront) << st.id << st.wall << st.base
                   << st.trim << st.glass << st.frame << st.roof << st.pitched << st.storefront << st.fire_escape << st.balcony << st.water_tank << st.awning << st.shutters
                   << st.accent_strips << opt_int(st.joint) << opt_int(st.seam) << (st.seam_dir ? std::string(1, st.seam_dir) : std::string("-")) << st.corners
                   << st.shop_base << st.base_h << look.bay << look.win_w << look.sill << look.head << look.type << look.lintel << look.storefront << look.lit_seed
                   << opt_int(look.joint) << look.casing << look.transom << look.boards << (&building_look(*env, seed) == &look));
    for (double len : kLens)
      for (double fl : kFloors) {
        const double H = env->story_h[static_cast<size_t>(js::min(fl, static_cast<double>(env->story_h.size()) - 1))];
        std::string row;
        for (double zr = 0; zr <= H + 1; zr += 1)
          for (double t = 0; t < len; t += 1) row += kCode[facade_cell(look, len, t, zr, H, fl)];
        Line l;
        l << "fc" << len << fl << H;
        if (s < kRaw)
          l << row;
        else
          l << static_cast<double>(fnv(row));
        out << l;
      }
    std::string wall;
    for (double len : {4.0, 9.0})
      for (double t = 0; t < len; t += 1)
        for (double zr = 0; zr < 7; zr += 1) wall += kCode[wall_class(look, len, t, zr)];
    std::string mats = "[";
    bool first = true;
    auto add = [&](int m) {
      if (!first) mats += ',';
      first = false;
      mats += mat(m);
    };
    for (int cls = 0; cls <= 11; ++cls)
      for (double fl : kFloors)
        for (double zr : kZrs) add(facade_material(look, cls, fl, zr));
    add(facade_material(look, 6, 3));
    add(facade_material(look, 8, 0));
    add(facade_material(look, 10, 0));
    mats += "]";
    out << (Line() << "fm" << wall << mats);
  }
  CHECK(rec::record("facade", out.text()) == rec::recorded_digest("facade"));
}

TEST_CASE("city facade: an envelope's look and frame are made once, the same from several threads") {
  register_all();
  const Value config = make_config(Value::object());
  const double seed = config["seed"].to_number();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  std::vector<std::shared_ptr<const Envelope>> envs;
  rec::Samples r(5);
  for (int k = 0; envs.size() < 60 && k < 600; ++k) {
    const Lot lot = scripted_lot(r, 100 + std::floor(r() * 200), 100 + std::floor(r() * 200), k, "residential");
    Rng rng(std::floor(r() * 4294967296.0));
    EnvelopeExtra extra;
    extra.config = &config;
    District d = district_registry().get("residential");
    const std::string& style = styles[static_cast<size_t>(k) % styles.size()];
    if (std::optional<Envelope> env = plan_building_envelope(lot, d, rng, extra)) {
      env->style = style;
      envs.push_back(std::make_shared<const Envelope>(std::move(*env)));
    }
  }
  REQUIRE(envs.size() == 60);
  // what a look and a frame are, made afresh
  std::vector<double> lit;
  std::vector<Rect> frames;
  for (const auto& e : envs) {
    lit.push_back(make_building_look(*e, seed).lit_seed);
    frames.push_back(frame_of(e->turn, e->U, e->V, e->front, e->R).R);
  }
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t n = 0; n < envs.size(); ++n) {
        const size_t i = t % 2 ? envs.size() - 1 - n : (n * 7 + static_cast<size_t>(t)) % envs.size();
        const BuildingLook& look = building_look(*envs[i], seed);
        if (look.lit_seed != lit[i] || &look != &building_look(*envs[i], seed)) bad += 1;
        if (!(envelope_frame(*envs[i]).R == frames[i])) bad += 1;
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK(bad.load() == 0);
}
