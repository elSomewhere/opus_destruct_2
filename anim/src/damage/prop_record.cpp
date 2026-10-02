#include "svx/anim/characters/attachments.hpp"
#include "svx/anim/damage/record.hpp"
namespace svx::anim {
std::vector<u8> PropRegistry::record_loose() const {
  record::Writer w;
  w.integer(1);
  w.integer(next_, 8);
  size_t count = 0;
  for (const auto& [id, p] : items_)
    if (p->location == PropLocation::Loose) ++count;
  w.integer(count);
  for (const auto& [id, p] : items_)
    if (p->location == PropLocation::Loose) {
      w.integer(id, 8);
      w.string(p->archetype->id);
      w.number(p->state.condition);
      w.number(p->state.strap);
      w.integer(p->state.ammunition);
      w.integer(p->state.contents.size());
      for (const auto& item : p->state.contents) w.string(item);
      w.vector(p->pos);
      w.quat(p->rotation);
      w.vector(p->velocity);
      w.vector(p->angular);
      w.number(p->retained_mass);
      w.integer(u8(p->last_release), 1);
      const auto& part = p->model().parts[0];
      for (i32 v : part.origin) w.integer(u32(v));
      for (i32 v : part.dims) w.integer(u32(v));
      w.block(part.cells);
      w.block(part.shade);
    }
  return w.bytes;
}
bool PropRegistry::restore_loose(std::span<const u8> bytes) {
  record::Reader r{bytes};
  if (r.integer() != 1) return false;
  const u64 next = r.integer(8), count = r.integer();
  if (!next || count > bytes.size() / 150) return false;
  std::map<u64, PropInstancePtr> loaded;
  for (u64 i = 0; i < count && r.ok; ++i) {
    auto p = std::make_shared<PropInstance>();
    p->id = r.integer(8);
    p->archetype = prop_archetype(r.string());
    p->state.condition = r.number();
    p->state.strap = r.number();
    p->state.ammunition = i32(r.integer());
    const auto contents = r.integer();
    if (contents > 4096) return false;
    for (u64 j = 0; j < contents && r.ok; ++j) p->state.contents.push_back(r.string());
    p->pos = r.vector();
    p->rotation = r.quat();
    p->velocity = r.vector();
    p->angular = r.vector();
    p->retained_mass = r.number();
    const auto reason = r.integer(1);
    if (!p->id || p->id >= next || loaded.contains(p->id) || !p->archetype || reason > 8 || p->state.condition < 0 || p->state.condition > 1 ||
        p->state.strap < 0 || p->state.strap > 1 || p->retained_mass < 0)
      return false;
    if (const auto existing = get(p->id); existing && existing->location == PropLocation::Attached) return false;
    p->last_release = ReleaseReason(reason);
    VoxelPart part;
    for (auto& v : part.origin) v = i32(u32(r.integer()));
    size_t cells = 1;
    for (auto& v : part.dims) {
      const auto size = r.integer();
      if (!size || size > 512) return false;
      v = i32(size);
      cells *= size_t(size);
    }
    const auto data = r.block(), shade = r.block();
    if (cells != data.size() || (!shade.empty() && shade.size() != cells)) return false;
    for (u8 v : data)
      if (v > 16) return false;
    part.cells.assign(data.begin(), data.end());
    part.shade.assign(shade.begin(), shade.end());
    part.count = part.initial_count = i32(std::count_if(data.begin(), data.end(), [](u8 v) { return v != 0; }));
    p->damaged_model = std::make_shared<VoxelModel>(p->archetype->model->skeleton, p->archetype->model->voxel_size, std::vector<VoxelPart>{std::move(part)});
    p->geometry_version = 1;
    loaded[p->id] = p;
  }
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
  return true;
}
}  // namespace svx::anim
