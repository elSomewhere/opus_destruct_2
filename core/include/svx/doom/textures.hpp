// structvox — Doom WAD graphics → RGBA8 (plan §B10; docs/API.md "textures" message).
//
// Decodes the palette (PLAYPAL, first of its 14 palettes), the raw light maps (COLORMAP), wall
// textures (PNAMES + TEXTURE1/TEXTURE2 maptexture records, composited from their patches),
// Doom pictures (column posts, including DeePsea "tall patch" relative top-deltas) and flats
// (F_START/F_END and FF_START/FF_END).
//
// Conventions:
//  - RGBA8, row-major, row 0 at the top; width*height*4 bytes. Palette colours are copied as is.
//  - Pixels no patch post covers get alpha 0 and RGB 0: see-through parts of mid textures
//    (grates, fences, vines) and uncovered gaps. Covered pixels and all flat pixels have
//    alpha 255. The maptexture "masked" flag is kept but is NOT a transparency signal (vanilla
//    ignores it and it is 0 for every Freedoom texture); use Texture::has_holes.
//  - Wall textures and flats are separate name spaces (Freedoom has a wall texture and a flat
//    both called STEP1), so lookups are per kind. Names are lump_key()s: upper case, <= 8 chars.
//  - Ids are plain indices into TextureSet::textures: walls first (TEXTURE1 order, then
//    TEXTURE2), then flats (directory order). They fit the u16 vertex field (0xFFFF = none).
//    Vanilla treats the first TEXTURE1 entry (AASHITTY / AASTINKY) as "no texture"; here it
//    is an ordinary texture, and maps refer to "no texture" as "-".
//  - Compositing follows the modern-port (ZDoom/Boom) rules: patches drawn in list order,
//    later ones on top, each clipped to its own height and to the texture rectangle, with a
//    correct top clip for negative y origins (vanilla's single-patch-column and negative-origin
//    quirks are not emulated).
//
// Robustness: every read is bounds-checked. Malformed records, patches, posts and flats are
// skipped (partially readable ones are clipped) and counted in TextureStats; nothing aborts.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "svx/base/types.hpp"
#include "svx/doom/wad.hpp"

namespace svx::doom {

struct Rgb {
  u8 r = 0, g = 0, b = 0;
};
using Palette = std::array<Rgb, 256>;

struct Texture {
  std::string name;  // upper case, <= 8 chars
  i32 width = 0, height = 0;
  bool is_flat = false;
  bool masked = false;     // maptexture "masked" field as stored (walls only; see above)
  bool has_holes = false;  // at least one pixel has alpha 0
  std::vector<u8> rgba;    // width * height * 4
};

struct TextureSet {
  Palette palette{};               // PLAYPAL palette 0
  std::vector<u8> colormap;        // raw COLORMAP lump (34 x 256 light maps in Doom); may be empty
  std::vector<Texture> textures;   // walls, then flats (see ids above)
  i32 first_flat = 0;              // == number of wall textures
  std::unordered_map<std::string, i32> wall_ids, flat_ids;  // upper-case name -> index

