// Doom WAD graphics: palette, PNAMES/TEXTUREx composites, pictures, flats, lump namespaces.
// Freedoom-backed cases are skipped if the WADs are not installed; synthetic cases always run.
#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "svx/doom/textures.hpp"
#include "svx/doom/wad.hpp"

using namespace svx;
using namespace svx::doom;

namespace {

bool open_freedoom(const char* file, Wad* wad) {
  const std::string path = std::string(SVX_SOURCE_DIR) + "/data/freedoom/" + file;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    MESSAGE(file << " not found; run scripts/fetch_freedoom.sh");
    return false;
  }
  std::fclose(f);
  std::string err;
  REQUIRE(wad->load(path, &err));
  return true;
}

size_t distinct_colours(const Texture& t) {
  std::set<u32> s;
  for (size_t i = 0; i + 3 < t.rgba.size(); i += 4)
    if (t.rgba[i + 3]) s.insert(u32(t.rgba[i]) | (u32(t.rgba[i + 1]) << 8) | (u32(t.rgba[i + 2]) << 16));
  return s.size();
}

std::array<u8, 4> px(const Texture& t, i32 x, i32 y) {
  const u8* p = &t.rgba[(size_t(y) * t.width + x) * 4];
  return {p[0], p[1], p[2], p[3]};
}

// ---- Synthetic WAD construction (little-endian).

void le16(std::vector<u8>* v, i32 x) {
  v->push_back(u8(x));
  v->push_back(u8(x >> 8));
}
void le32(std::vector<u8>* v, i64 x) {
  for (int s = 0; s < 32; s += 8) v->push_back(u8(u64(x) >> s));
}
void put32(std::vector<u8>* v, size_t at, i64 x) {
  for (int s = 0; s < 4; ++s) (*v)[at + s] = u8(u64(x) >> (8 * s));
}
void name8(std::vector<u8>* v, const std::string& n) {
  for (size_t i = 0; i < 8; ++i) v->push_back(i < n.size() ? u8(n[i]) : 0);
}

// Palette entry i = (i, 255 - i, 7i mod 256).
std::vector<u8> test_playpal() {
  std::vector<u8> b;
  for (int i = 0; i < 256; ++i) b.insert(b.end(), {u8(i), u8(255 - i), u8(i * 7)});
  return b;
}
std::array<u8, 4> pal_rgba(int i) { return {u8(i), u8(255 - i), u8(i * 7), 255}; }
constexpr std::array<u8, 4> kHole = {0, 0, 0, 0};

// Picture lump; each column is a list of posts {topdelta, pixel indices}.
using Column = std::vector<std::pair<int, std::vector<u8>>>;
std::vector<u8> picture(i32 w, i32 h, const std::vector<Column>& cols) {
  std::vector<u8> b;
  le16(&b, w);
  le16(&b, h);
  le16(&b, 3);  // left offset
  le16(&b, 5);  // top offset
  const size_t table = b.size();
  b.resize(b.size() + 4 * cols.size());
  for (size_t x = 0; x < cols.size(); ++x) {
    put32(&b, table + 4 * x, i64(b.size()));
    for (const auto& [top, pixels] : cols[x]) {
      b.insert(b.end(), {u8(top), u8(pixels.size()), 0});
      b.insert(b.end(), pixels.begin(), pixels.end());
      b.push_back(0);
    }
    b.push_back(0xFF);
  }
  return b;
}

std::vector<u8> pnames(const std::vector<std::string>& names, i32 declared) {
  std::vector<u8> b;
  le32(&b, declared);
  for (const std::string& n : names) name8(&b, n);
  return b;
}

struct TexDef {
  std::string name;
  i32 w = 0, h = 0;
  std::vector<std::array<i32, 3>> patches;  // {originx, originy, PNAMES index}
  i32 declared_patches = -1;               // patchcount field if >= 0 (else patches.size())
  i64 force_offset = -1;                   // directory entry if >= 0 (the record is not written)
};

