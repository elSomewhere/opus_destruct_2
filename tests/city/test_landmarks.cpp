// svx_city tests — an island's landmarks (voxel_city world/landmarks.js) against the reference
// (stage "landmarks"): the landmarks of every island world (the open-ground test's answers
// replayed from tools/procgen_ref/data/landmarks.json: the cell plans are a later stage), blocks,
// near, the feature source; free() itself on scripted roads and plans.
#include <doctest.h>

#include <map>
#include <utility>

#include "core/hash.hpp"
#include "network/arterials.hpp"
#include "records.hpp"
#include "road_inputs.hpp"
#include "shell_records.hpp"
#include "voxel/chunk.hpp"
#include "world/World.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"
#include "world/landmarks.hpp"
#include "worlds.hpp"

using namespace svx::city;
using namespace svx::city::test;
using rec::Line;

namespace {

uint32_t fnv(const std::string& s) {
  uint32_t h = 0x811c9dc5u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

std::string box_str(const Box3& b) { return js::cat(b.x0, ",", b.y0, ",", b.z0, ",", b.x1, ",", b.y1, ",", b.z1); }

using FreeAnswers = std::map<std::pair<double, double>, bool>;

}  // namespace

TEST_CASE("city landmarks: an island's landmarks and their source are the reference's (stage landmarks)") {
  rec::Out out;
  rec::Samples r(83);
  const Value data = read_json_file(std::string(SVX_SOURCE_DIR) + "/tools/procgen_ref/data/landmarks.json");
  std::vector<WorldCase> worlds = all_worlds();
  {
    Value v;
    REQUIRE(Value::parse_json(R"({"seed":17,"world":{"mode":"island","island":{"radius":1800,"peak":null,"population":3000}}})", &v));
    worlds.push_back({"islandNoPeak", v});
  }
  int unasked = 0;
  for (const WorldCase& wc : worlds) {
    const std::shared_ptr<World> wp = create_world(wc.overrides);
    const World& w = *wp;
    if (!w.landmarks) continue;
    // (the open-ground test's answers, as the reference gave them)
    auto answers = std::make_shared<FreeAnswers>();
    const Value& asked = data[wc.key];
    for (const Value& a : asked.items()) (*answers)[{a[size_t{0}].to_number(), a[size_t{1}].to_number()}] = a[size_t{2}].to_number() != 0;
    w.landmarks->free_source = [answers, &unasked](double x, double y) {
      auto it = answers->find({x, y});
      if (it == answers->end()) {
        unasked += 1;
        return false;
      }
      return it->second;
    };
    const std::vector<Landmark>& items = w.landmarks->all();
    out << (Line() << "world" << wc.key << static_cast<double>(items.size()) << static_cast<double>(asked.size()));
    for (const Landmark& it : items) {
      std::string boxes;
      for (size_t k = 0; k < it.boxes.size(); ++k) {
        const LandmarkBox& q = it.boxes[k];
        boxes += js::cat(k ? ";" : "", q.x0, ",", q.y0, ",", q.x1, ",", q.y1, ",", q.z0, ",", q.z1, ",", q.m);
      }
      out << (Line() << "lm" << it.kind << box_str(it.bb) << rect_str(it.foot) << static_cast<double>(it.boxes.size()) << fnv(boxes));
    }
    // blocks: round each landmark and over the island
    const IslandPlan& isl = *w.fields->island;
    const Rect b = isl.bounds();
    std::string bits;
    for (const Landmark& it : items)
      for (int k = 0; k < 24; ++k) {
        const double x = std::floor(it.foot.x0 - 40 + r() * (it.foot.x1 - it.foot.x0 + 80));
        const double y = std::floor(it.foot.y0 - 40 + r() * (it.foot.y1 - it.foot.y0 + 80));
        const double m = k % 3 == 0 ? 16 : k % 3 == 1 ? 0 : 6;
        bits += w.landmarks->blocks(x, y, m) ? '1' : '0';
      }
    for (int k = 0; k < 200; ++k) {
      const double x = std::floor((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const double y = std::floor((b.y0 + r() * (b.y1 - b.y0)) * 8);
      bits += w.landmarks->blocks(x, y) ? '1' : '0';
    }
    out << (Line() << "blocks" << bits);
    // near over sample rects
    for (int k = 0; k < 30; ++k) {
      const double x0 = std::floor((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const double y0 = std::floor((b.y0 + r() * (b.y1 - b.y0)) * 8);
      const double x1 = x0 + std::floor(r() * 20000);
      const double y1 = y0 + std::floor(r() * 20000);
      const Rect q{x0, y0, x1, y1};
      std::string ids;
      for (const Landmark* it : w.landmarks->near(q)) ids += js::cat(ids.empty() ? "" : ",", static_cast<double>(it - items.data()));
      out << (Line() << "near" << rect_str(q) << (ids.empty() ? std::string("-") : ids));
    }
    // the feature source round each landmark
    for (const Landmark& it : items) {
      const Box3& q = it.bb;
      const std::vector<std::array<double, 3>> pts = {
          {(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, (q.z0 + q.z1) / 2},
          {(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, q.z1},
          {(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, q.z1 + 300},
          {q.x0 - 3000, q.y0 - 3000, q.z0},
      };
      for (const ChunkAt& ca : chunks_at({0, 1, 2, 3, 4}, pts)) {
        ChunkBuffer ch(static_cast<int>(ca[0]), ca[1], ca[2], ca[3]);
        const Box3 bx = ch.world_box();
        double z0 = 0, z1 = 0;
        const bool has = landmark_z_range(w, Rect{bx.x0, bx.y0, bx.x1, bx.y1}, &z0, &z1);
        rasterize_landmarks(w, ch);
        Line l;
        l << "c" << ca[0] << ca[1] << ca[2] << ca[3] << (has ? js::cat(z0, ",", z1) : std::string("-"));
        chunk_digest(l, ch);
        out << l;
      }
    }
  }
  CHECK(unasked == 0);
  // ---- the source on worlds without landmarks
  {
    Value v;
    REQUIRE(Value::parse_json(R"({"seed":3})", &v));
    const std::shared_ptr<World> wp = create_world(v);
    ChunkBuffer ch(0, 0, 0, 0);
    const Box3 bx = ch.world_box();
    double z0 = 0, z1 = 0;
    const bool has = landmark_z_range(*wp, Rect{bx.x0, bx.y0, bx.x1, bx.y1}, &z0, &z1);
    out << (Line() << "none" << (wp->landmarks == nullptr) << (has ? js::cat(z0, ",", z1) : std::string("-")));
    rasterize_landmarks(*wp, ch);
    Line l;
    l << "nonec";
    chunk_digest(l, ch);
    out << l;
  }
  // ---- free() on scripted roads and plans
  {
    Value v;
    REQUIRE(Value::parse_json(R"({"seed":41})", &v));
    World w(v);
    struct Plot {
      bool lot;
      Rect r;
    };
    std::map<std::pair<double, double>, RoadList> roads;
    std::map<std::pair<double, double>, std::vector<Plot>> lots;
    w.cell_roads = [&roads](double i, double j) {
      auto it = roads.find({i, j});
      return it == roads.end() ? RoadList{} : it->second;
    };
    Landmarks lm(w);
    lm.plan_occupied = [&](double x, double y) {
      const CellIJ c = w.cell_at(x, y);
      auto it = lots.find({c.i, c.j});
      if (it == lots.end()) return false;
      for (const Plot& p : it->second)
        if (x >= p.r.x0 && x <= p.r.x1 && y >= p.r.y0 && y <= p.r.y1) return true;
      return false;
    };
    out << (Line() << "noisland" << static_cast<double>(lm.all().size()));
    for (int k = 0; k < 40; ++k) {
      const double i = 3 * (k % 8) - 12;
      const double j = 3 * std::floor(k / 8.0) - 6;
      const Rect rc = w.arterials->cell_rect(i, j);
      const double ox = rc.x0 + 1000;
      const double oy = rc.y0 + 1000;
      roads[{i, j}] = scripted_roads(r, k, ox, oy);
      std::vector<Plot> q;
      const double n = std::floor(r() * 8);
      std::string plan;
      for (double s = 0; s < n; s += 1) {
        const double x0 = ox + std::floor(r() * 1200);
        const double y0 = oy + std::floor(r() * 1200);
        const bool lot = r() < 0.5;
        const double x1 = x0 + std::floor(r() * 300);
        const double y1 = y0 + std::floor(r() * 300);
        q.push_back({lot, Rect{x0, y0, x1, y1}});
        plan += js::cat(plan.empty() ? "" : ";", lot ? "lot" : "space", rect_str(q.back().r));
      }
      lots[{i, j}] = q;
      out << (Line() << "plan" << i << j << (plan.empty() ? std::string("-") : plan));
      std::string bits;
      for (int s = 0; s < 400; ++s) {
        const double x = std::floor(ox - 100 + r() * 1400);
        const double y = std::floor(oy - 100 + r() * 1400);
        bits += lm.free(x, y) ? '1' : '0';
      }
      out << (Line() << "free" << bits);
    }
  }
  CHECK(rec::record("landmarks", out.text()) == rec::recorded_digest("landmarks"));
}
