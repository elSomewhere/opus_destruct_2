#include "svx/anim/character.hpp"
#include "svx/anim/damage/record.hpp"
namespace svx::anim {
namespace {
// v1, v2: cells and shades; v3: stains and tissues too, and the share of a prop's material left
constexpr u64 kRecordVersion = 3;
void write_model(record::Writer& w, const VoxelModel& m) {
  w.integer(m.parts.size());
  for (const auto& p : m.parts) {
    w.integer(p.bone);
    record::write_cells(w, p);
  }
}
bool read_model(record::Reader& r, VoxelModel& m, u64 version) {
  if (r.integer() != m.parts.size()) return false;
  for (auto& p : m.parts) {
    if (r.integer() != u64(p.bone)) return false;
    if (!record::read_cells(r, p, p.cells.size(), version >= 3)) return false;
    ++p.version;
  }
  return r.ok;
}
}  // namespace
std::vector<u8> Character::damage_record() const {
  if (alive() && !owns_model && behaviours.damage.wound_count() == 0) return {};
  record::Writer w;
  w.integer(0x44585653);
  w.integer(kRecordVersion);
  u16 lost = 0;
  for (int i = 0; i < 16; ++i)
    if (behaviours.lost[size_t(i)]) lost |= u16(1 << i);
  w.integer(lost, 2);
  write_model(w, *model);
  w.block(behaviours.damage.record());
  w.integer(effects.stains.size());
  for (const auto& s : effects.stains) {
    w.vector(s.pos);
    w.vector(s.normal);
    w.number(s.size);
    w.number(s.age);
  }
  size_t count = 0;
  for (const auto& p : motion.props.slots)
    if (p) ++count;
  w.integer(count);
  for (const auto& p : motion.props.slots)
    if (p) {
      w.integer(p->id, 8);
      w.string(p->archetype->id);
      w.integer(u8(p->point), 1);
      w.integer(u8(p->style), 1);
      w.string(p->socket);
      w.number(p->state.condition);
      w.number(p->state.strap);
      w.integer(p->state.charges);
      w.integer(p->state.contents.size());
      for (const auto& item : p->state.contents) w.string(item);
      w.vector(p->pos);
      w.quat(p->rotation);
      write_model(w, p->model());
      w.number(p->mass_fraction);
    }
  return w.bytes;
}
u64 Character::damage_hash() const {
  u64 h = 0xcbf29ce484222325ull;
  auto mix = [&h](u64 v) {
    h ^= v;
    h *= 0x100000001b3ull;
  };
  auto real = [&](f64 x) { mix(std::bit_cast<u64>(x)); };
  if (cells_hash_version_ != geometry_version) {
    u64 c = 0xcbf29ce484222325ull;
    for (const auto& p : model->parts)
      for (const auto* v : {&p.cells, &p.shade, &p.stain, &p.tissue}) {
        for (u8 b : *v) c = (c ^ b) * 0x100000001b3ull;
        c = (c ^ v->size()) * 0x100000001b3ull;
      }
    cells_hash_ = c;
    cells_hash_version_ = geometry_version;
  }
  mix(cells_hash_);
  for (int i = 0; i < 16; ++i) mix(behaviours.lost[size_t(i)] ? 1 : 0);
  const PhysiologySnapshot s = behaviours.damage.inspect();
  for (const auto& p : s.parts) {
    for (f64 x : {p.flesh, p.muscle, p.vessel, p.bleeding, p.pain, p.nerve}) real(x);
    mix(u64(p.bone) | (p.lost ? 8u : 0u));
  }
  for (f64 x : {s.blood, s.shock, s.consciousness, s.breathing, s.adrenaline}) real(x);
  mix(u64(s.cause));
  for (const auto& w : s.wounds) {
    mix(u64(w.part) | u64(w.bone) << 8 | u64(w.arterial) << 16);
    for (f64 x : {w.rest.x, w.rest.y, w.rest.z, w.normal.x, w.normal.y, w.normal.z, w.bleeding, w.age, w.pain}) real(x);
  }
  for (const auto& st : effects.stains)
    for (f64 x : {st.pos.x, st.pos.y, st.pos.z, st.size, st.age}) real(x);
  for (const auto& p : motion.props.slots)
    if (p) {
      mix(p->id);
      mix(p->geometry_version);
      for (f64 x : {p->state.condition, p->state.strap, p->mass_fraction}) real(x);
    }
  return h;
}
bool Character::restore_damage(std::span<const u8> data) {
  if (data.empty()) return true;
  if (data.size() < 2) return false;
  record::Reader r{data};
  const bool current = data.size() >= 8 && r.integer() == 0x44585653;
  if (!current) {  // Version 2 character records stored a lost mask and removal-only geometry.
    const u16 lost = u16(data[0]) | u16(u16(data[1]) << 8);
    auto m = model->clone();
    if (data.size() > 2 && !apply_damage(*m, data.subspan(2))) return false;
    model = std::move(m);
    owns_model = true;
    ++geometry_version;
    for (int i = 1; i < 16; ++i)
      if (lost & (1 << i)) behaviours.lose_limb(i);
    return true;
  }
  const auto version = r.integer();
  if (version < 1 || version > kRecordVersion) return false;
  const auto lost = r.integer(2);
  auto m = model->clone();
  if (!read_model(r, *m, version)) return false;
  DamageState state;
  state.configure(profile);
  if (!state.restore(r.block())) return false;
  std::deque<BloodStain> stains;
  const auto n = r.integer();
  if (n > size_t(std::max(600, effects.max_stains))) return false;
  for (size_t i = 0; i < n; ++i) {
    BloodStain s;
    s.pos = r.vector();
    s.normal = r.vector();
    s.size = r.number();
    s.age = r.number();
    s.color = kBlood;
    if (s.size < 0 || s.age < 0) return false;
    stains.push_back(s);
  }
  std::vector<PropInstancePtr> props;
  const auto count = r.integer();
  if (count > 9) return false;
  Attachments trial;
  for (size_t i = 0; i < count && r.ok; ++i) {
    auto p = std::make_shared<PropInstance>();
    p->id = r.integer(8);
    p->archetype = motion.props.registry->archetype(r.string());
    const auto point = r.integer(1), style = r.integer(1);
    p->socket = r.string();
    p->state.condition = r.number();
    p->state.strap = r.number();
    p->state.charges = i32(r.integer());
    if (version >= 2) {
      const auto count = r.integer();
      if (count > 4096) return false;
      for (size_t j = 0; j < count && r.ok; ++j) p->state.contents.push_back(r.string());
    }
    p->pos = r.vector();
    p->rotation = r.quat();
    if (!p->archetype || point >= 9 || style > 5 || p->state.condition < 0 || p->state.condition > 1 || p->state.strap < 0 || p->state.strap > 1) return false;
    if (!p->id) return false;
    for (const auto& existing : props)
      if (existing->id == p->id) return false;
    if (const auto existing = motion.props.registry->get(p->id);
        existing && existing->location == PropLocation::Attached && existing->character != motion.props.owner)
      return false;
    p->damaged_model = p->archetype->model->clone();
    if (!read_model(r, *p->damaged_model, version)) return false;
    p->geometry_version = 1;
    if (version >= 3) {
      p->mass_fraction = r.number();
      if (!r.ok || p->mass_fraction < 0 || p->mass_fraction > 1) return false;
    }
    if (!trial.accepts(*p->archetype, AttachPoint(point), p->socket, WieldStyle(style))) return false;
    p->point = AttachPoint(point);
    p->style = WieldStyle(style);
    trial.slots[size_t(point)] = p;
    props.push_back(p);
  }
  if (!r.done()) return false;
  for (auto& p : motion.props.slots)
    if (p) {
      motion.props.registry->retire(p);
      p.reset();
    }
  model = std::move(m);
  owns_model = true;
  ++geometry_version;
  for (int i = 1; i < 16; ++i)
    if (lost & (1 << i)) behaviours.lose_limb(i);
  behaviours.damage = std::move(state);
  effects.stains = std::move(stains);
  for (const auto& p : props) {
    const auto id = p->id;
    auto item = motion.props.registry->restore(p);
    if (!item) return false;
    attach(item, p->point, p->socket, p->style);
    (void)id;
  }
  return true;
}
}  // namespace svx::anim
