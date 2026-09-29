// structvox game — vehicles in a game (docs/VEHICLES.md): spawning them, the registry of the
// vehicles in the world (rebuilt from their wheels), and driving them - an engine and an
// automatic gearbox turning the driven wheels, brakes, speed-sensitive steering with Ackermann
// geometry, anti-roll bars and air drag - from the player's controls or a driver's (traffic.cpp).
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>

#include "svx/base/dmath.hpp"
#include "svx/game/game.hpp"
#include "svx/game/replay.hpp"

namespace svx {

namespace {

// The models are built once per kind (in a placeholder paint, sprayed per vehicle).
constexpr Paint kModelPaint = Paint::Red;

// A model's voxels in a paint of its own.
VoxelGrid sprayed(const VehicleModel& m, Paint p) {
  VoxelGrid g = m.voxels;
  if (p == kModelPaint) return g;
  const int L = g.layer_index("paint");
  if (L < 0) return g;
  std::vector<IVec3> body;
  for (const auto& [k, ch] : g.chunks()) {
    if (ch.layer[size_t(L)].empty()) continue;
    const IVec3 cc = unkey3(k);
    for (int i = 0; i < kChunkVox; ++i)
      if (ch.layer[size_t(L)][size_t(i)] == static_cast<u8>(kModelPaint))
        body.push_back({cc[0] * kChunk + i / (kChunk * kChunk), cc[1] * kChunk + (i / kChunk) % kChunk, cc[2] * kChunk + i % kChunk});
  }
  for (const IVec3& q : body) g.set_layer(L, q, static_cast<u8>(p));
  return g;
}

Quat yaw_quat(f64 yaw) { return Quat{0.0, 0.0, dm::sin(0.5 * yaw), dm::cos(0.5 * yaw)}; }

// The engine's torque at a speed (N m): a broad curve about its peak, nothing past the redline.
f64 engine_torque(const VehicleTuning& t, f64 rpm) {
  if (rpm > t.redline) return 0.0;
  const f64 u = (rpm - t.peak_rpm) / std::max(1.0, t.redline - t.idle_rpm);
  return t.torque * std::max(0.45, 1.0 - 0.9 * u * u);
}

constexpr f64 kTwoPi = 6.28318530717958647692;

}  // namespace

const VehicleModel& vehicle_model(VehicleKind k) {
  static std::mutex mu;
  static std::array<std::unique_ptr<VehicleModel>, static_cast<size_t>(VehicleKind::Count)> cache;
  const size_t i = std::min(static_cast<size_t>(k), cache.size() - 1);
  std::lock_guard<std::mutex> lock(mu);
  if (!cache[i]) cache[i] = std::make_unique<VehicleModel>(build_vehicle({static_cast<VehicleKind>(i), kModelPaint}));
  return *cache[i];
}

// ---------------------------------------------------------------------------------------------
// Commands

u32 Game::spawn_vehicle(const VehicleSpec& spec, const V3& pos, f64 yaw, u8 flags) {
  if (log_) {
    Command c;
    c.tick = world_.ticks();
    c.type = Command::Type::Vehicle;
    c.a = {1.0, static_cast<f64>(u32(spec.kind) + 256u * u32(spec.paint) + 65536u * u32(flags)), pos.x, pos.y, pos.z, yaw};
    log_->push(c);
  }
  return spawn_vehicle_internal(spec, pos, yaw, flags);
}

u32 Game::spawn_vehicle_internal(const VehicleSpec& spec0, const V3& pos, f64 yaw, u8 flags) {
  VehicleSpec spec = spec0;
  if (static_cast<u32>(spec.kind) >= static_cast<u32>(VehicleKind::Count)) spec.kind = VehicleKind::Sedan;
  if (static_cast<u32>(spec.paint) >= static_cast<u32>(Paint::Count)) spec.paint = Paint::White;
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(yaw)) return 0;
  const VehicleModel& m = vehicle_model(spec.kind);
  const Quat q = yaw_quat(yaw);
  GridDesc d;
  d.frame = GridFrame{pos + V3{0.0, 0.0, 0.05}, q};
  d.voxel_size = kVehicleVoxel;
  d.base = false;
  const GridId gid = world_.add_grid(d, sprayed(m, spec.paint));
  if (gid == 0) return 0;
  const u32 id = next_vehicle_++;
  Vehicle v;
  v.id = id;
  v.spec = spec;
  v.flags = flags;
  v.home = pos;
  const M3 R = to_matrix(q);
  const f64 static_load = 9.81 * 1200.0 / 4.0;
  for (size_t k = 0; k < m.wheels.size(); ++k) {
    const WheelSlot& s = m.wheels[k];
    WheelDesc wd;
    wd.mount.kind = JointAnchor::Kind::Grid;
    wd.mount.id = gid;
    wd.mount.point = d.frame.origin + R * s.mount;
    wd.down = R * V3{0.0, 0.0, -1.0};
    wd.axle = R * V3{0.0, 1.0, 0.0};
    wd.radius = s.radius;
    wd.width = s.width;
    wd.rest = s.rest;
    wd.travel = s.travel;
    wd.stiffness = s.stiffness;
    wd.damping = s.damping;
    wd.inertia = s.inertia;
    wd.grip = 1.0;
    // (a hard crash on a wheel tears it off: many times what it carries)
    wd.break_force = m.wheel_break > 0.0 ? m.wheel_break : 16.0 * static_load * (s.stiffness / 32e3);
    wd.group = id;
    WheelTag tag;
    tag.slot = static_cast<u8>(k);
    tag.kind = spec.kind;
    tag.paint = spec.paint;
    tag.flags = flags;
    wd.tag = tag.pack();
    v.wheels.push_back(world_.add_wheel(wd));
  }
  // (a piece at once, whole, its wheels on it: nothing to solve - no structure holds a car)
  v.chassis = world_.loosen_grid(gid);
  if (v.chassis != 0) {
    world_.set_piece_max_speed(v.chassis, 90.0);
    if (const Body* b = world_.piece(v.chassis)) v.voxels0 = b->count;
  }
  vehicles_[id] = std::move(v);
  return id;
}