std::vector<u8> texture_lump(const std::vector<TexDef>& defs) {
  std::vector<u8> b;
  le32(&b, i64(defs.size()));
  const size_t table = b.size();
  b.resize(b.size() + 4 * defs.size());
  for (size_t k = 0; k < defs.size(); ++k) {
    const TexDef& d = defs[k];
    if (d.force_offset >= 0) {
      put32(&b, table + 4 * k, d.force_offset);
      continue;
    }
    put32(&b, table + 4 * k, i64(b.size()));
    name8(&b, d.name);
    le32(&b, 0);  // masked
    le16(&b, d.w);
    le16(&b, d.h);
    le32(&b, 0);  // columndirectory
    le16(&b, d.declared_patches >= 0 ? d.declared_patches : i32(d.patches.size()));
    for (const auto& p : d.patches) {
      le16(&b, p[0]);
      le16(&b, p[1]);
      le16(&b, p[2]);
      le16(&b, 0);
      le16(&b, 0);
    }
  }
  return b;
}

struct WadBuilder {
  std::vector<std::pair<std::string, std::vector<u8>>> lumps;
  WadBuilder& add(const std::string& name, std::vector<u8> data = {}) {
    lumps.emplace_back(name, std::move(data));
    return *this;
  }
  std::vector<u8> build() const {
    std::vector<u8> b = {'P', 'W', 'A', 'D'};
    le32(&b, i64(lumps.size()));
    le32(&b, 0);  // directory offset, patched below
    std::vector<std::pair<i64, i64>> at;
    for (const auto& l : lumps) {
      at.emplace_back(i64(b.size()), i64(l.second.size()));
      b.insert(b.end(), l.second.begin(), l.second.end());
    }
    put32(&b, 8, i64(b.size()));
    for (size_t i = 0; i < lumps.size(); ++i) {
      le32(&b, at[i].first);
      le32(&b, at[i].second);
      name8(&b, lumps[i].first);
    }
    return b;
  }
  // Rewrites the file position of directory entry `name` in built bytes (simulated corruption).
  void set_pos(std::vector<u8>* wad, const std::string& name, i64 pos) const {
    const size_t dir = size_t(wad->at(8)) | (size_t(wad->at(9)) << 8) | (size_t(wad->at(10)) << 16) |
                       (size_t(wad->at(11)) << 24);
    for (size_t i = 0; i < lumps.size(); ++i)
      if (lumps[i].first == name) put32(wad, dir + 16 * i, pos);
  }
};

}  // namespace

