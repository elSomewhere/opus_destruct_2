// structvox core — World's public API: each call forwards to its implementation (world_impl.hpp).
#include <utility>

#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

World::World() : impl_(std::make_unique<Impl>(*this)) {
  grid_ = &impl_->grid_;
  oriented_ = &impl_->oriented_;
}
World::~World() = default;
World::World(World&& o) noexcept : impl_(std::move(o.impl_)), grid_(o.grid_), oriented_(o.oriented_) {
  if (impl_) impl_->self_ = this;
}
World& World::operator=(World&& o) noexcept {
  impl_ = std::move(o.impl_);
  grid_ = o.grid_;
  oriented_ = o.oriented_;
  if (impl_) impl_->self_ = this;
  return *this;
}

void World::configure(const WorldConfig& c) { impl_->configure(c); }
const WorldConfig& World::config() const { return impl_->config(); }
const MaterialTable& World::materials() const { return impl_->materials(); }
bool World::register_material(const Material& m, MaterialId* id) { return impl_->register_material(m, id); }
void World::set_material(MaterialId id, const Material& m) { impl_->set_material(id, m); }
void World::set_params(const WorldParams& p) { impl_->set_params(p); }
const WorldParams& World::params() const { return impl_->params(); }
void World::load(VoxelGrid&& g) { impl_->load(std::move(g)); }
bool World::bake(f64* ms) { return impl_->bake(ms); }
const World::DesignReport& World::design_report() const { return impl_->design_report(); }
void World::enable_streaming(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc) { impl_->enable_streaming(std::move(src), sc); }
bool World::streaming() const { return impl_->streaming(); }
const ChunkSource* World::source() const { return impl_->source(); }
void World::set_focus(const std::vector<V3>& points) { impl_->set_focus(points); }
void World::ensure_resident(const IVec3& lo, const IVec3& hi) { impl_->ensure_resident(lo, hi); }
bool World::chunk_resident(const IVec3& chunk) const { return impl_->chunk_resident(chunk); }
bool World::column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const {
  const auto* c = impl_->column_if(cx, cy);
  if (!impl_->streaming() || !c) return false;
  if (z_lo) *z_lo = c->z_lo;
  if (z_hi) *z_hi = c->z_hi;
  if (below) *below = c->below;
  return true;
}
std::vector<u8> World::save_delta() const { return impl_->save_delta(); }
bool World::load_delta(const std::vector<u8>& bytes) { return impl_->load_delta(bytes); }
bool World::modified() const { return impl_->modified(); }
GridId World::add_grid(const GridDesc& d, VoxelGrid&& voxels) { return impl_->add_grid(d, std::move(voxels)); }
i64 World::loosen_grid(GridId id) { return impl_->loosen_grid(id); }
bool World::remove_grid(GridId id) { return impl_->remove_grid(id); }
bool World::set_grid_frame(GridId id, const GridFrame& frame) { return impl_->set_grid_frame(id, frame); }
JointId World::add_joint(const JointDesc& d) { return impl_->add_joint(d); }
bool World::remove_joint(JointId id) { return impl_->remove_joint(id); }
bool World::set_joint_drive(JointId id, const JointDrive& drive) { return impl_->set_joint_drive(id, drive); }
bool World::set_joint_limits(JointId id, bool on, f64 lower, f64 upper) { return impl_->set_joint_limits(id, on, lower, upper); }
bool World::joint(JointId id, JointState* out) const { return impl_->joint(id, out); }
std::vector<i64> World::joined_pieces(i64 piece) const { return impl_->joined_pieces(piece); }
std::vector<JointId> World::joints() const { return impl_->joints(); }
WheelId World::add_wheel(const WheelDesc& d) { return impl_->add_wheel(d); }
bool World::remove_wheel(WheelId id) { return impl_->remove_wheel(id); }
bool World::set_wheel_input(WheelId id, f64 drive, f64 brake, f64 steer) { return impl_->set_wheel_input(id, drive, brake, steer); }
bool World::wheel(WheelId id, WheelState* out) const { return impl_->wheel(id, out); }
std::vector<WheelId> World::wheels() const { return impl_->wheels(); }
bool World::set_piece_max_speed(i64 piece, f64 max_speed) { return impl_->set_piece_max_speed(piece, max_speed); }
ArticulationId World::add_articulation(const ArticulationDesc& d) { return impl_->add_articulation(d); }
bool World::remove_articulation(ArticulationId id) { return impl_->remove_articulation(id); }
std::vector<ArticulationId> World::articulations() const { return impl_->articulations(); }
ArticulationControl* World::articulation_control(ArticulationId id) { return impl_->articulation_control(id); }
bool World::articulation_state(ArticulationId id, ArticulationState* out) const { return impl_->articulation_state(id, out); }
const std::vector<u8>* World::articulation_data(ArticulationId id) const { return impl_->articulation_data(id); }
bool World::set_articulation_data(ArticulationId id, std::vector<u8> data) { return impl_->set_articulation_data(id, std::move(data)); }
bool World::set_link(ArticulationId id, u16 link, const V3& pos, const Quat& rot, const V3& vel, const V3& ang) { return impl_->set_link(id, link, pos, rot, vel, ang); }
bool World::add_link_velocity(ArticulationId id, u16 link, const V3& dv, const V3& dw) { return impl_->add_link_velocity(id, link, dv, dw); }
bool World::apply_link_impulse(ArticulationId id, u16 link, const V3& point, const V3& impulse) { return impl_->apply_link_impulse(id, link, point, impulse); }
bool World::lose_link(ArticulationId id, u16 link, f64 mass_scale) { return impl_->lose_link(id, link, mass_scale); }
bool World::wake_articulation(ArticulationId id) { return impl_->wake_articulation(id); }
bool World::articulation_asleep(ArticulationId id) const { return impl_->articulation_asleep(id); }
i64 World::link_body(ArticulationId id, u16 link) const { return impl_->link_body(id, link); }
std::vector<GridId> World::grids() const { return impl_->grids(); }
const VoxelGrid* World::grid(GridId id) const { return impl_->grid(id); }
bool World::grid_frame(GridId id, GridFrame* out) const { return impl_->grid_frame(id, out); }
i32 World::grid_priority(GridId id) const { return impl_->grid_priority(id); }
const u8* World::grid_solids(const IVec3& world_chunk) const { return impl_->grid_solids(world_chunk); }
u64 World::grid_solids_stamp(const IVec3& world_chunk) const { return impl_->grid_solids_stamp(world_chunk); }
bool World::grid_voxel_at(const V3& world, GridId* grid, IVec3* voxel) const { return impl_->grid_voxel_at(world, grid, voxel); }
V3 World::grid_to_world(GridId id, const V3& lattice) const { return impl_->grid_to_world(id, lattice); }
V3 World::world_to_grid(GridId id, const V3& world) const { return impl_->world_to_grid(id, world); }
void World::carve(const V3& pos, f64 radius) { impl_->carve(pos, radius); }
void World::shoot(const V3& pos, f64 radius, f64 energy) { impl_->shoot(pos, radius, energy); }
void World::blast(const V3& pos, f64 radius, f64 energy) { impl_->blast(pos, radius, energy); }
i32 World::set_voxels(const std::vector<VoxelEdit>& edits, u32 flags) { return impl_->set_voxels(edits, flags); }
i32 World::set_voxels(GridId grid, const std::vector<VoxelEdit>& edits, u32 flags) { return impl_->set_voxels(grid, edits, flags); }
bool World::apply_impulse(i64 piece, const V3& point, const V3& impulse) { return impl_->apply_impulse(piece, point, impulse); }
bool World::remove_piece(i64 piece) { return impl_->remove_piece(piece); }
bool World::set_piece_keep(i64 piece, bool keep) { return impl_->set_piece_keep(piece, keep); }
void World::tick() { impl_->tick(); }
i64 World::ticks() const { return impl_->ticks(); }
f64 World::time() const { return impl_->time(); }
std::vector<WorldEvent> World::take_events() { return impl_->take_events(); }
std::vector<u64> World::take_changed_chunks() { return impl_->take_changed_chunks(); }
std::vector<u64> World::take_evicted_chunks() { return impl_->take_evicted_chunks(); }
std::vector<GridChunk> World::take_changed_grid_chunks() { return impl_->take_changed_grid_chunks(); }
std::vector<PieceState> World::pieces() const { return impl_->pieces(); }
const Body* World::piece(i64 id) const { return impl_->piece(id); }
const RigidWorld& World::rigid() const { return impl_->rigid(); }
int World::add_layer(const LayerSpec& spec) { return impl_->add_layer(spec); }
u8 World::layer(GridId grid, int L, const IVec3& p) const { return impl_->layer(grid, L, p); }
i32 World::set_layer(int L, const std::vector<LayerEdit>& edits) { return impl_->set_layer(L, edits); }
i32 World::set_layer(GridId grid, int L, const std::vector<LayerEdit>& edits) { return impl_->set_layer(grid, L, edits); }
std::vector<u64> World::take_layer_changes(int L) { return impl_->take_layer_changes(L); }
std::vector<u64> World::take_layer_changes(GridId grid, int L) { return impl_->take_layer_changes(grid, L); }
u8 World::piece_layer(i64 piece, i32 shape, int L, const IVec3& shape_voxel) const { return impl_->piece_layer(piece, shape, L, shape_voxel); }
i32 World::set_piece_layer(i64 piece, i32 shape, int L, const std::vector<LayerEdit>& shape_voxels) { return impl_->set_piece_layer(piece, shape, L, shape_voxels); }
bool World::remove_piece_voxels(i64 piece, i32 shape, const std::vector<IVec3>& shape_voxels, bool dust) { return impl_->remove_piece_voxels(piece, shape, shape_voxels, dust); }
void World::set_loads(u64 group, std::vector<VoxelLoad> loads) { impl_->set_loads(group, std::move(loads)); }
void World::apply_force(i64 piece, const V3& point, const V3& force) { impl_->apply_force(piece, point, force); }
void World::wake_piece(i64 piece) { impl_->wake_piece(piece); }
void World::add_system(std::shared_ptr<WorldSystem> s) { impl_->add_system(std::move(s)); }
const std::vector<std::shared_ptr<WorldSystem>>& World::systems() const { return impl_->systems(); }
bool World::in_range(const V3& p) const { return impl_->in_range(p); }
RayHit World::raycast(const V3& origin, const V3& dir, f64 max_dist) const { return impl_->raycast(origin, dir, max_dist); }
CollideResult World::collide(const V3& min, const V3& max, const V3& move) const { return impl_->collide(min, max, move); }
SweepHit World::sweep(const V3& min, const V3& max, const V3& move) const { return impl_->sweep(min, max, move); }
bool World::overlaps(const V3& min, const V3& max) const { return impl_->overlaps(min, max); }
f64 World::depenetrate(const V3& min, const V3& max, f64 max_rise) const { return impl_->depenetrate(min, max, max_rise); }
bool World::debug_field(GridId grid, const IVec3& chunk, DebugField field, std::vector<u8>* out) { return impl_->debug_field(grid, chunk, field, out); }
f64 World::probe_utilization(GridId grid, const IVec3& voxel, i32* over) { return impl_->probe_utilization(grid, voxel, over); }
void World::debug_voxel(const IVec3& p) { impl_->debug_voxel(p); }
WorldStats World::stats() const { return impl_->stats(); }
MemoryReport World::memory() const { return impl_->memory(); }
u64 World::state_hash() const { return impl_->state_hash(); }
u64 World::session_hash() const { return impl_->session_hash(); }

}  // namespace svx
