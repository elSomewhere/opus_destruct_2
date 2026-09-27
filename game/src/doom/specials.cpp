#include "svx/game/doom/specials.hpp"

namespace svx::doom {

// Vanilla p_spec.c (walk-over, gun-shot), p_switch.c (use) and p_doors.c (manual doors) with
// the speeds and waits of p_doors.c, p_floor.c, p_ceilng.c and p_plats.c: doors 2 units per
// tic (blazing 8), waiting 150 tics; lifts 4 (turbo 8), waiting 105; floors and ceilings 1
// (turbo 4, fast crushers 2); raising platforms and the donut 0.5; perpetual platforms 1,
// waiting 105; stairs 0.25 (turbo 4).
bool special_info(u16 s, SpecialInfo* out) {
  using LT = LineTrigger;
  using PT = PlaneTarget;
  using MK = MoveKind;
  SpecialInfo r;
  auto act = [&](bool ceiling, MK kind, PT target, PT back, f64 speed, i32 wait) -> SpecialAction& {
    SpecialAction& a = r.a[r.n++];
    a.ceiling = ceiling;
    a.kind = kind;
    a.target = target;
    a.back = back;
    a.speed = speed;
    a.wait = wait;
    return a;
  };
  auto trig = [&](LT t, bool once) {
    r.trigger = t;
    r.once = once;
  };
  constexpr f64 kDoor = 2.0, kBlaze = 8.0;
  auto door = [&](f64 v) { act(true, MK::Return, PT::DoorOpen, PT::OwnFloor, v, 150).door = true; };
  auto door_open = [&](f64 v) { act(true, MK::To, PT::DoorOpen, PT::Start, v, 0).door = true; };
  auto door_close = [&](f64 v) { act(true, MK::To, PT::OwnFloor, PT::Start, v, 0).door = true; };
  auto door_close30 = [&]() { act(true, MK::Return, PT::OwnFloor, PT::Start, kDoor, 30 * 35).door = true; };
  auto lift = [&](f64 v) { act(false, MK::Return, PT::LowestFloor, PT::Start, v, 105).lift = true; };
  auto floor_to = [&](PT t, f64 v) { act(false, MK::To, t, PT::Start, v, 0); };
  auto ceil_to = [&](PT t, f64 v) { act(true, MK::To, t, PT::Start, v, 0); };
  auto crusher = [&](f64 v) { act(true, MK::Cycle, PT::OwnFloorPlus8, PT::Start, v, 0); };
  auto perpetual = [&]() { act(false, MK::Cycle, PT::LowestFloor, PT::HighestFloorOrOwn, 1.0, 105); };
  auto stop = [&](bool ceiling) { act(ceiling, MK::Stop, PT::Start, PT::Start, 0.0, 0); };
  switch (s) {
    // manual doors (the line's back sector): open, wait, close / open and stay
    case 1: case 26: case 27: case 28: trig(LT::Manual, false); door(kDoor); break;
    case 117: trig(LT::Manual, false); door(kBlaze); break;
    case 31: case 32: case 33: case 34: trig(LT::Manual, true); door_open(kDoor); break;
    case 118: trig(LT::Manual, true); door_open(kBlaze); break;
    // tagged doors
    case 29: trig(LT::Use, true); door(kDoor); break;
    case 63: trig(LT::Use, false); door(kDoor); break;
    case 111: trig(LT::Use, true); door(kBlaze); break;
    case 114: trig(LT::Use, false); door(kBlaze); break;
    case 4: trig(LT::Walk, true); door(kDoor); break;
    case 90: trig(LT::Walk, false); door(kDoor); break;
    case 108: trig(LT::Walk, true); door(kBlaze); break;
    case 105: trig(LT::Walk, false); door(kBlaze); break;
    case 103: trig(LT::Use, true); door_open(kDoor); break;
    case 61: trig(LT::Use, false); door_open(kDoor); break;
    case 112: case 133: case 135: case 137: trig(LT::Use, true); door_open(kBlaze); break;
    case 115: case 99: case 134: case 136: trig(LT::Use, false); door_open(kBlaze); break;
    case 2: trig(LT::Walk, true); door_open(kDoor); break;
    case 86: trig(LT::Walk, false); door_open(kDoor); break;
    case 109: trig(LT::Walk, true); door_open(kBlaze); break;
    case 106: trig(LT::Walk, false); door_open(kBlaze); break;
    case 46: trig(LT::Shoot, false); door_open(kDoor); break;
    case 50: trig(LT::Use, true); door_close(kDoor); break;
    case 42: trig(LT::Use, false); door_close(kDoor); break;
    case 113: trig(LT::Use, true); door_close(kBlaze); break;
    case 116: trig(LT::Use, false); door_close(kBlaze); break;
    case 3: trig(LT::Walk, true); door_close(kDoor); break;
    case 75: trig(LT::Walk, false); door_close(kDoor); break;
    case 110: trig(LT::Walk, true); door_close(kBlaze); break;
    case 107: trig(LT::Walk, false); door_close(kBlaze); break;
    case 16: trig(LT::Walk, true); door_close30(); break;
    case 76: trig(LT::Walk, false); door_close30(); break;
    // lifts: down to the lowest floor around, wait 3 s, back up
    case 21: trig(LT::Use, true); lift(4.0); break;
    case 62: trig(LT::Use, false); lift(4.0); break;
    case 122: trig(LT::Use, true); lift(8.0); break;
    case 123: trig(LT::Use, false); lift(8.0); break;
    case 10: trig(LT::Walk, true); lift(4.0); break;
    case 88: trig(LT::Walk, false); lift(4.0); break;
    case 121: trig(LT::Walk, true); lift(8.0); break;
    case 120: trig(LT::Walk, false); lift(8.0); break;
    // floors
    case 19: trig(LT::Walk, true); floor_to(PT::HighestFloor, 1.0); break;
    case 83: trig(LT::Walk, false); floor_to(PT::HighestFloor, 1.0); break;
    case 102: trig(LT::Use, true); floor_to(PT::HighestFloor, 1.0); break;
    case 45: trig(LT::Use, false); floor_to(PT::HighestFloor, 1.0); break;
    case 38: case 37: trig(LT::Walk, true); floor_to(PT::LowestFloor, 1.0); break;
    case 82: case 84: trig(LT::Walk, false); floor_to(PT::LowestFloor, 1.0); break;
    case 23: trig(LT::Use, true); floor_to(PT::LowestFloor, 1.0); break;
    case 60: trig(LT::Use, false); floor_to(PT::LowestFloor, 1.0); break;
    case 36: trig(LT::Walk, true); floor_to(PT::TurboFloor, 4.0); break;
    case 98: trig(LT::Walk, false); floor_to(PT::TurboFloor, 4.0); break;
    case 71: trig(LT::Use, true); floor_to(PT::TurboFloor, 4.0); break;
    case 70: trig(LT::Use, false); floor_to(PT::TurboFloor, 4.0); break;
    case 5: trig(LT::Walk, true); floor_to(PT::LowestCeiling, 1.0); break;
    case 91: trig(LT::Walk, false); floor_to(PT::LowestCeiling, 1.0); break;
    case 101: trig(LT::Use, true); floor_to(PT::LowestCeiling, 1.0); break;
    case 64: trig(LT::Use, false); floor_to(PT::LowestCeiling, 1.0); break;
    case 24: trig(LT::Shoot, true); floor_to(PT::LowestCeiling, 1.0); break;
    case 56: trig(LT::Walk, true); floor_to(PT::LowestCeilingMinus8, 1.0); break;
    case 94: trig(LT::Walk, false); floor_to(PT::LowestCeilingMinus8, 1.0); break;
    case 55: trig(LT::Use, true); floor_to(PT::LowestCeilingMinus8, 1.0); break;
    case 65: trig(LT::Use, false); floor_to(PT::LowestCeilingMinus8, 1.0); break;
    case 119: trig(LT::Walk, true); floor_to(PT::NextFloor, 1.0); break;
    case 128: trig(LT::Walk, false); floor_to(PT::NextFloor, 1.0); break;
    case 18: trig(LT::Use, true); floor_to(PT::NextFloor, 1.0); break;
    case 69: trig(LT::Use, false); floor_to(PT::NextFloor, 1.0); break;
    case 130: trig(LT::Walk, true); floor_to(PT::NextFloor, 4.0); break;
    case 129: trig(LT::Walk, false); floor_to(PT::NextFloor, 4.0); break;
    case 131: trig(LT::Use, true); floor_to(PT::NextFloor, 4.0); break;
    case 132: trig(LT::Use, false); floor_to(PT::NextFloor, 4.0); break;
    case 58: case 59: trig(LT::Walk, true); floor_to(PT::Plus24, 1.0); break;
    case 92: case 93: trig(LT::Walk, false); floor_to(PT::Plus24, 1.0); break;
    case 140: trig(LT::Use, true); floor_to(PT::Plus512, 1.0); break;
    // platforms (texture changes left out)
    case 22: trig(LT::Walk, true); floor_to(PT::NextFloor, 0.5); break;
    case 95: trig(LT::Walk, false); floor_to(PT::NextFloor, 0.5); break;
    case 20: trig(LT::Use, true); floor_to(PT::NextFloor, 0.5); break;
    case 68: trig(LT::Use, false); floor_to(PT::NextFloor, 0.5); break;
    case 47: trig(LT::Shoot, true); floor_to(PT::NextFloor, 0.5); break;
    case 14: trig(LT::Use, true); floor_to(PT::Plus32, 0.5); break;
    case 67: trig(LT::Use, false); floor_to(PT::Plus32, 0.5); break;
    case 15: trig(LT::Use, true); floor_to(PT::Plus24, 0.5); break;
    case 66: trig(LT::Use, false); floor_to(PT::Plus24, 0.5); break;
    case 53: trig(LT::Walk, true); perpetual(); break;
    case 87: trig(LT::Walk, false); perpetual(); break;
    case 54: trig(LT::Walk, true); stop(false); break;
    case 89: trig(LT::Walk, false); stop(false); break;
    // ceilings and crushers (crushers wait for the player instead of crushing)
    case 40: trig(LT::Walk, true); ceil_to(PT::HighestCeiling, 1.0); floor_to(PT::LowestFloor, 1.0); break;
    case 41: trig(LT::Use, true); ceil_to(PT::OwnFloor, 1.0); break;
    case 43: trig(LT::Use, false); ceil_to(PT::OwnFloor, 1.0); break;
    case 44: trig(LT::Walk, true); ceil_to(PT::OwnFloorPlus8, 1.0); break;
    case 72: trig(LT::Walk, false); ceil_to(PT::OwnFloorPlus8, 1.0); break;
    case 25: case 141: trig(LT::Walk, true); crusher(1.0); break;
    case 73: trig(LT::Walk, false); crusher(1.0); break;
    case 49: trig(LT::Use, true); crusher(1.0); break;
    case 6: trig(LT::Walk, true); crusher(2.0); break;
    case 77: trig(LT::Walk, false); crusher(2.0); break;
    case 57: trig(LT::Walk, true); stop(true); break;
    case 74: trig(LT::Walk, false); stop(true); break;
    // stairs and the donut
    case 8: trig(LT::Walk, true); floor_to(PT::Stairs8, 0.25); break;
    case 7: trig(LT::Use, true); floor_to(PT::Stairs8, 0.25); break;
    case 100: trig(LT::Walk, true); floor_to(PT::Stairs16, 4.0); break;
    case 127: trig(LT::Use, true); floor_to(PT::Stairs16, 4.0); break;
    case 9: trig(LT::Use, true); floor_to(PT::Donut, 0.5); break;
    default: return false;
  }
  if (out) *out = r;
  return true;
}

}  // namespace svx::doom
