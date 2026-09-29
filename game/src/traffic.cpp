// structvox game — traffic (docs/VEHICLES.md): the cars that drive a streamed city's lanes around
// the viewer, and the ones parked at its kerbs.
//
// Every half second the traffic looks at the vehicles around the viewer: drivers and parked cars
// out of range that nothing touched go (a parked car comes back where it was, when its place is
// in range again); where there are fewer than there should be, new ones come - on a lane out of
// sight (beyond near_radius) whose ground is resident, or at a free kerbside place. Wrecks stay:
// the world keeps them with their region.
//
// A driver follows its lane's centre line (pure pursuit: it steers for a point ahead on its path,
// through the junction on the curve to the lane it takes on) at its lane's limit, less on a turn,
// stops for a red light, keeps its distance to what is ahead - a car, rubble, a wall - and gives
// up (a wreck) when it has hit something hard, lost a wheel or rolled over. Everything it does
// follows from the world's state and the tick: a replay drives the same traffic.
#include <algorithm>
#include <cmath>

#include "svx/base/dmath.hpp"
#include "svx/game/game.hpp"

namespace svx {

namespace {

inline u64 mix(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline f64 unit(u64 h) { return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0); }

f64 flat_dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

V3 flat(const V3& v) { return V3{v.x, v.y, 0.0}; }

// A driver's path ahead: its lane from where it is, the curve through the junction onto the next
// lane, the next lane.
struct Path {
  std::vector<V3> pts;  // polyline
  // the point `dist` ahead along it (m), and the tangent there
  V3 at(f64 dist, V3* tangent = nullptr) const {
    for (size_t k = 1; k < pts.size(); ++k) {
      const V3 d = pts[k] - pts[k - 1];
      const f64 l = norm(d);
      if (dist <= l || k + 1 == pts.size()) {
        if (tangent) *tangent = l > 1e-9 ? d * (1.0 / l) : V3{1, 0, 0};
        return pts[k - 1] + d * (l > 1e-9 ? std::min(dist, l) / l : 0.0);
      }
      dist -= l;
    }
    return pts.empty() ? V3{} : pts.back();
  }
};

// The quadratic curve from a lane's end to the next lane's start, through where the two lines meet
// (a straight on: the line between them).
void junction_curve(const Lane& from, const Lane& to, std::vector<V3>& out) {
  const V3 d0 = normalized(flat(from.b - from.a)), d1 = normalized(flat(to.b - to.a));
  const V3 p0 = from.b, p2 = to.a;
  const f64 cr = d0.x * d1.y - d0.y * d1.x;
  V3 p1 = (p0 + p2) * 0.5;
  if (std::abs(cr) > 0.2) {
    // (the lines' crossing)
    const V3 w = p2 - p0;
    const f64 t = (w.x * d1.y - w.y * d1.x) / cr;
    p1 = p0 + d0 * t;
  }
  for (int i = 1; i <= 8; ++i) {
    const f64 u = i / 8.0;
    out.push_back(p0 * ((1 - u) * (1 - u)) + p1 * (2 * u * (1 - u)) + p2 * (u * u));
  }
}

}  // namespace

void Game::steer_driver(Vehicle& v, const Body& b) {
  const RoadNetwork* roads = source_ ? source_->roads() : nullptr;
  v.input = VehicleInput{0.0, 1.0, 0.0, false};
  if (!roads) return;
  const Quat q = b.lattice_rot(0);
  const V3 fwd = rotate(q, V3{1.0, 0.0, 0.0});
  const f64 vx = dot(b.v, fwd);
  const V3 p = b.x;
  // its lane: the one it is on, heading its way
  Lane L;
  if (v.lane == 0 || !roads->lane(v.lane, &L)) {
    std::vector<Lane> near;
    roads->lanes_in(p - V3{8, 8, 0}, p + V3{8, 8, 0}, near);
    f64 best = 1e300;
    for (const Lane& c : near) {
      const V3 d = normalized(flat(c.b - c.a));
      if (dot(d, flat(fwd)) < 0.6) continue;
      const f64 len = flat_dist(c.a, c.b);
      const f64 s = dot(flat(p - c.a), d);
      if (s < -2.0 || s > len + 1.0) continue;
      const V3 off = flat(p - c.a) - d * s;
      const f64 score = norm(off);
      if (score < best && score < 6.0) {
        best = score;
        L = c;
      }
    }
    if (best >= 1e300) return;  // (no lane here: it waits, braked)
    v.lane = L.id;
    v.next = 0;
  }
  const V3 dir = normalized(flat(L.b - L.a));
  const f64 len = flat_dist(L.a, L.b);
  const f64 s = dot(flat(p - L.a), dir);
  // the lane it takes on, chosen as it nears the junction (straight on most of the time)
  Lane N;
  bool has_next = v.next != 0 && roads->lane(v.next, &N);
  if (!has_next && s > len - 30.0) {
    std::vector<std::pair<u64, int>> opts;
    roads->next(v.lane, opts);
    if (!opts.empty()) {
      const f64 r = unit(mix(static_cast<u64>(v.id) * 0x9E3779B97F4A7C15ull ^ v.lane));
      std::vector<f64> w;
      f64 total = 0.0;
      for (const auto& o : opts) {
        w.push_back(o.second == 0 ? 3.0 : 1.2);
        total += w.back();
      }
      f64 acc = 0.0;
      size_t pick = opts.size() - 1;
      for (size_t k = 0; k < opts.size(); ++k) {
        acc += w[k] / total;
        if (r < acc) {
          pick = k;
          break;
        }
      }
      v.next = opts[pick].first;
      has_next = roads->lane(v.next, &N);
    }
  }
  // (past the lane's end, on the next lane: that is its lane now)
  if (has_next) {
    const V3 nd = normalized(flat(N.b - N.a));
    if (dot(flat(p - N.a), nd) > 0.5 && s > len - 1.0) {
      v.lane = v.next;
      v.next = 0;
      L = N;
      has_next = false;
      return steer_driver(v, b);
    }
  }
  // its path ahead
  Path path;
  path.pts.push_back(L.a + dir * std::clamp(s, 0.0, len));
  path.pts.push_back(L.b);
  f64 turn = 0.0;  // (the junction's curvature ahead)
  if (has_next) {
    junction_curve(L, N, path.pts);
    path.pts.push_back(N.b);
    const V3 nd = normalized(flat(N.b - N.a));
    const f64 cr = dir.x * nd.y - dir.y * nd.x;
    if (std::abs(cr) > 0.5) turn = 1.0 / std::max(4.0, 0.5 * flat_dist(L.b, N.a) + 2.0);
  }
  // steering: pure pursuit on a point ahead
  const f64 speed = std::abs(vx);
  const f64 look = std::clamp(4.0 + 0.55 * speed, 5.0, 18.0);
  const V3 target = path.at(look);
  const V3 rel = target - p;
  const V3 left = rotate(q, V3{0.0, 1.0, 0.0});
  const f64 xl = dot(rel, fwd), yl = dot(rel, left);
  const f64 kappa = 2.0 * yl / std::max(1.0, xl * xl + yl * yl);
  const VehicleModel& m = vehicle_model(v.spec.kind);
  f64 base = 2.6;
  {
    f64 xf = -1e9, xr = 1e9;
    for (const WheelSlot& ws : m.wheels) {
      xf = std::max(xf, ws.mount.x);
      xr = std::min(xr, ws.mount.x);
    }
    base = std::max(1.0, xf - xr);
  }
  const f64 angle = dm::atan(base * kappa);
  const f64 steer = std::clamp(angle * (1.0 + speed / 16.0) / std::max(0.1, m.tuning.max_steer), -1.0, 1.0);
  // speed: its lane's limit, less for the turn ahead, a red light, what is ahead
  f64 want = L.speed * traffic_.speed_scale;
  const f64 to_end = len - s;
  if (turn > 0.0) {
    const f64 v_turn = std::sqrt(3.2 / turn);
    want = std::min(want, std::sqrt(v_turn * v_turn + 2.0 * 2.5 * std::max(0.0, to_end)));
  }
  if (!roads->green(v.lane, world_.time()) && to_end > -1.0) {
    const f64 d = to_end - 1.5;  // (its stop line)
    want = std::min(want, d > 0.0 ? std::sqrt(2.0 * 3.0 * d) : 0.0);
  }
  // (what is ahead: the other vehicles in its path)
  f64 gap = 1e9;
  for (const auto& [oid, o] : vehicles_) {
    if (oid == v.id || !o.chassis) continue;
    const Body* ob = world_.piece(o.chassis);
    if (!ob) continue;
    const V3 d = ob->x - p;
    const f64 ahead = dot(d, fwd), side = dot(d, left);
    if (ahead > 0.0 && ahead < 45.0 && std::abs(side) < 2.4) gap = std::min(gap, ahead);
  }
  // (rubble, a wall: a ray from its front)
  const VehicleModel& mm = m;
  const V3 nose = b.lattice_to_world(0, V3{mm.half_extent.x + 0.25, 0.0, 0.6});
  const RayHit hit = world_.raycast(nose, fwd, 30.0);
  if (hit.hit && hit.piece != v.chassis && std::abs(hit.normal.z) < 0.7) gap = std::min(gap, hit.distance + mm.half_extent.x);
  if (gap < 1e8) {
    const f64 room = gap - mm.half_extent.x - 4.5;
    want = std::min(want, room > 0.0 ? std::sqrt(2.0 * 3.5 * room) : 0.0);
  }
  // (ground not resident yet ahead - the edge of the streamed world: it stops short of it)
  {
    const V3 ahead = p + fwd * (8.0 + 0.5 * speed * speed / 3.0);
    const f64 h = world_.voxel_size();
    const IVec3 c = chunk_of(IVec3{static_cast<i32>(std::floor(ahead.x / h + 0.5)), static_cast<i32>(std::floor(ahead.y / h + 0.5)), -1});
    if (!world_.chunk_resident(c)) want = 0.0;
  }
  // pedals
  VehicleInput in;
  in.steer = steer;
  const f64 e = want - vx;
  if (want < 0.3 && vx < 0.6) {
    in.brake = 1.0;
  } else if (e > 0.0) {
    in.throttle = std::clamp(0.25 * e + 0.15, 0.0, 1.0);
  } else if (e < -0.4) {
    in.brake = std::clamp(-0.35 * e, 0.0, 1.0);
  }
  // (stuck - it means to go and does not - for long: it gives up)
  if (want > 2.0 && speed < 0.4) {
    v.stuck += world_.config().dt;
    if (v.stuck > 8.0) v.wreck = true;
  } else {
    v.stuck = 0.0;
  }
  v.input = in;
}

void Game::step_traffic() {
  const RoadNetwork* roads = source_ ? source_->roads() : nullptr;
  traffic_clock_ += world_.config().dt;
  if (!roads || traffic_clock_ < 0.5) return;
  traffic_clock_ = 0.0;
  // (about the player's car when they drive: the host's viewer may lag behind it)
  const Body* car = player_car();
  const V3 at = car ? car->x : viewer_;
  // (within the resident ground, and gone before they reach its edge: beyond it there is
  // nothing yet to drive on)
  const f64 radius = std::max(10.0, std::min(traffic_.radius, stream_.load_radius - 20.0));
  const f64 near = std::min(traffic_.near_radius, radius - 10.0);
  const f64 far = std::min(radius + 20.0, stream_.load_radius - 5.0);
  // out of range, untouched: gone (a parked car comes back when its place is in range again)
  std::vector<u32> gone;
  i32 driving = 0, parked = 0;
  std::vector<V3> taken;  // (where vehicles are: not spawned on top of)
  for (const auto& [id, v] : vehicles_) {
    const Body* b = v.chassis ? world_.piece(v.chassis) : nullptr;
    const V3 pos = b ? b->x : v.home;
    taken.push_back(pos);
    if (id == player_vehicle_ || !(v.flags & WheelTag::kTagNpc)) continue;
    const f64 d = flat_dist(pos, at);
    if (d > far && !v.touched && !v.wreck) {
      gone.push_back(id);
      continue;
    }
    if (d > far) continue;
    if (v.flags & WheelTag::kTagParked)
      ++parked;
    else if (!v.wreck)
      ++driving;
  }
  for (u32 id : gone) {
    const auto it = vehicles_.find(id);
    if (it == vehicles_.end()) continue;
    if (it->second.spot) parked_spots_.erase(it->second.spot);
    remove_vehicle_bodies(it->second);
    vehicles_.erase(it);
  }
  if (!traffic_.enabled) return;
  auto free_at = [&](const V3& p, f64 r) {
    for (const V3& t : taken)
      if (flat_dist(t, p) < r) return false;
    return true;
  };
  auto resident = [&](const V3& p) {
    const f64 h = world_.voxel_size();
    const IVec3 c = chunk_of(IVec3{static_cast<i32>(std::floor(p.x / h + 0.5)), static_cast<i32>(std::floor(p.y / h + 0.5)),
                                   static_cast<i32>(std::floor(p.z / h + 0.5)) - 1});
    return world_.chunk_resident(c) && world_.chunk_resident({c[0], c[1], c[2] + 1});
  };
  const u64 salt = mix(static_cast<u64>(world_.ticks()) * 0xD1B54A32D192ED03ull);
  auto pick_spec = [&](u64 h) {
    VehicleSpec s;
    const f64 r = unit(h);
    s.kind = r < 0.38 ? VehicleKind::Sedan : r < 0.68 ? VehicleKind::Compact : r < 0.82 ? VehicleKind::Van : r < 0.95 ? VehicleKind::Pickup : VehicleKind::Truck;
    s.paint = kCarPaints[size_t(mix(h ^ 0x51ED) % kCarPaints.size())];
    return s;
  };
  // drivers: on a lane out of sight, its ground resident, room around it
  if (driving < traffic_.cars) {
    std::vector<Lane> lanes;
    const V3 r{radius, radius, 0.0};
    roads->lanes_in(at - r, at + r, lanes);
    std::vector<std::pair<V3, const Lane*>> spots;
    for (const Lane& l : lanes) {
      const f64 len = flat_dist(l.a, l.b);
      for (f64 u : {0.25, 0.6}) {
        const V3 p = l.a + (l.b - l.a) * u;
        const f64 d = flat_dist(p, at);
        if (len < 12.0 || d < near || d > radius) continue;
        spots.push_back({p, &l});
      }
    }
    if (!spots.empty()) {
      const u64 h = mix(salt ^ 0xCA75);
      const auto& [p, l] = spots[size_t(h % spots.size())];
      if (free_at(p, 12.0) && resident(p)) {
        const V3 d = l->b - l->a;
        const u32 id = spawn_vehicle_internal(pick_spec(mix(h ^ 0x1234)), p, dm::atan2(d.y, d.x), WheelTag::kTagNpc);
        if (id) {
          Vehicle& v = vehicles_[id];
          v.lane = l->id;
          v.input.handbrake = false;
          taken.push_back(p);
        }
      }
    }
  }
  // parked cars: a quarter of the kerbside places (the same ones every time)
  if (parked < traffic_.parked) {
    std::vector<ParkingSpot> spots;
    const V3 r{radius, radius, 0.0};
    roads->parking_in(at - r, at + r, spots);
    i32 budget = 2;
    for (const ParkingSpot& s : spots) {
      if (budget <= 0 || parked >= traffic_.parked) break;
      const f64 d = flat_dist(s.pos, at);
      if (d < near || d > radius) continue;
      const u64 h = mix(s.id ^ 0x9A4C);
      if (h % 4 != 0 || parked_spots_.count(s.id)) continue;
      if (!free_at(s.pos, 4.0) || !resident(s.pos)) continue;
      VehicleSpec spec = pick_spec(mix(h ^ 0x77));
      if (spec.kind == VehicleKind::Truck) spec.kind = VehicleKind::Van;
      const u32 id = spawn_vehicle_internal(spec, s.pos, s.yaw, WheelTag::kTagNpc | WheelTag::kTagParked);
      if (!id) continue;
      vehicles_[id].home = s.pos;
      vehicles_[id].spot = s.id;
      parked_spots_.insert(s.id);
      taken.push_back(s.pos);
      ++parked;
      --budget;
    }
  }
}

}  // namespace svx
