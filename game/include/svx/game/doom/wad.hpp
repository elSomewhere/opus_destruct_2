// structvox — minimal Doom WAD reader (vanilla Doom map format) + raw lump access.
#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "svx/base/types.hpp"

namespace svx::doom {

struct Vertex {
  i32 x, y;
};

struct Linedef {
  u16 v1, v2;
  u16 flags, special, tag;
  u16 side[2];  // 0: right/front, 1: left/back; 0xFFFF = none
};

struct Sidedef {
  i16 xoff, yoff;
  std::array<char, 9> top, bottom, mid;
  u16 sector;
};

struct Sector {
  i16 floor, ceiling;
  std::array<char, 9> floor_pic, ceil_pic;
  i16 light;
  u16 special, tag;
  bool sky() const;  // ceiling flat is F_SKY*
};

struct Thing {
  i16 x, y;
  u16 angle, type, flags;
};

struct Map {
  std::string name;
  std::vector<Thing> things;
  std::vector<Vertex> vertices;
  std::vector<Linedef> linedefs;
  std::vector<Sidedef> sidedefs;
  std::vector<Sector> sectors;
};

// Upper-case lookup key of a lump / texture / flat name: at most 8 characters, cut at the first
// NUL. All name lookups below (and in textures.hpp) compare these keys.
std::string lump_key(std::string_view name);

// Marker-delimited lump namespaces. A range opens at either start marker and closes at the next
// end marker of the same namespace, so the vanilla pair, the Boom/PWAD pair and the DeuTeX
// "FF_START ... F_END" mix all work:
//   Flats:   F_START | FF_START ... F_END | FF_END
//   Patches: P_START | PP_START ... P_END | PP_END
//   Sprites: S_START | SS_START ... S_END | SS_END
enum class LumpNamespace { Global, Flats, Patches, Sprites };

class Wad {
 public:
  // Loads the whole file. Returns false (with message) on failure.
  bool load(const std::string& path, std::string* err);
  // Same, from the bytes of a WAD file already in memory (e.g. an ArrayBuffer handed to the worker).
  bool load_memory(std::vector<u8> bytes, std::string* err);
  std::vector<std::string> map_names() const;
  bool read_map(const std::string& name, Map* out, std::string* err) const;

  // ---- Raw lump access. Bounds-checked: never reads outside the file.
  // Lump indices are directory positions; -1 means "none". Name comparisons are case-insensitive
  // (like vanilla W_CheckNumForName) and use at most 8 characters.

  i32 lump_count() const { return static_cast<i32>(lumps_.size()); }
  // Name as stored in the directory (<= 8 chars, cut at the first NUL); "" if out of range.
  const std::string& lump_name(i32 i) const;
  // Bytes of lump i. Empty if i is out of range, the lump has no data (e.g. a marker), or its
  // directory entry does not lie inside the file (see lump_in_bounds).
  std::span<const u8> lump_data(i32 i) const;
  // False if lump i does not exist or its directory entry has a negative or out-of-file range.
  bool lump_in_bounds(i32 i) const;
  // Index of the LAST lump with this name (later lumps override earlier ones, as in vanilla),
  // optionally searching only inside a marker namespace. -1 if not found.
  i32 find_lump(std::string_view name, LumpNamespace ns = LumpNamespace::Global) const;
  // Bytes of find_lump(name, ns); empty span if not found.
  std::span<const u8> lump_by_name(std::string_view name, LumpNamespace ns = LumpNamespace::Global) const;
  // Indices, in directory order, of the lumps strictly inside every start ... end marker range
  // (e.g. "F_START", "F_END"). Optional start2/end2 are alias markers accepted interchangeably.
  // Nested marker lumps (names ending in "_START"/"_END", e.g. F1_START) are left out. A range
  // with no closing marker runs to the end of the directory.
  std::vector<i32> lumps_between(std::string_view start, std::string_view end, std::string_view start2 = {},
                                 std::string_view end2 = {}) const;
  // lumps_between() for a namespace's marker pairs; for Global, every lump that is not a marker.
  std::vector<i32> namespace_lumps(LumpNamespace ns) const;

 private:
  struct Lump {
    i32 pos, size;
    std::string name;
    std::string key;  // upper-case name, for case-insensitive lookups
  };
  std::vector<u8> data_;
  std::vector<Lump> lumps_;
};

}  // namespace svx::doom
