// structvox game — vehicles (docs/VEHICLES.md): their voxel models, drivetrains, and how a game
// drives them.
//
// A vehicle is a chassis of 6.25 cm voxels (sheet metal over a frame, an engine block, glass,
// plastic bumpers, lamps; painted in the "paint" layer) dropped into the world as a grid of its
// own, on the core's cast wheels (World::add_wheel). The core does the physics - suspension,
// tyres, crumpling crash damage, wheels torn off, walls broken through - and the game drives it:
// an engine and gearbox turning its driven wheels, brakes, steering, drag. Its wheels carry the
// vehicle's id (their group) and what they are to it (their tag: kind, paint, place), so the game
// finds its vehicles again after a saved session is loaded, or when one comes back from the
// streaming archive.
#pragma once

#include <array>
#include <vector>

#include "svx/base/vec.hpp"
#include "svx/world/grid.hpp"

namespace svx {

// The "paint" layer's values: a voxel's colour over its material's (0: its material's). Cars'
// paints, facades' plasters and the road's markings; the front end has their colours.
enum class Paint : u8 {
  None = 0,
  // car paints
  White, Silver, Black, Red, Blue, Green, Yellow, Orange, TaxiYellow, NavyBlue, Maroon, Beige, Graphite, Teal,
  // trim: bumpers, grilles, tail lamps, indicators
  Trim, TailRed, Amber,
  // facades
  Plaster, Cream, Terracotta, Sand, Slate, Ochre, Mint,
  // the road
  LineWhite, LineYellow, Kerb,
  Count
};

// A painted voxel's texture id for the renderer: kPaintTexture + its paint (palette slot 31 +
// paint: after the materials').
inline constexpr u16 kPaintTexture = 0xFF00 + 31;

// The paints a car is sprayed in (at random).
inline constexpr std::array<Paint, 14> kCarPaints = {Paint::White,  Paint::Silver,     Paint::Black,    Paint::Red,    Paint::Blue,
                                                     Paint::Green,  Paint::Yellow,     Paint::Orange,   Paint::NavyBlue, Paint::Maroon,
                                                     Paint::Beige,  Paint::Graphite,   Paint::Teal,     Paint::TaxiYellow};

enum class VehicleKind : u8 { Compact = 0, Sedan = 1, Van = 2, Pickup = 3, Truck = 4, Count };

struct VehicleSpec {
  VehicleKind kind = VehicleKind::Sedan;
  Paint paint = Paint::Red;
};

// How a vehicle drives: its engine, gearbox, brakes, steering and body.
struct VehicleTuning {
  f64 torque = 280.0;          // N m: the engine's peak torque ...
  f64 peak_rpm = 4200.0;       // ... at this speed
  f64 idle_rpm = 850.0, redline = 6500.0;
  std::array<f64, 6> gears{3.4, 2.1, 1.45, 1.1, 0.87, 0.72};
  int ngears = 6;
  f64 reverse = 3.2, final_drive = 3.9;
  f64 efficiency = 0.85;       // of the driveline
  f64 shift_time = 0.25;       // s without drive while shifting
  f64 brake = 2600.0;          // N m: each wheel's brake at full pedal ...
  f64 brake_front = 0.6;       // ... the front's share of the braking (x 2 wheels)
  f64 handbrake = 3000.0;      // N m on the rear wheels
  f64 max_steer = 0.62;        // rad at walking pace (less at speed)
  f64 steer_rate = 2.8;        // rad/s
  f64 drag_area = 0.7;         // m^2: Cd x A
  f64 anti_roll = 14e3;        // N/m per axle
};

// A wheel of a vehicle model, in its frame (x forward, y left, z up; the origin on the ground
// under the middle between its axles).
struct WheelSlot {
  V3 mount;                    // the top of its suspension (a voxel of the model is there)
  f64 radius = 0.32, width = 0.22;
  f64 rest = 0.5, travel = 0.2, stiffness = 32e3, damping = 3.2e3, inertia = 1.2;
  bool driven = false, steered = false;
};

struct VehicleModel {
  VehicleSpec spec;
  VoxelGrid voxels;            // (voxel size kVehicleVoxel; voxel p centred at h p in its frame; a "paint" layer)
  std::vector<WheelSlot> wheels;
  VehicleTuning tuning;
  f64 wheel_break = 0.0;       // N: its wheels come off beyond (0: never)
  V3 half_extent;              // m: its box about the origin (x, y), from the ground (z)
  V3 driver_seat;              // m: where the driver sits (the chase camera looks at it; the player gets out beside it)
};

inline constexpr f64 kVehicleVoxel = 0.0625;

// The model of a vehicle of this spec (deterministic).
VehicleModel build_vehicle(const VehicleSpec& spec);
// A kind's model (built once; its body in a placeholder paint).
const VehicleModel& vehicle_model(VehicleKind kind);

// The player's (or a driver's) controls.
struct VehicleInput {
  f64 throttle = 0.0;  // -1 .. 1 (backwards: reverse, or brake while rolling forward)
  f64 brake = 0.0;     // 0 .. 1
  f64 steer = 0.0;     // -1 (right) .. 1 (left)
  bool handbrake = false;
};

// A wheel's tag (WheelDesc::tag): its place on the vehicle and what the vehicle is.
struct WheelTag {
  u8 slot = 0;
  VehicleKind kind = VehicleKind::Sedan;
  Paint paint = Paint::None;
  u8 flags = 0;        // kTagNpc, kTagParked
  static constexpr u8 kTagNpc = 1, kTagParked = 2;
  u32 pack() const { return u32(slot) | (u32(kind) << 8) | (u32(paint) << 16) | (u32(flags) << 24); }
  static WheelTag unpack(u32 t) {
    WheelTag w;
    w.slot = static_cast<u8>(t & 0xFF);
    w.kind = static_cast<VehicleKind>(std::min<u32>((t >> 8) & 0xFF, u32(VehicleKind::Count) - 1));
    w.paint = static_cast<Paint>(std::min<u32>((t >> 16) & 0xFF, u32(Paint::Count) - 1));
    w.flags = static_cast<u8>(t >> 24);
    return w;
  }
};

}  // namespace svx
