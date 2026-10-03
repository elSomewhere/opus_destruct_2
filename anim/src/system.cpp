#include "svx/anim/system.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "svx/base/parallel.hpp"

namespace svx::anim {

namespace {

// (the articulations of characters, to their system: its host data's group)
constexpr u32 kCharacterGroup = 0x52414843;  // "CHAR"
// (the world's host records of the loose props out of range: World::archive_host_record)
constexpr u32 kPropRecords = 0x504f5250;  // "PROP"
// (a corpse's record is written when its damage changes, and at least this often: its wounds age)
constexpr i64 kRecordRefreshTicks = 600;

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
  props = std::make_shared<PropRegistry>(collision_.get());
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
  o.profile = d.profile;
  o.model = d.model;
  o.palette = d.palette;
  o.collision = collision_.get();
  o.weapon = d.weapon;
  o.loadout = d.loadout;
  o.wield = d.wield;
  o.prop_registry = props;
  o.health = d.health;
  o.seed = d.seed;
  o.mass = d.mass;
  o.girth = d.girth;
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

// An articulation's host data, the system's record: "SVXC", version, kind, alive, health, the
// host's data (version 1: the rest), then (version 2: its length first) the damage the character
// took (Character::damage_record: a corpse comes back as it was).
constexpr u32 kRecordMagic = 0x43585653u;  // "SVXC"
constexpr size_t kRecordHead = 4 + 1 + 4 + 1 + 8;

std::vector<u8> encode_record(u32 kind, bool alive, f64 health, const std::vector<u8>& data, const std::vector<u8>& damage) {
  std::vector<u8> out(kRecordHead + 4);
  std::memcpy(out.data(), &kRecordMagic, 4);
  out[4] = 2;
  std::memcpy(out.data() + 5, &kind, 4);
  out[9] = alive ? 1 : 0;
  std::memcpy(out.data() + 10, &health, 8);
  const u32 n = static_cast<u32>(data.size());
  std::memcpy(out.data() + kRecordHead, &n, 4);
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), damage.begin(), damage.end());
  return out;
}

bool decode_record(const std::vector<u8>* in, u32* kind, bool* alive, f64* health, std::vector<u8>* data, std::vector<u8>* damage) {
  if (!in || in->size() < kRecordHead) return false;
  u32 magic;
  std::memcpy(&magic, in->data(), 4);
  const u8 version = (*in)[4];
  if (magic != kRecordMagic || (version != 1 && version != 2)) return false;
  std::memcpy(kind, in->data() + 5, 4);
  *alive = (*in)[9] != 0;
  std::memcpy(health, in->data() + 10, 8);
  damage->clear();
  if (version == 1) {
    data->assign(in->begin() + kRecordHead, in->end());
    return true;
  }
  if (in->size() < kRecordHead + 4) return false;
  u32 n;
  std::memcpy(&n, in->data() + kRecordHead, 4);
  if (n > in->size() - kRecordHead - 4) return false;
  const auto at = in->begin() + static_cast<std::ptrdiff_t>(kRecordHead + 4);
  data->assign(at, at + n);
  damage->assign(at + n, in->end());
  return true;
}

}  // namespace