bool Game::remove_vehicle(u32 id) {
  const auto it = vehicles_.find(id);
  if (it == vehicles_.end()) return false;
  if (log_) log_->push({world_.ticks(), Command::Type::Vehicle, {2.0, static_cast<f64>(id), 0.0, 0.0, 0.0, 0.0}});
  for (WheelId w : it->second.wheels)
    if (w) world_.remove_wheel(w);
  if (it->second.chassis) world_.remove_piece(it->second.chassis);
  // (a grid not come loose yet goes with its wheels' mounts: nothing holds it)
  if (player_vehicle_ == id) player_vehicle_ = 0;
  vehicles_.erase(it);
  return true;
}

bool Game::enter_vehicle(u32 id) {
  const auto it = vehicles_.find(id);
  if (it == vehicles_.end()) return false;
  if (log_) log_->push({world_.ticks(), Command::Type::Vehicle, {3.0, static_cast<f64>(id), 0.0, 0.0, 0.0, 0.0}});
  player_vehicle_ = id;
  player_input_ = VehicleInput{};
  // (whoever drove it gets out: it is the player's now)
  Vehicle& v = it->second;
  v.flags = static_cast<u8>(v.flags & ~(WheelTag::kTagNpc | WheelTag::kTagParked));
  v.touched = true;
  v.lane = v.next = 0;
  if (v.chassis) world_.wake_piece(v.chassis);
  return true;
}