  // Case-insensitive; -1 if absent (including "-" and "").
  i32 find_wall(std::string_view name) const;
  i32 find_flat(std::string_view name) const;
  i32 find(std::string_view name, bool is_flat) const { return is_flat ? find_flat(name) : find_wall(name); }
};

struct TextureStats {
  i32 palettes = 0;             // palettes in PLAYPAL (14 in Doom); only the first is used
  i32 colormap_bytes = 0;       // COLORMAP size (8704 = 34 maps in Doom); 0 if absent
  i32 pnames = 0;               // PNAMES entries
  i32 patches_missing = 0;      // PNAMES entries with no lump of that name
  i32 patches_decoded = 0;      // distinct patch lumps decoded for the composites
  i32 patches_bad = 0;          // referenced patch lumps with an unusable header / column table
  i32 patch_bad_columns = 0;    // columns with an out-of-lump offset or truncated posts (clipped)
  i32 tall_patches = 0;         // patches that use DeePsea relative top-deltas
  i32 texture_lumps = 0;        // TEXTUREx lumps read (TEXTURE1, TEXTURE2)
  i32 texture_defs = 0;         // maptexture records declared by their directories
  i32 walls = 0;                // wall textures decoded
  i32 walls_skipped = 0;        // unreadable records (bad offset, name, size or patch count)
  i32 walls_duplicate = 0;      // repeated names: the first definition wins, as in vanilla
  i32 walls_with_holes = 0;     // decoded walls with at least one alpha-0 pixel
  i32 patch_refs = 0;           // patch placements drawn
  i32 patch_refs_skipped = 0;   // placements with a bad PNAMES index / missing or bad patch,
                                // or cut off by the end of the TEXTUREx lump
  i32 flats = 0;                // flats decoded
  i32 flats_skipped = 0;        // flat lumps of no usable size (see decode_flat), outside the file,
                                // or with an empty name
  i32 flats_duplicate = 0;      // repeated flat names: the last lump wins, as in vanilla
  i32 filtered_out = 0;         // valid items not decoded because of TextureOptions::only
  i32 over_budget = 0;          // items dropped once TextureOptions::max_rgba_bytes was reached

  // Items dropped because of malformed data or the memory budget (duplicates and filtering are
  // not errors).
  i32 skipped() const {
    return patches_missing + patches_bad + walls_skipped + patch_refs_skipped + flats_skipped + over_budget;
  }
};

struct TextureOptions {
  bool walls = true;  // composite TEXTURE1/TEXTURE2
  bool flats = true;  // decode flats
  // If non-empty, only walls and flats with these names (case-insensitive) are decoded, e.g.
  // referenced_textures(map). Empty: everything.
  std::vector<std::string> only;
  // Cap on the total RGBA bytes decoded (bounds memory on hostile WADs). All of Freedoom is
  // ~56 MB; single images are capped at 16 MB regardless.
  size_t max_rgba_bytes = size_t(512) << 20;
};

// Decodes the WAD's graphics into *out (replacing its contents). Returns false (with *err) only
// if there is no usable PLAYPAL; a WAD without PNAMES / TEXTURE1 / flats yields no walls / flats.
// stats may be null.
bool load_textures(const Wad& wad, const TextureOptions& opt, TextureSet* out, TextureStats* stats, std::string* err);

// First palette of a PLAYPAL lump; false if the lump is shorter than 768 bytes.
bool decode_palette(std::span<const u8> playpal, Palette* out);

// A decoded Doom picture (patch, sprite, menu graphic).
struct Picture {
  i32 width = 0, height = 0;
  i32 left = 0, top = 0;  // left/top offsets (sprite hotspot; not used by wall compositing)
  std::vector<u8> rgba;   // width * height * 4; alpha 0 where no post covers the pixel
};

// Decodes a picture lump. False if the header or column table is unusable. Posts are clipped to
// the picture; columns with a bad offset or truncated posts are counted in *bad_columns.
bool decode_picture(std::span<const u8> lump, const Palette& pal, Picture* out, i32* bad_columns = nullptr);

// Decodes a raw flat: 4096 bytes = 64x64. Exact 8x8, 16x16, 32x32, 128x128 and 256x256 sizes are
// recognised; any other size >= 4096 uses its first 4096 bytes (as vanilla does). False if it is
// smaller. Sets everything but out->name.
bool decode_flat(std::span<const u8> lump, const Palette& pal, Texture* out);

// Vanilla sky texture of a map: ExMy -> SKY1..SKY4 by episode (SKY1 for other episodes);
// MAPnn -> SKY1 (01-11), SKY2 (12-20), SKY3 (21+). "" if the name is neither form.
std::string sky_texture_name(std::string_view map_name);

// Unique upper-case names of the walls and flats a map uses (sidedef upper/lower/middle, sector
// floor/ceiling; "-" skipped), plus sky_texture_name(map.name) if any sector has a sky ceiling.
// Suitable for TextureOptions::only.
std::vector<std::string> referenced_textures(const Map& map);

}  // namespace svx::doom
