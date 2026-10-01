// structvox core — World::Impl: the world's machinery and state behind its public API
// (svx/world/world.hpp forwards to it). Internal: the world sources' own.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "svx/stress/stress.hpp"
#include "svx/world/world.hpp"

namespace svx {

namespace world_detail {
struct SecAcc;
struct VoxelAt;
struct JSample;
struct Rd;
class ChangeArchive;
}  // namespace world_detail

struct World::Impl {
  explicit Impl(World& self);
  ~Impl();
  World* self_;  // (the public face: what the systems are handed)
  using DesignReport = World::DesignReport;
  static constexpr int kDamageLayer = World::kDamageLayer;

  // ---- the public API's work (World forwards to these)
  void configure(const WorldConfig& c);
  const WorldConfig& config() const { return cfg_; }
  const MaterialTable& materials() const { return *mats_; }
  bool register_material(const Material& m, MaterialId* id) { return mats_->add(m, id); }
  void set_material(MaterialId id, const Material& m) {
    mats_->set(id, m);
    frag_memo_.clear();
  }
  void set_params(const WorldParams& p);
  const WorldParams& params() const { return par_; }

  void load(VoxelGrid&& g);
  bool bake(f64* ms = nullptr);
  const DesignReport& design_report() const { return design_; }
  void enable_streaming(std::shared_ptr<const ChunkSource> src, const StreamConfig& sc);
  bool streaming() const { return strm_.source != nullptr; }
  const ChunkSource* source() const { return strm_.source.get(); }
  void set_focus(const V3& p) { set_focus(std::vector<V3>{p}); }
  void set_focus(const std::vector<V3>& points);
  void ensure_resident(const IVec3& lo, const IVec3& hi);
  bool chunk_resident(const IVec3& chunk) const;

  std::vector<u8> save_delta() const;
  bool load_delta(const std::vector<u8>& bytes);
  bool modified() const;

  const VoxelGrid& grid() const { return grid_; }
  f64 voxel_size() const { return grid_.h; }

  GridId add_grid(const GridDesc& d, VoxelGrid&& voxels);
  GridId add_grid(const GridFrame& frame, VoxelGrid&& voxels, bool base = true) {
    GridDesc d;
    d.frame = frame;
    d.base = base;
    return add_grid(d, std::move(voxels));
  }
  i64 loosen_grid(GridId id);
  bool remove_grid(GridId id);
  bool set_grid_frame(GridId id, const GridFrame& frame);

  JointId add_joint(const JointDesc& d);
  bool remove_joint(JointId id);
  bool set_joint_drive(JointId id, const JointDrive& drive);
  bool set_joint_limits(JointId id, bool on, f64 lower, f64 upper);
  bool joint(JointId id, JointState* out) const;
  std::vector<i64> joined_pieces(i64 piece) const;
  std::vector<JointId> joints() const;

  WheelId add_wheel(const WheelDesc& d);
  bool remove_wheel(WheelId id);
  bool set_wheel_input(WheelId id, f64 drive, f64 brake, f64 steer);
  bool wheel(WheelId id, WheelState* out) const;
  std::vector<WheelId> wheels() const;
  bool set_piece_max_speed(i64 piece, f64 max_speed);

  ArticulationId add_articulation(const ArticulationDesc& d);
  bool remove_articulation(ArticulationId id);
  std::vector<ArticulationId> articulations() const;
  ArticulationControl* articulation_control(ArticulationId id);
  bool articulation_state(ArticulationId id, ArticulationState* out) const;
  const std::vector<u8>* articulation_data(ArticulationId id) const;
  bool set_articulation_data(ArticulationId id, std::vector<u8> data);
  bool set_link(ArticulationId id, u16 link, const V3& pos, const Quat& rot, const V3& vel, const V3& ang);
  bool add_link_velocity(ArticulationId id, u16 link, const V3& dv, const V3& dw);
  bool apply_link_impulse(ArticulationId id, u16 link, const V3& point, const V3& impulse);
  bool lose_link(ArticulationId id, u16 link, f64 mass_scale);
  bool wake_articulation(ArticulationId id);
  bool articulation_asleep(ArticulationId id) const;
  i64 link_body(ArticulationId id, u16 link) const;
  std::vector<GridId> grids() const;
  const VoxelGrid* grid(GridId id) const;
  bool grid_frame(GridId id, GridFrame* out) const;
  i32 grid_priority(GridId id) const;
  const u8* grid_solids(const IVec3& world_chunk) const;
  u64 grid_solids_stamp(const IVec3& world_chunk) const;
  bool grid_voxel_at(const V3& world, GridId* grid, IVec3* voxel) const;
  bool grid_solid(const IVec3& world_voxel) const {
    if (oriented_ == 0) return false;
    const u8* b = grid_solids(chunk_of(world_voxel));
    const i32 i = chunk_index(world_voxel);
    return b && ((b[i >> 3] >> (i & 7)) & 1);
  }
  V3 grid_to_world(GridId id, const V3& lattice) const;
  V3 world_to_grid(GridId id, const V3& world) const;

  void carve(const V3& pos, f64 radius);
  void shoot(const V3& pos, f64 radius, f64 energy);
  void blast(const V3& pos, f64 radius, f64 energy);
  i32 set_voxels(const std::vector<VoxelEdit>& edits, u32 flags = 0);
  i32 set_voxels(GridId grid, const std::vector<VoxelEdit>& edits, u32 flags = 0);
  bool apply_impulse(i64 piece, const V3& point, const V3& impulse);
  bool remove_piece(i64 piece);
  bool set_piece_keep(i64 piece, bool keep);

