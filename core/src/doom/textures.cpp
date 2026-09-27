#include "svx/doom/textures.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace svx::doom {

namespace {

constexpr i32 kMaxDim = 4096;                 // per side, for textures and pictures
constexpr i64 kMaxPixels = i64(4096) * 1024;  // per image (16 MB of RGBA)
constexpr size_t kMaxTextures = 0xFFFF;       // ids stay below the u16 "no texture" value 0xFFFF

i16 rd16(const u8* p) { return static_cast<i16>(u16(p[0]) | (u16(p[1]) << 8)); }
u32 ru32(const u8* p) { return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24); }
i32 rd32(const u8* p) { return static_cast<i32>(ru32(p)); }

std::string key8(const u8* p) { return lump_key(std::string_view(reinterpret_cast<const char*>(p), 8)); }

// One post clipped to its picture: rows [top, top + len), pixel indices at lump offset src.
struct Post {
  i32 top, len;
  u32 src;
};

// A picture lump parsed into clipped posts; the pixel bytes stay in the lump.
struct ParsedPicture {
  i32 width = 0, height = 0, left = 0, top = 0;
  std::vector<u32> col;  // width + 1 offsets into posts
  std::vector<Post> posts;
  i32 bad_columns = 0;
  bool tall = false;
};

// Picture format: i16 width, height, leftoffset, topoffset; u32 columnofs[width]; each column is
// a list of posts {u8 topdelta, u8 length, u8 pad, u8 pixels[length], u8 pad} ended by 0xFF.
bool parse_picture(std::span<const u8> b, ParsedPicture* out) {
  *out = ParsedPicture{};
  if (b.size() < 8) return false;
  const i32 w = rd16(&b[0]), h = rd16(&b[2]);
  if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim || i64(w) * h > kMaxPixels) return false;
  if (8 + size_t(w) * 4 > b.size()) return false;
  out->width = w;
  out->height = h;
  out->left = rd16(&b[4]);
  out->top = rd16(&b[6]);
  out->col.resize(size_t(w) + 1);
  for (i32 x = 0; x < w; ++x) {
    out->col[x] = u32(out->posts.size());
    size_t p = ru32(&b[8 + size_t(x) * 4]);
    i32 top = -1, stored = 0;
    bool ok = true;
    for (;;) {
      if (p >= b.size()) {  // no 0xFF terminator inside the lump
        ok = false;
        break;
      }
      const i32 delta = b[p];
      if (delta == 0xFF) break;
      if (p + 3 > b.size()) {
        ok = false;
        break;
      }
      // DeePsea tall patches: a top-delta not below the previous post's top is relative to it.
      if (delta <= top) {
        top += delta;
        out->tall = true;
      } else {
        top = delta;
      }
      const i32 len = b[p + 1];
      const size_t first = p + 3;
      i32 avail = len;
      if (first + size_t(len) > b.size()) {
        avail = i32(b.size() - first);
        ok = false;
      }
      const i32 y1 = std::min(top + avail, h);
      if (y1 > top) {
        if (++stored > h) {  // more posts than rows: junk (bounds memory on hostile data)
          ok = false;
          break;
        }
        out->posts.push_back({top, y1 - top, u32(first)});
      }
      if (!ok || top >= h) break;  // tops never decrease, so nothing further is visible
      p = first + size_t(len) + 1;
    }
    if (!ok) ++out->bad_columns;
  }
  out->col[w] = u32(out->posts.size());
  return true;
}

// Draws pic (pixels in src) with its top-left corner at (ox, oy) into a w x h RGBA image,
// clipped to the image. Covered pixels become opaque palette colours.
void draw_picture(const ParsedPicture& pic, std::span<const u8> src, i32 ox, i32 oy, const Palette& pal, u8* dst,
                  i32 w, i32 h) {
  const i32 x0 = std::max(0, -ox), x1 = std::min(pic.width, w - ox);
  for (i32 x = x0; x < x1; ++x) {
    const i32 tx = ox + x;
    for (u32 k = pic.col[x]; k < pic.col[x + 1]; ++k) {
      const Post& p = pic.posts[k];
      const i32 base = oy + p.top;
      const i32 y0 = std::max(0, base), y1 = std::min(h, base + p.len);
      for (i32 ty = y0; ty < y1; ++ty) {
        const Rgb c = pal[src[p.src + u32(ty - base)]];
        u8* o = dst + (size_t(ty) * size_t(w) + size_t(tx)) * 4;
        o[0] = c.r;
        o[1] = c.g;
        o[2] = c.b;
        o[3] = 255;
      }
    }
  }
}

