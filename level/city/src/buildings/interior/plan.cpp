// svx_city — voxel_city buildings/interior/plan.js (PlanBuilder).
#include "buildings/interior/plan.hpp"

#include "buildings/chamfer.hpp"

namespace svx::city {

std::shared_ptr<FloorGrid> PlanBuilder::new_grid(double f) const {
  const std::vector<Rect>& rects = tier_rects(env, f);
  // (a chamfered corner is cut off every floor from the ground floor up)
  FloorGrid::Cut cut = nullptr;
  if (env.chamfer && f >= 0) cut = [c = *env.chamfer, U = env.U](double u, double v) { return chamfer_cut(c, U, u, v); };
  auto g = std::make_shared<FloorGrid>(env.U, env.V, rects, std::move(cut));
  g->door_extra = env.turn ? door_extra_of(env.turn->yaw) : 0;
  return g;
}

PlanFloor& PlanBuilder::add_floor(double index, std::shared_ptr<FloorGrid> grid, const std::string& kind) {
  PlanFloor rec;
  rec.index = index;
  rec.z = z(index);
  rec.height = h(index);
  rec.grid = std::move(grid);
  rec.kind = kind;
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

}  // namespace svx::city
