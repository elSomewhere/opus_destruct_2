// svx_city — voxel_city buildings/sample.js.
#include "buildings/sample.hpp"

#include <array>
#include <cmath>

#include "svx/base/types.hpp"

namespace svx::city {

bool StagedEnvelopes::has(const std::string& id) const {
  std::lock_guard<std::mutex> lk(m_);
  for (const auto& e : list_)
    if (e->id == id) return true;
  return false;
}

void StagedEnvelopes::set(std::shared_ptr<const Envelope> env) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& e : list_)
    if (e->id == env->id) {
      e = std::move(env);
      return;
    }
  list_.push_back(std::move(env));
}

std::vector<std::shared_ptr<const Envelope>> StagedEnvelopes::all() const {
  std::lock_guard<std::mutex> lk(m_);
  return list_;
}

std::vector<std::shared_ptr<const Envelope>> StagedEnvelopes::envelopes_in(const Rect& rect, const std::vector<std::shared_ptr<const Envelope>>& base) const {
  std::lock_guard<std::mutex> lk(m_);
  auto staged = [&](const std::string& id) {
    for (const auto& e : list_)
      if (e->id == id) return true;
    return false;
  };
  std::vector<std::shared_ptr<const Envelope>> res;
  for (const auto& e : base)
    if (!staged(e->id)) res.push_back(e);
  for (const auto& e : list_)
    if (r_overlaps(e->bounds, rect)) res.push_back(e);
  return res;
}

const Envelope* StagePlan::building_by_id(const std::string& id) const {
  for (size_t k = buildings.size(); k-- > 0;)
    if (buildings[k]->id == id) return buildings[k].get();
  return nullptr;
}

std::vector<std::shared_ptr<const Envelope>> stage_archetype(const StageWorld& world, const std::string& archetype_id, const std::string& style_id,
                                                             const StageOpts& opts) {
  const Archetype& arch = archetype_registry().get(archetype_id);
  if (!world.staged || !world.cell_plan) SVX_FAIL("sample: a stage world without its plans or staged map");
  StagedEnvelopes& staged = *world.staged;
  std::vector<std::shared_ptr<const Envelope>> out;
  std::vector<std::array<double, 2>> cells;
  for (double r = 0; r <= opts.radius; r += 1)
    for (double j = -r; j <= r; j += 1)
      for (double i = -r; i <= r; i += 1)
        if (js::max(std::fabs(i), std::fabs(j)) == r) cells.push_back({i, j});
  for (const auto& c : cells) {
    if (static_cast<double>(out.size()) >= opts.n) return out;
    const std::shared_ptr<const StagePlan> plan = world.cell_plan(c[0], c[1]);
    for (const Lot& lot : plan->lots) {
      if (static_cast<double>(out.size()) >= opts.n) break;
      const Envelope* orig = !lot.building.empty() ? plan->building_by_id(lot.building) : nullptr;
      if (!orig || staged.has(orig->id)) continue;
      if (opts.from) {
        bool listed = false;
        for (const std::string& a : *opts.from)
          if (a == orig->archetype) listed = true;
        if (!listed) continue;
      }
      Lot rec = lot;
      if (opts.lot_rect) {
        const Frame lf(lot.rect, lot.front);
        const std::optional<Rect> sub = opts.lot_rect(lot, lf);
        if (!sub) continue;
        rec.rect = lf.rect_to_world(*sub);
      }
      const Frame f(rec.rect, rec.front);
      if (!arch.fits(f.U, f.V)) continue;
      District own;
      const District* d = opts.district;
      if (!d) {
        own.id = lot.district;
        own.floors = {2, 3};
        d = &own;
      }
      Rng rng = Rng::from(world.seed, lot.id, "stage", archetype_id);
      EnvelopeExtra extra;
      extra.u = lot.u;
      extra.core = lot.core;
      extra.ground_z = lot.ground_z;
      extra.config = world.config;
      std::optional<Envelope> env = plan_building_envelope_as(rec, archetype_id, style_id, *d, rng, extra);
      if (!env) continue;
      // same id as the building it replaces (finalizeEnvelope: `${lot.id}/B`)
      env->id = orig->id;
      env->lot = lot.id;
      if (world.drop_plan) world.drop_plan(env->id);
      std::shared_ptr<const Envelope> e = std::make_shared<const Envelope>(std::move(*env));
      staged.set(e);
      out.push_back(e);
    }
  }
  return out;
}

}  // namespace svx::city