TEST_CASE("doom textures: freedoom2 palette, walls, flats and sky") {
  Wad wad;
  if (!open_freedoom("freedoom2.wad", &wad)) return;

  // Raw lump access and namespaces.
  const i32 playpal = wad.find_lump("PLAYPAL");
  REQUIRE(playpal >= 0);
  CHECK(wad.find_lump("playpal") == playpal);
  CHECK(wad.lump_data(playpal).size() == 14 * 768);
  CHECK(wad.lump_by_name("PLAYPAL").data() == wad.lump_data(playpal).data());
  CHECK(wad.lump_name(playpal) == "PLAYPAL");
  CHECK(wad.lump_data(-1).empty());
  CHECK(wad.lump_data(wad.lump_count()).empty());
  CHECK(wad.find_lump("NOSUCHLUMP") == -1);
  CHECK(wad.namespace_lumps(LumpNamespace::Flats).size() == 240);  // F1_START/F1_END etc. excluded
  CHECK(wad.lumps_between("F_START", "F_END") == wad.namespace_lumps(LumpNamespace::Flats));
  CHECK(wad.lumps_between("F1_START", "F1_END").size() == 240);
  CHECK(wad.find_lump("F_SKY1", LumpNamespace::Flats) >= 0);
  CHECK(wad.find_lump("F_SKY1", LumpNamespace::Patches) == -1);
  // Quirk: PNAMES lists two teleport-fog sprite frames that live in S_START..S_END.
  CHECK(wad.find_lump("TFOGF0", LumpNamespace::Patches) == -1);
  CHECK(wad.find_lump("TFOGF0", LumpNamespace::Sprites) >= 0);

  TextureSet ts;
  TextureStats st;
  std::string err;
  REQUIRE(load_textures(wad, TextureOptions{}, &ts, &st, &err));

  // Palette + colormap.
  CHECK(ts.palette.size() == 256);
  CHECK(st.palettes == 14);
  CHECK(ts.palette[0].r == 0);
  CHECK(ts.palette[0].g == 0);
  CHECK(ts.palette[0].b == 0);
  std::set<u32> pal_colours;
  for (const Rgb& c : ts.palette) pal_colours.insert(u32(c.r) | (u32(c.g) << 8) | (u32(c.b) << 16));
  CHECK(pal_colours.size() > 200);
  CHECK(ts.colormap.size() == 34 * 256);

  // Counts: every TEXTURE1 record and every flat, nothing malformed.
  CHECK(st.texture_lumps == 1);
  CHECK(st.texture_defs == 963);
  CHECK(st.walls == 963);
  CHECK(st.flats == 240);
  CHECK(ts.first_flat == 963);
  CHECK(ts.textures.size() == 963 + 240);
  CHECK(st.pnames == 1054);
  CHECK(st.skipped() == 0);
  CHECK(st.walls_duplicate == 0);
  CHECK(st.flats_duplicate == 0);
  CHECK(st.patch_bad_columns == 0);
  CHECK(st.patches_decoded > 900);
  CHECK(st.patch_refs > 1500);

  for (size_t i = 0; i < ts.textures.size(); ++i) {
    const Texture& t = ts.textures[i];
    CHECK(t.is_flat == (i32(i) >= ts.first_flat));
    CHECK(t.rgba.size() == size_t(t.width) * t.height * 4);
    CHECK((t.is_flat ? ts.find_flat(t.name) : ts.find_wall(t.name)) == i32(i));
    if (t.is_flat) {  // flats: 64x64, fully opaque
      CHECK(t.width == 64);
      CHECK(t.height == 64);
      CHECK(t.rgba.size() == 4096 * 4);
      CHECK_FALSE(t.has_holes);
    }
  }

  // Known entries: the first TEXTURE1 record, multi-patch composites, the sky.
  CHECK(ts.find_wall("AASHITTY") == 0);
  CHECK(ts.textures[0].width == 64);
  CHECK(ts.textures[0].height == 64);
  const i32 door = ts.find_wall("bigdoor6");  // case-insensitive
  REQUIRE(door >= 0);
  CHECK(ts.textures[door].width == 128);
  CHECK(ts.textures[door].height == 112);
  CHECK_FALSE(ts.textures[door].has_holes);
  CHECK(distinct_colours(ts.textures[door]) > 32);
  const i32 wide = ts.find_wall("T-BLODG1");  // 8 patches, 512 wide
  REQUIRE(wide >= 0);
  CHECK(ts.textures[wide].width == 512);
  CHECK(distinct_colours(ts.textures[wide]) > 64);
  for (const char* sky : {"SKY1", "SKY2", "SKY3"}) {
    const i32 id = ts.find_wall(sky);
    REQUIRE(id >= 0);
    CHECK(ts.textures[id].width == 256);
    CHECK(ts.textures[id].height == 128);
    CHECK(distinct_colours(ts.textures[id]) > 16);
  }
  const i32 fsky = ts.find_flat("F_SKY1");
  REQUIRE(fsky >= ts.first_flat);
  CHECK(ts.textures[fsky].is_flat);
  CHECK(distinct_colours(ts.textures[ts.find_flat("FLOOR0_1")]) > 4);  // Freedoom's uses 8 colours
  i32 multi_colour_flats = 0;
  for (i32 i = ts.first_flat; i < i32(ts.textures.size()); ++i) multi_colour_flats += distinct_colours(ts.textures[i]) > 1;
  CHECK(multi_colour_flats > 230);

  // STEP1 is both a wall texture (32x16) and a flat: separate entries.
  const i32 step_wall = ts.find_wall("STEP1"), step_flat = ts.find_flat("STEP1");
  REQUIRE(step_wall >= 0);
  REQUIRE(step_flat >= 0);
  CHECK(step_wall != step_flat);
  CHECK(ts.textures[step_wall].width == 32);
  CHECK(ts.textures[step_wall].height == 16);
  CHECK(ts.find("STEP1", true) == step_flat);
  CHECK(ts.find("-", false) == -1);

  // Holes: see-through mid textures keep alpha 0; masked flags are all 0 in Freedoom.
  const i32 grate = ts.find_wall("MIDGRATE");
  REQUIRE(grate >= 0);
  CHECK(ts.textures[grate].has_holes);
  CHECK(st.walls_with_holes == 75);
  i32 masked = 0;
  for (const Texture& t : ts.textures) masked += t.masked;
  CHECK(masked == 0);

  // A single patch at (0, 0) covering the texture composites to exactly the decoded picture.
  Picture sky_pic;
  i32 bad = -1;
  REQUIRE(decode_picture(wad.lump_by_name("RSKY1", LumpNamespace::Patches), ts.palette, &sky_pic, &bad));
  CHECK(bad == 0);
  CHECK(sky_pic.width == 256);
  CHECK(sky_pic.height == 128);
  CHECK(sky_pic.rgba == ts.textures[ts.find_wall("SKY1")].rgba);
}