void Game::exit_vehicle() {
  if (log_) log_->push({world_.ticks(), Command::Type::Vehicle, {4.0, 0.0, 0.0, 0.0, 0.0, 0.0}});
  const auto it = vehicles_.find(player_vehicle_);
  if (it != vehicles_.end()) it->second.input = VehicleInput{0.0, 0.0, 0.0, true};  // (left with its handbrake on)
  player_vehicle_ = 0;
  player_input_ = VehicleInput{};
}

void Game::drive(const VehicleInput& in0) {
  VehicleInput in;
  in.throttle = std::isfinite(in0.throttle) ? std::clamp(in0.throttle, -1.0, 1.0) : 0.0;
  in.brake = std::isfinite(in0.brake) ? std::clamp(in0.brake, 0.0, 1.0) : 0.0;
  in.steer = std::isfinite(in0.steer) ? std::clamp(in0.steer, -1.0, 1.0) : 0.0;
  in.handbrake = in0.handbrake;
  if (in.throttle == player_input_.throttle && in.brake == player_input_.brake && in.steer == player_input_.steer &&
      in.handbrake == player_input_.handbrake)
    return;
  if (log_) log_->push({world_.ticks(), Command::Type::Drive, {in.throttle, in.brake, in.steer, in.handbrake ? 1.0 : 0.0, 0.0, 0.0}});
  player_input_ = in;
}

void Game::shoot(const V3& pos, f64 radius, f64 energy) {
  if (log_) log_->push({world_.ticks(), Command::Type::Shoot, {pos.x, pos.y, pos.z, radius, energy, 0.0}});
  if (shot_resolver && !movers_.empty())
    for (const MoverTrigger& t : shot_resolver(pos)) activate_mover(t.mover, t.move);
  if (!movers_.empty()) shots_.push_back({pos, radius});
  world_.shoot(pos, radius, energy);
}

void Game::set_traffic(const TrafficConfig& c) {
  if (log_)
    log_->push({world_.ticks(), Command::Type::Traffic,
                {c.enabled ? 1.0 : 0.0, static_cast<f64>(c.cars), static_cast<f64>(c.parked), c.near_radius, c.radius, c.speed_scale}});
  traffic_ = c;
  traffic_.cars = std::clamp(traffic_.cars, 0, 64);
  traffic_.parked = std::clamp(traffic_.parked, 0, 128);
  if (!std::isfinite(traffic_.near_radius)) traffic_.near_radius = 45.0;
  if (!std::isfinite(traffic_.radius)) traffic_.radius = 110.0;
  if (!std::isfinite(traffic_.speed_scale)) traffic_.speed_scale = 1.0;
  traffic_.radius = std::clamp(traffic_.radius, 20.0, 400.0);
  traffic_.near_radius = std::clamp(traffic_.near_radius, 0.0, traffic_.radius - 10.0);
  traffic_.speed_scale = std::clamp(traffic_.speed_scale, 0.1, 3.0);
}

// ---------------------------------------------------------------------------------------------
// Views

namespace {

// A chassis' frame: its model's axes in the world (the piece's first shape is its grid's lattice).
Quat frame_of(const Body& b) { return b.lattice_rot(0); }

}  // namespace