void CharacterSystem::record(World& w) {
  for (Entry& e : chars_) {
    const Character& c = *e.c;
    if (!c.bound()) continue;
    const ArticulationId id = c.articulation();
    // (the dead with their wounds: a corpse shot again is recorded again - when its damage, its
    // geometry or what it holds changed, and now and then for the age of its wounds)
    const bool same = id == e.recorded && c.alive() == e.recorded_alive;
    if (same && c.alive()) continue;
    if (same && e.recorded_geometry == c.geometry_version && e.recorded_damage == c.behaviours.damage.revision() &&
        e.recorded_props == c.attachments().revision && w.ticks() - e.recorded_tick < kRecordRefreshTicks)
      continue;
    w.set_articulation_data(id, encode_record(e.kind, c.alive(), c.health, e.data, c.alive() ? std::vector<u8>{} : c.damage_record()));
    e.recorded = id;
    e.recorded_alive = c.alive();
    e.recorded_geometry = c.geometry_version;
    e.recorded_damage = c.behaviours.damage.revision();
    e.recorded_props = c.attachments().revision;
    e.recorded_tick = w.ticks();
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
    std::vector<u8> data, damage;
    if (!decode_record(w.articulation_data(id), &kind, &alive, &health, &data, &damage)) {
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
    o.profile = d.profile;
    o.model = d.model;
    o.palette = d.palette;
    o.collision = collision_.get();
    o.girth = d.girth;
    o.weapon = d.weapon;
    o.loadout = d.loadout;
    o.wield = d.wield;
    o.prop_registry = props;
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
    if (!e.c->restore_damage(damage)) {
      // (a record that does not fit what the host made: the body stays as it lies, not whole)
      e.c->release_articulation();
      strangers.push_back(id);
      ++restore_failures_;
      continue;
    }
    e.recorded = id;
    e.recorded_alive = false;
    e.recorded_geometry = e.c->geometry_version;
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
  if (chunks.empty()) return;
  const f64 h = w.voxel_size();
  auto chunk_at = [&](const V3& p) {
    return IVec3{static_cast<i32>(std::floor(p.x / h + 0.5)) >> kChunkBits, static_cast<i32>(std::floor(p.y / h + 0.5)) >> kChunkBits,
                 static_cast<i32>(std::floor(p.z / h + 0.5)) >> kChunkBits};
  };
  // (a loose prop whose place went out of range goes into the world's archive with its region -
  // and comes back with it, as the world's own pieces do)
  std::vector<PropInstancePtr> leaving;
  for (const auto& [pid, p] : props->all()) {
    if (p->location != PropLocation::Loose) continue;
    const IVec3 cc = chunk_at(p->pos);
    for (i32 dz = -1; dz <= 0; ++dz)
      if (std::binary_search(chunks.begin(), chunks.end(), key3(cc[0], cc[1], cc[2] + dz))) {
        leaving.push_back(p);
        break;
      }
  }
  for (const auto& p : leaving) {
    w.archive_host_record(kPropRecords, p->id, p->pos, props->record_item(*p));
    props->retire(p);
  }
  if (chars_.empty()) return;
  // (a character whose ground went out of range goes with it: nothing holds it up there)
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
  for (const HostRecord& r : w.take_host_records(kPropRecords)) props->restore_item(r.data);
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
    for (const PieceBox& b : w.awake_pieces()) awake.push_back(Near{b.lo, b.hi});
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
  props->collision(collision_.get());
  props->update(w.config().dt);
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
  s.restore_failures = restore_failures_;
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
  i64 n = static_cast<i64>(sizeof(*this)) + static_cast<i64>(chars_.capacity() * sizeof(Entry)) + static_cast<i64>(strangers_.capacity() * sizeof(ArticulationId));
  for (const Entry& e : chars_) n += e.c->memory_bytes() + static_cast<i64>(e.data.capacity());
  if (props) n += props->memory_bytes();
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
    h = mix64(h, c.damage_hash());
  }
  for (const auto& [id, p] : props->all()) {
    h = mix64(h, id);
    h = mix64(h, u64(p->location));
    h = mix64(h, p->character);
    h = mix64(h, u64(p->point));
    h = mix64(h, u64(p->style));
    for (char ch : p->archetype->id) h = mix64(h, u8(ch));
    for (f64 v : {p->pos.x, p->pos.y, p->pos.z, p->rotation.x, p->rotation.y, p->rotation.z, p->rotation.w, p->velocity.x, p->velocity.y, p->velocity.z,
                  p->state.condition, p->state.strap})
      h = mix64(h, bits(v));
    h = mix64(h, p->geometry_version);
    if (p->damaged_model)
      for (const auto& part : p->model().parts)
        for (u8 cell : part.cells) h = mix64(h, cell);
  }
  return h;
}

std::optional<CharacterSystem::Hit> CharacterSystem::raycast(const V3& origin, const V3& dir, f64 max_dist) const {
  std::optional<Hit> best;
  f64 bd = max_dist;
  for (const Entry& e : chars_) {
    for (const auto& prop : e.c->attachments().slots)
      if (prop) {
        std::array<f32, 16> matrix{};
        write_rigid(matrix.data(), prop->pos, prop->rotation, {});
        if (auto hit = raycast_model(prop->model(), matrix, origin, dir, bd)) {
          hit->bone = -1;
          bd = hit->distance;
          best = Hit{e.id, *hit};
        }
      }
    const std::optional<CharacterHit> h = e.c->raycast(origin, dir, bd);
    if (h && h->distance < bd) {
      bd = h->distance;
      best = Hit{e.id, *h};
    }
  }
  return best;
}

}  // namespace svx::anim