  void tick();
  i64 ticks() const { return st_.ticks; }
  f64 time() const { return static_cast<f64>(steps_) * cfg_.dt; }

  std::vector<WorldEvent> take_events();
  std::vector<u64> take_changed_chunks();
  std::vector<u64> take_evicted_chunks();
  std::vector<GridChunk> take_changed_grid_chunks();
  std::vector<PieceState> pieces() const;
  const Body* piece(i64 id) const;
  const RigidWorld& rigid() const { return rigid_; }

  int add_layer(const LayerSpec& spec);
  int layer_index(const std::string& name) const { return grid_.layer_index(name); }
  u8 layer(int L, const IVec3& p) const { return grid_.layer(L, p); }
  u8 layer(GridId grid, int L, const IVec3& p) const;
  i32 set_layer(int L, const std::vector<LayerEdit>& edits);
  i32 set_layer(GridId grid, int L, const std::vector<LayerEdit>& edits);
  std::vector<u64> take_layer_changes(int L) { return grid_.take_layer_dirty(L); }
  std::vector<u64> take_layer_changes(GridId grid, int L);
  u8 piece_layer(i64 piece, int L, const IVec3& shape_voxel) const { return piece_layer(piece, 0, L, shape_voxel); }
  u8 piece_layer(i64 piece, i32 shape, int L, const IVec3& shape_voxel) const;
  i32 set_piece_layer(i64 piece, int L, const std::vector<LayerEdit>& shape_voxels) { return set_piece_layer(piece, 0, L, shape_voxels); }
  i32 set_piece_layer(i64 piece, i32 shape, int L, const std::vector<LayerEdit>& shape_voxels);
  bool remove_piece_voxels(i64 piece, const std::vector<IVec3>& shape_voxels, bool dust) {
    return remove_piece_voxels(piece, 0, shape_voxels, dust);
  }
  bool remove_piece_voxels(i64 piece, i32 shape, const std::vector<IVec3>& shape_voxels, bool dust);
  void set_loads(u64 group, std::vector<VoxelLoad> loads);
  void apply_force(i64 piece, const V3& point, const V3& force);
  void wake_piece(i64 piece);
  void add_system(std::shared_ptr<WorldSystem> s);
  const std::vector<std::shared_ptr<WorldSystem>>& systems() const { return ext_.systems; }

  bool in_range(const V3& p) const;
  RayHit raycast(const V3& origin, const V3& dir, f64 max_dist) const;
  bool has_oriented_grids() const { return oriented_ > 0; }
  CollideResult collide(const V3& min, const V3& max, const V3& move) const;
  SweepHit sweep(const V3& min, const V3& max, const V3& move) const;
  bool overlaps(const V3& min, const V3& max) const;
  f64 depenetrate(const V3& min, const V3& max, f64 max_rise) const;
  bool debug_field(const IVec3& chunk, DebugField field, std::vector<u8>* out) { return debug_field(kWorldGrid, chunk, field, out); }
  bool debug_field(GridId grid, const IVec3& chunk, DebugField field, std::vector<u8>* out);
  f64 probe_utilization(const IVec3& voxel, i32* over = nullptr) { return probe_utilization(kWorldGrid, voxel, over); }
  f64 probe_utilization(GridId grid, const IVec3& voxel, i32* over = nullptr);
  void debug_voxel(const IVec3& p);

  WorldStats stats() const;
  MemoryReport memory() const;
  u64 state_hash() const;
  u64 session_hash() const;


  // ---- the machinery
  struct Structure;
  struct GridState;
  struct JunctionScratch;
  struct PendingEvent {
    bool blast = false;
    V3 pos;
    f64 radius = 0.0, energy = 0.0;  // (a carve: energy < 0, a cut)
  };
  // Internally a grid is its slot (grids_ index; the world grid's is 0), not its id.
  // A voxel of a grid.
  struct GVox {
    IVec3 p{0, 0, 0};
    u16 grid = 0;
  };
  // a fragment's identity: grid, chunk and its first voxel (stable while that voxel stays)
  struct FragKey {
    u64 chunk = 0;
    i32 idx = -1;  // index in the chunk's FragChunk (valid for the chunk's current fragments)
    u16 grid = 0;
    bool operator==(const FragKey& o) const { return chunk == o.chunk && idx == o.idx && grid == o.grid; }
  };
  // a chunk of a grid, ordered by grid then chunk (the world grid's first, in key order)
  struct GKey {
    u16 grid = 0;
    u64 chunk = 0;
    bool operator==(const GKey& o) const { return grid == o.grid && chunk == o.chunk; }
    bool operator<(const GKey& o) const { return grid < o.grid || (grid == o.grid && chunk < o.chunk); }
  };
  struct GKeyHash {
    size_t operator()(const GKey& k) const {
      u64 x = k.chunk ^ (static_cast<u64>(k.grid) * 0x9E3779B97F4A7C15ull);
      x = (x ^ (x >> 31)) * 0xBF58476D1CE4E5B9ull;
      return static_cast<size_t>(x ^ (x >> 29));
    }
  };

