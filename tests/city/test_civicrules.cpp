// svx_city tests — the furnishing rules of civic rooms and shops (voxel_city
// buildings/interior/civicRules.js) against the reference (stage "civicrules" of
// tools/procgen_ref): the rules run in the stage's scripted room context, whose wall, free and
// wallBehind answer from a hash of their arguments and log every call.
#include <doctest.h>

#include <string>
#include <vector>

#include "buildings/interior/civicRules.hpp"
#include "prefab_records.hpp"
#include "records.hpp"

using namespace svx::city;
using irec::fo;
using rec::Line;

namespace {

double sum(const std::vector<PrefabBox>& boxes) {
  double s = 0;
  for (const PrefabBox& q : boxes) s += q.a0 + 3 * q.a1 + 5 * q.b0 + 7 * q.b1 + 11 * q.z0 + 13 * q.z1 + 17 * q.m;
  return s;
}

std::string fat(const std::optional<WallAt>& at) {
  if (!at) return "-";
  switch (at->kind) {
    case WallAt::Center: return "center";
    case WallAt::Start: return "start";
    case WallAt::End: return "end";
    case WallAt::Random: return "random";
    default: return js::num(at->value);
  }
}

class ScriptedCtx final : public FurnishCtx {
 public:
  ScriptedCtx(const Room& room_, const Rect& rect, double zf_, Rng rng_, bool cars_, double salt_, std::vector<Line>& log_)
      : FurnishCtx(room_, rect, zf_, rng_, cars_), salt(salt_), log(log_) {}

  std::string_view wall_behind(char side, double a) const override {
    static constexpr const char* kinds[8] = {"wall", "wall", "door", "ext", "wall", "open", "wall", "door"};
    return kinds[hash32(salt, static_cast<double>(side), a, 7) & 7];
  }

  std::optional<Placed> wall(std::string_view key, const WallOpts& opts) override {
    const Prefab* pf = furnish_prefab(key);
    REQUIRE(pf);
    calls += 1;
    const std::vector<char> sides = opts.sides ? *opts.sides : rng.shuffle(std::vector<char>{'N', 'E', 'S', 'W'});
    const char side = sides[0];
    const double L = side_len(side);
    const double w = opts.w ? *opts.w : pf->w;
    const double d = (opts.d ? *opts.d : pf->d) + pf->d_extra.value_or(0);
    const WallAt at = opts.at ? *opts.at : WallAt::center();
    const double a = at.kind == WallAt::Center ? js::round((L - w) / 2)
                     : at.kind == WallAt::Start ? 0
                     : at.kind == WallAt::End   ? L - w
                     : at.kind == WallAt::Number ? at.value
                                                 : rng.int_(0, js::max(0, L - w));
    const bool ok = (hash32(salt, calls, w, d) & 3) != 0 && w <= L;
    log.push_back(Line() << "wall" << std::string(key) << std::string(sides.begin(), sides.end()) << fat(opts.at) << fo(opts.w) << fo(opts.d) << fo(opts.keep_depth)
                         << fo(opts.pad) << (opts.prefab ? irec::prefab_opts(*opts.prefab) : std::string("-")) << a << ok);
    if (!ok) return std::nullopt;
    PrefabOpts p = opts.prefab ? *opts.prefab : PrefabOpts{};
    p.w = w;
    p.d = opts.d ? *opts.d : pf->d;
    const auto boxes = pf->build(rng, p);
    log.push_back(Line() << "wb" << static_cast<double>(boxes.size()) << sum(boxes));
    return Placed{std::string(key), side, a, 0, w, d};
  }