bool any_transparent(const std::vector<u8>& rgba) {
  for (size_t i = 3; i < rgba.size(); i += 4)
    if (rgba[i] == 0) return true;
  return false;
}

struct Decoder {
  const Wad& wad;
  bool filtering;
  std::unordered_set<std::string> only;
  size_t budget;  // RGBA bytes still allowed
  TextureSet& set;
  TextureStats& st;

  std::vector<i32> pn_lump;          // PNAMES index -> lump index (-1: missing)
  std::vector<i8> state;             // per lump: 0 not parsed, 1 ok, -1 bad
  std::vector<ParsedPicture> parsed;  // per lump

  bool wanted(const std::string& key) const { return !filtering || only.count(key) != 0; }

  // Reserves the RGBA bytes of a new w x h image; false (counted) once the budget is spent or the
  // texture ids would no longer fit a u16.
  bool reserve(i32 w, i32 h) {
    const size_t bytes = size_t(w) * size_t(h) * 4;
    if (bytes > budget || set.textures.size() >= kMaxTextures) {
      ++st.over_budget;
      return false;
    }
    budget -= bytes;
    return true;
  }

  // PNAMES: i32 count, then count 8-byte names. A name resolves to the last lump of that name in
  // the patch namespace (P_START/PP_START ranges), else anywhere in the WAD, as vanilla looks
  // patches up globally (Freedoom's PNAMES lists two sprite frames, TFOGF0 and TFOGI0).
  void read_pnames() {
    const std::span<const u8> pn = wad.lump_by_name("PNAMES");
    if (pn.size() < 4) return;
    const i64 declared = std::clamp<i64>(rd32(&pn[0]), 0, 32768);  // mappatch indices are i16
    const i64 readable = std::min<i64>(declared, i64(pn.size() - 4) / 8);
    std::unordered_map<std::string, i32> in_ns, global;
    for (i32 i : wad.namespace_lumps(LumpNamespace::Patches)) in_ns[lump_key(wad.lump_name(i))] = i;
    for (i32 i = 0; i < wad.lump_count(); ++i) global[lump_key(wad.lump_name(i))] = i;
    pn_lump.assign(size_t(declared), -1);
    for (i64 k = 0; k < readable; ++k) {
      const std::string name = key8(&pn[size_t(4 + 8 * k)]);
      if (name.empty()) continue;
      if (auto it = in_ns.find(name); it != in_ns.end()) {
        pn_lump[size_t(k)] = it->second;
      } else if (auto jt = global.find(name); jt != global.end()) {
        pn_lump[size_t(k)] = jt->second;
      }
    }
    st.pnames = i32(declared);
    for (i32 li : pn_lump) st.patches_missing += li < 0;
  }

  // Parsed patch for a mappatch index (cached per lump), or null.
  const ParsedPicture* patch(i32 pi) {
    if (pi < 0 || size_t(pi) >= pn_lump.size()) return nullptr;
    const i32 li = pn_lump[size_t(pi)];
    if (li < 0) return nullptr;
    i8& s = state[size_t(li)];
    if (s == 0) {
      ParsedPicture& pp = parsed[size_t(li)];
      if (parse_picture(wad.lump_data(li), &pp)) {
        s = 1;
        ++st.patches_decoded;
        st.patch_bad_columns += pp.bad_columns;
        st.tall_patches += pp.tall ? 1 : 0;
      } else {
        s = -1;
        ++st.patches_bad;
      }
    }
    return s > 0 ? &parsed[size_t(li)] : nullptr;
  }