bool Game::vehicle(u32 id, VehicleView* out) const {
  const auto it = vehicles_.find(id);
  if (it == vehicles_.end()) return false;
  const Vehicle& v = it->second;
  VehicleView o;
  o.id = v.id;
  o.chassis = v.chassis;
  o.kind = v.spec.kind;
  o.paint = v.spec.paint;
  o.input = v.id == player_vehicle_ ? player_input_ : v.input;
  o.gear = v.gear;
  o.rpm = v.rpm;
  const VehicleModel& m = vehicle_model(v.spec.kind);
  o.half_extent = m.half_extent;
  o.redline = m.tuning.redline;
  for (WheelId w : v.wheels) o.wheels += w != 0 ? 1 : 0;
  o.flags = static_cast<u8>((v.id == player_vehicle_ ? VehicleView::kPlayer : 0) | ((v.flags & WheelTag::kTagNpc) ? VehicleView::kNpc : 0) |
                            ((v.flags & WheelTag::kTagParked) ? VehicleView::kParked : 0) | (v.wreck ? VehicleView::kWreck : 0));
  const Body* b = v.chassis ? world_.piece(v.chassis) : nullptr;
  if (b) {
    const Quat q = frame_of(*b);
    o.pos = b->x;
    o.vel = b->v;
    o.rot = q;
    o.speed = dot(b->v, rotate(q, V3{1.0, 0.0, 0.0}));
    o.seat = b->lattice_to_world(0, m.driver_seat);
    o.origin = b->lattice_to_world(0, V3{});
    // (crumpled: each fold a little more, whatever it lost more)
    if (v.voxels0 > 0) o.damage = std::clamp(4.0 * (1.0 - static_cast<f64>(b->count) / v.voxels0), 0.0, 1.0);
    o.damage = std::clamp(std::max(o.damage, v.reshapes / 20.0), 0.0, 1.0);
  }
  *out = o;
  return true;
}

std::vector<VehicleView> Game::vehicles() const {
  std::vector<VehicleView> out;
  out.reserve(vehicles_.size());
  for (const auto& [id, v] : vehicles_) {
    VehicleView o;
    if (vehicle(id, &o)) out.push_back(o);
  }
  return out;
}

std::vector<WheelView> Game::wheel_views() const {
  std::vector<WheelView> out;
  for (const auto& [id, v] : vehicles_)
    for (WheelId w : v.wheels) {
      WheelState s;
      if (!w || !world_.wheel(w, &s) || s.piece == 0) continue;
      WheelView o;
      o.vehicle = id;
      o.id = w;
      o.centre = s.centre;
      // (spun about its axle)
      const Quat spin{0.0, dm::sin(0.5 * s.angle), 0.0, dm::cos(0.5 * s.angle)};
      o.rot = s.rot * spin;
      o.radius = s.radius;
      o.width = s.width;
      o.contact = s.contact;
      o.slip = std::sqrt(s.slip_long * s.slip_long + s.slip_lat * s.slip_lat);
      o.material = s.material;
      o.compression = s.compression;
      out.push_back(o);
    }
  return out;
}

