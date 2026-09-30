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

void CharacterSystem::rebind(World& w) {
  if (&w == world_) return;
  world_ = &w;
  if (collision_) collision_->rebind(w);
  for (Entry& e : chars_) e.c->rebind_world(&w);
}

void CharacterSystem::on_load(World& w) {
  rebind(w);
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
  e.data = d.data;
  e.c = std::make_unique<Character>(o);
  e.c->place(d.pos, d.yaw);
  chars_.push_back(std::move(e));
  return id;
}

// ---- the dead the world keeps ---------------------------------------------------------------------

namespace {

// An articulation's host data, the system's record: "SVXC", version, kind, alive, health, then
// the host's data.
constexpr u32 kRecordMagic = 0x43585653u;  // "SVXC"

std::vector<u8> encode_record(u32 kind, bool alive, f64 health, const std::vector<u8>& data) {
  std::vector<u8> out(4 + 1 + 4 + 1 + 8);
  std::memcpy(out.data(), &kRecordMagic, 4);
  out[4] = 1;
  std::memcpy(out.data() + 5, &kind, 4);
  out[9] = alive ? 1 : 0;
  std::memcpy(out.data() + 10, &health, 8);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

bool decode_record(const std::vector<u8>* in, u32* kind, bool* alive, f64* health, std::vector<u8>* data) {
  if (!in || in->size() < 18) return false;
  u32 magic;
  std::memcpy(&magic, in->data(), 4);
  if (magic != kRecordMagic || (*in)[4] != 1) return false;
  std::memcpy(kind, in->data() + 5, 4);
  *alive = (*in)[9] != 0;
  std::memcpy(health, in->data() + 10, 8);
  data->assign(in->begin() + 18, in->end());
  return true;
}

}  // namespace

void CharacterSystem::record(World& w) {
  for (Entry& e : chars_) {
    const Character& c = *e.c;
    if (!c.bound()) continue;
    const ArticulationId id = c.articulation();
    if (id == e.recorded && c.alive() == e.recorded_alive) continue;
    w.set_articulation_data(id, encode_record(e.kind, c.alive(), c.health, e.data));
    e.recorded = id;
    e.recorded_alive = c.alive();
  }
}

// The world's articulations not bound to a character: a dead one of the system's (its record says)
// is a character again (the host's restore), its body adopted where it lies; a living one's body
// goes (the host's population makes the living); the rest are strangers.
void CharacterSystem::take_back(World& w) {
  std::vector<ArticulationId> ours;
  for (const Entry& e : chars_)
    if (e.c->bound()) ours.push_back(e.c->articulation());
  std::sort(ours.begin(), ours.end());
  std::vector<ArticulationId> strangers;
  for (ArticulationId id : w.articulations()) {
    if (std::binary_search(ours.begin(), ours.end(), id)) continue;
    if (std::binary_search(strangers_.begin(), strangers_.end(), id)) {
      strangers.push_back(id);
      continue;
    }
    u32 kind = 0;
    bool alive = false;
    f64 health = 0.0;
    std::vector<u8> data;
    if (!decode_record(w.articulation_data(id), &kind, &alive, &health, &data)) {
      strangers.push_back(id);
      continue;
    }
    if (alive) {
      w.remove_articulation(id);
      continue;
    }
    CharacterDesc d;
    d.kind = kind;
    d.data = data;
    if (!restore || !restore(kind, data, &d) || !d.model) {
      strangers.push_back(id);
      continue;
    }
    CharacterOptions o;
    o.model = d.model;
    o.palette = d.palette;
    o.collision = collision_.get();
    o.weapon = d.weapon;
    o.health = d.health;
    o.seed = d.seed;
    o.mass = d.mass;
    o.world = &w;
    o.backend = BodyBackend::Deep;
    o.group = kCharacterGroup;
    Entry e;
    e.id = next_++;
    o.tag = e.id;
    e.kind = kind;
    e.data = data;
    e.c = std::make_unique<Character>(o);
    if (!e.c->adopt(id)) {
      strangers.push_back(id);
      continue;
    }
    e.c->die(nullptr, nullptr, 0.0);
    e.c->health = 0.0;
    e.recorded = id;
    e.recorded_alive = false;
    chars_.push_back(std::move(e));
  }
  strangers_ = std::move(strangers);
}

bool CharacterSystem::despawn(CharacterId id) {
  const auto it = std::lower_bound(chars_.begin(), chars_.end(), id, [](const Entry& e, CharacterId v) { return e.id < v; });
  if (it == chars_.end() || it->id != id) return false;
  it->c->unbound();  // (its articulation goes with it)
  chars_.erase(it);
  return true;
}

void CharacterSystem::on_evicted(World& w, const std::vector<u64>& chunks) {
  rebind(w);
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

// A frame's first half, before the mechanics: which bodies are deep (level of detail), what the
// shallow ones meet, the plans and the drives (pushed to the deep bodies' articulations).
void CharacterSystem::pre_step(World& w, f64 dt) {
  const auto t0 = std::chrono::steady_clock::now();
  rebind(w);
  if (w.ticks() - scan_tick_ >= 30) {
    scan_tick_ = w.ticks();
    take_back(w);
  }
  if (chars_.empty()) {
    stats_.pre_ms = 0.0;
    return;
  }
  level_of_detail(w);
  // the shallow bodies meet the others through obstacles (the deep ones collide in the core)
  std::vector<Character*> all;
  all.reserve(chars_.size());
  for (Entry& e : chars_) all.push_back(e.c.get());
  gather_obstacles(all);
  // the characters' frames begin: their starts one at a time (bodies woken, put to rest: the
  // world's articulations change), their bodies side by side (each reads the world and steps its
  // own), their drives to the world one at a time
  std::vector<Character*> go;
  go.reserve(all.size());
  for (Character* c : all)
    if (c->begin_start(dt)) go.push_back(c);
  // (the world's oriented grids cache their solids as they are asked: then one at a time)
  if (config.parallel && go.size() > 1 && !w.has_oriented_grids()) {
    parallel_for(static_cast<i64>(go.size()), 1, [&](i64 i0, i64 i1) {
      for (i64 i = i0; i < i1; ++i) go[size_t(i)]->begin_body();
    });
  } else {
    for (Character* c : go) c->begin_body();
  }
  for (Character* c : go) c->begin_push();
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
    // (the dead: bodies of the world - asleep at rest they cost nothing, and the world keeps them)
    if (!c.alive()) {
      c.set_backend(BodyBackend::Deep, &w);
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

void CharacterSystem::step(World& w, f64 /*dt*/) {
  const auto t0 = std::chrono::steady_clock::now();
  rebind(w);
  if (chars_.empty()) {
    stats_ = CharacterStats{};
    return;
  }
  // the second half: what the tick made of the deep bodies (a dead one whose body the world took
  // - archived with its region - goes: the world keeps it, and gives it back)
  std::vector<CharacterId> taken;
  for (Entry& e : chars_) {
    const bool was = e.c->bound();
    e.c->end();
    if (was && !e.c->bound() && !e.c->alive()) taken.push_back(e.id);
  }
  for (CharacterId id : taken) {
    const auto it = std::lower_bound(chars_.begin(), chars_.end(), id, [](const Entry& e, CharacterId v) { return e.id < v; });
    if (it != chars_.end() && it->id == id) chars_.erase(it);
  }
  record(w);
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