  // TEXTUREx: i32 count, i32 offsets[count]; each maptexture record is
  //   char name[8]; i32 masked; i16 width, height; i32 columndirectory (unused); i16 patchcount;
  //   {i16 originx, originy, patch, stepdir, colormap} patches[patchcount]
  void read_texture_lump(const char* lump_name) {
    const i32 li = wad.find_lump(lump_name);
    if (li < 0) return;
    ++st.texture_lumps;
    const std::span<const u8> b = wad.lump_data(li);
    const i64 declared = b.size() >= 4 ? std::min<i64>(rd32(&b[0]), 65536) : -1;
    if (declared < 0) {  // unreadable lump: count it as one skipped record
      ++st.walls_skipped;
      return;
    }
    const i64 readable = std::min<i64>(declared, i64(b.size() - 4) / 4);
    st.texture_defs += i32(declared);
    st.walls_skipped += i32(declared - readable);
    for (i64 k = 0; k < readable; ++k) {
      const i64 off = rd32(&b[size_t(4 + 4 * k)]);
      if (off < 0 || size_t(off) + 22 > b.size()) {
        ++st.walls_skipped;
        continue;
      }
      const u8* r = &b[size_t(off)];
      const std::string name = key8(r);
      const i32 w = rd16(r + 12), h = rd16(r + 14);
      i32 npatch = rd16(r + 20);
      if (name.empty() || w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim || i64(w) * h > kMaxPixels || npatch < 0) {
        ++st.walls_skipped;
        continue;
      }
      if (!wanted(name)) {
        ++st.filtered_out;
        continue;
      }
      if (set.wall_ids.count(name)) {
        ++st.walls_duplicate;
        continue;
      }
      if (!reserve(w, h)) continue;
      const i32 fit = i32((b.size() - size_t(off) - 22) / 10);
      if (npatch > fit) {
        st.patch_refs_skipped += npatch - fit;
        npatch = fit;
      }
      Texture t;
      t.name = name;
      t.width = w;
      t.height = h;
      t.masked = rd32(r + 8) != 0;
      t.rgba.assign(size_t(w) * size_t(h) * 4, 0);
      for (i32 j = 0; j < npatch; ++j) {
        const u8* m = r + 22 + 10 * j;
        const i32 pi = rd16(m + 4);
        const ParsedPicture* pic = patch(pi);
        if (!pic) {
          ++st.patch_refs_skipped;
          continue;
        }
        draw_picture(*pic, wad.lump_data(pn_lump[size_t(pi)]), rd16(m), rd16(m + 2), set.palette, t.rgba.data(), w, h);
        ++st.patch_refs;
      }
      t.has_holes = any_transparent(t.rgba);
      st.walls_with_holes += t.has_holes ? 1 : 0;
      set.wall_ids.emplace(name, i32(set.textures.size()));
      set.textures.push_back(std::move(t));
      ++st.walls;
    }
  }

  void read_walls() {
    state.assign(size_t(wad.lump_count()), 0);
    parsed.resize(size_t(wad.lump_count()));
    read_pnames();
    read_texture_lump("TEXTURE1");
    read_texture_lump("TEXTURE2");
    std::vector<ParsedPicture>().swap(parsed);  // release the post lists
  }

  void read_flats() {
    for (i32 li : wad.namespace_lumps(LumpNamespace::Flats)) {
      const std::string name = lump_key(wad.lump_name(li));
      if (name.empty()) {
        ++st.flats_skipped;
        continue;
      }
      if (!wanted(name)) {
        ++st.filtered_out;
        continue;
      }
      Texture t;
      if (!decode_flat(wad.lump_data(li), set.palette, &t)) {
        ++st.flats_skipped;
        continue;
      }
      t.name = name;
      if (auto it = set.flat_ids.find(name); it != set.flat_ids.end()) {  // later lumps override
        set.textures[size_t(it->second)] = std::move(t);
        ++st.flats_duplicate;
        continue;
      }
      if (!reserve(t.width, t.height)) continue;
      set.flat_ids.emplace(name, i32(set.textures.size()));
      set.textures.push_back(std::move(t));
      ++st.flats;
    }
  }
};

}  // namespace

i32 TextureSet::find_wall(std::string_view name) const {
  const auto it = wall_ids.find(lump_key(name));
  return it == wall_ids.end() ? -1 : it->second;
}

i32 TextureSet::find_flat(std::string_view name) const {
  const auto it = flat_ids.find(lump_key(name));
  return it == flat_ids.end() ? -1 : it->second;
}

bool decode_palette(std::span<const u8> playpal, Palette* out) {
  if (playpal.size() < 768) return false;
  for (size_t i = 0; i < 256; ++i) (*out)[i] = Rgb{playpal[3 * i], playpal[3 * i + 1], playpal[3 * i + 2]};
  return true;
}