TEST_CASE("doom textures: freedoom1 TEXTURE1 + TEXTURE2") {
  Wad wad;
  if (!open_freedoom("freedoom1.wad", &wad)) return;
  TextureSet ts;
  TextureStats st;
  std::string err;
  REQUIRE(load_textures(wad, TextureOptions{}, &ts, &st, &err));
  CHECK(st.texture_lumps == 2);
  CHECK(st.texture_defs == 801 + 162);
  CHECK(st.walls == 801 + 162);
  CHECK(st.flats == 240);
  CHECK(st.skipped() == 0);
  CHECK(ts.find_wall("AASTINKY") == 0);  // first TEXTURE1 record
  CHECK(ts.textures[0].width == 32);
  CHECK(ts.textures[0].height == 72);
  CHECK(ts.find_wall("ASHWALL") == 801);  // first TEXTURE2 record follows TEXTURE1
  CHECK(ts.textures[801].width == 64);
  CHECK(ts.textures[801].height == 128);
  for (const char* sky : {"SKY1", "SKY2", "SKY3", "SKY4"}) CHECK(ts.find_wall(sky) >= 0);
  CHECK(ts.find_flat("F_SKY1") >= ts.first_flat);
  i32 flat_colours_ok = 0;
  for (i32 i = ts.first_flat; i < i32(ts.textures.size()); ++i) flat_colours_ok += distinct_colours(ts.textures[i]) > 1;
  CHECK(flat_colours_ok > 230);
}

TEST_CASE("doom textures: per-map filter and sky names") {
  CHECK(sky_texture_name("MAP01") == "SKY1");
  CHECK(sky_texture_name("MAP11") == "SKY1");
  CHECK(sky_texture_name("map12") == "SKY2");
  CHECK(sky_texture_name("MAP20") == "SKY2");
  CHECK(sky_texture_name("MAP21") == "SKY3");
  CHECK(sky_texture_name("MAP32") == "SKY3");
  CHECK(sky_texture_name("E1M1") == "SKY1");
  CHECK(sky_texture_name("E2M9") == "SKY2");
  CHECK(sky_texture_name("e4m1") == "SKY4");
  CHECK(sky_texture_name("E5M1") == "SKY1");
  CHECK(sky_texture_name("TITLE") == "");

  Wad wad;
  if (!open_freedoom("freedoom2.wad", &wad)) return;
  Map map;
  std::string err;
  REQUIRE(wad.read_map("MAP01", &map, &err));
  TextureOptions opt;
  opt.only = referenced_textures(map);
  REQUIRE(opt.only.size() > 10);
  CHECK(std::set<std::string>(opt.only.begin(), opt.only.end()).size() == opt.only.size());
  TextureSet ts;
  TextureStats st;
  REQUIRE(load_textures(wad, opt, &ts, &st, &err));
  CHECK(ts.textures.size() < 300);
  CHECK(st.filtered_out > 900);
  i32 unresolved = 0;
  for (const std::string& n : opt.only) unresolved += ts.find_wall(n) < 0 && ts.find_flat(n) < 0;
  CHECK(unresolved == 0);
  for (const Sidedef& s : map.sidedefs)
    for (const auto* n : {&s.top, &s.bottom, &s.mid})
      if (std::string(n->data()) != "-") CHECK(ts.find_wall(n->data()) >= 0);
  for (const Sector& s : map.sectors) {
    CHECK(ts.find_flat(s.floor_pic.data()) >= 0);
    CHECK(ts.find_flat(s.ceil_pic.data()) >= 0);
  }
}