  // ---- joints (world_joints.cpp)
  struct JointRec;
  struct WheelRec;  // (world_wheels.cpp)
  JointId add_joint_impl(const JointDesc& d, JointId want);  // (want: its id, a source's joint; 0: the next)
  size_t insert_joint(const JointRec& r, const Joint& j);     // (in id order; returns its index)
  // Each substep: the solver's ends from the anchors (their bodies' poses now); a joint whose
  // anchor is gone is marked broken. Between substeps the broken ones (those too, that the solver
  // broke) are reaped: JointBroken, removed.
  void update_joint_ends();
  void reap_joints();
  bool fill_joint_end(JointRec& r, bool b_end);         // false: its anchor is gone
  void drop_joint(size_t k, f64 force, const V3& at);  // (gives way: event, removed)
  void wake_joint(size_t k);                            // (the pieces its ends are on)
  void joints_to_piece(const Body& b);                  // anchors on voxels that went into a new piece follow it
  void joints_follow_splits();                          // ... into the parts of pieces split (flush_body_changes)
  bool jointed(const std::vector<FragKey>& members);     // a joint's end holds on to one of these fragments
  // the joints' forces on the structures of the grids their ends hold on to (structure_loads)
  void joint_structure_loads(f64 dt_sub);

  // ---- the session's pieces and joints in records: deltas, the streaming archive (world_session.cpp)
  struct SessionDelta;
  std::vector<u8> piece_record(const Body& b) const;
  std::unique_ptr<Body> read_piece_record(const std::vector<u8>& rec) const;  // (nullptr: malformed)
  std::vector<u8> joint_record(size_t k) const;
  bool read_joint_record(world_detail::Rd& in, JointRec* r, Joint* j, u8 version) const;  // (version: its group's)
  std::vector<u8> wheel_record(size_t k) const;
  bool read_wheel_record(world_detail::Rd& in, WheelRec* r, Wheel* w, u8 version) const;
  std::vector<u8> session_entries() const;                          // (the delta's session part)
  bool read_session(world_detail::Rd& in, SessionDelta* s, u32 version) const;  // (checked whole; false: malformed; version: the trailer's)
  void apply_session(SessionDelta&& s);                             // (its pieces and joints for the ones there are)
  // (a group's pieces, the joints on them and their dead loads; read; added to the world)
  void write_group(std::vector<u8>& out, const std::vector<const Body*>& bodies, const std::vector<size_t>& joints,
                   const std::vector<size_t>& wheels) const;
  bool read_group(world_detail::Rd& in, SessionDelta* s, u8 version) const;
  void add_group(SessionDelta& s);
  // Pieces out of range (a streamed world): a group (pieces joined by joints or touching, ids
  // sorted) archived with its joints and dead loads in the change archive's budget, with its
  // chunks' region; back when every chunk it needs is resident again; gone with its region.
  void archive_group(const std::vector<i64>& ids, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of);
  std::vector<std::vector<i64>> piece_groups(bool touching) const;  // (joined by joints; and touching, if asked)
  void unload_joints();                                             // (those held by voxels gone out of range)
  void restore_groups();
  void forget_group(u64 key);
  struct ArchivedGroup {
    std::vector<u64> chunks;      // (resident, all: it comes back)
    std::vector<JointId> joints;  // (archived with it: a source's joint is not made again meanwhile)
    u32 pieces = 0;
  };

  // ---- grids (world_grids.cpp)
  VoxelGrid& vg(u16 g);
  const VoxelGrid& vg(u16 g) const;
  GridState& gs(u16 g);
  const GridState& gs(u16 g) const;
  bool live(u16 g) const { return g < grids_.size() && grids_[g] != nullptr; }
  i32 slot_of(GridId id) const;              // -1: none
  GridId id_of(u16 g) const;
  const LatticeXf& xf_of(u16 g) const;       // lattice -> world (the world grid: the identity)
  f64 h_of(u16 g) const;                     // its voxel size
  bool owns(u16 a, u16 b) const;             // a keeps its voxels where a and b overlap (priority, then id)
  V3 voxel_centre(const GVox& v) const;      // in the world
  u64 frag_ident_of(const FragKey& f, i32 first) const;  // (identities: the world grid's as they always were)
  u64 residency_key(u16 g, u64 chunk) const; // (warm starts, reference loads: dropped with their chunk or grid)
  bool resident_key(u64 k) const;
  void refresh_grid_box(u16 g);              // (after its chunks changed)
  void grids_changed();                      // (added, removed, grown: junction candidates are found again)
  // Other grids whose voxels a junction sample of this chunk may reach (the world grid first).
  const std::vector<u16>& near_grids(u16 g, u64 chunk);
  // Which side of a node of grid g a junction sample bonds it on (0..5: the axis and sign, in g's
  // lattice, of the normal out of the node): a node's junction supports are one bond per side
  // and grid, as its supports in its own grid are one per face direction.
  int junction_side(u16 g, const world_detail::JSample& j, bool fwd) const;
  std::vector<StaticGrid> static_grids() const;  // (the rigid bodies' static world)
  void remove_grid_slot(u16 g, bool event);
  // What grid g holds through junctions (the voxels of other grids its faces meet and those whose
  // faces meet it) is extracted again: it is about to let go of them (removed, moved).
  void release_junctions(u16 g);
  // Removes the voxels of grid `lose` whose centres lie in a solid voxel of grid `keep` (the owner
  // of their overlap: the same body), in lose's voxel box [lo, hi] (nullptr: wherever keep is).
  // tracked: a change of this session (else of the level: not saved). Returns how many went.
  i32 displace(u16 keep, u16 lose, const IVec3* lo, const IVec3* hi, bool tracked);
  // Every overlap of grid g with the other grids of its body resolved (displace).
  void displace_overlaps(u16 g, bool tracked);
  // ... within the voxel box [lo, hi] of grid g (voxels were written there).
  void displace_edits(u16 g, const IVec3& lo, const IVec3& hi, bool tracked);
  void tear_voxel(const GVox& v);            // (its faces bond to nothing any more: intra and junctions)
  // Junction samples (world_grids.cpp): of a chunk's faces into other grids, cached per extraction.
  const std::vector<world_detail::JSample>& junction_fwd(JunctionScratch& js, u16 g, u64 chunk);
  // The samples of other grids' faces that land in fragment f.
  const std::vector<world_detail::JSample>* junction_rev(JunctionScratch& js, const FragKey& f);
  // Calls fn(sample, fwd) for fragment f's junction samples: its faces' into other grids (fwd),
  // and other grids' faces' landing in it.
  template <class Fn>
  void each_junction(JunctionScratch& js, const FragKey& f, Fn&& fn);

