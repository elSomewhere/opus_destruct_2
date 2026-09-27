// structvox env — fire: burning, heat, and what they do to structures (docs/ENV.md).
//
// A WorldSystem (svx/world/world.hpp): it keeps its state in voxel layers, so fire burns in the
// static world and on pieces alike, streams, and is saved where it matters:
//   "heat" (transient, bound to the solid voxel): its temperature above ambient, kHeatUnit degC
//     per unit (up to 255 units);
//   "burn" (persistent, bound to the solid voxel): how much of a combustible voxel has burnt
//     (1 .. 255).
// Every step (0.1 s by default) the hot voxels exchange heat with their solid neighbours
// (conduction), burning ones heat the solids in their flame (up the wall above, less to the
// sides, hardly below) and cool slowly otherwise. A combustible voxel hotter than its (slightly
// jittered) ignition temperature burns: it holds its flame temperature, chars (the core's damage
// layer takes the strength of its sections as it goes) and, burnt out, is gone - structures lose
// members and fall. Metals and mineral materials weaken with temperature (steel: from 400 degC,
// nothing left at 1000 degC; permanent). Water in or next to a voxel quenches it.
// Work is bounded: at most max_hot voxels are tracked (the coolest, never burning ones first, go
// beyond), commands admit at most that many, and a command's sphere at most kMaxReach voxels.
#pragma once

#include <array>
#include <vector>

#include "svx/world/world.hpp"

namespace svx {

// How a material takes fire and heat.
struct FireMaterial {
  bool combustible = false;
  f64 ignition_c = 300.0;  // degC: a combustible voxel ignites above this
  f64 burn_s = 40.0;       // s a voxel burns before it is gone
  f64 flame_c = 900.0;     // degC a burning voxel keeps
  f64 conduct = 0.05;      // heat exchange with solid neighbours (1/s of the difference)
  f64 cool = 0.05;         // heat lost to the air (1/s of the excess over ambient)
  f64 char_damage = 0.9;   // damage of a combustible voxel burnt out (0..1): charring as it burns
  f64 weaken_c = 0.0;      // degC from which it loses strength (0: never) ...
  f64 gone_c = 0.0;        // ... all of it at this (permanent damage; heat tops out at 1020 degC)
};

struct FireConfig {
  bool enabled = true;         // (off: nothing steps; commands do nothing)
  f64 step_s = 0.1;            // fire steps this often (at least the world's tick, at most 0.25 s)
  f64 flame_reach = 0.15;      // 1/s: how fast a flame heats the solid right above it (the others in its reach: less)
  i32 max_hot = 40000;         // voxels tracked at once (budget: beyond, the coolest non-burning ones go)
  f64 ignition_jitter = 0.15;  // ignition temperatures vary per voxel by this fraction (ragged fronts) ...
  f64 burn_jitter = 0.35;      // ... and burn times by this
  u64 seed = 0;                // (of those variations)
  f64 quench_c = 60.0;         // degC water brings a voxel down to
  i32 damage_quantum = 8;      // damage is written when it rises by this much (1..64; less: re-judged more often)
  i32 piece_batch_steps = 10;  // steps between burnt-out voxels leaving a piece (each batch re-announces its parts)
  f64 glow_c = 520.0;          // degC from which a voxel glows (reported for renderers: take_glow_changes)
};

class FireSystem final : public WorldSystem {
 public:
  static constexpr f64 kHeatUnit = 4.0;  // degC per heat unit
  static constexpr i32 kMaxReach = 48;   // voxels: the largest sphere a command acts on

  explicit FireSystem(const FireConfig& c = {});
  void configure(const FireConfig& c);  // (values are brought into their ranges)
  const FireConfig& config() const { return cfg_; }
  // Per-material properties (defaults for the standard materials: wood burns, steel weakens).
  void set_material(MaterialId id, const FireMaterial& m);
  const FireMaterial& fire_material(MaterialId id) const { return mats_[static_cast<size_t>(id) & 0x7F]; }
  bool ok() const { return heat_ >= 0 && burn_ >= 0; }  // (attached: its layers registered)

  // Commands (taking effect at once; the world must have attached the system).
  void ignite(World& w, const V3& pos, f64 radius);             // combustibles in the sphere catch fire, the rest heats up
  void heat(World& w, const V3& pos, f64 radius, f64 celsius);  // brings solids in the sphere to (at least) this
  void extinguish(World& w, const V3& pos, f64 radius);         // cools the sphere (a fire extinguisher)

  // Queries.
  f64 temperature(const World& w, const IVec3& voxel) const;  // degC above ambient (world voxels)
  f64 burnt(const World& w, const IVec3& voxel) const;        // 0 .. 1

  // Output: the voxels burning at the last step (world positions: flames, embers, smoke sources).
  struct Flame {
    V3 pos;
    f32 heat = 0.0f;  // degC
    i64 piece = 0;    // 0: the static world
  };
  const std::vector<Flame>& flames() const { return flames_; }
  // For renderers: chunks where a voxel began or stopped glowing, and pieces whose charring or
  // glow changed, since the last take.
  std::vector<u64> take_glow_changes();
  std::vector<i64> take_piece_changes();
  struct Stats {
    i32 hot = 0, burning = 0, burning_pieces = 0;
    i64 burnt_out = 0, ignited = 0, dropped = 0;  // (totals)
    i64 steps = 0;
    f64 step_ms = 0.0;   // (this tick)
    f64 total_ms = 0.0;  // (since the load)
  };
  const Stats& stats() const { return st_; }
  int heat_layer() const { return heat_; }
  int burn_layer() const { return burn_; }

  // WorldSystem
  const char* name() const override { return "fire"; }
  void attach(World& w) override;
  void on_load(World& w) override;
  void on_generated(World& w, const std::vector<u64>& chunks) override;
  void on_evicted(World& w, const std::vector<u64>& chunks) override;
  void step(World& w, f64 dt) override;
  i64 memory_bytes() const override;
  u64 state_hash() const override;

 private:
  void fire_step(World& w, f64 dt);
  void step_pieces(World& w, f64 dt, std::vector<V3>& heat_world);
  void track_heat(const World& w, const std::vector<u64>& chunks);  // (heat already in these chunks)
  void admit(std::vector<std::pair<f64, u64>>& cand);               // (a command's voxels, nearest first, within the budget)
  void heat_sphere(World& w, const V3& pos, f64 radius, f64 celsius, bool own);
  f64 ignition(const FireMaterial& m, u64 key) const;
  f64 burn_time(const FireMaterial& m, u64 key) const;
  u8 dither(f64 v, u64 key) const;  // v >= 0 rounded up with probability of its fraction (deterministic)
  u8 glow_units() const;

  FireConfig cfg_;
  std::array<FireMaterial, 128> mats_{};
  int heat_ = -1, burn_ = -1, water_ = -1;  // layers
  std::vector<u64> hot_;                    // voxel keys with heat (sorted)
  std::vector<Flame> flames_;
  std::vector<u64> glow_changes_;
  std::vector<i64> piece_changes_;
  f64 clock_ = 0.0;
  i64 steps_ = 0;
  Stats st_;
};

}  // namespace svx
