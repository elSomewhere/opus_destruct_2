#include "svx/anim/system.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "svx/base/parallel.hpp"
#include "svx/phys/rigid.hpp"

namespace svx::anim {

namespace {

// (the articulations of characters, to their system: its host data's group)
constexpr u32 kCharacterGroup = 0x52414843;  // "CHAR"

f64 ms_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t).count(); }

u64 mix64(u64 h, u64 v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h;
}

u64 bits(f64 x) {
  u64 u;
  std::memcpy(&u, &x, sizeof u);
  return u;
}

}  // namespace

CharacterSystem::CharacterSystem(const CharacterSystemConfig& cfg) : config(cfg) {}

CharacterSystem::~CharacterSystem() = default;

void CharacterSystem::attach(World& w) {
  world_ = &w;
  collision_ = std::make_unique<WorldCollision>(w);
}

void CharacterSystem::on_load(World& /*w*/) {
  // (a new grid: the world's articulations went with the old one, and so do the characters)
  chars_.clear();
}

CharacterSystem::Entry* CharacterSystem::entry(CharacterId id) {
  const auto it = std::lower_bound(chars_.begin(), chars_.end(), id, [](const Entry& e, CharacterId v) { return e.id < v; });
  return it != chars_.end() && it->id == id ? &*it : nullptr;
}

const CharacterSystem::Entry* CharacterSystem::entry(CharacterId id) const { return const_cast<CharacterSystem*>(this)->entry(id); }

Character* CharacterSystem::get(CharacterId id) {
  Entry* e = entry(id);
  return e ? e->c.get() : nullptr;
}

const Character* CharacterSystem::get(CharacterId id) const {
  const Entry* e = entry(id);
  return e ? e->c.get() : nullptr;
}

u32 CharacterSystem::kind_of(CharacterId id) const {
  const Entry* e = entry(id);
  return e ? e->kind : 0;
}

std::vector<CharacterId> CharacterSystem::ids() const {
  std::vector<CharacterId> out;
  out.reserve(chars_.size());
  for (const Entry& e : chars_) out.push_back(e.id);
  return out;
}

CharacterId CharacterSystem::spawn(const CharacterDesc& d) {
  if (!world_ || !d.model || !world_->in_range(d.pos)) return 0;
  CharacterOptions o;
  o.model = d.model;
  o.palette = d.palette;
  o.collision = collision_.get();
  o.weapon = d.weapon;
  o.health = d.health;
  o.seed = d.seed;
  o.mass = d.mass;
  o.world = world_;
  o.backend = config.policy == BodyPolicy::Shallow ? BodyBackend::Shallow : BodyBackend::Deep;
  o.group = kCharacterGroup;
  const CharacterId id = next_++;
  o.tag = id;
  Entry e;
  e.id = id;
  e.kind = d.kind;
  e.c = std::make_unique<Character>(o);
  e.c->place(d.pos, d.yaw);
  chars_.push_back(std::move(e));
  return id;
}

bool CharacterSystem::despawn(CharacterId id) {
  const auto it = std::lower_bound(chars_.begin(), chars_.end(), id, [](const Entry& e, CharacterId v) { return e.id < v; });
  if (it == chars_.end() || it->id != id) return false;
  it->c->unbound();  // (its articulation goes with it)
  chars_.erase(it);
  return true;
}

void CharacterSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  if (chars_.empty() || chunks.empty()) return;
  // (a character whose ground went out of range goes with it: nothing holds it up there)
  const f64 h = w.voxel_size();
  std::vector<CharacterId> gone;
  for (const Entry& e : chars_) {
    const V3 p = e.c->pose.p[H::pelvis];
    const IVec3 cc{static_cast<i32>(std::floor(p.x / h + 0.5)) >> kChunkBits, static_cast<i32>(std::floor(p.y / h + 0.5)) >> kChunkBits,
                   static_cast<i32>(std::floor(p.z / h + 0.5)) >> kChunkBits};
    for (i32 dz = -1; dz <= 0; ++dz)
      if (std::binary_search(chunks.begin(), chunks.end(), key3(cc[0], cc[1], cc[2] + dz))) {
        gone.push_back(e.id);
        break;
      }
  }
  for (CharacterId id : gone) despawn(id);
}

void CharacterSystem::pre_step(World& /*w*/, f64 /*dt*/) {
  const auto t0 = std::chrono::steady_clock::now();
  for (Entry& e : chars_) e.c->push();
  stats_.pre_ms = ms_since(t0);
}

