// structvox env — fire: burning, heat, and what they do to structures (docs/ENV.md).
//
// A WorldSystem (svx/world/world.hpp): it keeps its state in voxel layers, so fire burns in the
// static world and on pieces alike, streams, and is saved where it matters:
//   "heat" (transient): a voxel's temperature above ambient, 4 degC per unit;
//   "burn" (persistent): how much of a combustible voxel has burnt (1 .. 255).
// Every step (0.1 s by default) the hot voxels exchange heat with their solid neighbours
// (conduction), burning ones heat the solids in their flame (up the wall above, less to the
// sides, hardly below) and cool slowly otherwise. A combustible voxel hotter than its (slightly
// jittered) ignition temperature burns: it holds its flame temperature, chars (the core's damage
// layer takes the strength of its sections as it goes) and, burnt out, is gone - structures lose
// members and fall. Metals weaken with temperature (steel: from 400 degC, nothing left at
// 1000 degC; permanent). Water in or next to a voxel quenches it.
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
  f64 gone_c = 0.0;        // ... all of it at this (permanent damage)
};

struct FireConfig {
  f64 step_s = 0.1;        // fire steps this often (a multiple of the world's tick)
  f64 flame_reach = 0.15;  // 1/s: how fast a flame heats the solid right above it (the others in its reach: less)
  i32 max_hot = 200000;    // voxels tracked at once: beyond, the coolest are let go (budget)
  f64 ignition_jitter = 0.15;  // ignition temperatures vary per voxel by this fraction (ragged fronts) ...
  f64 burn_jitter = 0.35;      // ... and burn times by this
  f64 quench_c = 60.0;     // degC water brings a voxel down to
};

class FireSystem final : public WorldSystem {
 public:
  explicit FireSystem(const FireConfig& c = {});
  void configure(const FireConfig& c) { cfg_ = c; }
  const FireConfig& config() const { return cfg_; }
  // Per-material properties (defaults for the standard materials: wood burns, steel weakens).
  void set_material(MaterialId id, const FireMaterial& m) { mats_[static_cast<size_t>(id) & 0x7F] = m; }
  const FireMaterial& fire_material(MaterialId id) const { return mats_[static_cast<size_t>(id) & 0x7F]; }

  // Commands (taking effect at once; the world must have attached the system).
  void ignite(World& w, const V3& pos, f64 radius);             // combustibles in the sphere catch fire, the rest heats up
  void heat(World& w, const V3& pos, f64 radius, f64 celsius);  // brings solids in the sphere to (at least) this
  void extinguish(World& w, const V3& pos, f64 radius);         // cools the sphere (a fire extinguisher)

  // Output: the voxels burning at the last step (world positions: flames, embers, smoke sources).
  struct Flame {
    V3 pos;
    f32 heat = 0.0f;  // degC
    i64 piece = 0;    // 0: the static world
  };
  const std::vector<Flame>& flames() const { return flames_; }
  struct Stats {
    i32 hot = 0, burning = 0, burning_pieces = 0;
    i64 burnt_out = 0, ignited = 0, dropped = 0;  // (totals)
    f64 step_ms = 0.0;
  };
  const Stats& stats() const { return st_; }
  int heat_layer() const { return heat_; }
  int burn_layer() const { return burn_; }

  // WorldSystem
  const char* name() const override { return "fire"; }
  void attach(World& w) override;
  void on_load(World& w) override;
  void on_evicted(World& w, const std::vector<u64>& chunks) override;
  void step(World& w, f64 dt) override;
  i64 memory_bytes() const override;
  u64 state_hash() const override;

 private:
  void fire_step(World& w, f64 dt);
  void step_pieces(World& w, f64 dt, std::vector<V3>& heat_world);
  f64 ignition(const FireMaterial& m, u64 key) const;
  f64 burn_time(const FireMaterial& m, u64 key) const;
  bool wet(const World& w, const IVec3& p) const;
  u8 dither(f64 v, u64 key) const;  // v >= 0 rounded up with probability of its fraction (deterministic)

  FireConfig cfg_;
  std::array<FireMaterial, 128> mats_{};
  int heat_ = -1, burn_ = -1, water_ = -2;  // layers (water_: -2 = not looked up yet)
  std::vector<u64> hot_;                    // voxel keys with heat (sorted)
  std::vector<Flame> flames_;
  f64 clock_ = 0.0;
  i64 steps_ = 0;
  Stats st_;
};

}  // namespace svx
