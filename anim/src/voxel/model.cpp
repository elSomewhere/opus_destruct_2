#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

VoxelModel::VoxelModel(SkeletonPtr sk, f64 s, std::vector<VoxelPart> ps, std::string nm)
    : skeleton(std::move(sk)), voxel_size(s), parts(std::move(ps)), name(std::move(nm)) {
  part_of_bone.assign(size_t(skeleton->count), -1);
  for (size_t i = 0; i < parts.size(); ++i) {
    const i32 b = parts[i].bone;
    if (b >= 0 && b < skeleton->count && part_of_bone[size_t(b)] < 0) part_of_bone[size_t(b)] = static_cast<i16>(i);
  }
}

std::shared_ptr<VoxelModel> VoxelModel::clone() const { return std::make_shared<VoxelModel>(skeleton, voxel_size, parts, name); }

i32 VoxelModel::voxel_count() const {
  i32 n = 0;
  for (const VoxelPart& p : parts) n += p.count;
  return n;
}

void part_bounds(const VoxelPart& p, f64 s, V3* lo, V3* hi) {
  *lo = V3{p.origin[0] * s, p.origin[1] * s, p.origin[2] * s};
  *hi = V3{(p.origin[0] + p.dims[0]) * s, (p.origin[1] + p.dims[1]) * s, (p.origin[2] + p.dims[2]) * s};
}

VoxelPart shrink_part(const VoxelPart& p) {
  const i32 nx = p.dims[0], ny = p.dims[1], nz = p.dims[2];
  i32 x0 = nx, y0 = ny, z0 = nz, x1 = -1, y1 = -1, z1 = -1;
  for (i32 z = 0; z < nz; ++z)
    for (i32 y = 0; y < ny; ++y)
      for (i32 x = 0; x < nx; ++x) {
        if (p.cells[size_t(x + nx * (y + ny * z))] == 0) continue;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        z0 = std::min(z0, z);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
        z1 = std::max(z1, z);
      }
  VoxelPart out;
  out.bone = p.bone;
  out.initial_count = p.initial_count;
  out.version = p.version + 1;
  if (x1 < 0) {
    out.origin = p.origin;
    out.version = p.version;
    return out;
  }
  out.origin = {p.origin[0] + x0, p.origin[1] + y0, p.origin[2] + z0};
  out.dims = {x1 - x0 + 1, y1 - y0 + 1, z1 - z0 + 1};
  const size_t n = size_t(out.dims[0]) * size_t(out.dims[1]) * size_t(out.dims[2]);
  out.cells.assign(n, 0);
  out.shade.assign(n, 0);
  for (i32 z = 0; z < out.dims[2]; ++z)
    for (i32 y = 0; y < out.dims[1]; ++y)
      for (i32 x = 0; x < out.dims[0]; ++x) {
        const size_t src = size_t(x + x0 + nx * (y + y0 + ny * (z + z0)));
        const size_t dst = size_t(x + out.dims[0] * (y + out.dims[1] * z));
        out.cells[dst] = p.cells[src];
        out.shade[dst] = p.shade[src];
        if (out.cells[dst] != 0) ++out.count;
      }
  return out;
}

}  // namespace svx::anim