  std::optional<Placed> free(std::string_view key, const FreeOpts& opts) override {
    const Prefab* pf = furnish_prefab(key);
    REQUIRE(pf);
    calls += 1;
    const char side = opts.side.value_or('N');
    const double w = opts.w ? *opts.w : pf->w;
    const double d = opts.d ? *opts.d : pf->d;
    const double pad = opts.pad ? *opts.pad : pf->pad ? *pf->pad : 2;
    const double L = side_len(side);
    const double D = side_depth(side);
    const double a = opts.a ? *opts.a : js::round((L - w) / 2);
    const double b = opts.b ? *opts.b : js::round((D - d) / 2);
    const double range = (opts.exact && *opts.exact) ? 0 : opts.range.value_or(12);
    const bool ok = hash32(salt, calls, a, b) % 3 != 0 && w + 2 * pad <= L && d + 2 * pad <= D;
    log.push_back(Line() << "free" << std::string(key) << side << fo(opts.a) << fo(opts.b) << fo(opts.w) << fo(opts.d) << fo(opts.pad) << fo(opts.range)
                         << fo(opts.exact) << (opts.prefab ? irec::prefab_opts(*opts.prefab) : std::string("-")) << range << ok);
    if (!ok) return std::nullopt;
    // ({w, d, ...prefab}: the rule's prefab options over the size)
    PrefabOpts p = opts.prefab ? *opts.prefab : PrefabOpts{};
    if (!p.w) p.w = w;
    if (!p.d) p.d = d;
    const auto boxes = pf->build(rng, p);
    log.push_back(Line() << "fb" << static_cast<double>(boxes.size()) << sum(boxes));
    return Placed{std::string(key), side, a, b, w, d};
  }

  void rule(std::string_view type) override { log.push_back(Line() << "rule" << std::string(type) << rng.next()); }

  double salt;
  std::vector<Line>& log;
  double calls = 0;
};

}  // namespace

TEST_CASE("city civic rules: rules conform to the reference (stage civicrules)") {
  rec::Out out;
  std::string types;
  for (const CivicRule& rule : civic_rules()) types += js::cat(types.empty() ? "" : ",", rule.type);
  out << (Line() << "rules" << types);
  rec::Samples r(29);
  for (const CivicRule& rule : civic_rules()) {
    for (int i = 0; i < 10; ++i) {
      const double W = 6 + std::floor(r() * 110);
      const double H = 6 + std::floor(r() * 90);
      const double x0 = std::floor(r() * 50);
      const double y0 = std::floor(r() * 50);
      const Rect rect{x0, y0, x0 + W - 1, y0 + H - 1};
      const bool fire = r() < 0.5;
      const bool classical = r() < 0.5;
      const double fk = std::floor(r() * 3);
      const bool cars = r() < 0.7;
      const double zf = std::floor(r() * 100);
      const double salt = std::floor(r() * 1e9);
      const Rng rng(std::floor(r() * 4294967296.0));
      Room room;
      room.id = i;
      room.type = rule.type;
      room.rects = {rect};
      room.fire = fire;
      room.classical = classical;
      if (fk == 0) room.front = 'N';
      if (fk == 1) room.front = 'S';
      std::vector<Line> log;
      ScriptedCtx c(room, rect, zf, rng, cars, salt, log);
      rule.run(c);
      out << (Line() << "room" << rule.type << i << W << H << x0 << y0 << fire << classical << (room.front ? std::string(1, *room.front) : std::string("-")) << cars
                     << zf << salt);
      for (const Line& l : log) out << l;
      Line boxes;
      boxes << "boxes";
      for (const CanonBox& q : c.boxes) boxes << q.x0 << q.y0 << q.x1 << q.y1 << q.z0 << q.z1 << q.m;
      out << boxes;
      out << (Line() << "end" << c.rng.next());
    }
  }
  CHECK(rec::record("civicrules", out.text()) == rec::recorded_digest("civicrules"));
}

TEST_CASE("city civic rules: lookups by room type") {
  CHECK(civic_rules().size() == 52);
  CHECK(civic_rule("ward") != nullptr);
  CHECK(civic_rule("living") == nullptr);  // (one of furnish.js's own)
}
