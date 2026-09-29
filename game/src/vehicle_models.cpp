// structvox game — vehicle models (docs/VEHICLES.md): cars, vans, pickups and trucks in 6.25 cm
// voxels.
//
// A body is an envelope - its roof line along its length, a plan with rounded corners, the
// greenhouse drawn in above the belt line, wheel arches cut out - of which only the shell is
// kept: one voxel of sheet metal (painted) with glass where the windows are, plastic bumpers and
// grille, lamps. Inside it: a floor pan, two frame rails the length of it with cross members, a
// firewall, an engine block between the front wheels, seats and a dashboard, and at each wheel
// the top of its strut, where the wheel hangs from. What crumples in a crash is the body's own:
// the front folds back to the engine block, the rails fold last.
#include "svx/game/vehicles.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace svx {

namespace {

// A model under construction: voxels and their paint in a box of the model's frame.
class Canvas {
 public:
  Canvas(const IVec3& lo, const IVec3& hi) : lo_(lo), hi_(hi) {
    for (int a = 0; a < 3; ++a) dim_[a] = hi[a] - lo[a] + 1;
    vox_.assign(size_t(dim_[0]) * size_t(dim_[1]) * size_t(dim_[2]), kAir);
    paint_.assign(vox_.size(), 0);
  }
  bool in(const IVec3& p) const {
    for (int a = 0; a < 3; ++a)
      if (p[a] < lo_[a] || p[a] > hi_[a]) return false;
    return true;
  }
  Vox get(const IVec3& p) const { return in(p) ? vox_[idx(p)] : kAir; }
  void set(const IVec3& p, MaterialId m, Paint c = Paint::None) {
    if (!in(p)) return;
    vox_[idx(p)] = make_vox(m, false);
    paint_[idx(p)] = static_cast<u8>(c);
  }
  void clear(const IVec3& p) {
    if (!in(p)) return;
    vox_[idx(p)] = kAir;
    paint_[idx(p)] = 0;
  }
  // [lo, hi] inclusive
  void box(const IVec3& lo, const IVec3& hi, MaterialId m, Paint c = Paint::None) {
    for (i32 x = lo[0]; x <= hi[0]; ++x)
      for (i32 y = lo[1]; y <= hi[1]; ++y)
        for (i32 z = lo[2]; z <= hi[2]; ++z) set({x, y, z}, m, c);
  }
  // mirrored in y: [lo, hi] and its mirror image
  void box2(const IVec3& lo, const IVec3& hi, MaterialId m, Paint c = Paint::None) {
    box(lo, hi, m, c);
    box({lo[0], -hi[1], lo[2]}, {hi[0], -lo[1], hi[2]}, m, c);
  }
  // only where it is air (parts inside the shell, not through it)
  void fill_air(const IVec3& lo, const IVec3& hi, MaterialId m, Paint c = Paint::None) {
    for (i32 x = lo[0]; x <= hi[0]; ++x)
      for (i32 y = lo[1]; y <= hi[1]; ++y)
        for (i32 z = lo[2]; z <= hi[2]; ++z)
          if (!vox_solid(get({x, y, z}))) set({x, y, z}, m, c);
  }
  const IVec3& lo() const { return lo_; }
  const IVec3& hi() const { return hi_; }
  VoxelGrid grid() const {
    VoxelGrid g;
    g.h = kVehicleVoxel;
    const int L = g.add_layer({"paint", true, LayerBind::Solid});
    for (i32 x = lo_[0]; x <= hi_[0]; ++x)
      for (i32 y = lo_[1]; y <= hi_[1]; ++y)
        for (i32 z = lo_[2]; z <= hi_[2]; ++z) {
          const size_t i = idx({x, y, z});
          if (!vox_solid(vox_[i])) continue;
          g.set(x, y, z, vox_[i]);
          if (paint_[i]) g.set_layer(L, {x, y, z}, paint_[i]);
        }
    g.compact();
    return g;
  }

