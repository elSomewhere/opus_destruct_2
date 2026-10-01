// svx_city — voxel_city sites/kit.js (box, finishStructure) and the per-structure part of
// world/sites.js's siteSource.rasterize.
#include "sites/kit.hpp"

#include <algorithm>

#include "voxel/chunk.hpp"

namespace svx::city {

SiteStructure finish_structure(BoxLists lists) {
  SiteStructure st;
  st.boxes.reserve(lists.shells.size() + lists.carves.size() + lists.details.size());
  st.boxes.insert(st.boxes.end(), lists.shells.begin(), lists.shells.end());
  st.boxes.insert(st.boxes.end(), lists.carves.begin(), lists.carves.end());
  st.boxes.insert(st.boxes.end(), lists.details.begin(), lists.details.end());
  st.custom = std::move(lists.custom);
  auto grow = [&](double x0, double y0, double z0, double x1, double y1, double z1) {
    if (st.bb) {
      Box3& b = *st.bb;
      b = {js::min(b.x0, x0), js::min(b.y0, y0), js::min(b.z0, z0), js::max(b.x1, x1), js::max(b.y1, y1), js::max(b.z1, z1)};
    } else {
      st.bb = Box3{x0, y0, z0, x1, y1, z1};
    }
  };
  for (size_t i = 0; i < st.boxes.size(); ++i) {
    const SiteBox& q = st.boxes[i];
    st.grid.insert(static_cast<uint32_t>(i), Rect{q.x0, q.y0, q.x1, q.y1});
    grow(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1);
  }
  for (const SiteVolume& c : st.custom) grow(c.bb.x0, c.bb.y0, c.bb.z0, c.bb.x1, c.bb.y1, c.bb.z1);
  return st;
}

void rasterize_structure(const SiteStructure& st, ChunkBuffer& chunk, double deep) {
  const Box3 box = chunk.world_box();
  for (const SiteVolume& c : st.custom) {
    const Box3& b = c.bb;
    if (b.x1 < box.x0 || b.x0 > box.x1 || b.y1 < box.y0 || b.y0 > box.y1 || b.z1 < box.z0 || b.z0 > box.z1) continue;
    c.rasterize(chunk);
  }
  std::vector<uint32_t> qs = st.grid.query(Rect{box.x0, box.y0, box.x1, box.y1});
  // (JS sorts the boxes by their index: distinct integers, so any sort gives this order)
  std::sort(qs.begin(), qs.end());
  for (const uint32_t i : qs) {
    const SiteBox& q = st.boxes[i];
    if (q.z1 < box.z0 || q.z0 > box.z1 || q.z1 < deep) continue;
    chunk.fill_box(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.mode);
  }
}

}  // namespace svx::city