u32 Game::vehicle_near(const V3& pos, f64 reach) const {
  u32 best = 0;
  f64 bd = reach * reach;
  for (const auto& [id, v] : vehicles_) {
    const Body* b = v.chassis ? world_.piece(v.chassis) : nullptr;
    if (!b) continue;
    const f64 d2 = norm2(b->x - pos);
    if (d2 < bd) {
      bd = d2;
      best = id;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------------------------
// The registry

void Game::sync_vehicles() {
  // the wheels by vehicle (their groups), in id order
  std::map<u32, std::vector<std::pair<WheelTag, WheelId>>> groups;
  std::map<u32, i64> chassis;
  for (WheelId w : world_.wheels()) {
    WheelState s;
    if (!world_.wheel(w, &s) || s.group == 0) continue;
    groups[s.group].push_back({WheelTag::unpack(s.tag), w});
    if (s.piece != 0) chassis[s.group] = s.piece;
  }
  // vehicles gone: out of range (they come back with their wheels), or their body gone. One
  // whose wheels have all come off is still one while its body is there (a wreck on its belly:
  // its driver is in it) - but only for this session: a saved one is found by its wheels.
  for (auto it = vehicles_.begin(); it != vehicles_.end();) {
    Vehicle& v = it->second;
    if (!groups.count(it->first) && !(v.chassis != 0 && world_.piece(v.chassis))) {
      if (player_vehicle_ == it->first) player_vehicle_ = 0;
      it = vehicles_.erase(it);
    } else {
      if (!groups.count(it->first)) {
        std::fill(v.wheels.begin(), v.wheels.end(), WheelId{0});
        v.wreck = v.wreck || (v.flags & WheelTag::kTagNpc) != 0;
      }
      ++it;
    }
  }
  for (auto& [g, ws] : groups) {
    Vehicle& v = vehicles_[g];
    if (v.id == 0) {
      // (one back from the archive, or a loaded session's)
      v.id = g;
      v.spec.kind = ws.front().first.kind;
      v.spec.paint = ws.front().first.paint;
      v.flags = ws.front().first.flags;
      v.touched = true;
      v.input.handbrake = true;
    }
    next_vehicle_ = std::max(next_vehicle_, g + 1);
    const size_t slots = vehicle_model(v.spec.kind).wheels.size();
    v.wheels.assign(slots, 0);
    for (const auto& [tag, w] : ws)
      if (tag.slot < slots) v.wheels[tag.slot] = w;
    const auto c = chassis.find(g);
    const i64 was = v.chassis;
    v.chassis = c != chassis.end() ? c->second : 0;
    if (v.chassis != 0 && v.chassis != was) {
      // (a piece now: faster than rubble may go, never culled)
      world_.set_piece_max_speed(v.chassis, 90.0);
      if (const Body* b = world_.piece(v.chassis); b && v.voxels0 == 0) v.voxels0 = b->count;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Driving

void Game::drive_vehicle(Vehicle& v, const Body& b, f64 dt) {
  const VehicleModel& m = vehicle_model(v.spec.kind);
  const VehicleTuning& t = m.tuning;
  const VehicleInput in = v.id == player_vehicle_ ? player_input_ : v.input;
  const Quat q = frame_of(b);
  const V3 fwd = rotate(q, V3{1.0, 0.0, 0.0});
  const f64 vx = dot(b.v, fwd);
  // the wheels now
  const size_t n = v.wheels.size();
  std::vector<WheelState> ws(n);
  std::vector<bool> on(n, false);
  f64 spin = 0.0;
  i32 driven = 0;
  for (size_t k = 0; k < n; ++k) {
    on[k] = v.wheels[k] != 0 && world_.wheel(v.wheels[k], &ws[k]);
    if (on[k] && m.wheels[k].driven) {
      spin += ws[k].spin;
      ++driven;
    }
  }
  if (driven > 0) spin /= driven;
  // pedals: the throttle drives forwards, or backwards once stopped (reversing); against the
  // way it rolls, it brakes
  f64 throttle = 0.0, brake = in.brake;
  if (in.throttle > 0.02) {
    if (v.gear < 0 && vx < -0.8) {
      brake = std::max(brake, in.throttle);
    } else {
      if (v.gear < 1) v.gear = 1;
      throttle = in.throttle;
    }
  } else if (in.throttle < -0.02) {
    if (v.gear > 0 && vx > 0.8) {
      brake = std::max(brake, -in.throttle);
    } else {
      v.gear = -1;
      throttle = -in.throttle;
    }
  }
  // the engine's speed: the driven wheels' through the gearbox (spinning, they rev it), the
  // clutch slipping below idle; the road's speed through the gearbox is what the gearbox shifts on
  f64 radius = 0.0;
  for (size_t k = 0; k < n; ++k)
    if (on[k] && m.wheels[k].driven) radius += m.wheels[k].radius;
  radius = driven > 0 ? radius / driven : 0.32;
  auto ratio_of = [&](int gear) { return gear > 0 ? t.gears[size_t(std::min(gear, t.ngears) - 1)] : gear < 0 ? t.reverse : 0.0; };
  f64 ratio = ratio_of(v.gear);
  const f64 to_rpm = t.final_drive * 60.0 / kTwoPi;
  const f64 wheel_rpm = std::abs(spin) * ratio * to_rpm;
  const f64 road_rpm = std::abs(vx) / radius * ratio * to_rpm;
  const f64 launch = t.idle_rpm + throttle * 0.35 * (t.peak_rpm - t.idle_rpm);
  // the gearbox: up near the redline, down when it lugs or for a kick-down (not while shifting)
  if (v.shift > 0.0) v.shift -= dt;
  if (v.gear > 0 && v.shift <= 0.0) {
    if (road_rpm > 0.88 * t.redline && v.gear < t.ngears) {
      ++v.gear;
      v.shift = t.shift_time;
    } else if (v.gear > 1) {
      const f64 lower = road_rpm * ratio_of(v.gear - 1) / ratio;
      if ((road_rpm < std::max(1.3 * t.idle_rpm, 0.38 * t.peak_rpm) && lower < 0.8 * t.redline) || (throttle > 0.95 && lower < 0.62 * t.redline)) {
        --v.gear;
        v.shift = t.shift_time;
      }
    }
    ratio = ratio_of(v.gear);
  }
  v.rpm = std::clamp(std::max({wheel_rpm, road_rpm * ratio / std::max(1e-9, ratio_of(v.gear)), launch}), t.idle_rpm, t.redline * 1.02);
  // the torque at the driven wheels (none while shifting; traction control eases it off a
  // spinning tyre); coasting, the engine brakes
  f64 wheel_torque = 0.0;
  if (driven > 0 && v.shift <= 0.0 && ratio > 0.0) {
    const f64 te = throttle * engine_torque(t, v.rpm);
    const f64 sr = (std::abs(spin) * radius - std::abs(vx)) / std::max(std::abs(vx), 2.0);
    const f64 tc = sr > 0.2 ? std::max(0.3, 1.0 - 2.5 * (sr - 0.2)) : 1.0;
    wheel_torque = te * tc * ratio * t.final_drive * t.efficiency * (v.gear < 0 ? -1.0 : 1.0) / driven;
  }
  const f64 engine_brake = (throttle < 0.02 && v.gear > 0 && driven > 0) ? 0.12 * t.torque * ratio * t.final_drive * (v.rpm / t.redline) / driven : 0.0;
  // steering: less at speed, eased
  const f64 target = in.steer * t.max_steer / (1.0 + std::abs(vx) / 16.0);
  v.steer += std::clamp(target - v.steer, -t.steer_rate * dt, t.steer_rate * dt);
  // (Ackermann: the inner wheel turns more)
  f64 track = 1.5, base = 2.6;
  {
    f64 xf = -1e9, xr = 1e9;
    for (const WheelSlot& s : m.wheels) {
      xf = std::max(xf, s.mount.x);
      xr = std::min(xr, s.mount.x);
      track = 2.0 * std::abs(s.mount.y);
    }
    base = std::max(1.0, xf - xr);
  }
  f64 inner = v.steer, outer = v.steer;
  if (std::abs(v.steer) > 1e-4) {
    const f64 a = std::abs(v.steer);
    const f64 r = base * dm::cos(a) / dm::sin(a);  // (the turn's radius at the axle's middle)
    inner = dm::atan2(base, std::max(0.5, r - 0.5 * track));
    outer = dm::atan2(base, r + 0.5 * track);
    if (v.steer < 0.0) {
      inner = -inner;
      outer = -outer;
    }
  }
  // each wheel's input
  for (size_t k = 0; k < n; ++k) {
    if (!on[k]) continue;
    const WheelSlot& s = m.wheels[k];
    const bool front = s.steered || s.mount.x > 0.0;
    f64 bk = brake * t.brake * (front ? 2.0 * t.brake_front : 2.0 * (1.0 - t.brake_front));
    if (in.handbrake && !front) bk = std::max(bk, t.handbrake);
    if (s.driven) bk += engine_brake;
    // (left wheels have y > 0: turning left they are the inner ones)
    const f64 st = s.steered ? ((s.mount.y > 0.0) == (v.steer > 0.0) ? inner : outer) : 0.0;
    world_.set_wheel_input(v.wheels[k], s.driven ? wheel_torque : 0.0, bk, st);
  }
  // anti-roll bars: each axle's pair of wheels (slots 2i, 2i + 1)
  for (size_t k = 0; k + 1 < n; k += 2) {
    if (!on[k] || !on[k + 1] || !ws[k].contact || !ws[k + 1].contact) continue;
    const f64 dc = (ws[k].length - ws[k + 1].length);  // (> 0: the left hangs lower than the right)
    const V3 up = rotate(q, V3{0.0, 0.0, 1.0});
    const V3 f = up * (t.anti_roll * dc);  // (pulls the left down, pushes the right up)
    world_.apply_force(b.id, ws[k].mount, f * -1.0);
    world_.apply_force(b.id, ws[k + 1].mount, f);
  }
  // air drag
  const f64 sp = norm(b.v);
  if (sp > 0.5) world_.apply_force(b.id, b.x, b.v * (-0.5 * 1.225 * t.drag_area * sp));
}

void Game::vehicles_before_tick() {
  sync_vehicles();
  if (vehicles_.empty()) return;
  const f64 dt = world_.config().dt;
  // (the ground the player drives on is there before the car is: what it will reach in the next
  // second and a half, made resident now if the streaming has not yet)
  if (source_ && player_vehicle_) {
    const auto it = vehicles_.find(player_vehicle_);
    const Body* b = it != vehicles_.end() && it->second.chassis ? world_.piece(it->second.chassis) : nullptr;
    if (b) {
      const V3 ahead = b->x + b->v * 1.5;
      const f64 h = world_.voxel_size();
      auto vox = [&](const V3& p) { return IVec3{static_cast<i32>(std::floor(p.x / h)), static_cast<i32>(std::floor(p.y / h)), static_cast<i32>(std::floor(p.z / h))}; };
      const V3 lo{std::min(b->x.x, ahead.x) - 6.0, std::min(b->x.y, ahead.y) - 6.0, -2.0}, hi{std::max(b->x.x, ahead.x) + 6.0, std::max(b->x.y, ahead.y) + 6.0, 3.0};
      world_.ensure_resident(vox(lo), vox(hi));
    }
  }
  for (auto& [id, v] : vehicles_) {
    const Body* b = v.chassis ? world_.piece(v.chassis) : nullptr;
    if (!b) continue;
    if (id != player_vehicle_) {
      if ((v.flags & WheelTag::kTagNpc) && !(v.flags & WheelTag::kTagParked) && !v.wreck)
        steer_driver(v, *b);
      else
        v.input = VehicleInput{0.0, 0.0, 0.0, true};
    }
    // (a vehicle at rest with its handbrake on sleeps: nothing to drive)
    if (b->asleep && id != player_vehicle_) continue;
    drive_vehicle(v, *b, dt);
  }
}

void Game::vehicles_after_tick() {
  if (vehicles_.empty() && !(source_ && source_->roads() && traffic_.enabled)) return;
  sync_vehicles();
  // wrecks: a driven car that hit something hard, rolled over or lost a wheel stops for good
  for (auto& [id, v] : vehicles_) {
    const Body* b = v.chassis ? world_.piece(v.chassis) : nullptr;
    if (!b) continue;
    const Quat q = frame_of(*b);
    const f64 sp = norm(b->v);
    const bool jolt = std::abs(sp - v.hit_speed) > 4.0;
    v.hit_speed = sp;
    if (jolt || (v.flags & WheelTag::kTagParked && sp > 1.0)) v.touched = true;
    bool lost = false;
    for (WheelId w : v.wheels) lost = lost || w == 0;
    const bool rolled = rotate(q, V3{0.0, 0.0, 1.0}).z < 0.4;
    const bool crushed = v.voxels0 > 0 && b->count < v.voxels0 * 0.97;
    if ((v.flags & WheelTag::kTagNpc) && (jolt || lost || rolled || crushed)) v.wreck = true;
  }
  step_traffic();
}

}  // namespace svx
