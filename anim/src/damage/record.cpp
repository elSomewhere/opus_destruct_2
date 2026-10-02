#include "svx/anim/damage/record.hpp"
#include "svx/anim/damage/state.hpp"
namespace svx::anim {
std::vector<u8> DamageState::record() const {
  record::Writer w;
  w.integer(1);
  w.integer(physical_wounds_, 1);
  for (const auto& p : state_.parts) {
    for (f64 x : {p.flesh, p.muscle, p.vessel, p.bleeding, p.pain, p.nerve}) w.number(x);
    w.integer(u8(p.bone), 1);
    w.integer(p.lost, 1);
  }
  for (f64 x : {state_.blood, state_.shock, state_.consciousness, state_.breathing, state_.adrenaline}) w.number(x);
  w.integer(u8(state_.cause), 1);
  w.integer(state_.wounds.size());
  for (const auto& p : state_.wounds) {
    w.integer(p.part, 1);
    w.integer(p.bone, 1);
    w.vector(p.rest);
    w.vector(p.normal);
    w.number(p.bleeding);
    w.number(p.age);
    w.number(p.pain);
    w.integer(p.arterial, 1);
  }
  return w.bytes;
}
bool DamageState::restore(std::span<const u8> data) {
  record::Reader r{data};
  if (r.integer() != 1) return false;
  const bool physical = r.integer(1) != 0;
  PhysiologySnapshot s;
  auto unit = [&]() {
    const f64 v = r.number();
    if (v < 0 || v > 1) r.ok = false;
    return v;
  };
  for (auto& p : s.parts) {
    p.flesh = unit();
    p.muscle = unit();
    p.vessel = unit();
    p.bleeding = r.number();
    p.pain = unit();
    p.nerve = unit();
    const auto bone = r.integer(1);
    if (bone > 2) r.ok = false;
    p.bone = BoneState(bone);
    p.lost = r.integer(1) != 0;
  }
  s.blood = r.number();
  s.shock = unit();
  s.consciousness = unit();
  s.breathing = unit();
  s.adrenaline = unit();
  const auto cause = r.integer(1);
  if (cause > 5 || s.blood < 0 || s.blood > 5) r.ok = false;
  s.cause = DeathCause(cause);
  const auto count = r.integer();
  if (count > data.size() / 75) return false;
  for (size_t i = 0; i < count && r.ok; ++i) {
    PersistentWound p;
    p.part = i32(r.integer(1));
    p.bone = i32(r.integer(1));
    p.rest = r.vector();
    p.normal = r.vector();
    p.bleeding = r.number();
    p.age = r.number();
    p.pain = unit();
    p.arterial = r.integer(1) != 0;
    if (p.part >= 16 || p.bone >= 23 || p.bleeding < 0 || p.age < 0) r.ok = false;
    s.wounds.push_back(p);
  }
  if (!r.done()) return false;
  state_ = std::move(s);
  physical_wounds_ = physical;
  injuries_ = Injuries{};
  update(0);
  return true;
}
}  // namespace svx::anim
