// svx_city — voxel_city city/grading.js.
#include "city/grading.hpp"

#include <cmath>

#include "core/js.hpp"
#include "svx/base/types.hpp"

namespace svx::city {

namespace {

const double kMinBand = vx(1.5);
const double kMaxBand = vx(16);
constexpr double kEase = 2.3;

bool civic_forecourt(const Envelope& env) { return env.extra && !env.extra->civic.empty() && env.extra->set_f > 0; }

}  // namespace

bool level_lot(const Lot& lot, const Envelope* env) {
  (void)lot;
  if (!env) return false;
  if (env->yard || (env->extra && env->extra->parking) || env->archetype == "school" || env->archetype == "wharfhouse") return true;
  for (const EnvelopeAnnex& a : env->annexes)
    if (a.kind == "canopy") return true;
  return false;
}

SiteGrading::SiteGrading(const std::vector<Lot>& lots, const std::function<const Envelope*(const std::string& id)>& building_by_id) {
  const double reach = kApron + kMaxBand;
  for (const Lot& lot : lots) {
    if (lot.building.empty() || lot.underground || lot.under_highway) continue;
    const Envelope* env = building_by_id(lot.building);
    if (!env) continue;
    const bool level = level_lot(lot, env);
    if (level) levels_.insert(&lot);
    // padRects: the building's pad (its footprint, annexes and a civic forecourt), or the lot for a
    // level lot; a turned building's are its canonical rects, measured in its own axes
    Pad pad;
    if (level) {
      pad.rects.push_back({lot.rect, false});
    } else {
      const Frame& frame = envelope_frame(*env);
      if (frame.turned) {
        pad.frame = frame;
        for (const Rect& r : tier_rects(*env, 0)) pad.rects.push_back({r, true});
        for (const EnvelopeAnnex& a : env->annexes) {
          if (a.kind == "pylon") continue;
          if (!a.canon) SVX_FAIL("grading: a turned building's annex without its canonical rect");
          pad.rects.push_back({*a.canon, true});
        }
        if (civic_forecourt(*env)) {
          const Rect& fp = env->tiers[0].rects[0];
          pad.rects.push_back({Rect{fp.x0, fp.y0 - env->extra->set_f, fp.x1, fp.y0}, true});
        }
      } else {
        for (const Rect& r : tier_rects(*env, 0)) pad.rects.push_back({frame.rect_to_world(r), false});
        for (const EnvelopeAnnex& a : env->annexes)
          if (a.kind != "pylon") pad.rects.push_back({a.world, false});
        // a civic building's forecourt (portico steps, porch, banners) is level with its door
        if (civic_forecourt(*env)) {
          const Rect& fp = env->tiers[0].rects[0];
          pad.rects.push_back({frame.rect_to_world(Rect{fp.x0, fp.y0 - env->extra->set_f, fp.x1, fp.y0}), false});
        }
      }
    }
    Rect bb{js::kInf, js::kInf, -js::kInf, -js::kInf};
    for (const PadRect& q : pad.rects) {
      const Rect r = q.turned ? pad.frame->rect_to_world(q.r) : q.r;
      bb.x0 = js::min(bb.x0, r.x0 - reach);
      bb.y0 = js::min(bb.y0, r.y0 - reach);
      bb.x1 = js::max(bb.x1, r.x1 + reach);
      bb.y1 = js::max(bb.y1, r.y1 + reach);
    }
    pad.level = lot.ground_z;
    pad.apron = level ? 0 : kApron;
    pad.block = lot.block;
    grid_.insert(static_cast<uint32_t>(pads_.size()), bb);
    pads_.push_back(std::move(pad));
  }
}

double SiteGrading::at(double x, double y, double base, const std::string& block_id, const Lot* lot) const {
  if (lot && levels_.count(lot)) return lot->ground_z;
  std::vector<uint32_t> found;
  grid_.query_point(x, y, found);
  if (found.empty()) return js::round(base);
  double sw = 0;
  double sd = 0;
  for (const uint32_t k : found) {
    const Pad& p = pads_[k];
    if (p.block != block_id) continue;
    double d = js::kInf;
    for (const PadRect& r : p.rects) {
      double rd;
      if (r.turned) {
        rd = p.frame->distance(r.r, x, y);
      } else {
        const double dx = js::max(r.r.x0 - x, 0.0, x - r.r.x1);
        const double dy = js::max(r.r.y0 - y, 0.0, y - r.r.y1);
        rd = dx == 0 ? dy : dy == 0 ? dx : js::hypot(dx, dy);
      }
      d = js::min(d, rd);
    }
    d -= p.apron;
    if (d <= 0) return p.level;
    const double dz = p.level - base;
    const double band = js::max(kMinBand, js::min(kMaxBand, std::fabs(dz) * kEase));
    if (d >= band) continue;
    const double t = d / band;
    const double w = 1 - t * t * (3 - 2 * t);
    sw += w;
    sd += w * dz;
  }
  return js::round(base + sd / js::max(1.0, sw));
}

}  // namespace svx::city
