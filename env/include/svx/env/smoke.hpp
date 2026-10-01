// structvox env — smoke: a coarse, sparse density field that rises, spreads under ceilings,
// drifts with the wind and thins out (docs/ENV.md).
//
// A WorldSystem. The field lives in cells of kCell^3 voxels (0.5 m at the usual voxel size),
// grouped in blocks of one chunk each, only where there is smoke: memory follows the smoke,
// and a budget (max_blocks) bounds it. Solids block it: a cell mostly solid, or holding a wall
// (a voxel plane across it nearly all solid), holds none and passes none. Every step (0.1 s) each cell sends part of its smoke up (buoyancy), down the
// wind, and to thinner open neighbours (diffusion); smoke under a ceiling therefore spreads
// sideways and fills the room from the top down. It dissipates with a lifetime. Sources: the
// flames of a FireSystem it follows, and emit() (blasts, dust). Smoke is transient (not
// saved) and leaves with evicted chunks.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "svx/world/world.hpp"

namespace svx {

class FireSystem;

struct SmokeConfig {
  bool enabled = true;        // (off: nothing steps; the field stays as it is)
  f64 step_s = 0.1;           // (at least the world's tick, at most 0.5 s)
  f64 rise = 0.8;             // m/s: buoyant rise
  f64 diffuse = 0.6;          // 1/s: exchange with thinner open neighbours
  f64 lifetime = 20.0;        // s: dissipation (e-folding)
  V3 wind{0.0, 0.0, 0.0};     // m/s
  f64 per_flame = 0.12;       // density per second a flame gives off
  f64 ceiling_m = 24.0;       // smoke above the world's top by more than this is gone
  i32 max_blocks = 1024;      // budget (a block: one chunk's cells, 4 KB): beyond, the thinnest go
};

class SmokeSystem final : public WorldSystem {
 public:
  static constexpr i32 kCell = 4;                    // voxels per cell edge
  static constexpr i32 kSide = kChunk / kCell;       // cells per block edge
  static constexpr i32 kCells = kSide * kSide * kSide;

  static constexpr f64 kMaxEmitRadius = 8.0;  // m
  static constexpr size_t kMaxPending = 1024;  // emits between steps

  explicit SmokeSystem(const SmokeConfig& c = {});
  void configure(const SmokeConfig& c);  // (values are brought into their ranges)
  const SmokeConfig& config() const { return cfg_; }

  // Sources: the flames of fire (nullptr: none), and smoke added at a point or over a sphere
  // (at most kMaxEmitRadius; taking effect at the next step).
  void follow(std::shared_ptr<const FireSystem> fire) { fire_ = std::move(fire); }
  void emit(const V3& pos, f64 amount);
  void emit_sphere(const V3& pos, f64 radius, f64 amount);

  // The density at a point (0: clear; about 1: thick).
  f64 density(const World& w, const V3& pos) const;

  // Output: the cells with at least min_density (centre, density), at most max of them (the
  // densest).
  struct Cell {
    V3 pos;
    f32 density = 0.0f;
  };
  std::vector<Cell> cells(const World& w, i32 max, f32 min_density = 0.02f) const;
  struct Stats {
    i32 blocks = 0, cells = 0;
    i64 dropped = 0;  // blocks let go by the budget (total)
    i64 steps = 0;
    f64 step_ms = 0.0;   // (this tick)
    f64 total_ms = 0.0;  // (since the load)
  };
  const Stats& stats() const { return st_; }

  // WorldSystem
  const char* name() const override { return "smoke"; }
  void on_load(World& w) override;
  void on_evicted(World& w, const std::vector<u64>& chunks) override;
  void on_voxels_changed(World& w, const std::vector<u64>& chunks) override;
  void step(World& w, f64 dt) override;
  i64 memory_bytes() const override;
  u64 state_hash() const override;

 private:
  struct Block {
    std::array<f32, kCells> d{};
    std::array<f32, kCells> next{};
  };
  struct Solid {  // a chunk's cells that are mostly solid (1), or not resident (all 1)
    std::array<u8, kCells> s{};
    bool stale = true;
    u64 grids = 0;  // (the oriented grids' solids it was made with: World::grid_solids_stamp)
  };
  void smoke_step(World& w, f64 dt);
  const Solid& solid(const World& w, const IVec3& chunk);  // (valid until the step ends: the cache is pruned after it)
  Block* block(const IVec3& chunk, bool create);
  void add(World& w, const IVec3& cell, f64 amount);
  void add_at(World& w, const V3& pos, f64 amount);  // (into the cell at pos, or an open one next to it)
  void prune_solid();

  SmokeConfig cfg_;
  std::shared_ptr<const FireSystem> fire_;
  std::map<u64, Block> blocks_;  // chunk key -> block (key order: deterministic)
  std::unordered_map<u64, Solid> solid_;  // (a cache of the grid: pruned to the blocks' neighbourhood)
  struct Emit {
    V3 pos;
    f64 radius = 0.0, amount = 0.0;
  };
  std::vector<Emit> pending_;
  f64 clock_ = 0.0;
  Stats st_;
};

}  // namespace svx
