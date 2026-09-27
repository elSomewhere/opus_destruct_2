// svx_wad_textures — decode a WAD's palette, wall textures and flats (plan §B10) and optionally
// write them out as PNG files for inspection.
//
// usage: svx_wad_textures --wad FILE [--out DIR] [--list] [--sheet FILE.png] [--cell PX]
//                         [--map NAME] [--only NAME,NAME,...]
//   --out DIR     write DIR/walls/<NAME>.png and DIR/flats/<NAME>.png (RGBA; holes transparent)
//   --list        print one line per texture
//   --sheet FILE  write one contact-sheet PNG of every decoded texture, each scaled into a
//                 --cell sized square (default 64 px); holes are drawn as a grey checkerboard
//   --map NAME    decode only what that map references (+ its sky), as the engine would
//   --only LIST   decode only these comma-separated names
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "svx/doom/textures.hpp"
#include "svx/doom/wad.hpp"

using namespace svx;

namespace {

// ---- Minimal PNG writer: RGBA8, zlib stream of stored (uncompressed) deflate blocks.

u32 crc32(const u8* p, size_t n, u32 crc = 0) {
  static u32 table[256];
  static bool init = false;
  if (!init) {
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put_be32(std::vector<u8>* v, u32 x) {
  for (int s = 24; s >= 0; s -= 8) v->push_back(static_cast<u8>(x >> s));
}

void put_chunk(std::vector<u8>* png, const char* type, const std::vector<u8>& data) {
  put_be32(png, static_cast<u32>(data.size()));
  const size_t start = png->size();
  png->insert(png->end(), type, type + 4);
  png->insert(png->end(), data.begin(), data.end());
  put_be32(png, crc32(png->data() + start, png->size() - start));
}

bool write_png(const std::string& path, i32 w, i32 h, const u8* rgba) {
  std::vector<u8> raw;  // filter byte 0 + row
  raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
  for (i32 y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
  }
  std::vector<u8> z = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<u8>(n));
    z.push_back(static_cast<u8>(n >> 8));
    z.push_back(static_cast<u8>(~n));
    z.push_back(static_cast<u8>(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  u32 a = 1, b = 0;
  for (u8 c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(&z, (b << 16) | a);

  std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<u8> ihdr;
  put_be32(&ihdr, u32(w));
  put_be32(&ihdr, u32(h));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no filter, no interlace
  put_chunk(&png, "IHDR", ihdr);
  put_chunk(&png, "IDAT", z);
  put_chunk(&png, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  return std::fclose(f) == 0 && ok;
}

// File-name-safe version of a lump name (names may contain '\', which some tools emit).
std::string file_name(const std::string& name) {
  std::string s;
  for (char c : name) s.push_back((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? c : '~');
  return s;
}

// Contact sheet: every texture scaled (nearest, aspect kept) into a cell x cell square.
bool write_sheet(const std::string& path, const doom::TextureSet& ts, i32 cell) {
  const i32 gap = 2, cols = std::max(1, 1536 / (cell + gap));
  const i32 n = static_cast<i32>(ts.textures.size());
  const i32 rows = std::max(1, (n + cols - 1) / cols);
  const i32 W = cols * (cell + gap), H = rows * (cell + gap);
  std::vector<u8> img(size_t(W) * H * 4, 0);
  for (size_t i = 3; i < img.size(); i += 4) img[i] = 255;  // opaque black background
  for (i32 t = 0; t < n; ++t) {
    const doom::Texture& tx = ts.textures[t];
    const f64 s = std::min(f64(cell) / tx.width, f64(cell) / tx.height);
    const i32 dw = std::max(1, i32(tx.width * s)), dh = std::max(1, i32(tx.height * s));
    const i32 x0 = (t % cols) * (cell + gap), y0 = (t / cols) * (cell + gap);
    for (i32 y = 0; y < dh; ++y)
      for (i32 x = 0; x < dw; ++x) {
        const i32 sx = std::min(tx.width - 1, i32(x / s)), sy = std::min(tx.height - 1, i32(y / s));
        const u8* p = &tx.rgba[(size_t(sy) * tx.width + sx) * 4];
        u8* o = &img[(size_t(y0 + y) * W + x0 + x) * 4];
        if (p[3]) {
          o[0] = p[0], o[1] = p[1], o[2] = p[2];
        } else {  // hole: checkerboard
          const u8 g = ((x / 4 + y / 4) & 1) ? 96 : 160;
          o[0] = o[1] = o[2] = g;
        }
      }
  }
  return write_png(path, W, H, img.data());
}

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> out;
  size_t b = 0;
  while (b <= s.size()) {
    const size_t e = std::min(s.find(',', b), s.size());
    if (e > b) out.push_back(s.substr(b, e - b));
    b = e + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string wadp, outdir, sheet, mapn, only;
  bool list = false;
  i32 cell = 64;
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : "";
    if (k == "--wad") wadp = v, ++i;
    else if (k == "--out") outdir = v, ++i;
    else if (k == "--sheet") sheet = v, ++i;
    else if (k == "--cell") cell = std::clamp(std::atoi(v), 8, 512), ++i;
    else if (k == "--map") mapn = v, ++i;
    else if (k == "--only") only = v, ++i;
    else if (k == "--list") list = true;
    else {
      std::fprintf(stderr, "unknown argument %s\n", k.c_str());
      wadp.clear();
      break;
    }
  }
  doom::Wad wad;
  std::string err;
  if (wadp.empty() || !wad.load(wadp, &err)) {
    std::fprintf(stderr,
                 "usage: svx_wad_textures --wad FILE [--out DIR] [--list] [--sheet FILE.png] [--cell PX] "
                 "[--map NAME] [--only A,B,...] %s\n",
                 err.c_str());
    return 2;
  }

  doom::TextureOptions opt;
  opt.only = split_commas(only);
  if (!mapn.empty()) {
    doom::Map map;
    if (!wad.read_map(mapn, &map, &err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 1;
    }
    const std::vector<std::string> used = doom::referenced_textures(map);
    opt.only.insert(opt.only.end(), used.begin(), used.end());
    std::printf("map %s references %zu wall/flat names (sky texture %s)\n", mapn.c_str(), used.size(),
                doom::sky_texture_name(mapn).c_str());
  }

  const auto t0 = std::chrono::steady_clock::now();
  doom::TextureSet ts;
  doom::TextureStats st;
  if (!doom::load_textures(wad, opt, &ts, &st, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  size_t bytes = 0;
  for (const doom::Texture& t : ts.textures) bytes += t.rgba.size();

  std::printf("wad %s: %d lumps\n", wadp.c_str(), wad.lump_count());
  std::printf("palette: PLAYPAL has %d palettes (using #0), COLORMAP %d bytes\n", st.palettes, st.colormap_bytes);
  std::printf("patches: PNAMES %d names, %d missing; %d patch lumps decoded, %d bad, %d bad columns, %d tall\n",
              st.pnames, st.patches_missing, st.patches_decoded, st.patches_bad, st.patch_bad_columns,
              st.tall_patches);
  std::printf("walls: %d TEXTUREx lumps, %d records -> %d textures (%d skipped, %d duplicate), %d with holes; "
              "%d patch placements (%d skipped)\n",
              st.texture_lumps, st.texture_defs, st.walls, st.walls_skipped, st.walls_duplicate, st.walls_with_holes,
              st.patch_refs, st.patch_refs_skipped);
  std::printf("flats: %d decoded (%d skipped, %d duplicate)\n", st.flats, st.flats_skipped, st.flats_duplicate);
  if (!opt.only.empty()) std::printf("filter: %d items not requested\n", st.filtered_out);
  std::printf("sky: F_SKY1 flat id %d; SKY1 %d SKY2 %d SKY3 %d SKY4 %d\n", ts.find_flat("F_SKY1"),
              ts.find_wall("SKY1"), ts.find_wall("SKY2"), ts.find_wall("SKY3"), ts.find_wall("SKY4"));
  std::printf("total: %zu textures (%d walls + %d flats), %.1f MB RGBA, decoded in %.1f ms; skipped (malformed) %d\n",
              ts.textures.size(), ts.first_flat, i32(ts.textures.size()) - ts.first_flat, bytes / 1048576.0, ms,
              st.skipped());

  if (list)
    for (size_t i = 0; i < ts.textures.size(); ++i) {
      const doom::Texture& t = ts.textures[i];
      std::printf("%5zu %-5s %-8s %4dx%-4d%s%s\n", i, t.is_flat ? "flat" : "wall", t.name.c_str(), t.width, t.height,
                  t.has_holes ? " holes" : "", t.masked ? " masked" : "");
    }

  int failed = 0;
  if (!outdir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(outdir + "/walls", ec);
    std::filesystem::create_directories(outdir + "/flats", ec);
    for (const doom::Texture& t : ts.textures) {
      const std::string p = outdir + (t.is_flat ? "/flats/" : "/walls/") + file_name(t.name) + ".png";
      if (!write_png(p, t.width, t.height, t.rgba.data())) {
        std::fprintf(stderr, "cannot write %s\n", p.c_str());
        ++failed;
      }
    }
    std::printf("wrote %zu PNGs to %s/{walls,flats}\n", ts.textures.size() - failed, outdir.c_str());
  }
  if (!sheet.empty()) {
    if (write_sheet(sheet, ts, cell)) {
      std::printf("wrote contact sheet %s\n", sheet.c_str());
    } else {
      std::fprintf(stderr, "cannot write %s\n", sheet.c_str());
      ++failed;
    }
  }
  return failed ? 1 : 0;
}