  // ---- fragments
  FragChunk& frag_chunk(u16 g, const IVec3& cc);  // (re)builds when stale
  FragChunk& adopt_fragments(u16 g, u64 key, FragChunk&& nf);  // (a chunk's new fragments into the cache)
  void prefragment(u16 g, const IVec3& seed_chunk, f64 max_radius);  // (the chunks a walk can reach, in parallel)
  FragChunk* frag_chunk_if(u16 g, u64 key);  // current or nullptr (no rebuild)
  FragParams frag_params(u16 g) const;       // (a grid of another voxel size: the world's rubble in metres)
  FragChunk* frag_chunk_if(const FragKey& f) { return frag_chunk_if(f.grid, f.chunk); }
  bool frag_at(const GVox& v, FragKey* out);  // the free fragment holding voxel v
  i64 owner_of(const FragKey& f) const;      // structure id holding it (0: none)
  world_detail::VoxelAt voxel_at(const GVox& v) const;                  // (sections: the grid's voxel)
  world_detail::VoxelAt piece_voxel_at(const Body& b, i32 shape, const IVec3& p) const;  // (a piece's shape voxel)
  void voxels_of(const FragKey& f, std::vector<IVec3>& out);  // (its grid's coordinates)
  u8 frag_class(const FragKey& f);           // weakest design class of its voxels
  V3 frag_com(const FragKey& f);             // its centre of mass in the world
  // owners of a grid's chunk: stale, `changed` (default: that chunk) to patch
  void mark_owners_stale(u16 g, u64 chunk_key, GKey changed = GKey{0xFFFF, 0});

  // ---- structures (world.cpp)
  Structure* structure(i64 id);
  void process(const PendingEvent& e);
  // (energy < 0: a cut; else an impact of that energy: voxels whose penetration resistance its
  // energy density does not reach stay)
  void carve_world(const V3& c, f64 r, f64 energy, std::vector<GVox>* removed);
  bool penetrates(const Material& M, f64 energy, f64 r, f64 d) const;
  void blast_world(const PendingEvent& e);
  void seed_near(const std::vector<GVox>& removed);
  // Material left grid g (a piece's shape S: the voxels it took, in g's coordinates): the free
  // voxels next to it - its 26 neighbours - that no structure holds are seeded (recheck_vacated).
  void recheck_vacated(u16 g, const BodyShape& S);
  void seed_fragments_near(u16 g, const V3& centre, f64 r);  // (g's fragments within the box of half side r, lattice metres)
  void support_changed(const GVox& v, std::vector<GKey>* chunks);  // (before an anchored voxel goes / after one comes)
  void prune_caches();
  // Extracts the structure holding fragment f (bounded: max_nodes / max_radius, 0 = config): a
  // new Structure, or nullptr after detaching it (it reaches no support).
  // detach_free false (bake): a piece reaching no support is removed from the source world.
  Structure* extract(const FragKey& f, i32 max_nodes = 0, f64 max_radius = 0.0, bool detach_free = true);
  // Streamed worlds are designed on first touch: a structure reaching chunks generated since
  // (not yet designed) is solved under its own weight and its overloaded members strengthened
  // before anything happens to it (an event designs what it will hit before it hits).
  void design_structure(Structure& s, bool dry = false);  // dry: only report
  bool touches_undesigned(const Structure& s) const;
  bool pristine(const Structure& s) const;  // no broken bond, no changed chunk
  void design_near(const V3& c, f64 r);
  void design_node(const Structure& s, i32 node, u8 cls, i64* strengthened);  // (its voxels to class cls)
  void refresh_structures();                 // seeds and stale structures -> (re)extracted
  StressOptions solver_options() const;  // (a stress solve's options, as the configuration asks)
  void step_structures();                    // solves within the work budget, judging
  void judge(Structure& s);
  void break_structure_bond(Structure& s, i32 b);  // (its faces and junction samples, in the grids)
  // A ductile member's section giving way in bending (docs/DAMAGE.md §3): the part that comes
  // loose turns about a plastic hinge there - a hinge that holds the section's plastic moment
  // while it turns (a friction drive at rest) and tears once turned past its rotation capacity -
  // rather than dropping off. At the section's compression edge, about the axis it bends.
  struct HingeCut {
    i32 a = -1, b = -1;  // the bond's nodes (b < 0: a support)
    u16 grid = 0;        // (slot) the grid of its section
    V3 p, axis, n;       // the pivot (world), the axis it turns about, the bond's normal (a to b)
    f64 mp = 0.0;        // N m: the section's plastic moment
    f64 pull = 0.0;      // N: what tears it apart
  };
  bool plastic_hinge(const Structure& s, i32 b, HingeCut* out) const;
  // (the hinge of a bond that failed under this load, its sides turned so: in the bond's frame -
  // a structure's world, a piece's shape frame; false: not a ductile section failing in bending)
  bool hinge_of(const SBond& B, const BondLoad& L, const V3& rot_a, const V3& rot_b, HingeCut* out) const;
  // A loose piece's ductile sections that failed in bending (hinges: its shape frame): a plastic
  // hinge between the parts it comes apart in, anchored on the voxels either side before it splits.
  void piece_hinges(Body& b, const std::vector<HingeCut>& hinges);
  void detach_unsupported(Structure& s, const std::vector<HingeCut>* hinges = nullptr);
  void drop_structure(i64 id);
  // Updates a structure whose chunks were re-fragmented: nodes there retire, the new fragments
  // join with their bonds; the solver keeps its preconditioner. False: re-extract instead.
  bool patch_structure(Structure& s);
  // Appends the nodes of fragments `frags` (clusters of `cell` voxels, 0: one per fragment) and the
  // bonds of `fine` (endpoints: fragment indices, kExisting + existing node, < 0 supports); the
  // fragments become the structure's (the ids of structures they belonged to go to superseded).
  void append_nodes(Structure& s, const std::vector<FragKey>& frags, const std::vector<world_detail::SecAcc>& fine,
                    i32 cell, std::vector<i64>* superseded);
  i32 cluster_cell(i64 fragments, i32 limit = 0) const;  // (limit: 0 = cluster_nodes)
  void retire_structure_nodes(Structure& s, const std::vector<i32>& list);
  void reseed(const Structure& s);  // seeds for extracting a dropped structure's fragments again
  void structure_loads(f64 dt_sub);          // contact forces of bodies on world fragments
  void finish_loads(int substeps);           // smoothing, dead loads, load triggers
  void apply_blast_loads();
  void crack_event(const V3& p, const V3& n, f64 phi);
  void dust_event(const V3& p, const V3& v, i32 voxels, bool crushed);
  std::vector<f64> load_vector(const Structure& s) const;
  i32 world_set_voxels(u16 g, const std::vector<VoxelEdit>& edits, u32 flags);

