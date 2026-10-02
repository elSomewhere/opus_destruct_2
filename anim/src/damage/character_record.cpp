#include "svx/anim/character.hpp"
#include "svx/anim/damage/record.hpp"
namespace svx::anim {
namespace {
void write_model(record::Writer& w, const VoxelModel& m) {
  w.integer(m.parts.size());
  for (const auto& p : m.parts) {
    w.integer(p.bone);
    w.block(p.cells);
    w.block(p.shade);
  }
}
bool read_model(record::Reader& r, VoxelModel& m) {
  if (r.integer() != m.parts.size()) return false;
  for (auto& p : m.parts) {
    if (r.integer() != u64(p.bone)) return false;
    const auto cells = r.block(), shade = r.block();
    if (cells.size() != p.cells.size() || (!shade.empty() && shade.size() != cells.size())) return false;
    for (u8 cell : cells)
      if (cell > 16) return false;
    p.cells.assign(cells.begin(), cells.end());
    p.shade.assign(shade.begin(), shade.end());
    p.count = i32(std::count_if(p.cells.begin(), p.cells.end(), [](u8 c) { return c != 0; }));
    ++p.version;
  }
  return r.ok;
}
}  // namespace
std::vector<u8> Character::damage_record() const {
  if (alive() && !owns_model && behaviours.damage.wound_count() == 0) return {};
  record::Writer w;
  w.integer(0x44585653);
  w.integer(2);
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
      w.integer(p->state.ammunition);
      w.integer(p->state.contents.size());
      for (const auto& item : p->state.contents) w.string(item);
      w.vector(p->pos);
      w.quat(p->rotation);
      write_model(w, p->model());
    }
  return w.bytes;
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
  if (version != 1 && version != 2) return false;
  const auto lost = r.integer(2);
  auto m = model->clone();
  if (!read_model(r, *m)) return false;
  DamageState state;
  if (!state.restore(r.block())) return false;
  std::deque<BloodStain> stains;
  const auto n = r.integer();
  if (n > 600) return false;
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
    p->archetype = prop_archetype(r.string());
    const auto point = r.integer(1), style = r.integer(1);
    p->socket = r.string();
    p->state.condition = r.number();
    p->state.strap = r.number();
    p->state.ammunition = i32(r.integer());
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
    if (!read_model(r, *p->damaged_model)) return false;
    if (!trial.attach(p, AttachPoint(point), p->socket, WieldStyle(style))) return false;
    p->location = PropLocation::Loose;
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
