// structvox env — water: a voxel fluid that falls, spreads and settles, presses on structures,
// floats pieces and puts out fires (docs/ENV.md).
//
// A WorldSystem. The water is a persistent voxel layer, "water": how full an air voxel is
// (1 .. 255 full). So it streams and is saved like the voxels, and the fire system sees it (a
// wet voxel does not burn). Only water that moves is stepped (the active set): it falls (up to
// `fall` voxels a step), and water on a floor or on water spreads towards lower neighbours;
// water that did not change rests until something changes near it (a wall shot away, a voxel
// placed, water arriving). Thin films do not spread (min_spread) and very thin ones dry up.
// Levels and sources place water at rest (ChunkSource::generate_layer("water")).
// Its interactions, all through the core's extension points:
//   - loads: the hydrostatic pressure of the water on the structures holding it (walls,
//     floors: free voxels), as World::set_loads (a dam or a tank breaks when it cannot hold);
//   - pieces: buoyancy and drag (World::apply_force), woken when they would float; splashes;
//   - fire: through the layer (FireSystem quenches wet voxels).
// Pressure is not propagated (water does not rise in a U-tube). A budget (max_active) bounds
// the voxels a step moves (beyond it, water waits its turn); water at rest - full voxels held
// by solids and full water - is never stepped, even when woken.
#pragma once

#include <unordered_map>
#include <vector>

#include "svx/world/world.hpp"

namespace svx {

struct WaterConfig {
  bool enabled = true;        // (off: nothing flows; loads and buoyancy stay as they are)
  f64 step_s = 1.0 / 30.0;    // the fluid steps this often (at least the world's tick, at most 0.1 s)
  i32 fall = 4;               // voxels water falls in a step at most
  u8 min_spread = 16;         // thinner water does not spread (a puddle) ...
  u8 min_amount = 3;          // ... and thinner still dries up
  i32 max_active = 60000;     // voxels stepped at most per step (beyond: they wait their turn)
  f64 density = 1000.0;       // kg/m^3
  bool loads = true;          // hydrostatic loads on structures
  f64 load_s = 0.5;           // s: loads updated at most this often
  i32 max_loads = 65536;      // (the largest)
  u64 load_group = 0x5741544552ull;  // World::set_loads group ("WATER")
  bool buoyancy = true;       // buoyancy and drag on pieces
  f64 drag = 1.5;             // 1/s: drag on the submerged part of a piece (of its velocity)
  i32 samples = 256;          // voxels of a piece sampled for buoyancy at most
};

class WaterSystem final : public WorldSystem {
 public:
  static constexpr i32 kMaxReach = 32;  // voxels: the largest sphere a command acts on

  explicit WaterSystem(const WaterConfig& c = {});
  void configure(const WaterConfig& c);  // (values are brought into their ranges)
  const WaterConfig& config() const { return cfg_; }
  bool ok() const { return water_ >= 0; }  // (attached: its layer registered)

  // Commands (taking effect at once; the world must have attached the system).
  void pour(World& w, const V3& pos, f64 radius);   // fills the air in the sphere
  void drain(World& w, const V3& pos, f64 radius);  // removes the water in the sphere
  u8 amount(const World& w, const IVec3& voxel) const;
  int water_layer() const { return water_; }

  // Output: pieces that hit the water hard in the last tick (for effects).
  struct Splash {
    V3 pos;
    f32 strength = 0.0f;  // kg m/s (the piece's momentum into the water)
  };
  const std::vector<Splash>& splashes() const { return splashes_; }
  struct Stats {
    i32 active = 0, moved = 0, loads = 0, floating = 0;  // (of the last step)
    i64 steps = 0;
    f64 step_ms = 0.0;   // (this tick)
    f64 total_ms = 0.0;  // (since the load)
  };
  const Stats& stats() const { return st_; }

  // WorldSystem
  const char* name() const override { return "water"; }
  void attach(World& w) override;
  void on_load(World& w) override;
  void on_generated(World& w, const std::vector<u64>& chunks) override;
  void on_evicted(World& w, const std::vector<u64>& chunks) override;
  void on_voxels_changed(World& w, const std::vector<u64>& chunks) override;
  void step(World& w, f64 dt) override;
  i64 memory_bytes() const override;
  u64 state_hash() const override;

 private:
  void flow_step(World& w);
  void update_loads(World& w);
  void chunk_loads(const World& w, const IVec3& chunk, std::vector<VoxelLoad>& out) const;
  void float_pieces(World& w);
  void wake_chunk(const World& w, u64 chunk);  // (its water that can move)
  void loads_dirty(const World& w, u64 chunk);  // (its loads and those of the water columns below it)

  WaterConfig cfg_;
  int water_ = -1;
  std::vector<u64> active_;                                  // voxel keys (sorted)
  std::vector<u64> wake_;                                    // to add to the active set
  std::vector<u64> load_dirty_;                              // chunks whose loads are stale
  bool loads_all_ = true;                                    // (all of them: after a load)
  bool loads_changed_ = false;                               // (chunks with loads left)
  std::unordered_map<u64, std::vector<VoxelLoad>> loads_;    // chunk -> its loads
  std::unordered_map<i64, u8> wet_;                          // pieces in the water last tick
  struct Samples {
    i32 count = -1;           // (the shape's voxel count they were taken for)
    std::vector<i32> cells;   // shape cells sampled for buoyancy
  };
  std::unordered_map<i64, Samples> samples_;                 // per piece
  u64 loads_group_ = 0;                                      // (the group its loads are in the world under)
  bool loads_set_ = false;
  std::vector<Splash> splashes_;
  f64 clock_ = 0.0, load_clock_ = 0.0;
  Stats st_;
};

}  // namespace svx
