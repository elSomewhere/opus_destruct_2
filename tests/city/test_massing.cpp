// svx_city tests — the coarse building voxelizer (voxel_city buildings/massing.js) against the
// reference (stage "massing"): snowAt, roof snow covers, and the chunks voxelizeMassing and
// pitchedRoofOnly draw round envelopes of every archetype, in every season.
#include <doctest.h>

#include <atomic>
#include <thread>

#include "buildings/archetypes.hpp"
#include "buildings/massing.hpp"
#include "buildings/styles.hpp"
#include "records.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/register_all.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

const std::vector<int> kLods = {0, 1, 2, 3, 5};
const double kCovers[] = {0.05, 0.3, 0.5, 0.8, 0.99, 1, 1.3};
const double kSnows[] = {0, 0.45, 1};

std::string chamfer_str(const Envelope& e) {
  if (!e.chamfer) return "-";
  return js::cat(std::string(1, e.chamfer->side), e.chamfer->a, "/", e.chamfer->b);
}

}  // namespace

TEST_CASE("city massing: snow, roof covers and the chunks a shell draws are the reference's (stage massing)") {
  register_all();
  rec::Out out;
  rec::Samples r(61);
  // ---- snowAt over covers and columns
  for (int k = 0; k < 300; ++k) {
    const double seed = std::floor((r() - 0.5) * 4294967296.0);
    const double cover = kCovers[k % 7];
    const double x0 = std::floor((r() - 0.5) * 200000);
    const double y0 = std::floor((r() - 0.5) * 200000);
    std::string bits;
    for (int j = 0; j < 24; ++j)
      for (int i = 0; i < 24; ++i) bits += snow_at(seed, cover, x0 + i * 3, y0 + j * 5) ? '1' : '0';
    out << (Line() << "snow" << seed << cover << x0 << y0 << bits);
  }
  // ---- envelopes of every archetype in every world
  const std::vector<District> DS = district_list();
  std::vector<std::string> styles;
  for (const Style& s : style_registry().all()) styles.push_back(s.id);
  const std::vector<Archetype>& all = archetype_registry().all();
  // (every archetype eight times, then more churches: steeples, domes, tented spires)
  std::vector<const Archetype*> kinds;
  for (int n = 0; n < 8; ++n)
    for (const Archetype& a : all) kinds.push_back(&a);
  for (int n = 0; n < 24; ++n) kinds.push_back(&archetype_registry().get("church"));
  double k = 0;
  for (const Archetype* ap : kinds) {
    const Archetype& a = *ap;
    const size_t wk = static_cast<size_t>(std::floor(r() * static_cast<double>(shell_worlds().size())));
    const World& w = shell_world(wk);
    const std::string* style = r() < 0.15 ? nullptr : &styles[static_cast<size_t>(std::floor(r() * static_cast<double>(styles.size())))];
    const District& d = DS[static_cast<size_t>(std::floor(r() * static_cast<double>(DS.size())))];
    ShellCase sc = shell_envelope(r, a, style, d, w, (k += 1));
    std::optional<Envelope>& env = sc.env;
    const double ch = r();
    const double cl = r();
    const double ck = r();
    const double sn = r();
    const double oh = r();
    if (env && env->roof.type == "flat" && ch < 0.3) {
      const double m = 2 + std::floor(ck * 2);
      env->chamfer = Chamfer{cl < 0.5 ? 'L' : 'R', (ck < 0.5 ? 20 : 21) * m, (ck < 0.5 ? 21 : 20) * m};
    }
    if (env && sn < 0.06) env->snow = kSnows[static_cast<size_t>(std::floor(sn * 50))];
    // (a roof whose overhang leaves sides out: they take the default)
    if (env && env->roof.type != "flat" && oh < 0.15) {
      env->roof.overhang_kind = EnvRoof::Overhang::Sides;
      env->roof.overhang_f = oh < 0.075 ? 1 : js::kNaN;
      env->roof.overhang_b = oh < 0.075 ? js::kNaN : 4;
      env->roof.overhang_l = oh < 0.075 ? js::kNaN : 2;
      env->roof.overhang_r = oh < 0.075 ? 5 : js::kNaN;
    }
    out << (Line() << "m" << a.id << static_cast<double>(wk) << (style ? *style : std::string("-")) << d.id << (env ? chamfer_str(*env) : std::string("-")) << env_line(env));
    if (!env) continue;
    out << (Line() << "cover" << roof_snow_cover(w, *env));
    const Rect& b = env->bounds;
    const Rect& R = env->R;
    const double z_top = floor_z(*env, env->floors);
    std::vector<std::array<double, 3>> pts = {
        {(R.x0 + R.x1) / 2, (R.y0 + R.y1) / 2, z_top + 6},
        {(R.x0 + R.x1) / 2, (R.y0 + R.y1) / 2, (z_top + env->top_z) / 2},
        {R.x0, R.y0, env->base_z + 4},
    };
    for (const EnvelopeAnnex& an : env->annexes) pts.push_back({(an.world.x0 + an.world.x1) / 2, (an.world.y0 + an.world.y1) / 2, env->base_z + 4});
    if (env->steeple) {
      // (the tower's belfry and its spire's finial)
      const Rect t = envelope_frame(*env).rect_to_world(env->tiers[0].rects[static_cast<size_t>(env->steeple->rect)]);
      const double top = z_top + env->steeple->shaft + env->steeple->spire;
      pts.push_back({(t.x0 + t.x1) / 2, (t.y0 + t.y1) / 2, z_top + env->steeple->shaft - 10});
      pts.push_back({(t.x0 + t.x1) / 2, (t.y0 + t.y1) / 2, top + 6});
    }
    // (chunks the building does not reach: beside it, above and below it)
    pts.push_back({b.x1 + 80, b.y1 + 80, z_top});
    pts.push_back({(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, env->top_z + 80});
    pts.push_back({(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, env->bottom_z - 80});
    const double fx = R.x0 + r() * (R.x1 - R.x0);
    const double fz = env->base_z + r() * (z_top - env->base_z);
    pts.push_back({fx, R.y0, fz});
    for (int q = 0; q < 2; ++q) {
      const double x = b.x0 + r() * (b.x1 - b.x0);
      const double y = b.y0 + r() * (b.y1 - b.y0);
      const double z = env->bottom_z + r() * (env->top_z - env->bottom_z);
      pts.push_back({x, y, z});
    }
    for (const ChunkAt& c : chunks_at(kLods, pts)) {
      ChunkBuffer chunk(static_cast<int>(c[0]), c[1], c[2], c[3]);
      voxelize_massing(w, *env, chunk);
      Line l;
      l << "c" << c[0] << c[1] << c[2] << c[3];
      chunk_digest(l, chunk);
      out << l;
    }
    std::vector<std::array<double, 3>> roof_pts(pts.begin(), pts.begin() + static_cast<long>(3 + env->annexes.size()));
    roof_pts.push_back(pts[pts.size() - 6]);
    for (const ChunkAt& c : chunks_at({0, 1}, roof_pts)) {
      ChunkBuffer chunk(static_cast<int>(c[0]), c[1], c[2], c[3]);
      pitched_roof_only(w, *env, chunk);
      Line l;
      l << "p" << c[0] << c[1] << c[2] << c[3];
      chunk_digest(l, chunk);
      out << l;
    }
  }
  CHECK(rec::record("massing", out.text()) == rec::recorded_digest("massing"));
}

TEST_CASE("city massing: an envelope's snow cover is made once, the same from several threads") {
  register_all();
  const World& w = shell_world(1);
  const std::vector<District> DS = district_list();
  std::vector<std::shared_ptr<const Envelope>> envs;
  rec::Samples r(9);
  const std::vector<Archetype>& all = archetype_registry().all();
  double k = 0;
  for (int n = 0; envs.size() < 40 && n < 400; ++n) {
    ShellCase sc = shell_envelope(r, all[static_cast<size_t>(n) % all.size()], nullptr, DS[static_cast<size_t>(n) % DS.size()], w, (k += 1));
    if (sc.env) envs.push_back(std::make_shared<const Envelope>(std::move(*sc.env)));
  }
  REQUIRE(envs.size() == 40);
  // what each cover and one chunk of each are, made on a fresh copy
  std::vector<double> covers;
  std::vector<std::vector<uint16_t>> chunks;
  auto chunk_of = [](const Envelope& e) {
    ChunkBuffer c(1, std::floor(((e.R.x0 + e.R.x1) / 2) / 64), std::floor(((e.R.y0 + e.R.y1) / 2) / 64), std::floor((e.base_z + 8) / 64));
    return c;
  };
  for (const auto& e : envs) {
    const Envelope copy = *e;
    covers.push_back(roof_snow_cover(w, copy));
    ChunkBuffer c = chunk_of(copy);
    voxelize_massing(w, copy, c);
    chunks.push_back(c.data);
  }
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
    threads.emplace_back([&, t] {
      for (size_t n = 0; n < envs.size(); ++n) {
        const size_t i = t % 2 ? envs.size() - 1 - n : (n * 7 + static_cast<size_t>(t)) % envs.size();
        if (roof_snow_cover(w, *envs[i]) != covers[i]) bad += 1;
        ChunkBuffer c = chunk_of(*envs[i]);
        voxelize_massing(w, *envs[i], c);
        if (c.data != chunks[i]) bad += 1;
      }
    });
  for (std::thread& th : threads) th.join();
  CHECK(bad.load() == 0);
}