 private:
  size_t idx(const IVec3& p) const {
    return (size_t(p[0] - lo_[0]) * size_t(dim_[1]) + size_t(p[1] - lo_[1])) * size_t(dim_[2]) + size_t(p[2] - lo_[2]);
  }
  IVec3 lo_, hi_, dim_;
  std::vector<Vox> vox_;
  std::vector<u8> paint_;
};

// A body's envelope and how its shell is made (voxels of the model's frame).
struct Envelope {
  i32 x0 = 0, x1 = 0;                          // rear, front (inclusive)
  i32 hw = 14;                                 // half width
  i32 bottom = 3;                              // floor
  std::vector<std::array<i32, 2>> top;         // roof line: (x, z), x ascending
  i32 belt = 14;                               // the greenhouse above: drawn in
  i32 tumble = 3;                              // (a voxel in per this many up)
  i32 corner = 3;                              // plan corners rounded
  // wheel arches: axle x's, their centre height and radius, cut from |y| >= arch_y
  std::vector<i32> axles;
  i32 arch_z = 5, arch_r = 7, arch_y = 10;
  // an open box cut from the top (a pickup's bed): [x0, x1] x |y| <= hw - wall, above z
  bool bed = false;
  i32 bed_x0 = 0, bed_x1 = 0, bed_z = 0, bed_wall = 1;

