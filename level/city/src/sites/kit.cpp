// svx_city — voxel_city sites/kit.js and the per-structure part of world/sites.js's
// siteSource.rasterize.
#include "sites/kit.hpp"

#include <algorithm>

#include "city/cellNetwork.hpp"
#include "core/math.hpp"
#include "voxel/chunk.hpp"
#include "world/sites.hpp"

namespace svx::city {

SiteGate plan_gate(const World& world, const Site& site, double drive_half) {
  const Rect& r = site.rect;
  const Rect& cr = site.cell_rect;
  const double i = site.cell.i;
  const double j = site.cell.j;
  struct Side {
    char side;
    bool road;  // (e.cls: the edge has a road)
    double d;
  };
  std::vector<Side> sides = {
      {'N', !edge_info(world, 1, j, i).cls.empty(), r.y0 - cr.y0},
      {'S', !edge_info(world, 1, j + 1, i).cls.empty(), cr.y1 - r.y1},
      {'W', !edge_info(world, 0, i, j).cls.empty(), r.x0 - cr.x0},
      {'E', !edge_info(world, 0, i + 1, j).cls.empty(), cr.x1 - r.x1},
  };
  sides.erase(std::remove_if(sides.begin(), sides.end(), [](const Side& s) { return !s.road; }), sides.end());
  js::sort(sides, [](const Side& a, const Side& b) { return a.d - b.d; });
  SiteGate out;
  out.gate_side = sides.empty() ? 'S' : sides[0].side;
  const char g = out.gate_side;
  const double gw = vx(8);
  const double mid = g == 'N' || g == 'S' ? js::round((r.x0 + r.x1) / 2) : js::round((r.y0 + r.y1) / 2);
  if (g == 'N') {
    out.gate = {mid - gw / 2, r.y0, mid + gw / 2, r.y0};
    out.drive = {mid - drive_half, cr.y0, mid + drive_half, r.y0 + vx(12)};
  } else if (g == 'S') {
    out.gate = {mid - gw / 2, r.y1, mid + gw / 2, r.y1};
    out.drive = {mid - drive_half, r.y1 - vx(12), mid + drive_half, cr.y1};
  } else if (g == 'W') {
    out.gate = {r.x0, mid - gw / 2, r.x0, mid + gw / 2};
    out.drive = {cr.x0, mid - drive_half, r.x0 + vx(12), mid + drive_half};
  } else {
    out.gate = {r.x1, mid - gw / 2, r.x1, mid + gw / 2};
    out.drive = {r.x1 - vx(12), mid - drive_half, cr.x1, mid + drive_half};
  }
  return out;
}

void fence(std::vector<SiteBox>& out, const Rect& r, double z, char gate_side, const Rect& gate, double h, const FenceSkip& skip) {
  auto run = [&](double x0, double y0, double x1, double y1) {
    box(out, x0, y0, z + 1, x1, y1, z + h, MAT::CHAINLINK);
    box(out, x0, y0, z + h + 1, x1, y1, z + h + 2, MAT::STEEL_BEAM);
  };
  // (the side's run either side of the gate)
  auto split_at = [](double a0, double a1, double g0, double g1, const auto& mk) {
    if (g0 > a0) mk(a0, g0 - 1);
    if (g1 < a1) mk(g1 + 1, a1);
  };
  if (gate_side == 'N')
    split_at(r.x0, r.x1, gate.x0, gate.x1, [&](double a, double b) { run(a, r.y0, b, r.y0); });
  else if (!skip.N)
    run(r.x0, r.y0, r.x1, r.y0);
  if (gate_side == 'S')
    split_at(r.x0, r.x1, gate.x0, gate.x1, [&](double a, double b) { run(a, r.y1, b, r.y1); });
  else if (!skip.S)
    run(r.x0, r.y1, r.x1, r.y1);
  if (gate_side == 'W')
    split_at(r.y0, r.y1, gate.y0, gate.y1, [&](double a, double b) { run(r.x0, a, r.x0, b); });
  else if (!skip.W)
    run(r.x0, r.y0, r.x0, r.y1);
  if (gate_side == 'E')
    split_at(r.y0, r.y1, gate.y0, gate.y1, [&](double a, double b) { run(r.x1, a, r.x1, b); });
  else if (!skip.E)
    run(r.x1, r.y0, r.x1, r.y1);
  auto post = [&](double x, double y) { box(out, x, y, z + 1, x, y, z + h + 3, MAT::STEEL_BEAM); };
  for (double x = r.x0; x <= r.x1; x += 24) {
    if (!skip.N) post(x, r.y0);
    if (!skip.S) post(x, r.y1);
  }
  for (double y = r.y0; y <= r.y1; y += 24) {
    if (!skip.W) post(r.x0, y);
    if (!skip.E) post(r.x1, y);
  }
}

void gate_booth(std::vector<SiteBox>& out, const Rect& gate, char gate_side, double z) {
  const double gx = js::round((gate.x0 + gate.x1) / 2);
  const double gy = js::round((gate.y0 + gate.y1) / 2);
  // the unit vector from the gate into the site, and across it
  const double inward[2] = {gate_side == 'W' ? 1.0 : gate_side == 'E' ? -1.0 : 0.0, gate_side == 'N' ? 1.0 : gate_side == 'S' ? -1.0 : 0.0};
  const double perp[2] = {inward[1], inward[0]};
  const double bx = gx + inward[0] * vx(6) + perp[0] * vx(7);
  const double by = gy + inward[1] * vx(6) + perp[1] * vx(7);
  box(out, bx - 14, by - 14, z + 1, bx + 14, by + 14, z + 22, MAT::CONCRETE_LIGHT);
  box(out, bx - 12, by - 12, z + 2, bx + 12, by + 12, z + 20, 0);
  box(out, bx - 14, by - 14, z + 9, bx + 14, by + 14, z + 18, MAT::GLASS_TINT);
  box(out, bx - 12, by - 12, z + 9, bx + 12, by + 12, z + 18, 0);
  box(out, bx - 16, by - 16, z + 23, bx + 16, by + 16, z + 24, MAT::METAL_PANEL_DARK);
  box(out, bx - 4 * perp[0] - 3 * std::fabs(inward[0]), by - 4 * perp[1] - 3 * std::fabs(inward[1]), z + 2, bx - 4 * perp[0], by - 4 * perp[1], z + 17, 0);
  const double ax0 = gx + inward[0] * vx(3) - perp[0] * vx(4);
  const double ay0 = gy + inward[1] * vx(3) - perp[1] * vx(4);
  box(out, ax0, ay0, z + 1, ax0, ay0, z + 8, MAT::HAZARD_BLACK);
  box(out, ax0, ay0, z + 8, ax0 + perp[0] * vx(7), ay0 + perp[1] * vx(7), z + 8, MAT::HAZARD_YELLOW);
}

void watchtowers(std::vector<SiteBox>& out, const Rect& r, double z) {
  const double corners[4][2] = {{r.x0 + 6, r.y0 + 6}, {r.x1 - 6, r.y0 + 6}, {r.x0 + 6, r.y1 - 6}, {r.x1 - 6, r.y1 - 6}};
  const double legs[4][2] = {{-8, -8}, {8, -8}, {-8, 8}, {8, 8}};
  const double posts[4][2] = {{-12, -12}, {12, -12}, {-12, 12}, {12, 12}};
  for (const auto& c : corners) {
    const double cx = c[0];
    const double cy = c[1];
    for (const auto& d : legs) box(out, cx + d[0], cy + d[1], z + 1, cx + d[0], cy + d[1], z + 64, MAT::STEEL_BEAM);
    box(out, cx - 12, cy - 12, z + 64, cx + 12, cy + 12, z + 65, MAT::WOOD_DARK);
    box(out, cx - 12, cy - 12, z + 66, cx + 12, cy + 12, z + 74, MAT::WOOD_DARK);
    box(out, cx - 11, cy - 11, z + 66, cx + 11, cy + 11, z + 74, 0);
    box(out, cx - 12, cy - 12, z + 70, cx + 12, cy + 12, z + 73, 0);
    box(out, cx - 14, cy - 14, z + 82, cx + 14, cy + 14, z + 83, MAT::CORRUGATED);
    for (const auto& d : posts) box(out, cx + d[0], cy + d[1], z + 66, cx + d[0], cy + d[1], z + 81, MAT::STEEL_BEAM);
    for (double k = z + 2; k < z + 64; k += 3) box(out, cx - 2, cy - 9, k, cx + 2, cy - 9, k, MAT::STEEL_BEAM);
    box(out, cx - 2, cy - 9, z + 64, cx + 2, cy - 9, z + 65, 0);
  }
}

void radar_mast(std::vector<SiteBox>& out, double x, double y, double z) {
  box(out, x - 1, y - 1, z + 1, x + 1, y + 1, z + 120, MAT::STEEL_BEAM);
  for (double k = 0; k < 12; k += 1) box(out, x - 14 + k, y - 14 + k * 2, z + 120 + k, x + 14 - k, y + 14 - k, z + 120 + k, MAT::METAL_PANEL);
  box(out, x, y, z + 132, x, y, z + 136, MAT::SIGNAL_RED);
}

void radome(std::vector<SiteBox>& out, double x, double y, double z, double R) {
  box(out, x - R + 4, y - R + 4, z + 1, x + R - 4, y + R - 4, z + 24, MAT::CONCRETE_LIGHT);
  for (double dz = 0; dz <= R; dz += 1) {
    const double rr = js::round(std::sqrt(js::max(0.0, R * R - dz * dz)));
    box(out, x - rr, y - js::round(rr * 0.4), z + 24 + dz, x + rr, y + js::round(rr * 0.4), z + 24 + dz, MAT::PANEL_WHITE);
    box(out, x - js::round(rr * 0.4), y - rr, z + 24 + dz, x + js::round(rr * 0.4), y + rr, z + 24 + dz, MAT::PANEL_WHITE);
    box(out, x - js::round(rr * 0.72), y - js::round(rr * 0.72), z + 24 + dz, x + js::round(rr * 0.72), y + js::round(rr * 0.72), z + 24 + dz, MAT::PANEL_WHITE);
  }
  box(out, x, y, z + 24 + R + 1, x, y, z + 24 + R + 6, MAT::SIGNAL_RED);
}

void dish_array(std::vector<SiteBox>& out, double x0, double y, double z, double n, double gap) {
  for (double k = 0; k < n; k += 1) {
    const double x = x0 + k * gap;
    box(out, x - 2, y - 2, z + 1, x + 2, y + 2, z + 30, MAT::CONCRETE_LIGHT);
    for (double t = 0; t < 10; t += 1) box(out, x - 22 + t * 2, y - 4 + t, z + 30 + t * 2, x + 22 - t * 2, y - 2 + t, z + 31 + t * 2, MAT::PANEL_WHITE);
    box(out, x, y + 6, z + 38, x, y + 8, z + 50, MAT::STEEL_BEAM);
  }
}

void fuel_tanks(std::vector<SiteBox>& out, double x0, double y, double z, double n) {
  for (double k = 0; k < n; k += 1) {
    const double tx = x0 + k * vx(9);
    for (double dz = 1; dz <= 40; dz += 1) {
      box(out, tx - 22, y - 10, z + dz, tx + 22, y + 10, z + dz, MAT::TANK_WHITE);
      box(out, tx - 10, y - 22, z + dz, tx + 10, y + 22, z + dz, MAT::TANK_WHITE);
      box(out, tx - 18, y - 18, z + dz, tx + 18, y + 18, z + dz, MAT::TANK_WHITE);
    }
    box(out, tx - 18, y - 3, z + 12, tx + 18, y + 3, z + 14, MAT::HAZARD_YELLOW);
  }
}

void truck(std::vector<SiteBox>& out, double x, double y, double z, Rng& rng) {
  static const std::vector<uint16_t> kColors = {MAT::CONTAINER_GREEN, MAT::CAR_GREEN, MAT::CONTAINER_BLUE};
  const uint16_t c = rng.pick(kColors);
  // (pushed as they are: no bounds ordered)
  auto b = [&](double x0, double y0, double z0, double x1, double y1, double z1, uint16_t m) { out.push_back({x0, y0, z0, x1, y1, z1, m, 0}); };
  b(x, y, z + 3, x + 18, y + 54, z + 8, c);
  b(x + 1, y, z + 9, x + 17, y + 14, z + 18, c);
  b(x + 2, y, z + 13, x + 16, y, z + 16, MAT::CAR_GLASS);
  b(x + 1, y + 16, z + 9, x + 17, y + 54, z + 22, MAT::FABRIC_GREEN);
  for (const double yy : {y + 6, y + 36, y + 46}) {
    b(x - 1, yy, z, x + 1, yy + 5, z + 5, MAT::TIRE);
    b(x + 17, yy, z, x + 19, yy + 5, z + 5, MAT::TIRE);
  }
}

void portal_block(BoxLists& lists, const Rect& bk, double z, uint16_t accent) {
  box(lists.shells, bk.x0, bk.y0, z + 1, bk.x1, bk.y1, z + 40, MAT::CONCRETE_DARK);
  box(lists.carves, bk.x0 + 4, bk.y0 + 4, z + 2, bk.x1 - 4, bk.y1 - 4, z + 36, 0);
  for (double k = 0; k < 4; k += 1) box(lists.details, bk.x0 + k * 2, bk.y0 + k * 2, z + 41 + k, bk.x1 - k * 2, bk.y1 - k * 2, z + 41 + k, MAT::CONCRETE);
  box(lists.shells, bk.x0 + 4, bk.y0 + 4, z + 1, bk.x1 - 4, bk.y1 - 4, z + 1, MAT::FLOOR_EPOXY);
  const double dmx = js::round((bk.x0 + bk.x1) / 2);
  box(lists.carves, dmx - 10, bk.y1 - 4, z + 2, dmx + 10, bk.y1, z + 26, 0);
  box(lists.details, dmx - 12, bk.y1, z + 2, dmx - 11, bk.y1, z + 27, accent);
  box(lists.details, dmx + 11, bk.y1, z + 2, dmx + 12, bk.y1, z + 27, accent);
  box(lists.details, dmx - 12, bk.y1, z + 27, dmx + 12, bk.y1, z + 28, MAT::HAZARD_BLACK);
  box(lists.details, dmx - 22, bk.y1 - 3, z + 2, dmx - 13, bk.y1 - 1, z + 26, MAT::METAL_PANEL_DARK);
  box(lists.details, bk.x0 + 6, bk.y1, z + 30, bk.x0 + 30, bk.y1, z + 34, MAT::SIGNAGE_BLUE);
}

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
