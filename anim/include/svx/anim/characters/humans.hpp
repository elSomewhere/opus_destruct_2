// svx_anim — procedural voxel humans: a sculpted body (skin, flesh and bone inside) dressed in
// layers (trousers or shorts, shirts, jackets, boots or shoes, hair, headgear, tactical gear), all
// bound to the humanoid rig. `sculpt_human` builds one geometry from a HumanSpec; colours come from
// the palette of each instance, so a geometry serves many looks.
#pragma once

#include <optional>
#include <string>

#include "svx/anim/characters/palette.hpp"
#include "svx/anim/characters/props.hpp"
#include "svx/anim/rig.hpp"

namespace svx::anim {

enum class TopStyle : u8 { Tshirt, Longsleeve, Jacket, Hoodie, Uniform, Tanktop };
enum class BottomStyle : u8 { Trousers, Shorts, Uniform };
enum class ShoeStyle : u8 { Shoes, Sneakers, Boots };
enum class HairStyle : u8 { None, Buzz, Short, Long, Ponytail, Bun };
enum class HeadGear : u8 { None, Helmet, HelmetGoggles, Cap, Beanie, Beret, Balaclava };

// The original's names of the styles ("tshirt", "helmetGoggles", ...).
const char* style_name(TopStyle v);
const char* style_name(BottomStyle v);
const char* style_name(ShoeStyle v);
const char* style_name(HairStyle v);
const char* style_name(HeadGear v);

struct HumanSpec {
  HumanoidBuild build;
  bool female = false;
  TopStyle top = TopStyle::Tshirt;
  BottomStyle bottom = BottomStyle::Trousers;
  ShoeStyle shoes = ShoeStyle::Shoes;
  HairStyle hair = HairStyle::Short;
  HeadGear headgear = HeadGear::None;
  bool beard = false;
  bool glasses = false;
  bool vest = false;
  bool backpack = false;
  bool belt = false;
  bool gloves = false;
  bool kneepads = false;
  bool camo = false;                  // a two-tone noise pattern on Top/Bottom (Top2/Bottom2)
  f64 voxel_size = kDefaultVoxelSize;  // voxel pitch (m)
  i32 seed = 0;
  std::string name;
};

ModelPtr sculpt_human(const HumanSpec& spec);

// ---- presets

struct HumanVariant {
  ModelPtr model;
  Palette palette;
  HumanSpec spec;
};

struct HumanOptions {
  f64 voxel_size = kDefaultVoxelSize;
  std::optional<i32> scheme{};  // (soldiers) the camouflage scheme; none: by seed
};

// The geometry key: variants sharing it can share meshes (only their palettes differ).
std::string geometry_key(const HumanSpec& s);

// A soldier / mercenary: helmet or balaclava, plate carrier, camouflage, boots.
HumanSpec soldier_spec(i32 seed, const HumanOptions& opts = {});
Palette soldier_palette(i32 seed, std::optional<i32> scheme = std::nullopt);
// A civilian: varied builds, clothes, hair and accessories.
HumanSpec civilian_spec(i32 seed, const HumanOptions& opts = {});
Palette civilian_palette(i32 seed);
// A thug: heavier set, hoodies and dark jackets, beanies, caps or a balaclava, gloves.
HumanSpec thug_spec(i32 seed, const HumanOptions& opts = {});
// Dark street clothes: black, charcoal, olive, maroon, navy, with a bright accent now and then.
Palette thug_palette(i32 seed);

HumanVariant make_thug(i32 seed, const HumanOptions& opts = {});
HumanVariant make_soldier(i32 seed, const HumanOptions& opts = {});
HumanVariant make_civilian(i32 seed, const HumanOptions& opts = {});

}  // namespace svx::anim
