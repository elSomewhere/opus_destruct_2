// svx_anim — content as data (docs/ANIM.md §10): voxel models and prop archetypes read from and
// written as JSON, so a host brings characters and props without building the library. The
// library's own prop catalogue is such data (data/content/props.json, embedded).
//
// A model: {"format": "svx-anim-voxel-model", "version": 2, "rig": "humanoid" | "prop",
//   "build": {"height", "shoulders", "hips", "girth"} (humanoid), "voxel_size",
//   "tissues": [16 names: "soft" | "flesh" | "bone" | "metal", by slot],
//   "parts": [{"bone", "origin": [x, y, z], "dims": [x, y, z], "cells", "shade", "tissue"}]}
// each per-cell array (x fastest, then y, then z) run-length coded as [value, run, value, run, ...]
// ("shade" and "tissue" may be absent: 128, and the slot's). Cells hold slot + 1 (0: empty).
// A prop: {"format": "svx-anim-prop", "version": 1, "id", "name", "mass", "tags", "attachments",
//   "material": {"density", "penetration", "fracture"}, "hold": {...}, "grip", "support", "butt",
//   "tip", "reload_point", "hanging_rotation", "ready_pitch", "ready_roll", "ready_butt_offset",
//   "one_handed", "sockets", "points", "features", "centre", "inertia", "dimensions", "model"}.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "svx/anim/characters/props.hpp"
#include "svx/anim/rig.hpp"
#include "svx/anim/voxel/model.hpp"

namespace svx::anim {

// (build: the humanoid's, written with it; null: a prop's one-bone rig)
std::string model_json(const VoxelModel& m, const HumanoidBuild* build);
// Null (and *error) when the text is not such a model: every field is checked.
ModelPtr read_model(std::string_view json, std::string* error = nullptr, HumanoidBuild* build = nullptr);

std::string prop_json(const Prop& p);
PropPtr read_prop(std::string_view json, std::string* error = nullptr);
// A list of props: [prop, ...] (the catalogue's form).
std::vector<PropPtr> read_props(std::string_view json, std::string* error = nullptr);

}  // namespace svx::anim