bool decode_picture(std::span<const u8> lump, const Palette& pal, Picture* out, i32* bad_columns) {
  ParsedPicture pp;
  if (!parse_picture(lump, &pp)) {
    *out = Picture{};
    if (bad_columns) *bad_columns = 0;
    return false;
  }
  out->width = pp.width;
  out->height = pp.height;
  out->left = pp.left;
  out->top = pp.top;
  out->rgba.assign(size_t(pp.width) * size_t(pp.height) * 4, 0);
  draw_picture(pp, lump, 0, 0, pal, out->rgba.data(), pp.width, pp.height);
  if (bad_columns) *bad_columns = pp.bad_columns;
  return true;
}

bool decode_flat(std::span<const u8> lump, const Palette& pal, Texture* out) {
  i32 side = 0;
  switch (lump.size()) {
    case 8 * 8: side = 8; break;
    case 16 * 16: side = 16; break;
    case 32 * 32: side = 32; break;
    case 128 * 128: side = 128; break;
    case 256 * 256: side = 256; break;
    default: side = lump.size() >= 64 * 64 ? 64 : 0; break;
  }
  if (side == 0) return false;
  const size_t n = size_t(side) * size_t(side);
  out->width = out->height = side;
  out->is_flat = true;
  out->masked = false;
  out->has_holes = false;
  out->rgba.resize(n * 4);
  for (size_t i = 0; i < n; ++i) {
    const Rgb c = pal[lump[i]];
    out->rgba[4 * i + 0] = c.r;
    out->rgba[4 * i + 1] = c.g;
    out->rgba[4 * i + 2] = c.b;
    out->rgba[4 * i + 3] = 255;
  }
  return true;
}

bool load_textures(const Wad& wad, const TextureOptions& opt, TextureSet* out, TextureStats* stats, std::string* err) {
  TextureSet set;
  TextureStats st;
  const i32 pal_lump = wad.find_lump("PLAYPAL");
  const std::span<const u8> playpal = wad.lump_data(pal_lump);
  if (!decode_palette(playpal, &set.palette)) {
    if (err) *err = pal_lump < 0 ? "no PLAYPAL lump" : "PLAYPAL lump is too short";
    return false;
  }
  st.palettes = i32(playpal.size() / 768);
  const std::span<const u8> colormap = wad.lump_by_name("COLORMAP");
  set.colormap.assign(colormap.begin(), colormap.end());
  st.colormap_bytes = i32(colormap.size());

  Decoder d{wad, !opt.only.empty(), {}, opt.max_rgba_bytes, set, st, {}, {}, {}};
  for (const std::string& n : opt.only) d.only.insert(lump_key(n));
  if (opt.walls) d.read_walls();
  set.first_flat = i32(set.textures.size());
  if (opt.flats) d.read_flats();

  *out = std::move(set);
  if (stats) *stats = st;
  return true;
}

std::string sky_texture_name(std::string_view map_name) {
  const std::string k = lump_key(map_name);
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (k.size() == 4 && k[0] == 'E' && k[2] == 'M' && digit(k[1]) && digit(k[3])) {
    const int episode = k[1] - '0';
    return std::string("SKY") + char('0' + (episode >= 1 && episode <= 4 ? episode : 1));
  }
  if (k.size() == 5 && k.compare(0, 3, "MAP") == 0 && digit(k[3]) && digit(k[4])) {
    const int map = (k[3] - '0') * 10 + (k[4] - '0');
    return map < 12 ? "SKY1" : map < 21 ? "SKY2" : "SKY3";
  }
  return "";
}

std::vector<std::string> referenced_textures(const Map& map) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  auto add = [&](std::string key) {
    if (!key.empty() && key != "-" && seen.insert(key).second) out.push_back(std::move(key));
  };
  auto add_name = [&](const std::array<char, 9>& n) { add(lump_key(std::string_view(n.data(), 8))); };
  for (const Sidedef& s : map.sidedefs) {
    add_name(s.top);
    add_name(s.bottom);
    add_name(s.mid);
  }
  bool sky = false;
  for (const Sector& s : map.sectors) {
    add_name(s.floor_pic);
    add_name(s.ceil_pic);
    sky = sky || s.sky();
  }
  if (sky) add(sky_texture_name(map.name));
  return out;
}

}  // namespace svx::doom
