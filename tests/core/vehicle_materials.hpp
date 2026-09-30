// The core tests' vehicle materials: what a host registers for its cars and roads (the game does:
// svx/game/materials.hpp) - smeared sheet metal and frames that crumple, a solid engine block,
// plastic, tyres, asphalt that wheels grip - at the ids after the standard presets, as the game has them,
// so the tests of the core's generic mechanics (crumpling, wheels, penetration) run on
// representative materials. Call ensure() before making a world that uses them.
#pragma once

#include "svx/material/material.hpp"

namespace testmat {

using svx::f64;
using svx::Material;
using svx::MaterialId;

inline constexpr MaterialId Sheet = static_cast<MaterialId>(svx::kStandardMaterials + 0);
inline constexpr MaterialId CarFrame = static_cast<MaterialId>(svx::kStandardMaterials + 1);
inline constexpr MaterialId Engine = static_cast<MaterialId>(svx::kStandardMaterials + 2);
inline constexpr MaterialId Tyre = static_cast<MaterialId>(svx::kStandardMaterials + 4);
inline constexpr MaterialId Plastic = static_cast<MaterialId>(svx::kStandardMaterials + 5);
inline constexpr MaterialId Asphalt = static_cast<MaterialId>(svx::kStandardMaterials + 6);

inline Material make(const char* name, f64 E, f64 G, f64 rho, f64 ft, f64 fb, f64 fc, f64 coh, f64 mu, f64 Gf, f64 frag, f64 noise, bool ductile) {
  Material m;
  m.name = name;
  m.E = E;
  m.G = G;
  m.rho = rho;
  m.ft = ft;
  m.fb = fb;
  m.fc = fc;
  m.cohesion = coh;
  m.friction = mu;
  m.Gf = Gf;
  m.frag_x = m.frag_y = m.frag_z = frag;
  m.frag_noise = noise;
  m.ductile = ductile;
  return m;
}

// (idempotent: again only if a test reset the process's table)
inline void ensure() {
  svx::MaterialTable& t = svx::default_materials();
  if (t.registered(Asphalt) && t[Asphalt].name == "asphalt") return;
  Material sheet = make("sheet", 3.4e9, 1.3e9, 260, 4e6, 6e6, 3e6, 2.5e6, 0.4, 2e4, 3.5, 0.3, true);
  sheet.crush = 9e4;
  sheet.penetration = 2e4;
  Material frame = make("car_frame", 10e9, 4e9, 700, 18e6, 20e6, 12e6, 10e6, 0.4, 5e4, 4.0, 0.25, true);
  frame.crush = 1.2e6;
  frame.penetration = 1.5e5;
  Material engine = make("engine", 40e9, 16e9, 1200, 30e6, 30e6, 100e6, 20e6, 0.4, 1e5, 6.0, 0.2, true);
  engine.crush = 1.5e6;
  engine.penetration = 4e5;
  Material plastic = make("plastic", 2e9, 0.8e9, 300, 2e6, 3e6, 2e6, 1.5e6, 0.5, 1e4, 3.0, 0.4, true);
  plastic.crush = 1.5e5;
  plastic.penetration = 1e4;
  Material tyre = make("tyre", 0.05e9, 0.02e9, 265, 5e6, 5e6, 5e6, 3e6, 0.9, 1e5, 8.0, 0.0, true);
  tyre.penetration = 3e4;
  Material asphalt = make("asphalt", 10e9, 4e9, 2300, 1e6, 1.5e6, 20e6, 1.2e6, 0.8, 300, 4.0, 0.45, false);
  asphalt.grip = 1.0;
  t.set(Sheet, sheet);
  t.set(CarFrame, frame);
  t.set(Engine, engine);
  t.set(Tyre, tyre);
  t.set(Plastic, plastic);
  t.set(Asphalt, asphalt);
}

}  // namespace testmat