  // ---- pieces (world_pieces.cpp)
  // A body from world fragments (their voxels leave their grids), with a velocity field.
  // v: its velocity (of its centre of mass); about (optional): v is that of this world point,
  // the velocity field v + w x (X - about) (what it broke off from was moving so)
  Body* make_body_from_world(const std::vector<FragKey>& frags, const V3& v, const V3& w, const V3* about = nullptr);
  int fracture_hook(f64 dt);                 // rigid substep hook: body stress, splits (0 none, 1 bodies changed, 2 solve again)
  struct PointForce {
    i32 frag;                                // body fragment
    V3 F, p;                                 // world force and point
    // what pushes there: a body (its id), a static grid (-1 - its slot); 0: a joint's or a
    // wheel's pull (it acts where it is: not spread over the contacts)
    i64 with = 0;
  };
  // the joints' pulls on pieces (fracture_hook): per body index of rigid_.bodies, into per and fsum
  void joint_piece_forces(std::vector<std::vector<PointForce>>& per, std::vector<f64>& fsum) const;
  // Stress of body b under forces (and its inertia); returns the bonds to break (worst first).
  // energy >= 0: the cracks may cost at most this much (J): an impact pays for its fractures.
  // crushed: (optional) the broken bonds that failed by crushing.
  std::vector<i32> body_stress(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy = -1.0,
                               std::vector<i32>* crushed = nullptr);
  // The check itself touches only the piece (pieces are checked concurrently); what it did for
  // the stats and the events is applied afterwards, in body order.
  struct StressOut {
    std::vector<i32> broken, crushed;
    std::vector<HingeCut> hinges;           // (ductile sections failed in bending: plastic hinges)
    std::vector<std::pair<V3, V3>> cracks;  // world position, normal
    i64 checks = 0, pcg_iters = 0, impact_breaks = 0, steady_breaks = 0;
    i64 modes[4] = {0, 0, 0, 0};
    f64 spent = 0.0;                        // J: what an impact's cracks cost
    f64 assemble_ms = 0.0, solve_ms = 0.0;  // (profiling)
  };
  void body_stress_run(Body& b, const std::vector<PointForce>& forces, bool inertia, f64 energy, StressOut& o);
  void apply_stress_out(const StressOut& o);
  // Crushed material turns to gravel and dust: the fragments on the lighter side of crushed
  // bonds leave the piece (dust events). Returns whether the shape changed.
  bool pulverize(Body& b, const std::vector<i32>& crushed);
  void rebuild_body_graph(Body& b);
  void refragment_body(Body& b);
  // Splits b into its components (pieces take the velocity field of the pre-solve velocities if
  // use_pre); returns true if it split or changed shape (b is then retired: pw_.pending_retire).
  // carried: this substep's contact impulses on b (chips that broke off with most of them take
  // them along, but for what crushing them cost: the rest of the piece goes on without); spent:
  // the fracture energy of the step's cracks.
  struct Carried {
    i32 frag = -1;
    V3 J, p;         // (impulse on b, world point)
    f64 e = 0.0;     // (the kinetic energy it took out of the collision, b's share)
  };
  // A piece in several parts (its bond graph's components; force_replace: reshaped - carved,
  // crushed - whole or not): its parts are new pieces. A piece that keeps its identity (in_place,
  // or keeps_identity: a carrier on its wheels, a jointed part, a kept piece) keeps its largest
  // part - the one most of its wheels are on - edited in place; only the rest are new pieces.
  bool split_body(Body& b, bool use_pre, bool force_replace, const std::vector<Carried>* carried = nullptr, f64 spent = 0.0, bool in_place = false);
  bool keeps_identity(const Body& b) const;
  void wake_around(const Body& b);  // (the sleepers touching b's box)
  // A voxel of a body: its shape and cell.
  struct SVox {
    u16 shape = 0;
    i32 cell = 0;
  };
  std::unique_ptr<Body> sub_body(const Body& parent, const std::vector<SVox>& voxels, bool use_pre);
  void carve_bodies(const V3& c, f64 r, f64 energy);
  void blast_bodies(const PendingEvent& e);
  void flush_body_changes();                 // pending additions / retirements into the world
  void announce_bodies();                    // PieceAdded events for new pieces
  void remove_bodies(std::vector<i64> ids, PieceEnd end);  // PieceRemoved events (announced pieces)
  void limit_bodies();
  static i64 body_bytes(const Body& b, Bytes kind = Bytes::Held);

