// The C API as the web worker drives it (web/src/worker/wasm-worker.ts): the drive city's people
// polled after every tick - their meshes, palettes, the characters, skins, props and blood - while
// the viewer goes among them, shoots one (a shot's ray, then the round into what it found a few
// ticks later, as the messages go), blasts the body, goes far away and comes back. Every buffer
// the API hands out is read to its end (a sanitizer build checks each read) and every count is
// checked against what it describes.
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "doctest.h"
#include "svx/game/api/svx_api.h"
#include "svx/anim/rig.hpp"

namespace {

constexpr int kCharStride = 12;
constexpr int kSkinFloats = 23 * 16;

struct Person {
  unsigned id = 0, mesh = 0, palette = 0, flags = 0;
  double head[3]{};
  double c[3]{}, radius = 0.0, flash = 0.0, health = 0.0;
};

// What the worker does after a tick: everything polled, every buffer copied out.
struct Worker {
  svx_engine* e = nullptr;
  std::set<unsigned> meshes, palettes;
  std::vector<Person> people;
  double checksum = 0.0;  // (the reads are used)
  int blood = 0, stains = 0;

  void flush() {
    const int nm = svx_poll_character_meshes(e);
    const int nr = svx_poll_character_meshes_removed(e);
    const int np = svx_poll_character_palettes(e);
    REQUIRE(nm >= 0);
    REQUIRE(nr >= 0);
    REQUIRE(np >= 0);
    for (int i = 0; i < nm; ++i) {
      double info[3];
      svx_character_mesh_info(e, i, info);
      const unsigned id = static_cast<unsigned>(info[0]);
      const size_t vc = static_cast<size_t>(info[1]), ic = static_cast<size_t>(info[2]);
      REQUIRE(id != 0);
      REQUIRE(ic % 3 == 0);
      std::vector<unsigned char> v(vc * 20);
      std::vector<unsigned> idx(ic);
      if (vc > 0) std::memcpy(v.data(), svx_character_mesh_vertices(e, i), v.size());
      if (ic > 0) std::memcpy(idx.data(), svx_character_mesh_indices(e, i), ic * 4);
      for (unsigned k : idx) REQUIRE(k < vc);
      for (size_t k = 0; k < vc; ++k) {
        const unsigned bone = v[k * 20 + 16];
        REQUIRE(bone < 23);
      }
      meshes.insert(id);
    }
    for (int i = 0; i < nr; ++i) meshes.erase(svx_character_mesh_removed(e, i));
    for (int i = 0; i < np; ++i) {
      float rgb[48];
      const unsigned id = svx_character_palette(e, i, rgb);
      for (float f : rgb) checksum += f;
      palettes.insert(id);
    }
    const int n = svx_characters(e);
    REQUIRE(n >= 0);
    people.assign(static_cast<size_t>(n), Person{});
    if (n > 0) {
      std::vector<double> d(static_cast<size_t>(n) * kCharStride);
      std::vector<float> skin(static_cast<size_t>(n) * kSkinFloats), prop(static_cast<size_t>(n) * 16);
      std::memcpy(d.data(), svx_characters_data(e), d.size() * sizeof(double));
      std::memcpy(skin.data(), svx_characters_skin(e), skin.size() * sizeof(float));
      std::memcpy(prop.data(), svx_characters_prop(e), prop.size() * sizeof(float));
      for (float f : skin) REQUIRE(std::isfinite(f));
      for (int k = 0; k < n; ++k) {
        const double* r = &d[static_cast<size_t>(k) * kCharStride];
        Person& p = people[static_cast<size_t>(k)];
        p.id = static_cast<unsigned>(r[0]);
        p.mesh = static_cast<unsigned>(r[1]);
        p.palette = static_cast<unsigned>(r[2]);
        p.flags = static_cast<unsigned>(r[3]);
        for (int a = 0; a < 3; ++a) p.c[a] = r[4 + a];
        const auto brain = svx::anim::humanoid_skeleton()->rest_head[svx::anim::H::head] + svx::V3{0, 0, .1};
        const float* head = &skin[(static_cast<size_t>(k) * 23 + svx::anim::H::head) * 16];
        for (int a = 0; a < 3; ++a) p.head[a] = head[a] * brain.x + head[4 + a] * brain.y + head[8 + a] * brain.z + head[12 + a];
        p.radius = r[7];
        p.flash = r[8];
        p.health = r[10];
        // (what it is drawn with came before it)
        REQUIRE(meshes.count(p.mesh));
        REQUIRE(palettes.count(p.palette));
        if (r[9] > 0) REQUIRE(meshes.count(static_cast<unsigned>(r[9])));
      }
    }
    blood = svx_blood(e);
    stains = svx_blood_stain_count(e);
    REQUIRE(blood >= 0);
    REQUIRE(stains >= 0);
    std::vector<float> drops(static_cast<size_t>(blood) * 7), st(static_cast<size_t>(stains) * 8);
    if (blood > 0) std::memcpy(drops.data(), svx_blood_drops(e), drops.size() * sizeof(float));
    if (stains > 0) std::memcpy(st.data(), svx_blood_stains(e), st.size() * sizeof(float));
    for (float f : drops) checksum += f;
    for (float f : st) checksum += f;
  }

  const Person* find(unsigned id) const {
    for (const Person& p : people)
      if (p.id == id) return &p;
    return nullptr;
  }
};

}  // namespace