TEST_CASE("doom textures: picture posts, tall patches and clipping") {
  Palette pal;
  REQUIRE(decode_palette(test_playpal(), &pal));
  // Column 0: two posts. Column 1: DeePsea tall patch (second top-delta 1 <= 2 is relative: row 3).
  // Column 2: a post running past the bottom (clipped). Column 3: empty.
  const std::vector<u8> lump = picture(4, 6, {{{0, {10}}, {2, {11, 12}}}, {{2, {20}}, {1, {21, 22}}}, {{4, {30, 31, 32, 33}}}, {}});
  Picture pic;
  i32 bad = -1;
  REQUIRE(decode_picture(lump, pal, &pic, &bad));
  CHECK(bad == 0);
  CHECK(pic.width == 4);
  CHECK(pic.height == 6);
  CHECK(pic.left == 3);
  CHECK(pic.top == 5);
  REQUIRE(pic.rgba.size() == 4 * 6 * 4);
  auto at = [&](i32 x, i32 y) {
    const u8* p = &pic.rgba[(size_t(y) * 4 + x) * 4];
    return std::array<u8, 4>{p[0], p[1], p[2], p[3]};
  };
  const std::array<int, 6> col0 = {10, -1, 11, 12, -1, -1}, col1 = {-1, -1, 20, 21, 22, -1},
                           col2 = {-1, -1, -1, -1, 30, 31};
  for (i32 y = 0; y < 6; ++y) {
    CHECK(at(0, y) == (col0[y] < 0 ? kHole : pal_rgba(col0[y])));
    CHECK(at(1, y) == (col1[y] < 0 ? kHole : pal_rgba(col1[y])));
    CHECK(at(2, y) == (col2[y] < 0 ? kHole : pal_rgba(col2[y])));
    CHECK(at(3, y) == kHole);
  }

  // Truncated pixel data: the readable part is kept, the column is reported.
  std::vector<u8> cut = picture(1, 4, {{{0, {40, 41, 42, 43}}}});
  cut.resize(8 + 4 + 3 + 2);  // header, column table, post header, 2 of 4 pixels
  REQUIRE(decode_picture(cut, pal, &pic, &bad));
  CHECK(bad == 1);
  CHECK(pic.rgba[3] == 255);
  CHECK(pic.rgba[4 + 3] == 255);
  CHECK(pic.rgba[8 + 3] == 0);
  // Column offset outside the lump; unusable headers.
  std::vector<u8> off = picture(2, 2, {{{0, {1, 2}}}, {{0, {3, 4}}}});
  put32(&off, 12, 1 << 20);
  REQUIRE(decode_picture(off, pal, &pic, &bad));
  CHECK(bad == 1);
  CHECK(pic.rgba[3] == 255);      // (0, 0) from column 0
  CHECK(pic.rgba[4 + 3] == 0);    // (1, 0): column 1 unreadable
  CHECK_FALSE(decode_picture(std::vector<u8>{1, 0, 1}, pal, &pic));
  CHECK_FALSE(decode_picture(picture(0, 4, {}), pal, &pic));
  std::vector<u8> short_table = picture(3, 3, {{}, {}, {}});
  short_table.resize(12);  // column table cut off
  CHECK_FALSE(decode_picture(short_table, pal, &pic));

  // Flats: exact square sizes, >= 4096 bytes reads the first 64x64, smaller is rejected.
  Texture flat;
  REQUIRE(decode_flat(std::vector<u8>(4096 + 64, 9), pal, &flat));
  CHECK(flat.width == 64);
  CHECK(flat.is_flat);
  REQUIRE(decode_flat(std::vector<u8>(128 * 128, 9), pal, &flat));
  CHECK(flat.width == 128);
  CHECK(flat.height == 128);
  CHECK(px(flat, 127, 127) == pal_rgba(9));
  CHECK_FALSE(decode_flat(std::vector<u8>(4000, 9), pal, &flat));
}