  // ---- wheels (world_wheels.cpp)
  WheelId add_wheel_impl(const WheelDesc& d, WheelId want);
  size_t insert_wheel(const WheelRec& r, const Wheel& w);  // (in id order; returns its index)
  bool fill_wheel_mount(size_t k);   // the solver's mount from its anchor (false: its voxel is gone)
  void update_wheel_mounts();        // (each substep; a wheel whose voxel is gone comes off)
  void reap_wheels();                // (those that came off: WheelDetached, a wheel piece)
  void wheels_to_piece(const Body& b);  // (mounts on voxels that went into a new piece follow it)
  void wheels_follow_splits();          // (... into the parts of pieces split)
  void wheel_structure_loads(f64 dt_sub);  // (the wheels' forces on the structures they stand on)
  void wheel_piece_forces(std::vector<std::vector<PointForce>>& per, std::vector<f64>& fsum) const;
  i64 make_wheel_body(const Wheel& w);  // (a wheel that came off, as a piece; its id, 0: none)

  // ---- articulations (world_articulations.cpp)
  struct ArticulationRec;
  std::vector<std::unique_ptr<ArticulationRec>> arts_;  // ascending ids
  ArticulationId next_art_ = 1;
  u32 next_target_ = 1;
  ArticulationRec* art(ArticulationId id);
  const ArticulationRec* art(ArticulationId id) const;
  Body* art_link(const ArticulationRec& a, u16 link);
  const Body* art_link(const ArticulationRec& a, u16 link) const;
  // (a tick begins: the hosts' drives into the solver, the links' senses afresh)
  void apply_articulation_controls();
  ArticulationId add_articulation_now(const ArticulationDesc& d, ArticulationId want = 0);  // (no tick check; want: its id, if free)
  // Records (the streaming archive, sessions): an articulation whole - its links as they are now,
  // its joints (their anchors as they are), targets, rules, control and host data.
  struct ArticulationSaved;
  std::vector<u8> articulation_record(const ArticulationRec& a) const;
  bool read_articulation_record(world_detail::Rd& in, ArticulationSaved* out) const;  // (checked whole; false: malformed)
  ArticulationId restore_articulation(ArticulationSaved&& s);                          // (ArticulationAdded; 0: refused)
  // Out of range (a streamed world): archived with its region's chunks, back when they all are.
  void archive_articulation(ArticulationId id, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of);
  void restore_articulations();
  void forget_articulation(u64 key);
  // (its links, joints and targets go; ArticulationRemoved)
  void drop_articulation(ArticulationId id, PieceEnd end);
  void clear_articulations();  // (load: all go, quietly)
  void articulations_out_of_world(f64 floor_z);  // (a link fallen out, or no longer finite: its articulation goes)

  // ---- crumpling (world_crumple.cpp)
  // After a substep: where a crumpling contact carried its cap, its crumpling side folds - its
  // voxels pressed into what it hit dent in, buckle or compact, and glass near them shatters.
  void crumple(f64 dt);
  // (a crumpling body pressing through a static structure: what it knocks out)
  bool punch(Body& b, u16 g, const V3& n, const V3& lo, const V3& hi, f64 force, f64 dt);
  // A piece whose voxels changed in place (crumpled): its fragments, mass and samples again at the
  // same place in the world; it keeps its id while it holds together (else it splits).
  void reshape_in_place(Body& b);
  // (its voxels changed, its lattice where it was: mass and samples again, the same place and motion)
  void refresh_in_place(Body& b, const V3& com0, const V3& x0);

