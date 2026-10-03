#include "svx/anim/behaviour/injuries.hpp"

#include <algorithm>

#include "svx/anim/body/humanoid.hpp"

namespace svx::anim {

const char* zone_name(Zone z) {
  switch (z) {
    case Zone::Head: return "head";
    case Zone::Chest: return "chest";
    case Zone::Gut: return "gut";
    case Zone::Pelvis: return "pelvis";
    case Zone::ArmL: return "armL";
    case Zone::ArmR: return "armR";
    case Zone::LegL: return "legL";
    case Zone::LegR: return "legR";
  }
  return "chest";
}

Zone zone_of_part(i32 part) {
  switch (part) {
    case B::head: return Zone::Head;
    case B::chest: return Zone::Chest;
    case B::spine: return Zone::Gut;
    case B::pelvis: return Zone::Pelvis;
    case B::upperarmL:
    case B::forearmL:
    case B::handL: return Zone::ArmL;
    case B::upperarmR:
    case B::forearmR:
    case B::handR: return Zone::ArmR;
    case B::thighL:
    case B::shinL:
    case B::footL: return Zone::LegL;
    default: return Zone::LegR;
  }
}

namespace {
f64 hurt(const Injury& i) {
  const f64 settle = exp(-i.age / 20.0);
  return (i.lasting + (i.severity - i.lasting) * settle) * std::min(1.0, i.age / 0.35);
}
}  // namespace

// Past `cap` the oldest injury of a zone that has two joins the next one there: their hurt and
// lasting share combine as `update` sums them, so the zone's summary stays what it was.
void Injuries::add(const Injury& i) {
  list.push_back(i);
  while (list.size() > cap) {
    size_t from = list.size(), into = list.size();
    for (size_t a = 0; a < list.size() && from == list.size(); ++a)
      for (size_t b = a + 1; b < list.size(); ++b)
        if (list[b].zone == list[a].zone) {
          from = a;
          into = b;
          break;
        }
    if (from == list.size()) {
      list.pop_front();
      continue;
    }
    const Injury& a = list[from];
    Injury& b = list[into];
    const f64 now = 1 - (1 - hurt(a)) * (1 - hurt(b));
    b.lasting = 1 - (1 - a.lasting) * (1 - b.lasting);
    const f64 ramp = std::max(1e-3, std::min(1.0, b.age / 0.35));
    b.severity = clamp(b.lasting + (now / ramp - b.lasting) / exp(-b.age / 20.0), b.lasting, 1.0);
    b.hold_until = std::max(b.hold_until, b.age + std::max(0.0, a.hold_until - a.age));
    list.erase(list.begin() + std::ptrdiff_t(from));
  }
}

void Injuries::update(f64 dt) {
  legL = legR = armL = armR = trunk = head = 0.0;
  f64 p = 0.0;
  for (Injury& i : list) {
    i.age += dt;
    // the sting fades into the lasting share over half a minute
    const f64 settle = exp(-i.age / 20.0);
    // (the hurt sinks in over a moment: the blow shows first)
    const f64 s = (i.lasting + (i.severity - i.lasting) * settle) * std::min(1.0, i.age / 0.35);
    auto add = [s](f64 v) { return std::min(1.0, v + s * (1.0 - v)); };
    switch (i.zone) {
      case Zone::LegL: legL = add(legL); break;
      case Zone::LegR: legR = add(legR); break;
      case Zone::ArmL: armL = add(armL); break;
      case Zone::ArmR: armR = add(armR); break;
      case Zone::Head: head = add(head); break;
      default: trunk = add(trunk);
    }
    p = std::min(1.0, p + s * 0.6 * (1.0 - p));
  }
  pain = p;
}

const Injury* Injuries::to_hold() const {
  const Injury* best = nullptr;
  for (const Injury& i : list) {
    if (i.age > i.hold_until || i.kind == HitKind::Blunt) continue;
    if (!best || i.severity > best->severity) best = &i;
  }
  return best;
}

}  // namespace svx::anim
