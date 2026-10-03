#include "svx/anim/characters/attachments.hpp"
#include "svx/anim/damage/record.hpp"
namespace svx::anim {
namespace {
// v1: cells and shades; v2: stains, tissues, the share of its material left, when it went loose
constexpr u64 kItemVersion = 2;
void write_item(record::Writer& w, const PropInstance& p) {
  w.integer(p.id, 8);
  w.string(p.archetype->id);
  w.number(p.state.condition);
  w.number(p.state.strap);
  w.integer(p.state.charges);
  w.integer(p.state.contents.size());
  for (const auto& item : p.state.contents) w.string(item);
  w.vector(p.pos);
  w.quat(p.rotation);
  w.vector(p.velocity);
  w.vector(p.angular);
  w.number(p.retained_mass);
  w.integer(u8(p.last_release), 1);
  const auto& part = p.model().parts[0];
  for (i32 v : part.origin) w.integer(u32(v));
  for (i32 v : part.dims) w.integer(u32(v));
  record::write_cells(w, part);
  w.number(p.mass_fraction);
  w.integer(p.loose_since, 8);
}
PropInstancePtr read_item(record::Reader& r, u64 version, const PropRegistry& registry) {
  auto p = std::make_shared<PropInstance>();
  p->id = r.integer(8);
  p->archetype = registry.archetype(r.string());
  p->state.condition = r.number();
  p->state.strap = r.number();
  p->state.charges = i32(r.integer());
  const auto contents = r.integer();
  if (contents > 4096) return {};
  for (u64 j = 0; j < contents && r.ok; ++j) p->state.contents.push_back(r.string());
  p->pos = r.vector();
  p->rotation = r.quat();
  p->velocity = r.vector();
  p->angular = r.vector();
  p->retained_mass = r.number();
  const auto reason = r.integer(1);
  if (!r.ok || !p->id || !p->archetype || reason > 8 || p->state.condition < 0 || p->state.condition > 1 || p->state.strap < 0 || p->state.strap > 1 ||
      p->retained_mass < 0)
    return {};
  p->last_release = ReleaseReason(reason);
  VoxelPart part;
  for (auto& v : part.origin) v = i32(u32(r.integer()));
  size_t cells = 1;
  for (auto& v : part.dims) {
    const auto size = r.integer();
    if (!size || size > 512) return {};
    v = i32(size);
    cells *= size_t(size);
  }
  if (!record::read_cells(r, part, cells, version >= 2)) return {};
  part.initial_count = part.count;
  if (version >= 2) {
    p->mass_fraction = r.number();
    p->loose_since = r.integer(8);
    if (!r.ok || p->mass_fraction < 0 || p->mass_fraction > 1) return {};
  }
  p->damaged_model = std::make_shared<VoxelModel>(p->archetype->model->skeleton, p->archetype->model->voxel_size, std::vector<VoxelPart>{std::move(part)});
  p->damaged_model->tissue = p->archetype->model->tissue;
  p->geometry_version = 1;
  return p;
}
}  // namespace
std::vector<u8> PropRegistry::record_loose() const {
  record::Writer w;
  w.integer(kItemVersion);
  w.integer(next_, 8);
  size_t count = 0;
  for (const auto& [id, p] : items_)
    if (p->location == PropLocation::Loose) ++count;
  w.integer(count);
  for (const auto& [id, p] : items_)
    if (p->location == PropLocation::Loose) write_item(w, *p);
  w.integer(released_, 8);
  return w.bytes;
}
bool PropRegistry::restore_loose(std::span<const u8> bytes) {
  record::Reader r{bytes};
  const auto version = r.integer();
  if (version < 1 || version > kItemVersion) return false;
  const u64 next = r.integer(8), count = r.integer();
  if (!next || count > bytes.size() / 150) return false;
  std::map<u64, PropInstancePtr> loaded;
  for (u64 i = 0; i < count && r.ok; ++i) {
    auto p = read_item(r, version, *this);
    if (!p || p->id >= next || loaded.contains(p->id)) return false;
    if (const auto existing = get(p->id); existing && existing->location == PropLocation::Attached) return false;
    loaded[p->id] = p;
  }
  const u64 released = version >= 2 ? r.integer(8) : 0;
  if (!r.done()) return false;
  for (auto it = items_.begin(); it != items_.end();) {
    if (it->second->location == PropLocation::Loose) {
      reclaim(it->second);
      it = items_.erase(it);
    } else
      ++it;
  }
  items_.insert(loaded.begin(), loaded.end());
  next_ = std::max(next_, next);
  released_ = std::max(released_, released);
  return true;
}
std::vector<u8> PropRegistry::record_item(const PropInstance& p) const {
  record::Writer w;
  w.integer(kItemVersion);
  write_item(w, p);
  return w.bytes;
}
PropInstancePtr PropRegistry::restore_item(std::span<const u8> bytes) {
  record::Reader r{bytes};
  const auto version = r.integer();
  if (version < 1 || version > kItemVersion) return {};
  auto p = read_item(r, version, *this);
  if (!p || !r.done()) return {};
  if (const auto existing = get(p->id); existing && existing->location == PropLocation::Attached) return {};
  p->location = PropLocation::Loose;
  restore(p);
  return p;
}
}  // namespace svx::anim