  // ---- streaming (world_io.cpp)
  int stream_update();
  void ensure_chunks(const IVec3& vlo, const IVec3& vhi);
  bool generate_chunk(u64 key);
  void evict_chunk(u64 key);
  void reset_archive(size_t bytes);  // (empty, bounded to bytes - 0: unbounded - with nothing listed as in it)
  void tear_fragment(const FragKey& f, const std::vector<IVec3>& vox);  // (torn out whole: WorldConfig::shards_hold_together)
  void insert_generated(u64 key, bool any, std::vector<Vox>&& voxels);
  f64 focus_distance(const IVec3& chunk) const;  // horizontal, m (to the nearest focus point)
  void generate_grids(u64 key);              // (the source's grids at home in a chunk just generated)
  void evict_grid(u16 g, u64 home_region);   // (a streamed grid out of range: its changes archived)
  // add_grid's work: id 0 = the next; home: the world chunk a streamed grid came with (~0: none);
  // seed: extract its structures at the next tick (a grid added after the design pass)
  GridId add_grid_impl(const GridDesc& d, VoxelGrid&& voxels, GridId id, u64 home, bool seed);
  // A grid's changes as saved (its id, whether a level's, its frame, its chunk records: the
  // changed ones, or all of them for a grid of this session), and applying one's records to a
  // grid (its frame is the caller's: a streamed grid is made where the entry has it).
  std::vector<u8> grid_entry(u16 g) const;
  bool apply_grid_entry(u16 g, const std::vector<u8>& entry, std::vector<u64>* touched);
  // (the oriented grids' and the pieces' voxels, and the world grid's too if asked)
  SweepHit sweep_solids(const V3& mn, const V3& mx, const V3& mv, bool world_grid) const;
  // (each solid voxel of a grid or a piece whose cube may meet the world box [lo, hi]: its centre,
  // axes and half side; f returns true to stop)
  template <class F>
  void for_voxel_cubes(const V3& lo, const V3& hi, bool world_grid, F&& f) const;

  // ---- the subsystems' state
  // Joints and wheels (world_joints.cpp, world_wheels.cpp): their anchors and mounts, parallel to
  // the solver's rigid_.joints and rigid_.wheels (ascending ids).
  struct Attachments {
    std::vector<JointRec> joints;
    JointId next_joint = 1;
    std::vector<WheelRec> wheels;  // their mounts, parallel to rigid_.wheels (ascending ids)
    WheelId next_wheel = 1;
  };
  Attachments att_;
  // Streaming (world_io.cpp) and the change archive (world_session.cpp): what the source made and
  // what is resident, the focus, what went out of range and is kept.
  struct Streaming {
    std::shared_ptr<const ChunkSource> source;
    StreamConfig config{};
    IVec3 lo{}, hi{};  // the source's extent in chunks, held within range (enable_streaming)
    std::unordered_set<u64> generated;
    std::unordered_map<u64, i32> column_count;
    // (the eviction scan: every few ticks, or at once when the focus moved far - it walks every
    // resident chunk; eviction has the radii's hysteresis to spare)
    i64 evict_scan_tick = -1000000;
    std::vector<V3> evict_scan_focus;
    std::unique_ptr<world_detail::ChangeArchive> archive;
    std::unordered_map<u64, i32> region_resident;  // region -> resident chunks
    std::unordered_map<u64, std::vector<u16>> home_grids;  // (streamed) chunk key -> the grids at home in it
    std::map<u64, ArchivedGroup> archived_groups;  // archive key ((3 << 62) | its first piece's id) -> ...
    std::unordered_set<JointId> archived_joints;
    // archive key ((3 << 62) | (1 << 61) | id) -> the chunks it needs
    std::map<u64, std::vector<u64>> archived_arts;
    std::vector<u64> evicted_chunks;
    std::vector<V3> focus;
    bool focus_set = false;
  };
  Streaming strm_;
  std::vector<std::pair<i64, i64>> touching_;  // (streamed) the pieces touching at the last step's end, by id
  // The extensions (world_ext.cpp): layers, systems, host loads, and what changed for the systems
  // and the host.
  struct Extensions {
    std::vector<LayerSpec> layers;
    std::vector<std::shared_ptr<WorldSystem>> systems;
    // group -> loads (ordered: sums in the same order everywhere)
    std::map<u64, std::vector<VoxelLoad>> loads;
    std::unordered_set<u64> host_dirty;  // chunks changed since the host last took them
    bool host_dirty_all = false;
    std::vector<u64> sys_changed, sys_generated, sys_evicted;  // (for the systems' next step)
  };
  Extensions ext_;
  // This tick's piece work (world_pieces.cpp, world_crumple.cpp): pieces made and retired, pieces
  // reshaped in place or split keeping their id.
  struct PieceWork {
    std::vector<std::unique_ptr<Body>> pending_add;
    std::vector<i64> pending_retire;
    std::vector<i64> reshaped;  // (pieces reshaped this tick: PieceReshaped at its end)
    // (pieces split in place, until the joints and wheels on their parts follow them)
    std::vector<i64> split_kept;
  };
  PieceWork pw_;