TEST_CASE("doom textures: synthetic WAD composites, name spaces and lookups") {
  // PA: 2x2 opaque {1 3 / 2 4}. PB: 1x3 with only row 1 covered (index 9).
  WadBuilder wb;
  wb.add("PLAYPAL", test_playpal())
      .add("COLORMAP", std::vector<u8>(34 * 256, 1))
      .add("PNAMES", pnames({"PA", "pb"}, 2))
      .add("TEXTURE1", texture_lump({{"WALLA", 3, 3, {{0, 0, 0}, {1, 0, 1}}},  // PB drawn over PA
                                     {"wallb", 2, 2, {{-1, -1, 0}}},          // negative origin: clipped
                                     {"SAME", 2, 2, {{0, 0, 0}}}}))
      .add("P_START")
      .add("PA", picture(2, 2, {{{0, {1, 2}}}, {{0, {3, 4}}}}))
      .add("PB", picture(1, 3, {{{1, {9}}}}))
      .add("P_END")
      .add("F_START")
      .add("SAME", std::vector<u8>(4096, 7))
      .add("F_END")
      .add("FF_START")
      .add("FLATB", std::vector<u8>(4096, 8))
      .add("FF_END");
  Wad wad;
  std::string err;
  REQUIRE(wad.load_memory(wb.build(), &err));
  CHECK(wad.namespace_lumps(LumpNamespace::Flats).size() == 2);  // F_ and FF_ ranges
  CHECK(wad.find_lump("same") == wad.find_lump("SAME", LumpNamespace::Flats));  // last one wins
  CHECK(wad.find_lump("PA", LumpNamespace::Patches) >= 0);
  CHECK(wad.find_lump("PA", LumpNamespace::Flats) == -1);

  TextureSet ts;
  TextureStats st;
  REQUIRE(load_textures(wad, TextureOptions{}, &ts, &st, &err));
  CHECK(st.skipped() == 0);
  CHECK(st.walls == 3);
  CHECK(st.flats == 2);
  CHECK(st.patches_decoded == 2);
  CHECK(st.patch_refs == 4);
  CHECK(ts.first_flat == 3);
  CHECK(ts.colormap.size() == 34 * 256);

  const i32 a = ts.find_wall("walla");
  REQUIRE(a == 0);
  const Texture& ta = ts.textures[a];
  CHECK(px(ta, 0, 0) == pal_rgba(1));
  CHECK(px(ta, 0, 1) == pal_rgba(2));
  CHECK(px(ta, 1, 0) == pal_rgba(3));
  CHECK(px(ta, 1, 1) == pal_rgba(9));  // later patch on top
  CHECK(px(ta, 2, 0) == kHole);
  CHECK(px(ta, 0, 2) == kHole);
  CHECK(ta.has_holes);

  const i32 b = ts.find_wall("WALLB");
  REQUIRE(b == 1);
  CHECK(ts.textures[b].name == "WALLB");
  CHECK(px(ts.textures[b], 0, 0) == pal_rgba(4));  // PA's (1, 1): the clipped rows are skipped
  CHECK(px(ts.textures[b], 1, 0) == kHole);
  CHECK(px(ts.textures[b], 0, 1) == kHole);

  // "SAME" is both a wall and a flat.
  const i32 sw = ts.find_wall("SAME"), sf = ts.find_flat("SAME");
  REQUIRE(sw == 2);
  REQUIRE(sf == 3);
  CHECK_FALSE(ts.textures[sw].is_flat);
  CHECK(ts.textures[sf].is_flat);
  CHECK(px(ts.textures[sf], 63, 63) == pal_rgba(7));
  CHECK(px(ts.textures[ts.find_flat("FLATB")], 0, 0) == pal_rgba(8));

  // Walls or flats only.
  TextureOptions walls_only;
  walls_only.flats = false;
  REQUIRE(load_textures(wad, walls_only, &ts, &st, &err));
  CHECK(ts.textures.size() == 3);
  CHECK(ts.find_flat("SAME") == -1);
  TextureOptions flats_only;
  flats_only.walls = false;
  REQUIRE(load_textures(wad, flats_only, &ts, &st, &err));
  CHECK(ts.textures.size() == 2);
  CHECK(ts.first_flat == 0);
  // Memory budget: WALLA (3x3) and WALLB (2x2) fit, the rest is dropped and counted.
  TextureOptions small;
  small.max_rgba_bytes = (3 * 3 + 2 * 2) * 4;
  REQUIRE(load_textures(wad, small, &ts, &st, &err));
  CHECK(ts.textures.size() == 2);
  CHECK(st.over_budget == 3);
  CHECK(st.skipped() == 3);

  // No PLAYPAL: the only fatal error.
  WadBuilder none;
  none.add("PNAMES", pnames({"PA"}, 1));
  REQUIRE(wad.load_memory(none.build(), &err));
  CHECK_FALSE(load_textures(wad, TextureOptions{}, &ts, &st, &err));
  CHECK(err == "no PLAYPAL lump");
}

