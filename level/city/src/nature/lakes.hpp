// svx_city — lakes (voxel_city nature/lakes.js): natural basins on a jittered lattice (one
// candidate per ~3.5 km cell); a few are big lakes several kilometres across (towns on their
// shores get a port). A lake has an irregular shore (noise on the radius), a bowl-shaped bed and a
// water level just below the lowest point of its rim, so water never stands above the surrounding
// land. Lakes form in lowlands and in the mountains alike (alpine lakes) but keep out of towns.
//
// Like rivers they are pure functions of position that only the ground tile carves; planning
// keeps lots and sites off them with hits_rect.
//
// A lake is made once per lattice cell and kept in a cache (core/cache.hpp: any thread, the first
// to ask makes it); a lake is a pure function of its cell (the terrain samples it reads are pure:
// terrain/terrain.hpp), so the cache only saves work. A settlement's port lake is cached on its
// record (Settlement::port_lake). (The reference's at() returns one shared object that the next
// call overwrites; here a value, which is what its callers read.)
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/noise.hpp"
#include "core/rect.hpp"
#include "world/wrap.hpp"

namespace svx::city {

class World;
struct Settlement;

// A lake: id "L{a}_{b}" (its canonical cell's; a wrapping world's lake keeps it on every lap), its
// centre (voxels) and its canonical centre (cx, cy: the shore noise follows it, so every lap has
// the same shore), the radius r0 (voxels), the water level and the depth (voxels), whether it is
// a big lake, the id of the town whose port it is ("": none, JS's null) and its reach (the
// radius beyond which it touches nothing).
struct Lake {
  std::string id;
  double x = 0, y = 0, cx = 0, cy = 0;
  double r0 = 0, level = 0, depth = 0;
  bool big = false;
  std::string port;
  double reach = 0;
};

// Lakes.at's record: the water level, the voxel z of the bottom, the normalized shore distance k
// (< 1 in the water) and the lake.
struct LakeInfo {
  double level = 0, bed = 0, k = 0;
  std::shared_ptr<const Lake> lake;
};

// Lakes.shoreNear's record: a big lake, the distance (voxels) to its water's edge and the unit
// vector from the lake's centre.
struct LakeShore {
  std::shared_ptr<const Lake> lake;
  double dist = 0, nx = 0, ny = 0;
};

class Lakes {
 public:
  // cache_capacity: the lattice cells kept (a few hundred bytes each; the reference keeps 512;
  // results never depend on it).
  explicit Lakes(const World& world, size_t cache_capacity = 8192);
  Lakes(const Lakes&) = delete;
  Lakes& operator=(const Lakes&) = delete;

  const World* world;
  // config.lakes (read once): enabled (truthiness), chance, bigChance, scale ?? 1,
  // townProximity ?? 0.2
  bool enabled = false;
  double chance = 0, big_chance = 0, scale = 1, town_proximity = 0.2;
  // the lattice cell (voxels); a wrapping world has n cells round it (0: unbounded)
  double cell = 0;
  Wrap wrap;
  double n = 0;
  SimplexNoise shore;

  // The lake of lattice cell (a, b), or null.
  std::shared_ptr<const Lake> lake(double a, double b) const;
  // The lake of cell (a, b), made (lake() caches it).
  std::shared_ptr<const Lake> build(double a, double b) const;
  // The big lake a town was built against (its port), or null. Cached on the settlement.
  std::shared_ptr<const Lake> port_lake_of(const Settlement& s) const;
  // Lakes whose reach overlaps a rect (voxels), in lattice order.
  std::vector<std::shared_ptr<const Lake>> near(const Rect& r) const;
  // Normalized shore distance k (< 1 inside the water) of a point for lake L.
  double shore_k(const Lake& L, double x, double y, bool smooth = false) const;
  // Lake info at (x, y), or nothing.
  std::optional<LakeInfo> at(double x, double y) const;
  // Big-lake shore near a land point (within max_dist voxels of its edge), or nothing. Towns build
  // their port where it is close.
  std::optional<LakeShore> shore_near(double x, double y, double max_dist) const;
  // Carved ground (voxels) for a column whose surface is h.
  double ground_at(const LakeInfo& info, double h) const;
  // Does a lake (plus a margin in m) touch a rect (voxels)?
  bool hits_rect(const Rect& r, double margin_m = 6) const;

 private:
  // (JS's key `${a},${b}`: the exact cell, -0 as 0)
  struct Cell {
    double a, b;
    bool operator==(const Cell& o) const { return a == o.a && b == o.b; }
  };
  struct CellHash {
    size_t operator()(const Cell& c) const;
  };
  mutable MemoCache<Cell, Lake, CellHash> cache_;
};

}  // namespace svx::city