  // ---- the world's own
  WorldConfig cfg_;
  WorldParams par_;
  // (on the heap: what the world builds refers to it)
  std::unique_ptr<MaterialTable> mats_ = std::make_unique<MaterialTable>(default_materials());
  const MaterialTable& mats() const { return *mats_; }
  VoxelGrid grid_;                           // the world grid (slot 0)
  i64 steps_ = 0;  // unpaused ticks since the level loaded (time())
  std::vector<std::unique_ptr<GridState>> grids_;  // by slot (nullptr: free); slot 0: the world grid's state
  std::unordered_map<GridId, u16> slots_;    // oriented grid id -> slot
  GridId next_grid_ = 1;
  i32 oriented_ = 0;                         // oriented grids there are (none: no junctions anywhere)
  u64 grid_epoch_ = 1;                       // (junction candidates: found again when it changes)
  // (grid_solids: per world chunk, the grids' solids there and what they were made from)
  struct SolidsCache {
    u64 epoch = 0, stamp = 0;
    std::vector<std::array<u64, 3>> from;  // (slot, revision, placement) of the grids in it
    std::vector<u8> bits;                  // (empty: none)
  };
  mutable std::unordered_map<u64, SolidsCache> solids_;
  mutable u64 solids_seq_ = 0;
  const SolidsCache* solids_entry(const IVec3& world_chunk) const;
  void world_chunks_of(const LatticeXf& xf, const V3& lo, const V3& hi, std::vector<u64>& out) const;
  std::vector<GridChunk> grid_dirty_;        // (oriented grids' changed chunks the host has not taken)
  std::vector<GridId> removed_base_;         // base grids removed since load, ascending (saved in deltas)
  void note_removed_base(GridId id);         // (once: a streamed one never comes back)
  std::vector<PendingEvent> queue_;
  std::vector<WorldEvent> events_;
  i64 next_id_ = 1;
  WorldStats st_;
  DesignReport design_;

  FragChunk empty_frags_;                    // (frag_chunk of a chunk that is not there)
  // Fragments of session grids' chunks by their content (fragment_chunk reads the chunk alone):
  // the assemblies a host drops in of one kind are the same voxels, fragmented once. Checked
  // voxel for voxel.
  struct FragMemo {
    IVec3 cc{0, 0, 0};
    f64 scale = 1.0;
    FragParams par;
    std::vector<Vox> v;
    std::vector<u8> broken;
    FragChunk frags;
  };
  std::unordered_map<u64, FragMemo> frag_memo_;
  std::vector<std::unique_ptr<Structure>> structures_;  // ascending id
  i64 solve_from_ = 0;                       // (the first structure the last tick's budget did not reach: first now)
  std::vector<GVox> seeds_;                  // voxels whose structures must be (re)extracted
  i64 fresh_from_ = INT64_MAX;               // (during a refresh: the first id made in it)
  // (both keep the chunk of the node they belong to: dropped with it when it leaves)
  struct WarmStart {
    u64 chunk = 0;
    std::array<f32, 6> u{};
  };
  struct Judged {
    u64 chunk = 0;
    BondLoad load;
  };
  std::unordered_map<u64, WarmStart> warm_u_;  // fragment identity -> last displacement
  std::unordered_map<u64, Judged> judged_;     // bond identity -> load at its last judged state
  u64 node_chunk(const Structure& s, i32 node) const;
  bool designed_all_ = false;
  bool in_tick_ = false;  // (a system calling tick / load from inside a tick is refused)
  bool busy_ = false;     // (the last tick was busy: RigidParams::busy_hold)
  // (the systems' part of a tick - pre_step, step: the articulations are theirs to add, remove and
  // move then; the mechanics' part is not)
  bool systems_phase_ = false;
  struct DeadLoad {
    GVox vox;
    V3 p, F;
  };
  std::unordered_map<i64, std::vector<DeadLoad>> dead_loads_;  // sleeping body -> its resting forces
  struct BlastLoad {
    i32 first;                               // the fragment's identity (grid, chunk, first voxel)
    u64 chunk;
    u16 grid;
    V3 F, at;                                // (at: the fragment's centre of mass)
    V3 J;                                    // the momentum the blast gives the fragment (N s)
  };
  std::vector<BlastLoad> blast_loads_;
  i32 crack_budget_ = 0;
  i32 impact_budget_ = 0;
  bool rollback_ = false;  // (fracture hook) a part of some size came apart: the contact step is solved again
  i32 ensuring_ = 0;  // (nesting of first-touch chunk generation)

  RigidWorld rigid_;
  std::vector<StaticGrid> statics_;          // (the rigid bodies' static world of this tick)

  // streaming
  u64 region_of(u64 chunk_key) const;
  // The chunks about a piece's box and their neighbours: what it may touch next (it keeps them
  // resident while it moves, is archived with them when it sleeps).
  void body_chunks(const Body& b, const std::function<void(u64)>& f) const;
  void unload_sleepers(const std::vector<u64>& chunks,
                       const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of);
  // Keeps a chunk's changes (the archive; a full bounded archive forgets old regions first).
  void archive_record(u64 key, const std::vector<u8>& rec, u64 region);
  bool forget_regions(size_t need, u64 keep);  // (need bytes free; `keep`: never this region)
  void forget_region(u64 region);
  void forget_stale_regions();

  // extensions
  void refresh_strengths(const std::vector<GVox>& voxels);  // (their damage changed)
  i32 world_set_layer(u16 g, int L, const std::vector<LayerEdit>& edits);
  void finish_tick_changes();                              // voxel changes: to the systems and the host
  void step_systems(bool step = true);  // (notifications; and the steps unless paused)
  void add_external_loads();                               // (finish_loads)
  void add_loads_to(const Structure& s, std::vector<f64>& F) const;  // (design solves)

  // memory budgets (MemoryBudget)
  void enforce_budgets();
  void trim_fragment_caches();
  void trim_structures();
  void trim_output();
  i64 structure_bytes(const Structure& s, Bytes kind = Bytes::Held) const;
};

}  // namespace svx