TEST_CASE("doom textures: malformed WAD data is skipped and counted, never crashes") {
  std::vector<u8> ptrunc = picture(1, 4, {{{0, {40, 41, 42, 43}}}});
  ptrunc.resize(8 + 4 + 3 + 2);  // pixel data cut after 2 of 4 pixels
  std::vector<u8> texture2;       // declares 1000 records, holds 3 offsets and no records
  le32(&texture2, 1000);
  le32(&texture2, -1);
  le32(&texture2, 0x7FFFFFFF);
  le32(&texture2, 12);
  WadBuilder wb;
  wb.add("PLAYPAL", test_playpal())
      .add("PNAMES", pnames({"PA", "NOPE", "PBAD", "PTRUNC"}, 6))  // 6 declared, 4 present
      .add("TEXTURE1", texture_lump({
                           {"GOOD", 2, 2, {{0, 0, 0}}},
                           {"BEYOND", 2, 2, {}, -1, 0x7FFF0000},            // offset outside the lump
                           {"ZEROW", 0, 2, {{0, 0, 0}}},                     // width 0
                           {"REFS", 2, 2, {{0, 0, 99}, {0, 0, -1}, {0, 0, 1}, {0, 0, 2}, {0, 0, 5}, {0, 0, 3}}},
                           {"GOOD", 4, 4, {{0, 0, 0}}},                      // duplicate name
                           {"", 2, 2, {{0, 0, 0}}},                          // empty name
                           {"CUTOFF", 2, 2, {{1, 1, 0}}, 50},                // 50 declared, 1 in the lump
                       }))
      .add("TEXTURE2", texture2)
      .add("P_START")
      .add("PA", picture(2, 2, {{{0, {1, 2}}}, {{0, {3, 4}}}}))
      .add("PBAD", {1, 2, 3})
      .add("PTRUNC", ptrunc)
      .add("P_END")
      .add("F_START")
      .add("F1_START")
      .add("OKFLAT", std::vector<u8>(4096, 5))
      .add("TINY", std::vector<u8>(100, 5))
      .add("OKFLAT", std::vector<u8>(4096, 6))  // later duplicate overrides
      .add("HUGE", std::vector<u8>(4096, 7))    // directory entry corrupted below
      .add("F1_END")
      .add("F_END")
      .add("MAP01")
      .add("THINGS")
      .add("LINEDEFS")
      .add("SIDEDEFS")
      .add("VERTEXES", std::vector<u8>(8, 0))  // directory entry corrupted below
      .add("SEGS")
      .add("SSECTORS")
      .add("NODES")
      .add("SECTORS");
  std::vector<u8> bytes = wb.build();
  wb.set_pos(&bytes, "HUGE", 0x7FFFFFF0);
  wb.set_pos(&bytes, "VERTEXES", 0x7FFFFFF0);

  Wad wad;
  std::string err;
  REQUIRE(wad.load_memory(bytes, &err));
  const i32 huge = wad.find_lump("HUGE");
  REQUIRE(huge >= 0);
  CHECK_FALSE(wad.lump_in_bounds(huge));
  CHECK(wad.lump_data(huge).empty());
  CHECK(wad.namespace_lumps(LumpNamespace::Flats).size() == 4);  // F1_START/F1_END left out
  CHECK(wad.map_names() == std::vector<std::string>{"MAP01"});
  Map map;
  CHECK_FALSE(wad.read_map("MAP01", &map, &err));  // out-of-file VERTEXES: error, not a crash

  TextureSet ts;
  TextureStats st;
  REQUIRE(load_textures(wad, TextureOptions{}, &ts, &st, &err));
  CHECK(st.pnames == 6);
  CHECK(st.patches_missing == 3);  // NOPE + 2 names the lump does not hold
  CHECK(st.patches_bad == 1);      // PBAD
  CHECK(st.patches_decoded == 2);  // PA, PTRUNC
  CHECK(st.patch_bad_columns == 1);
  CHECK(st.texture_lumps == 2);
  CHECK(st.texture_defs == 7 + 1000);
  CHECK(st.walls == 3);              // GOOD, REFS, CUTOFF
  CHECK(st.walls_skipped == 3 + 1000);
  CHECK(st.walls_duplicate == 1);
  CHECK(st.patch_refs == 3);
  CHECK(st.patch_refs_skipped == 5 + 49);
  CHECK(st.flats == 1);
  CHECK(st.flats_skipped == 2);  // TINY, HUGE
  CHECK(st.flats_duplicate == 1);
  CHECK(st.skipped() == 3 + 1 + 1003 + 54 + 2);

  const i32 good = ts.find_wall("GOOD");
  REQUIRE(good >= 0);
  CHECK(ts.textures[good].width == 2);  // the first definition wins
  const i32 refs = ts.find_wall("REFS");
  REQUIRE(refs >= 0);
  CHECK(px(ts.textures[refs], 0, 0) == pal_rgba(40));  // readable part of PTRUNC
  CHECK(px(ts.textures[refs], 0, 1) == pal_rgba(41));
  CHECK(px(ts.textures[refs], 1, 0) == kHole);
  CHECK(px(ts.textures[refs], 1, 1) == kHole);
  const i32 cutoff = ts.find_wall("CUTOFF");
  REQUIRE(cutoff >= 0);
  CHECK(px(ts.textures[cutoff], 1, 1) == pal_rgba(1));
  CHECK(px(ts.textures[cutoff], 0, 0) == kHole);
  const i32 okflat = ts.find_flat("OKFLAT");
  REQUIRE(okflat >= 0);
  CHECK(px(ts.textures[okflat], 0, 0) == pal_rgba(6));

  // Directory-level corruption is rejected by the loader.
  CHECK_FALSE(wad.load_memory({}, &err));
  CHECK_FALSE(wad.load_memory({'P', 'W', 'A', 'D', 0, 0, 0, 0}, &err));
  CHECK_FALSE(wad.load_memory({'J', 'U', 'N', 'K', 0, 0, 0, 0, 0, 0, 0, 0}, &err));
  CHECK_FALSE(wad.load_memory({'I', 'W', 'A', 'D', 1, 0, 0, 0, 12, 0, 0, 0}, &err));
  // 0x10000001 entries: n * 16 wraps to 16 in 32-bit size_t (WASM) and would pass a naive check.
  std::vector<u8> wrap = {'I', 'W', 'A', 'D', 0x01, 0, 0, 0x10, 12, 0, 0, 0};
  wrap.resize(12 + 16, 0);
  CHECK_FALSE(wad.load_memory(wrap, &err));
  CHECK(err == "bad directory");
  CHECK(wad.lump_count() == 0);

  // Random byte corruption (deterministic): decoding must never crash and results stay consistent.
  const std::vector<u8> clean = wb.build();
  u64 rng = 0x9E3779B97F4A7C15ull;
  auto next = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  i32 loaded = 0;
  for (int iter = 0; iter < 400; ++iter) {
    std::vector<u8> fuzz = clean;
    const int flips = 1 + int(next() % 16);
    for (int k = 0; k < flips; ++k) fuzz[12 + next() % (fuzz.size() - 12)] = u8(next());
    if (!wad.load_memory(std::move(fuzz), &err)) continue;
    if (!load_textures(wad, TextureOptions{}, &ts, &st, &err)) continue;
    ++loaded;
    for (const Texture& t : ts.textures) REQUIRE(t.rgba.size() == size_t(t.width) * t.height * 4);
    for (const auto& [name, id] : ts.wall_ids) REQUIRE((id >= 0 && id < ts.first_flat));
    for (const auto& [name, id] : ts.flat_ids) REQUIRE((id >= ts.first_flat && id < i32(ts.textures.size())));
  }
  CHECK(loaded > 100);
}