TEST_CASE("C API: the people as the web worker polls them - shot, blasted, left and found again") {
  svx_set_threads(4);
  svx_engine* e = svx_create(0.125);
  REQUIRE(e);
  svx_set_pedestrians(e, 1, 24, 30.0, 70.0, 2, 24);
  svx_set_traffic(e, 1, 10, 6, 40.0, 110.0, 1.0);
  REQUIRE(svx_load_procedural(e, "drive", 7) == 0);
  svx_bake(e);
  double info[13];
  svx_world_info(e, info);
  double eye[3] = {info[7], info[8], info[9] + 1.6};
  Worker wk;
  wk.e = e;
  auto tick = [&](int n) {
    for (int t = 0; t < n; ++t) {
      svx_viewer(e, eye[0], eye[1], eye[2]);
      svx_tick(e);
      wk.flush();
    }
  };
  tick(60 * 8);
  REQUIRE(wk.people.size() >= 8);
  // the nearest one alive; the viewer 5 m from it
  const Person* target = nullptr;
  double best = 1e9;
  for (const Person& p : wk.people) {
    const double d = std::hypot(p.c[0] - eye[0], p.c[1] - eye[1]);
    if ((p.flags & 1) && d < best) {
      best = d;
      target = &p;
    }
  }
  REQUIRE(target);
  const unsigned id = target->id;
  eye[0] = target->c[0] - 5.0;
  eye[1] = target->c[1];
  eye[2] = target->c[2] + 0.8;
  tick(30);
  // rounds into it: a shot's ray, and the round a few ticks later (the main thread's answer)
  bool dead = false;
  int rounds = 0;
  for (int k = 0; k < 24 && !dead; ++k) {
    const Person* p = wk.find(id);
    if (!p) break;
    double d[3] = {p->c[0] - eye[0], p->c[1] - eye[1], p->c[2] + 0.35 - eye[2]};
    const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (double& v : d) v /= l;
    double out[10];
    const int hit = svx_raycast_shot(e, eye[0], eye[1], eye[2], d[0], d[1], d[2], 100.0, out);
    tick(3);
    if (hit == 2) {
      // (the body walked on since the ray: the round still finds it)
      CHECK(svx_wound_character(e, static_cast<unsigned>(out[8]), out[0], out[1], out[2], 0.15, 500.0) == 1);
      ++rounds;
    } else if (hit == 1) {
      svx_shoot(e, out[0], out[1], out[2], 0.15, 500.0);
    }
    tick(6);
    const Person* q = wk.find(id);
    dead = q && !(q->flags & 1);
    // (the viewer keeps up with it)
    if (q) {
      eye[0] = q->c[0] - 4.0;
      eye[1] = q->c[1];
    }
  }
  MESSAGE("rounds " << rounds << ", dead " << dead);
  CHECK(rounds > 0);
  // Repeated channels can pass through an existing wound. Death is anatomical,
  // so exercise a fresh brain channel through the descriptor API explicitly.
  if (!dead)
    if (const Person* p = wk.find(id)) {
      double shot[25] = {0, p->head[0], p->head[1] + .3, p->head[2], 0, -1, 0, .004, 900, .00556, .003, 1, 1, .2, 3, 100000, 0, svx::anim::H::head, 0};
      REQUIRE(svx_damage_character(e, id, shot, 25) == 1);
      tick(2);
      const auto* q = wk.find(id);
      dead = q && !(q->flags & 1);
    }
  CHECK(dead);
  // rounds into the body where it lies
  tick(60 * 3);
  for (int k = 0; k < 6; ++k) {
    const Person* p = wk.find(id);
    if (!p) break;
    double out[10];
    if (svx_raycast_shot(e, p->c[0], p->c[1], p->c[2] + 2.0, 0.0, 0.0, -1.0, 5.0, out) == 2) {
      tick(2);
      svx_wound_character(e, static_cast<unsigned>(out[8]), out[0], out[1], out[2], 0.15, 500.0);
    }
    tick(10);
  }
  tick(60 * 5);
  // a rocket into it
  if (const Person* p = wk.find(id)) {
    svx_blast(e, p->c[0], p->c[1], p->c[2] - 0.3, 1.0, 1.0e6);
    tick(60 * 6);
  }
  int gibs = 0;
  for (const Person& p : wk.people) gibs += (p.flags & 32) ? 1 : 0;
  MESSAGE("after the blast: " << wk.people.size() << " drawn, " << gibs << " gibs, " << wk.blood << " drops, " << wk.stains << " stains");
  // a round at every one near (async answers: some are gone by then)
  std::vector<std::pair<unsigned, std::array<double, 3>>> aims;
  for (const Person& p : wk.people)
    if (std::hypot(p.c[0] - eye[0], p.c[1] - eye[1]) < 40.0) aims.push_back({p.id, {p.c[0], p.c[1], p.c[2]}});
  for (const auto& [who, at] : aims) {
    svx_wound_character(e, who, at[0], at[1], at[2] + 0.3, 0.15, 500.0);
    tick(1);
  }
  tick(60 * 4);
  // far away and back: the bodies go with their regions and come back
  const double home[3] = {eye[0], eye[1], eye[2]};
  eye[0] += 600.0;
  tick(60 * 10);
  eye[0] = home[0];
  tick(60 * 10);
  MESSAGE("back: " << wk.people.size() << " characters, " << wk.meshes.size() << " meshes, " << wk.palettes.size() << " palettes");
  CHECK(!wk.people.empty());
  svx_destroy(e);
}