  f64 top_at(i32 x) const {
    if (top.empty()) return bottom;
    if (x <= top.front()[0]) return top.front()[1];
    for (size_t k = 1; k < top.size(); ++k)
      if (x <= top[k][0]) {
        const f64 t = static_cast<f64>(x - top[k - 1][0]) / std::max(1, top[k][0] - top[k - 1][0]);
        return top[k - 1][1] + t * (top[k][1] - top[k - 1][1]);
      }
    return top.back()[1];
  }
  i32 half_width(i32 x, i32 z) const {
    const i32 d = std::min(x - x0, x1 - x);
    i32 w = hw - std::max(0, corner - d);
    if (z > belt && tumble > 0) w -= (z - belt + tumble - 1) / tumble;
    return w;
  }
  bool inside(i32 x, i32 y, i32 z) const {
    if (x < x0 || x > x1 || z < bottom || static_cast<f64>(z) > top_at(x) + 0.5) return false;
    if (std::abs(y) > half_width(x, z)) return false;
    for (i32 a : axles)
      if (std::abs(y) >= arch_y && (x - a) * (x - a) + (z - arch_z) * (z - arch_z) <= arch_r * arch_r) return false;
    if (bed && x >= bed_x0 && x <= bed_x1 && z > bed_z && std::abs(y) <= half_width(x, z) - bed_wall) return false;
    return true;
  }
};

// Where a shell voxel faces out.
struct Facing {
  bool top = false, bottom = false, side = false, front = false, rear = false;
};

// Its shell, each voxel of it as `paint_of` has it (material, paint), from where it faces out.
void shell(Canvas& c, const Envelope& e, const std::function<std::pair<MaterialId, Paint>(const IVec3&, const Facing&)>& paint_of) {
  const i32 zmax = static_cast<i32>(std::ceil(std::max_element(e.top.begin(), e.top.end(), [](auto& a, auto& b) { return a[1] < b[1]; })->at(1))) + 1;
  for (i32 x = e.x0; x <= e.x1; ++x)
    for (i32 y = -e.hw; y <= e.hw; ++y)
      for (i32 z = e.bottom; z <= zmax; ++z) {
        if (!e.inside(x, y, z)) continue;
        Facing f;
        f.top = !e.inside(x, y, z + 1);
        f.bottom = !e.inside(x, y, z - 1);
        f.side = !e.inside(x, y + 1, z) || !e.inside(x, y - 1, z);
        f.front = !e.inside(x + 1, y, z);
        f.rear = !e.inside(x - 1, y, z);
        if (!(f.top || f.bottom || f.side || f.front || f.rear)) {
          // (behind a step of the surface - a diagonal neighbour out: it joins the shell's faces up)
          bool edge = false;
          for (int dx = -1; dx <= 1 && !edge; ++dx)
            for (int dy = -1; dy <= 1 && !edge; ++dy)
              for (int dz = -1; dz <= 1 && !edge; ++dz) edge = !e.inside(x + dx, y + dy, z + dz);
          if (!edge) continue;
        }
        const auto [m, p] = paint_of({x, y, z}, f);
        c.set({x, y, z}, m, p);
      }
}

// A wheel of the model: its strut's top (a frame voxel block the wheel hangs from, in the top of
// its arch) and its slot.
void wheel(Canvas& c, VehicleModel& m, i32 ax, i32 wy, i32 mount_z, f64 radius, f64 width, bool driven, bool steered, const WheelSlot& base) {
  c.box2({ax - 1, wy - 1, mount_z}, {ax + 1, wy + 1, mount_z + 1}, MaterialId::CarFrame, Paint::Graphite);
  for (int s : {1, -1}) {
    WheelSlot w = base;
    w.mount = V3{kVehicleVoxel * ax, kVehicleVoxel * wy * s, kVehicleVoxel * (mount_z + 0.5)};
    w.radius = radius;
    w.width = width;
    // (at rest its centre a radius over the ground, a tenth of a metre up its travel)
    const f64 hang = w.mount.z - radius;
    w.rest = hang + 0.1;
    w.driven = driven;
    w.steered = steered;
    m.wheels.push_back(w);
  }
}

// Seats: a cushion and a backrest (y from its outboard side inwards, x its front).
void seat(Canvas& c, i32 xf, i32 y0, i32 y1, i32 floor, i32 cushion, i32 back) {
  c.fill_air({xf - 5, y0, floor + 1}, {xf, y1, cushion}, MaterialId::Plastic, Paint::Graphite);
  c.fill_air({xf - 7, y0, floor + 1}, {xf - 6, y1, back}, MaterialId::Plastic, Paint::Graphite);
}

// A car body: bumpers, grille and lamps front and back, glass in its greenhouse.
struct CarLook {
  i32 ws0, ws1;               // windshield (top-facing glass between these x's)
  i32 rw0, rw1;               // rear window (top-facing), or a rear glass (rear-facing) above rear_glass_z
  i32 side0, side1;           // side glass between these x's ...
  std::vector<std::array<i32, 2>> pillars;  // ... but for these x ranges
  i32 rear_glass_z = -1;      // (a hatch or a van: its rear faces glass above this)
  bool front_glass = false;   // (a van: its front faces glass above the belt)
  i32 lamp_z0 = 9, lamp_z1 = 10, lamp_y0 = 8;
  i32 tail_z0 = 11, tail_z1 = 12, tail_y0 = 9;
};

std::pair<MaterialId, Paint> car_skin(const Envelope& e, const CarLook& k, Paint body, const IVec3& p, const Facing& f) {
  const i32 x = p[0], y = std::abs(p[1]), z = p[2];
  const i32 w = e.half_width(x, z);
  const f64 top = e.top_at(x);
  if (f.bottom && z == e.bottom) return {MaterialId::Sheet, Paint::Graphite};
  // bumpers: the lowest band front and back
  if (z <= e.bottom + 4 && (x >= e.x1 - 2 || x <= e.x0 + 2)) return {MaterialId::Plastic, Paint::Trim};
  if (z > e.belt) {
    if (f.top && x > k.ws0 && x < k.ws1 && y < w) return {MaterialId::Window, Paint::None};
    if (f.top && x > k.rw0 && x < k.rw1 && y < w) return {MaterialId::Window, Paint::None};
    if (f.front && k.front_glass && y < w - 1 && z < top - 1) return {MaterialId::Window, Paint::None};
    if (f.rear && k.rear_glass_z >= 0 && z > k.rear_glass_z && y < w - 1 && z < top - 1) return {MaterialId::Window, Paint::None};
    if (f.side && !f.top && x > k.side0 && x < k.side1 && z < top - 1) {
      bool pillar = false;
      for (const auto& pr : k.pillars) pillar = pillar || (x >= pr[0] && x <= pr[1]);
      if (!pillar) return {MaterialId::Window, Paint::None};
    }
  }
  if (f.front && !f.top && x >= e.x1 - 1) {
    if (z >= k.lamp_z0 && z <= k.lamp_z1 && y >= k.lamp_y0 && y < w - 1) return {MaterialId::Lamp, Paint::None};
    if (z >= e.bottom + 5 && z < k.lamp_z0 && y < k.lamp_y0) return {MaterialId::Plastic, Paint::Trim};  // (the grille)
    if (z >= k.lamp_z0 && z <= k.lamp_z1 && y == w - 1) return {MaterialId::Lamp, Paint::Amber};
  }
  if (f.rear && !f.top && x <= e.x0 + 1 && z >= k.tail_z0 && z <= k.tail_z1 && y >= k.tail_y0 && y < w) return {MaterialId::Lamp, Paint::TailRed};
  return {MaterialId::Sheet, body};
}

VehicleModel sedan(Paint paint) {
  VehicleModel m;
  Envelope e;
  e.x0 = -37;
  e.x1 = 36;
  e.hw = 14;
  e.bottom = 3;
  e.top = {{-37, 14}, {-34, 15}, {-24, 16}, {-14, 23}, {4, 23}, {14, 14}, {30, 13}, {36, 12}};
  e.belt = 14;
  e.axles = {-21, 22};
  CarLook k{};
  k.ws0 = 4;
  k.ws1 = 14;
  k.rw0 = -24;
  k.rw1 = -14;
  k.side0 = -19;
  k.side1 = 4;
  k.pillars = {{-7, -5}};
  Canvas c({-38, -15, 0}, {37, 15, 25});
  shell(c, e, [&](const IVec3& p, const Facing& f) { return car_skin(e, k, paint, p, f); });
  // the frame: rails, cross members, the firewall, the engine between the front wheels
  c.box2({-36, 8, 4}, {35, 9, 5}, MaterialId::CarFrame, Paint::Graphite);
  for (i32 x : {-30, -21, -8, 8, 22, 31}) c.fill_air({x, -13, 4}, {x, 13, 4}, MaterialId::CarFrame, Paint::Graphite);
  c.fill_air({14, -13, 4}, {15, 13, 13}, MaterialId::Sheet, Paint::Graphite);
  c.box({17, -6, 5}, {29, 6, 11}, MaterialId::Engine, Paint::Graphite);
  // the cabin: dashboard, seats
  c.fill_air({10, -13, 11}, {13, 13, 13}, MaterialId::Plastic, Paint::Graphite);
  seat(c, 1, 2, 10, 3, 8, 15);
  seat(c, 1, -10, -2, 3, 8, 15);
  seat(c, -13, -12, 12, 3, 8, 15);
  // the wheels (front steered, the rear driven)
  WheelSlot base;
  wheel(c, m, 22, 12, 11, 0.32, 0.22, false, true, base);
  wheel(c, m, -21, 12, 11, 0.32, 0.22, true, false, base);
  m.voxels = c.grid();
  m.tuning = VehicleTuning{};
  m.half_extent = V3{2.35, 0.95, 1.5};
  m.driver_seat = V3{-0.15, 0.37, 0.9};
  return m;
}

VehicleModel compact(Paint paint) {
  VehicleModel m;
  Envelope e;
  e.x0 = -32;
  e.x1 = 31;
  e.hw = 13;
  e.bottom = 3;
  e.top = {{-32, 21}, {-29, 23}, {-4, 24}, {7, 23}, {15, 14}, {26, 13}, {31, 12}};
  e.belt = 14;
  e.axles = {-19, 20};
  e.arch_y = 9;
  CarLook k{};
  k.ws0 = 7;
  k.ws1 = 15;
  k.rw0 = 999;
  k.rw1 = -999;
  k.rear_glass_z = 14;
  k.side0 = -27;
  k.side1 = 7;
  k.pillars = {{-9, -7}};
  Canvas c({-33, -14, 0}, {32, 14, 26});
  shell(c, e, [&](const IVec3& p, const Facing& f) { return car_skin(e, k, paint, p, f); });
  c.box2({-31, 7, 4}, {30, 8, 5}, MaterialId::CarFrame, Paint::Graphite);
  for (i32 x : {-26, -19, -6, 8, 20, 27}) c.fill_air({x, -12, 4}, {x, 12, 4}, MaterialId::CarFrame, Paint::Graphite);
  c.fill_air({14, -12, 4}, {15, 12, 13}, MaterialId::Sheet, Paint::Graphite);
  c.box({17, -5, 5}, {27, 5, 10}, MaterialId::Engine, Paint::Graphite);
  c.fill_air({10, -12, 11}, {13, 12, 13}, MaterialId::Plastic, Paint::Graphite);
  seat(c, 2, 2, 9, 3, 8, 16);
  seat(c, 2, -9, -2, 3, 8, 16);
  seat(c, -13, -11, 11, 3, 8, 16);
  WheelSlot base;
  base.stiffness = 26e3;
  base.damping = 2.6e3;
  wheel(c, m, 20, 11, 11, 0.3, 0.2, true, true, base);  // (front wheel drive)
  wheel(c, m, -19, 11, 11, 0.3, 0.2, false, false, base);
  m.voxels = c.grid();
  VehicleTuning t;
  t.torque = 190.0;
  t.peak_rpm = 4500.0;
  t.gears = {3.6, 2.2, 1.5, 1.12, 0.9, 0.0};
  t.ngears = 5;
  t.final_drive = 4.1;
  t.brake = 2000.0;
  t.handbrake = 2400.0;
  t.drag_area = 0.62;
  t.anti_roll = 11e3;
  m.tuning = t;
  m.half_extent = V3{2.05, 0.88, 1.55};
  m.driver_seat = V3{-0.1, 0.35, 0.95};
  return m;
}

VehicleModel van(Paint paint) {
  VehicleModel m;
  Envelope e;
  e.x0 = -40;
  e.x1 = 39;
  e.hw = 15;
  e.bottom = 4;
  e.top = {{-40, 32}, {24, 32}, {27, 31}, {31, 20}, {39, 18}};
  e.belt = 17;
  e.tumble = 0;
  e.corner = 2;
  e.axles = {-24, 26};
  e.arch_r = 7;
  e.arch_y = 11;
  CarLook k{};
  k.ws0 = 26;
  k.ws1 = 32;
  k.rw0 = 999;
  k.rw1 = -999;
  k.side0 = 12;
  k.side1 = 28;
  k.pillars = {};
  k.rear_glass_z = 22;
  k.lamp_z0 = 12;
  k.lamp_z1 = 13;
  k.tail_z0 = 12;
  k.tail_z1 = 17;
  k.tail_y0 = 12;
  Canvas c({-41, -16, 0}, {40, 16, 33});
  shell(c, e, [&](const IVec3& p, const Facing& f) {
    auto r = car_skin(e, k, paint, p, f);
    // (its cargo's sides are blank; a stripe of trim along its flanks)
    if (r.first == MaterialId::Sheet && f.side && p[2] == 12) r.second = Paint::Trim;
    return r;
  });
  c.box2({-39, 9, 5}, {38, 10, 6}, MaterialId::CarFrame, Paint::Graphite);
  for (i32 x : {-32, -24, -12, 0, 12, 26, 34}) c.fill_air({x, -14, 5}, {x, 14, 5}, MaterialId::CarFrame, Paint::Graphite);
  c.fill_air({8, -14, 5}, {9, 14, 31}, MaterialId::Sheet, Paint::Graphite);  // (the bulkhead behind the cab)
  c.box({27, -6, 6}, {36, 6, 14}, MaterialId::Engine, Paint::Graphite);
  c.fill_air({22, -14, 14}, {25, 14, 17}, MaterialId::Plastic, Paint::Graphite);
  seat(c, 17, 3, 12, 4, 11, 21);
  seat(c, 17, -12, -3, 4, 11, 21);
  WheelSlot base;
  base.stiffness = 48e3;
  base.damping = 4.6e3;
  base.inertia = 1.6;
  wheel(c, m, 26, 12, 12, 0.34, 0.22, false, true, base);
  wheel(c, m, -24, 12, 12, 0.34, 0.22, true, false, base);
  m.voxels = c.grid();
  VehicleTuning t;
  t.torque = 330.0;
  t.peak_rpm = 3200.0;
  t.redline = 5200.0;
  t.gears = {3.9, 2.3, 1.5, 1.1, 0.86, 0.0};
  t.ngears = 5;
  t.final_drive = 4.3;
  t.brake = 3400.0;
  t.handbrake = 3600.0;
  t.max_steer = 0.58;
  t.drag_area = 1.25;
  t.anti_roll = 20e3;
  m.tuning = t;
  m.half_extent = V3{2.5, 1.0, 2.05};
  m.driver_seat = V3{1.0, 0.45, 1.2};
  return m;
}

VehicleModel pickup(Paint paint) {
  VehicleModel m;
  Envelope e;
  e.x0 = -43;
  e.x1 = 42;
  e.hw = 15;
  e.bottom = 5;
  e.top = {{-43, 17}, {-10, 17}, {-9, 27}, {7, 27}, {15, 18}, {35, 17}, {42, 16}};
  e.belt = 18;
  e.axles = {-26, 27};
  e.arch_z = 6;
  e.arch_r = 8;
  e.arch_y = 10;
  e.bed = true;
  e.bed_x0 = -42;
  e.bed_x1 = -11;
  e.bed_z = 7;
  CarLook k{};
  k.ws0 = 7;
  k.ws1 = 15;
  k.rw0 = 999;
  k.rw1 = -999;
  k.rear_glass_z = 18;
  k.side0 = -9;
  k.side1 = 8;
  k.pillars = {};
  k.lamp_z0 = 12;
  k.lamp_z1 = 13;
  k.tail_z0 = 11;
  k.tail_z1 = 15;
  k.tail_y0 = 12;
  Canvas c({-44, -16, 0}, {43, 16, 28});
  shell(c, e, [&](const IVec3& p, const Facing& f) {
    auto r = car_skin(e, k, paint, p, f);
    // (the cab's back is glass above the bed's walls)
    if (p[0] == -10 && f.rear && p[2] > 18 && p[2] < 26 && std::abs(p[1]) < 11) r = {MaterialId::Window, Paint::None};
    return r;
  });
  c.box2({-42, 9, 6}, {41, 10, 7}, MaterialId::CarFrame, Paint::Graphite);
  for (i32 x : {-36, -26, -14, 0, 14, 27, 36}) c.fill_air({x, -14, 6}, {x, 14, 6}, MaterialId::CarFrame, Paint::Graphite);
  c.fill_air({15, -14, 6}, {16, 14, 17}, MaterialId::Sheet, Paint::Graphite);
  c.box({19, -6, 7}, {33, 6, 15}, MaterialId::Engine, Paint::Graphite);
  c.fill_air({10, -14, 15}, {14, 14, 18}, MaterialId::Plastic, Paint::Graphite);
  seat(c, 3, 3, 12, 5, 10, 20);
  seat(c, 3, -12, -3, 5, 10, 20);
  WheelSlot base;
  base.stiffness = 45e3;
  base.damping = 4.4e3;
  base.inertia = 1.8;
  wheel(c, m, 27, 12, 14, 0.38, 0.26, true, true, base);  // (four wheel drive)
  wheel(c, m, -26, 12, 14, 0.38, 0.26, true, false, base);
  c.box2({-27, 14, 14}, {-25, 14, 15}, MaterialId::CarFrame, Paint::Graphite);  // (the rear struts' tops on the bed's walls)
  m.voxels = c.grid();
  VehicleTuning t;
  t.torque = 430.0;
  t.peak_rpm = 3400.0;
  t.redline = 5600.0;
  t.gears = {3.8, 2.3, 1.5, 1.14, 0.87, 0.69};
  t.final_drive = 3.7;
  t.brake = 3600.0;
  t.handbrake = 3800.0;
  t.drag_area = 1.05;
  t.anti_roll = 20e3;
  m.tuning = t;
  m.half_extent = V3{2.7, 1.0, 1.75};
  m.driver_seat = V3{-0.05, 0.45, 1.25};
  return m;
}

VehicleModel truck(Paint paint) {
  VehicleModel m;
  // the cab
  Envelope cab;
  cab.x0 = 30;
  cab.x1 = 58;
  cab.hw = 18;
  cab.bottom = 9;
  cab.top = {{30, 46}, {48, 46}, {51, 44}, {54, 30}, {58, 26}};
  cab.belt = 28;
  cab.tumble = 0;
  cab.corner = 2;
  cab.axles = {45};
  cab.arch_z = 8;
  cab.arch_r = 9;
  cab.arch_y = 12;
  CarLook k{};
  k.ws0 = 49;
  k.ws1 = 55;
  k.rw0 = 999;
  k.rw1 = -999;
  k.side0 = 38;
  k.side1 = 51;
  k.pillars = {};
  k.lamp_z0 = 16;
  k.lamp_z1 = 18;
  k.lamp_y0 = 11;
  k.tail_z0 = -1;
  k.tail_z1 = -2;
  Canvas c({-60, -19, 0}, {59, 19, 58});
  shell(c, cab, [&](const IVec3& p, const Facing& f) { return car_skin(cab, k, paint, p, f); });
  // the box on the frame behind it
  Envelope box;
  box.x0 = -58;
  box.x1 = 28;
  box.hw = 18;
  box.bottom = 14;
  box.top = {{-58, 56}, {28, 56}};
  box.belt = 99;
  box.tumble = 0;
  box.corner = 1;
  shell(c, box, [&](const IVec3& p, const Facing& f) -> std::pair<MaterialId, Paint> {
    if (f.rear && p[2] >= 18 && p[2] <= 20 && std::abs(p[1]) >= 13) return {MaterialId::Lamp, Paint::TailRed};
    if (f.bottom) return {MaterialId::Sheet, Paint::Graphite};
    return {MaterialId::Sheet, (p[2] >= 50 || p[2] <= 16) ? paint : Paint::White};
  });
  // the frame: two heavy rails the length of it, cross members, the engine under the cab
  c.box2({-58, 9, 9}, {57, 11, 13}, MaterialId::CarFrame, Paint::Graphite);
  for (i32 x : {-52, -40, -28, -16, -4, 8, 20, 32, 45, 54}) c.fill_air({x, -13, 10}, {x, 13, 12}, MaterialId::CarFrame, Paint::Graphite);
  c.box({40, -7, 14}, {54, 7, 26}, MaterialId::Engine, Paint::Graphite);
  c.fill_air({44, -16, 27}, {48, 16, 30}, MaterialId::Plastic, Paint::Graphite);
  seat(c, 41, 3, 15, 9, 20, 32);
  seat(c, 41, -15, -3, 9, 20, 32);
  // bumper
  c.box({57, -17, 10}, {59, 17, 14}, MaterialId::CarFrame, Paint::Trim);
  WheelSlot base;
  base.stiffness = 160e3;
  base.damping = 14e3;
  base.inertia = 6.0;
  base.travel = 0.18;
  wheel(c, m, 45, 14, 17, 0.5, 0.32, false, true, base);
  // (the rear axles' arches: cut from the box's flanks and the frame's)
  for (i32 ax : {-30, -47})
    for (i32 x = ax - 9; x <= ax + 9; ++x)
      for (i32 z = 0; z <= 17; ++z)
        for (i32 y = 12; y <= 19; ++y)
          if ((x - ax) * (x - ax) + (z - 8) * (z - 8) <= 81)
            for (int s : {1, -1}) c.clear({x, y * s, z});
  // (their struts' tops on posts up from the rails)
  for (i32 ax : {-30, -47}) {
    c.box2({ax - 1, 10, 13}, {ax + 1, 11, 18}, MaterialId::CarFrame, Paint::Graphite);
    c.box2({ax - 1, 12, 17}, {ax + 1, 15, 18}, MaterialId::CarFrame, Paint::Graphite);
  }
  wheel(c, m, -30, 14, 17, 0.5, 0.32, true, false, base);
  wheel(c, m, -47, 14, 17, 0.5, 0.32, true, false, base);
  m.voxels = c.grid();
  VehicleTuning t;
  t.torque = 1100.0;
  t.peak_rpm = 1600.0;
  t.idle_rpm = 600.0;
  t.redline = 2600.0;
  t.gears = {6.2, 3.8, 2.4, 1.6, 1.15, 0.85};
  t.final_drive = 4.6;
  t.shift_time = 0.45;
  t.brake = 9000.0;
  t.brake_front = 0.35;
  t.handbrake = 9000.0;
  t.max_steer = 0.55;
  t.steer_rate = 1.8;
  t.drag_area = 4.5;
  t.anti_roll = 80e3;
  m.tuning = t;
  m.half_extent = V3{3.7, 1.2, 3.6};
  m.driver_seat = V3{2.4, 0.55, 2.0};
  return m;
}

}  // namespace

VehicleModel build_vehicle(const VehicleSpec& spec) {
  VehicleModel m;
  switch (spec.kind) {
    case VehicleKind::Compact:
      m = compact(spec.paint);
      break;
    case VehicleKind::Van:
      m = van(spec.paint);
      break;
    case VehicleKind::Pickup:
      m = pickup(spec.paint);
      break;
    case VehicleKind::Truck:
      m = truck(spec.paint);
      break;
    case VehicleKind::Sedan:
    case VehicleKind::Count:
    default:
      m = sedan(spec.paint);
      break;
  }
  m.spec = spec;
  return m;
}

}  // namespace svx
