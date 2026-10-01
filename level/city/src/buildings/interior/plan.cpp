// svx_city — voxel_city buildings/interior/plan.js.
#include "buildings/interior/plan.hpp"

#include <map>
#include <set>
#include <utility>

#include "buildings/chamfer.hpp"
#include "buildings/interior/apartments.hpp"
#include "buildings/interior/cabins.hpp"
#include "buildings/interior/civicPrograms.hpp"
#include "buildings/interior/garage.hpp"
#include "buildings/interior/houses.hpp"
#include "buildings/interior/industrial.hpp"
#include "buildings/interior/offices.hpp"
#include "buildings/interior/school.hpp"
#include "core/obb.hpp"
#include "network/roadLevel.hpp"
#include "network/roadView.hpp"
#include "svx/base/types.hpp"
#include "world/World.hpp"
#include "world/caches.hpp"

namespace svx::city {

// ---- BuildingPlan

const PlanFloor* BuildingPlan::floor_by_index(double index) const {
  for (const PlanFloor& f : floors)
    if (f.index == index) return &f;
  return nullptr;
}

PlanFloor* BuildingPlan::floor_by_index(double index) {
  for (PlanFloor& f : floors)
    if (f.index == index) return &f;
  return nullptr;
}

// ---- PlanBuilder

std::shared_ptr<FloorGrid> PlanBuilder::new_grid(double f) const {
  const std::vector<Rect>& rects = tier_rects(env, f);
  // (a chamfered corner is cut off every floor from the ground floor up)
  FloorGrid::Cut cut = nullptr;
  if (env.chamfer && f >= 0) cut = [c = *env.chamfer, U = env.U](double u, double v) { return chamfer_cut(c, U, u, v); };
  auto g = std::make_shared<FloorGrid>(env.U, env.V, rects, std::move(cut));
  g->door_extra = env.turn ? door_extra_of(env.turn->yaw) : 0;
  return g;
}

PlanFloor& PlanBuilder::add_floor(double index, std::shared_ptr<FloorGrid> grid, const std::string& kind, const FloorExtra& extra) {
  PlanFloor rec;
  rec.index = index;
  rec.z = z(index);
  rec.height = h(index);
  rec.grid = std::move(grid);
  rec.kind = kind;
  // ({...extra}: what it gives replaces the record's)
  if (extra.z) rec.z = *extra.z;
  if (extra.height) rec.height = *extra.height;
  rec.low_regions = extra.low_regions;
  rec.mezzanine = extra.mezzanine;
  floors.push_back(std::move(rec));
  return floors.back();
}

std::shared_ptr<Stair> PlanBuilder::add_stair(Stair st) {
  st.id = static_cast<double>(stairs.size());
  if (!st.flights) {
    std::vector<StairFlight> flights;
    for (double f = st.f0; f < st.f1; f += 1) flights.push_back({f, z(f), h(f)});
    st.flights = std::move(flights);
  }
  auto p = std::make_shared<Stair>(std::move(st));
  stairs.push_back(p);
  return p;
}

Ramp PlanBuilder::add_ramp(Ramp r) {
  r.id = static_cast<double>(ramps.size());
  ramps.push_back(r);
  return r;
}

Elevator PlanBuilder::add_elevator(Elevator el) {
  el.id = static_cast<double>(elevators.size());
  elevators.push_back(el);
  return el;
}

BuildingPlan PlanBuilder::build() {
  js::sort(floors, [](const PlanFloor& a, const PlanFloor& b) { return js::or_(a.z - b.z, a.index - b.index); });
  // ramps a floor has to draw: its own (rising from it) and the one arriving from below
  for (PlanFloor& f : floors) {
    std::vector<Ramp> rs;
    for (const Ramp& r : ramps)
      if (r.f == f.index || r.f + 1 == f.index) rs.push_back(r);
    if (!rs.empty()) f.ramps = std::move(rs);
  }
  BuildingPlan plan;
  plan.floors = std::move(floors);
  plan.stairs = std::move(stairs);
  plan.ramps = std::move(ramps);
  plan.links = std::move(links);
  plan.elevators = std::move(elevators);
  plan.issues = std::move(issues);
  floors.clear();
  stairs.clear();
  ramps.clear();
  links.clear();
  elevators.clear();
  issues.clear();
  validate_plan(plan);
  return plan;
}

// ---- validatePlan

namespace {

// UNREACHABLE_OK: rooms nobody needs to reach
bool unreachable_ok(const std::string& type) { return type == "shaft" || type == "elevator" || type == "mechanicalShaft" || type == "void"; }

using Node = std::pair<double, double>;  // [floor index, room id] (JS's `${fi}:${rid}` keys)

}  // namespace

const std::vector<PlanIssue>& validate_plan(BuildingPlan& plan) {
  std::set<Node> seen;  // (membership only)
  std::vector<Node> queue;
  // floorsByIdx: new Map(plan.floors.map((f) => [f.index, f])) - the last floor of an index
  auto floor_of = [&](double fi) -> const PlanFloor* {
    const PlanFloor* out = nullptr;
    for (const PlanFloor& f : plan.floors)
      if (f.index == fi) out = &f;
    return out;
  };
  auto visit = [&](double fi, double rid) {
    if (seen.insert({fi, rid}).second) queue.push_back({fi, rid});
  };
  for (const PlanFloor& f : plan.floors)
    for (const auto& d : f.grid->doors)
      if (d->b == -1 && d->kind != "window" && d->kind != "balcony" && f.index == 0) visit(f.index, d->a);
  // extra links (ramps) in both directions
  std::map<Node, std::vector<Node>> link_adj;
  for (const PlanLink& l : plan.links) {
    link_adj[{l.fa, l.ra}].push_back({l.fb, l.rb});
    link_adj[{l.fb, l.rb}].push_back({l.fa, l.ra});
  }
  // adjacency per grid (cached): the door graph, elevator doors closed
  std::vector<std::pair<const FloorGrid*, DoorGraph>> adj_cache;
  auto adj_of = [&](const FloorGrid& grid) -> const DoorGraph& {
    for (const auto& e : adj_cache)
      if (e.first == &grid) return e.second;
    DoorGraph a = grid.door_graph();
    for (const auto& d : grid.doors) {
      if (d->kind == "elevator") {
        a.erase(d->a, d->b);
        a.erase(d->b, d->a);
      }
    }
    adj_cache.emplace_back(&grid, std::move(a));
    return adj_cache.back().second;
  };
  while (!queue.empty()) {
    const Node n = queue.back();
    queue.pop_back();
    const double fi = n.first, rid = n.second;
    const PlanFloor* f = floor_of(fi);
    if (!f || rid < 0 || rid >= static_cast<double>(f->grid->rooms.size())) SVX_FAIL("validatePlan: a room that is not on its floor");
    const Room& room = *f->grid->rooms[static_cast<size_t>(rid)];
    const DoorGraph& adj = adj_of(*f->grid);
    if (const std::vector<double>* nbs = adj.get(rid)) {
      const std::vector<double> list = *nbs;  // (adj_cache may grow below: copy)
      for (double nb : list) {
        if (nb < 0) continue;
        visit(fi, nb);
      }
    }
    if (room.type == "stair") {
      if (!room.stair || *room.stair < 0 || *room.stair >= static_cast<double>(plan.stairs.size())) SVX_FAIL("validatePlan: a stairwell without its stair");
      const Stair& st = *plan.stairs[static_cast<size_t>(*room.stair)];
      for (double g : {fi - 1, fi + 1}) {
        if (g < st.f0 || g > st.f1) continue;
        const PlanFloor* gf = floor_of(g);
        if (!gf) continue;
        const Room* other = nullptr;
        for (const auto& r : gf->grid->rooms)
          if (r->type == "stair" && r->stair == room.stair) {
            other = r.get();
            break;
          }
        if (!other) continue;
        visit(g, other->id);
      }
    }
    auto it = link_adj.find({fi, rid});
    if (it != link_adj.end())
      for (const Node& q : it->second) visit(q.first, q.second);
    // mezzanines: rooms flagged `linkUp` connect to the matching room above
    if (room.link_to) visit(room.link_to->floor, room.link_to->room);
  }
  for (const PlanFloor& f : plan.floors) {
    for (const auto& r : f.grid->rooms) {
      if (unreachable_ok(r->type)) continue;
      if (!seen.count({f.index, r->id})) plan.issues.push_back({f.index, r->id, r->type, "unreachable"});
    }
  }
  return plan.issues;
}

// ---- PLANNERS
//
// (JS looks the planner up in an object literal, PLANNERS[env.archetype]: an archetype id named
// like a member of Object.prototype - "constructor", "toString" ... - would find a function there.
// No such archetype is registered: the port knows the planners' ids alone.)

bool has_planner(const std::string& a) {
  return a == "walkup" || a == "midrise" || a == "tower" || a == "office" || a == "house" || a == "rowhouse" || a == "warehouse" || a == "factory" ||
         a == "barn" || a == "panelSlab" || a == "panelTower" || a == "garage" || a == "school" || a == "townhouse" || a == "wharfhouse" ||
         a == "cabin" || a == "church" || has_civic_planner(a);
}

bool run_planner(const Envelope& env, Rng& rng, PlanBuilder& pb) {
  const std::string& a = env.archetype;
  if (a == "walkup" || a == "midrise" || a == "panelSlab" || a == "panelTower" || a == "wharfhouse")
    plan_apartment_building(env, rng, pb);
  else if (a == "tower") {
    if (env.program.upper == "apartments")
      plan_apartment_building(env, rng, pb);
    else
      plan_office_building(env, rng, pb);
  } else if (a == "office")
    plan_office_building(env, rng, pb);
  else if (a == "house" || a == "rowhouse")
    plan_house(env, rng, pb);
  else if (a == "warehouse" || a == "factory" || a == "barn")
    plan_industrial(env, rng, pb);
  else if (a == "garage")
    plan_garage(env, rng, pb);
  else if (a == "school")
    plan_school(env, rng, pb);
  else if (a == "townhouse") {
    // nordic: town houses are one family house or small flats (shop below)
    if (env.program.upper == "house")
      plan_house(env, rng, pb);
    else
      plan_apartment_building(env, rng, pb);
  } else if (a == "cabin")
    plan_cabin(env, rng, pb);
  else if (a == "church")
    plan_church(env, pb);
  else
    // civic buildings, venues and big shops (civicPrograms.js)
    return run_civic_planner(a, env, rng, pb);
  return true;
}

// ---- planBuilding

namespace {

// SILL_MAX: the most a street door's sill is raised above its floor (voxels): steeper, the door
// stays as it is.
constexpr double kSillMax = 8;

// The street in front of each ground-floor door to it (a building on a slope: one floor level, a
// street climbing along its front): where it is off the building's own level (env.groundZ, the
// street's in front of the lot's middle) by more than a voxel, the door records it (d.street, the
// top of the sidewalk or road 6 cells out, as cellPlan streetDoorLevels measures it). Where it
// stands higher than a walker can step up from the floor, the door is raised to it (d.sill, voxels
// over the floor, steps down inside, fixtures.js); where it lies lower, the steps up outside start
// from it (entranceSteps).
void street_levels(const World& world, const Envelope& env, BuildingPlan& plan) {
  PlanFloor* F = plan.floor_by_index(0);
  // (a World always has roadView and cellAt)
  if (!F) return;
  const Frame& frame = envelope_frame(env);
  for (const auto& dp : F->grid->doors) {
    Door& d = *dp;
    if (d.b != -1 || d.kind == "balcony" || d.kind == "window") continue;
    // (out from the wall the door is in, 6 cells)
    double u = (d.u0 + d.u1 + 1) / 2;
    double v = (d.v0 + d.v1 + 1) / 2;
    if (d.orient == 'h')
      v = d.v0 <= 1 ? d.v0 - 6 : d.v1 + 7;
    else
      u = d.u0 <= 1 ? d.u0 - 6 : d.u1 + 7;
    const XY p = frame.turned ? frame.point_to_world(u, v) : local_point_to_world(frame.placement, u, v);
    const double x = p[0], y = p[1];
    const CellIJ c = world.cell_at(x, y);
    const std::shared_ptr<const RoadView> view = world.road_view(c.i, c.j);
    const std::optional<RoadLevel> r = road_level_at(world, *view, x, y, 24);
    if (!r) continue;
    const double L = js::round(r->z) + (r->sidewalk ? 1 : 0);
    if (std::fabs(L - env.ground_z) <= 1) continue;
    d.street = L;
    // (feet on the street L + 1, on the floor F.z + 2)
    const double rise = L - F->z - 1;
    // (under the ceiling a walker's 1.75 m over the sill: 14 voxels, and the slab and a lintel)
    if (rise > 2 && rise <= js::min(kSillMax, F->height - 18)) d.sill = rise;
  }
}

}  // namespace

std::shared_ptr<const BuildingPlan> plan_building(const World& world, const Envelope& env) {
  Rng rng = Rng::from(world.seed, env.id, "interior");
  PlanBuilder pb(env, rng);
  if (!run_planner(env, rng, pb)) return nullptr;
  auto plan = std::make_shared<BuildingPlan>(pb.build());
  street_levels(world, env, *plan);
  return plan;
}

RoomAtCell room_at_cell(const PlanFloor& floor, double u, double v) {
  const int l = floor.grid->get(u, v);
  RoomAtCell out;
  if (l >= FloorGrid::ROOM0) {
    const size_t i = static_cast<size_t>(l - FloorGrid::ROOM0);
    if (i < floor.grid->rooms.size()) out.room = floor.grid->rooms[i].get();
    return out;
  }
  out.door = l == FloorGrid::DOOR;
  return out;
}

// createWorld.js: world.buildingPlan(env), the World's cache of plans.
std::shared_ptr<const BuildingPlan> World::building_plan(const Envelope& env) const {
  return caches().building_plans.get(js::cat(env.id, "@", env.R.x0, ",", env.R.y0),
                                     [&]() -> std::shared_ptr<const BuildingPlan> { return plan_building(*this, env); });
}

}  // namespace svx::city
