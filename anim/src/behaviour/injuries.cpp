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

void Injuries::add(const Injury& i) { list.push_back(i); }

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
