// structvox procgen — a district of the reference city (voxel_city), dumped through its structvox
// export by tools/procgen_ref/district.mjs: a bounded testbed of the merge (docs/CITY.md). Its
// chunks, layers (look, flora, water), props, column content ranges, regions, oriented grids,
// lanes and highway touch points, behind a GameSource.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "svx/game/source.hpp"
#include "svx/material/material.hpp"
#include "svx/world/world.hpp"

namespace svx {

struct District {
  f64 h = 0.125;
  // the city's own materials: register them (after the game's) and they must get these ids
  struct Mat {
    i32 id = 0;
    Material m;
  };
  std::vector<Mat> mats;
  IVec3 lo{0, 0, 0}, hi{0, 0, 0};  // extent in chunks [lo, hi)
  V3 focus;                       // on the ground at the district's centre (m)
  struct Column {
    i32 cx = 0, cy = 0, z_lo = 0, z_hi = 0;  // its content (chunks, inclusive)
    u64 region = 0;
  };
  std::vector<Column> columns;
  struct Prop {
    u32 index = 0;  // in its chunk (Chunk::v order)
    Vox vox = kAir;
    u8 look = 0;
  };
  struct ChunkRec {
    std::vector<Vox> vox;  // kChunkVox
    std::vector<u8> look, flora, water;  // kChunkVox each, or empty
    std::vector<Prop> props;             // the export's isolated voxels (furniture, street props)
  };
  std::map<u64, ChunkRec> chunks;  // by key3
  struct GridChunk {
    IVec3 c{0, 0, 0};
    std::vector<Vox> vox;
    std::vector<u8> look, flora;
  };
  struct Grid {
    SourceGrid g;
    IVec3 home{0, 0, 0};
    bool anchored = false;
    std::string kind;
    std::vector<GridChunk> chunks;
  };
  std::vector<Grid> grids;
  std::vector<std::pair<V3, V3>> lanes;
  std::vector<V3> touches;
  // the export's physics classes (id -> name, and the city materials behind its looks) and flora
  struct Class {
    i32 id = 0;
    std::string name;
    std::vector<std::string> looks;
  };
  std::vector<Class> classes;
  std::vector<std::string> flora;
};

// Reads a district file (false: missing, malformed or another format).
bool read_district(const std::string& path, District* out);

// How a district's props and plants come into the world.
struct DistrictOptions {
  enum class Props : u8 {
    None,       // left out
    Structure,  // in the chunk's voxels, bonded like any structure
    Isolated,   // left out of the chunk: the host writes them (district_props) with kEditIsolated
  };
  Props props = Props::Structure;
  bool flora = false;  // plants as solid voxels of the foliage class (else air)
};

class DistrictSource : public GameSource {
 public:
  DistrictSource(std::shared_ptr<const District> d, DistrictOptions o = {});
  bool generate(const IVec3& chunk, std::vector<Vox>& out) const override;
  IVec3 chunk_lo() const override { return d_->lo; }
  IVec3 chunk_hi() const override { return d_->hi; }
  u64 region(const IVec3& chunk) const override;
  bool generate_layer(const IVec3& chunk, const std::string& layer, std::vector<u8>& out) const override;
  std::vector<SourceGrid> grids(const IVec3& chunk) const override;
  bool generate_grid(u32 id, VoxelGrid& out) const override;
  V3 spawn_pos() const override { return d_->focus; }
  V3 spawn_dir() const override { return V3{1, 0, 0}; }
  const District& district() const { return *d_; }
  const DistrictOptions& options() const { return o_; }

 private:
  std::shared_ptr<const District> d_;
  DistrictOptions o_;
  std::map<u64, std::vector<size_t>> by_home_;
  std::map<u64, size_t> columns_;
  i32 foliage_ = -1;  // the foliage class (flora as solid voxels)
};

// The isolated props of a resident chunk (Props::Isolated: the host writes them).
std::vector<VoxelEdit> district_props(const District& d, const IVec3& chunk);

}  // namespace svx