void CharacterSystem::level_of_detail(World& w) {
  // the awake pieces (not the characters' own links)
  struct Near {
    V3 lo, hi;
  };
  std::vector<Near> awake;
  if (config.piece_radius > 0.0)
    for (const auto& bp : w.rigid().bodies) {
      const Body& b = *bp;
      if (b.link || b.asleep) continue;
      awake.push_back(Near{b.box_lo, b.box_hi});
    }
  const f64 hy = config.hysteresis;
  struct Want {
    f64 d = 0.0;
    size_t i = 0;
  };
  std::vector<Want> deep_candidates;
  for (size_t i = 0; i < chars_.size(); ++i) {
    Character& c = *chars_[i].c;
    const V3 p = c.pose.p[H::pelvis];
    f64 d = 1e300;
    for (const V3& f : focus) d = std::min(d, vdist(p, f));
    if (focus.empty()) d = 0.0;
    const bool deep_now = c.backend() == BodyBackend::Deep;
    // physics while calm: within the physics radius
    const bool phys_now = c.physics;
    c.physics = d < config.physics_radius + (phys_now ? hy : -hy);
    if (config.policy == BodyPolicy::Deep) {
      c.set_backend(BodyBackend::Deep, &w);
      continue;
    }
    if (config.policy == BodyPolicy::Shallow) {
      c.set_backend(BodyBackend::Shallow, &w);
      continue;
    }
    // hybrid: near a focus point, or near an awake piece: deep
    bool near_piece = false;
    const f64 r = config.piece_radius + (deep_now ? hy : 0.0);
    for (const Near& n : awake)
      if (p.x > n.lo.x - r && p.x < n.hi.x + r && p.y > n.lo.y - r && p.y < n.hi.y + r && p.z > n.lo.z - r - 1.0 && p.z < n.hi.z + r + 1.0) {
        near_piece = true;
        break;
      }
    if (near_piece) {
      c.physics = true;  // (what can touch the world's bodies is one of them)
      c.set_backend(BodyBackend::Deep, &w);
      continue;
    }
    if (d < config.deep_radius + (deep_now ? hy : -hy)) deep_candidates.push_back(Want{d, i});
    else c.set_backend(BodyBackend::Shallow, &w);
  }
  // (the nearest ones, as many as allowed)
  std::sort(deep_candidates.begin(), deep_candidates.end(), [](const Want& a, const Want& b) { return a.d < b.d || (a.d == b.d && a.i < b.i); });
  for (size_t k = 0; k < deep_candidates.size(); ++k)
    chars_[deep_candidates[k].i].c->set_backend(static_cast<i32>(k) < config.max_deep ? BodyBackend::Deep : BodyBackend::Shallow, &w);
}

void CharacterSystem::step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  if (chars_.empty()) {
    stats_ = CharacterStats{};
    return;
  }
  level_of_detail(w);
  // the shallow bodies meet the others through obstacles (the deep ones collide in the core)
  std::vector<Character*> all;
  all.reserve(chars_.size());
  for (Entry& e : chars_) all.push_back(e.c.get());
  gather_obstacles(all);
  for (Entry& e : chars_) e.c->update(dt);
  // stats
  CharacterStats s;
  s.pre_ms = stats_.pre_ms;
  for (const Entry& e : chars_) {
    const Character& c = *e.c;
    ++s.characters;
    if (c.asleep()) ++s.asleep;
    else if (!c.behaviours.physical) ++s.plan_only;
    else if (c.bound()) ++s.deep;
    else ++s.shallow;
  }
  s.step_ms = ms_since(t0);
  stats_ = s;
}

i64 CharacterSystem::memory_bytes() const {
  i64 n = static_cast<i64>(sizeof(*this)) + static_cast<i64>(chars_.capacity() * sizeof(Entry));
  for (const Entry& e : chars_) n += e.c->memory_bytes();
  return n;
}

u64 CharacterSystem::state_hash() const {
  u64 h = 0x43484152ull;
  for (const Entry& e : chars_) {
    h = mix64(h, e.id);
    const Character& c = *e.c;
    h = mix64(h, static_cast<u64>(c.behaviours.mode));
    for (i32 b : {H::pelvis, H::head, H::handL, H::handR, H::footL, H::footR}) {
      const V3& p = c.pose.p[size_t(b)];
      h = mix64(h, bits(p.x));
      h = mix64(h, bits(p.y));
      h = mix64(h, bits(p.z));
    }
    h = mix64(h, bits(c.health));
  }
  return h;
}

std::optional<CharacterSystem::Hit> CharacterSystem::raycast(const V3& origin, const V3& dir, f64 max_dist) const {
  std::optional<Hit> best;
  f64 bd = max_dist;
  for (const Entry& e : chars_) {
    const std::optional<CharacterHit> h = e.c->raycast(origin, dir, bd);
    if (h && h->distance < bd) {
      bd = h->distance;
      best = Hit{e.id, *h};
    }
  }
  return best;
}

}  // namespace svx::anim
