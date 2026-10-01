// svx_city tests — complex specs and records shared by the stages of the site complexes and the
// sites (tools/procgen_ref/lib/complexes.mjs is the Node twin): specs drawn from a sample stream
// in the shapes the reference's site kinds give planComplex, and random ones; a complex's plan as
// record lines; box lists (in full, or digests of every 32 boxes); ground-filled chunks and their
// digests.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "records.hpp"
#include "sites/complex.hpp"
#include "sites/kit.hpp"
#include "voxel/chunk.hpp"
#include "voxel/materials.hpp"

namespace svx::city::test {

inline const std::vector<std::string>& complex_theme_ids() {
  static const std::vector<std::string> t = {"military", "lab", "power", "barracks", "hangar", "containment"};
  return t;
}

// A stair shaft's rect (stairDims(30)) centred on x, its near end at y0.
inline Rect complex_shaft_rect(double cx, double y0) {
  const StairDims sd = stair_dims(30);
  return {cx - std::floor(sd.W / 2), y0, cx - std::floor(sd.W / 2) + sd.W - 1, y0 + sd.L - 1};
}

struct DrawnSpec {
  ComplexSpec spec;
  double top = 0;
};

// lib/complexes.mjs drawSpec.
inline DrawnSpec draw_complex_spec(rec::Samples& r, int kind) {
  const auto& THEMES = complex_theme_ids();
  const double ox = js::round((r() - 0.5) * 400000);
  const double oy = js::round((r() - 0.5) * 400000);
  const double top = js::round((r() - 0.4) * 3000);
  auto theme = [&]() -> std::string {
    if (r() < 0.1) return "";
    return THEMES[static_cast<size_t>(std::floor(r() * static_cast<double>(THEMES.size())))];
  };
  auto levels = [&]() -> double {
    const double t = r();
    return t < 0.1 ? 1 : t < 0.55 ? 2 : t < 0.9 ? 3 : 4;
  };
  DrawnSpec out;
  out.top = top;
  ComplexSpec& spec = out.spec;
  if (kind == 0) {
    const double W = vx(170 + r() * 120);
    const double H = vx(140 + r() * 100);
    const Rect rect{ox, oy, ox + W - 1, oy + H - 1};
    const Rect bounds{rect.x0 + vx(8), rect.y0 + vx(8), rect.x1 - vx(8), rect.y1 - vx(8)};
    const double bx = js::round(rect.x0 + W * (0.72 + r() * 0.2));
    const double by = js::round(rect.y0 + H * (0.05 + r() * 0.25));
    ComplexSectorSpec s;
    if (r() < 0.5) s.rooms = std::array<double, 2>{9, 14};
    s.bounds = bounds;
    s.z0 = top - vx(14);
    s.levels = levels();
    s.theme = theme();
    spec.sectors.push_back(s);
    ComplexEntrySpec e;
    if (r() < 0.8)
      e.open_top = true;
    else if (r() < 0.5)
      e.open_top = false;
    e.rect = complex_shaft_rect(bx, by + 12);
    e.z_top = top;
    e.sector = 0;
    e.dir = r() < 0.8 ? -1 : 1;
    spec.entries.push_back(e);
    return out;
  }
  if (kind == 1) {
    const double W = vx(300 + r() * 80);
    const double H = vx(240 + r() * 80);
    const Rect r0{ox, oy, ox + W - 1, oy + H - 1};
    const double pad = vx(10);
    const double mx = js::round((r0.x0 + r0.x1) / 2);
    const double my = js::round((r0.y0 + r0.y1) / 2);
    const std::vector<Rect> quads = {
        {r0.x0 + pad, r0.y0 + pad, mx - pad / 2, my - pad / 2},
        {mx + pad / 2, r0.y0 + pad, r0.x1 - pad, my - pad / 2},
        {r0.x0 + pad, my + pad / 2, mx - pad / 2, r0.y1 - pad},
        {mx + pad / 2, my + pad / 2, r0.x1 - pad, r0.y1 - pad},
    };
    std::vector<std::string> themes = {"lab", "containment", "power", "barracks"};
    for (int i = 3; i > 0; --i) {
      const size_t j = static_cast<size_t>(std::floor(r() * (i + 1)));
      std::swap(themes[static_cast<size_t>(i)], themes[j]);
    }
    for (size_t k = 0; k < quads.size(); ++k) {
      ComplexSectorSpec s;
      s.bounds = quads[k];
      s.z0 = top - vx(16) - static_cast<double>(k) * vx(9);
      s.levels = 2 + std::floor(r() * 2);
      s.theme = themes[k];
      s.rooms = std::array<double, 2>{8, 12};
      spec.sectors.push_back(s);
    }
    double deepest = js::kInf;
    for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * kLevelGap);
    const double fx0 = 0.18 + r() * 0.1;
    const double fx1 = 0.6 + r() * 0.1;
    const double fs[2][2] = {{fx0, 0.38}, {fx1, 0.4}};
    std::vector<double> seen;
    for (const auto& f : fs) {
      const double cx = js::round(r0.x0 + W * f[0]);
      const double qy = js::round(r0.y0 + H * f[1]);
      double sector = -1;
      for (size_t k = 0; k < quads.size(); ++k)
        if (cx >= quads[k].x0 && cx <= quads[k].x1 && qy >= quads[k].y0 && qy <= quads[k].y1) {
          sector = static_cast<double>(k);
          break;
        }
      if (sector < 0) sector = 0;
      bool dup = false;
      for (const double s : seen)
        if (s == sector) dup = true;
      if (dup) continue;
      seen.push_back(sector);
      ComplexEntrySpec e;
      e.rect = complex_shaft_rect(cx, qy + 12);
      e.z_top = top;
      e.sector = sector;
      e.dir = -1;
      e.open_top = true;
      spec.entries.push_back(e);
    }
    spec.tram_z = deepest - kLevelGap;
    return out;
  }
  if (kind == 2) {
    const double L = vx(70 + r() * 20);
    const double Wd = vx(55 + r() * 15);
    const double half_l = js::round(L * 1.05);
    const double half_w = js::round(Wd * 1.05);
    const bool along_x = r() < 0.5;
    auto q = [&](double u0, double v0, double u1, double v1) -> Rect {
      return along_x ? Rect{ox + u0, oy + v0, ox + u1, oy + v1} : Rect{ox + v0, oy + u0, ox + v1, oy + u1};
    };
    const std::vector<Rect> quads = {q(vx(6), -half_w, half_l, half_w), q(-half_l, -half_w, -vx(6), half_w)};
    const std::vector<std::string> themes = r() < 0.5 ? std::vector<std::string>{"military", "power"} : std::vector<std::string>{"lab", "containment"};
    for (size_t k = 0; k < quads.size(); ++k) {
      ComplexSectorSpec s;
      s.bounds = quads[k];
      s.z0 = top - vx(18) - static_cast<double>(k) * vx(8);
      s.levels = 2 + std::floor(r() * 2);
      s.theme = themes[k];
      s.rooms = std::array<double, 2>{8, 12};
      spec.sectors.push_back(s);
    }
    double deepest = js::kInf;
    for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * vx(15));
    const double px = ox + js::round((r() - 0.5) * L);
    const double py = oy + js::round((r() - 0.5) * Wd);
    double sector = -1;
    for (size_t k = 0; k < quads.size(); ++k)
      if (px >= quads[k].x0 && px <= quads[k].x1 && py >= quads[k].y0 && py <= quads[k].y1) {
        sector = static_cast<double>(k);
        break;
      }
    if (sector < 0) sector = 0;
    ComplexEntrySpec e;
    e.rect = complex_shaft_rect(px, py + 12);
    e.z_top = top + 1;
    e.sector = sector;
    e.dir = -1;
    e.open_top = true;
    spec.entries.push_back(e);
    spec.tram_z = deepest - vx(15);
    return out;
  }
  if (kind == 5) {
    const double W = vx(150 + r() * 100);
    const Rect bounds{ox, oy, ox + W, oy + W};
    const double ne = 10 + std::floor(r() * 20);
    for (double k = 0; k < ne; k += 1) {
      const double cx = js::round(ox + r() * W);
      const double cy = js::round(oy + r() * W);
      ComplexEntrySpec e;
      e.rect = complex_shaft_rect(cx, cy);
      e.z_top = top;
      e.sector = 0;
      e.dir = r() < 0.5 ? -1 : 1;
      e.open_top = true;
      spec.entries.push_back(e);
    }
    ComplexSectorSpec s;
    s.bounds = bounds;
    s.z0 = top - vx(14);
    s.levels = 1 + std::floor(r() * 2);
    s.theme = theme();
    s.rooms = std::array<double, 2>{ne + 10, ne + 30};
    spec.sectors.push_back(s);
    return out;
  }
  const double n = kind == 4 ? 1 + std::floor(r() * 2) : 1 + std::floor(r() * 5);
  double x = ox;
  for (double k = 0; k < n; k += 1) {
    const double w = kind == 4 ? vx(20 + r() * 40) : vx(60 + r() * 340);
    const double h = kind == 4 ? vx(20 + r() * 40) : vx(60 + r() * 340);
    const double y = oy + js::round((r() - 0.5) * vx(200));
    const double t = r();
    ComplexSectorSpec s;
    if (t < 0.3) {
    } else if (t < 0.6) {
      s.rooms = std::array<double, 2>{8, 12};
    } else {
      const double a = std::floor(r() * 14);
      s.rooms = std::array<double, 2>{a, a + std::floor(r() * 8)};
    }
    s.bounds = {x, y, x + w - 1, y + h - 1};
    s.z0 = top - vx(12 + r() * 30);
    s.levels = levels();
    s.theme = theme();
    spec.sectors.push_back(s);
    x += w + vx(10 + r() * 60);
  }
  const double ne = std::floor(r() * 4);
  for (double k = 0; k < ne; k += 1) {
    const double s0 = std::floor(r() * (n + 1));
    const double s = s0 - (r() < 0.1 ? 1 : 0);
    const Rect& b = spec.sectors[static_cast<size_t>(js::max(0, js::min(n - 1, s)))].bounds;
    const double cx = js::round(b.x0 + (b.x1 - b.x0) * r());
    const double cy = js::round(b.y0 + (b.y1 - b.y0) * r());
    ComplexEntrySpec e;
    if (r() < 0.6)
      e.open_top = true;
    else if (r() < 0.5)
      e.open_top = false;
    e.rect = complex_shaft_rect(cx, cy);
    e.z_top = top + std::floor(r() * 3);
    e.sector = s;
    e.dir = r() < 0.5 ? -1 : 1;
    spec.entries.push_back(e);
  }
  if (r() < 0.5) {
    double deepest = js::kInf;
    for (const ComplexSectorSpec& s : spec.sectors) deepest = js::min(deepest, s.z0 - (s.levels - 1) * kLevelGap);
    spec.tram_z = deepest - kLevelGap - std::floor(r() * vx(10));
  }
  return out;
}

inline std::string rect_list(const std::vector<Rect>& list) {
  std::string s;
  for (size_t i = 0; i < list.size(); ++i) s += js::cat(i ? ";" : "", list[i].x0, ",", list[i].y0, ",", list[i].x1, ",", list[i].y1);
  return s.empty() ? "none" : s;
}

inline void rect_fields(rec::Line& l, const Rect& q) { l << q.x0 << q.y0 << q.x1 << q.y1; }

// A complex's plan as record lines (lib/complexes.mjs complexLines).
inline void complex_lines(rec::Out& out, const Complex& cx) {
  using rec::Line;
  for (const ComplexSector& s : cx.sectors) {
    Line l;
    l << "sec" << s.id << s.theme;
    rect_fields(l, s.bounds);
    l << s.center.x << s.center.y;
    rect_fields(l, s.shaft_rect);
    std::string ks;
    for (size_t i = 0; i < s.levels.size(); ++i) ks += js::cat(i ? "," : "", cx.levels[s.levels[i]].k);
    l << ks << s.theme_def->id;
    out << l;
  }
  for (const ComplexLevel& lv : cx.levels) {
    std::string blocked;
    for (size_t i = 0; i < lv.blocked.size(); ++i) blocked += js::cat(i ? "," : "", lv.blocked[i][0], ":", lv.blocked[i][1]);
    out << (Line() << "lv" << lv.sector << lv.k << lv.zf << lv.theme << rect_list(lv.keep) << (lv.keep_clear.empty() ? std::string("-") : rect_list(lv.keep_clear))
                   << (blocked.empty() ? std::string("none") : blocked));
    for (const ComplexRoom& q : lv.rooms) {
      Line l;
      l << "rm";
      rect_fields(l, q.rect());
      l << q.type;
      if (q.shape.empty())
        l << rec::kUndef;
      else
        l << q.shape;
      if (q.fixed)
        l << true;
      else
        l << rec::kUndef;
      if (q.lower == q.lower)
        l << q.lower;
      else
        l << rec::kUndef;
      out << l;
    }
    out << (Line() << "co" << rect_list(lv.corridors));
  }
  for (const ComplexShaft& sh : cx.shafts) {
    const Stair& st = sh.st;
    Line l;
    l << "sh";
    rect_fields(l, sh.rect);
    std::string levels, flights;
    for (size_t i = 0; i < sh.levels.size(); ++i) levels += js::cat(i ? "," : "", sh.levels[i]);
    for (size_t i = 0; i < st.flights->size(); ++i) {
      const StairFlight& fl = (*st.flights)[i];
      flights += js::cat(i ? "," : "", fl.f, "/", fl.z0, "/", fl.H);
    }
    l << sh.dir << sh.open_top << sh.z_low << sh.z_high << levels;
    rect_fields(l, st.rect);
    l << st.axis << st.dir << st.lane_low << st.lane << st.landing << st.f0 << st.f1 << st.L << st.W << st.open << flights;
    out << l;
  }
  for (const ComplexLadder& L : cx.ladders) {
    Line l;
    l << "la";
    rect_fields(l, L.rect);
    l << L.z_top << L.z_bot << L.upper << L.lower << L.sector;
    out << l;
  }
  if (cx.tram) {
    const ComplexTram& t = *cx.tram;
    std::string routes;
    for (size_t i = 0; i < t.routes.size(); ++i) {
      if (i) routes += "|";
      for (size_t k = 0; k < t.routes[i].size(); ++k) routes += js::cat(k ? " " : "", t.routes[i][k].x, ",", t.routes[i][k].y);
    }
    out << (Line() << "tram" << t.z << rect_list(t.segs) << routes);
    for (const TramStation& s : t.stations) {
      Line l;
      l << "st" << s.sector;
      rect_fields(l, s.hall.rect());
      l << s.hall.type << s.hall.fixed << s.track.x << s.track.y;
      out << l;
    }
  }
  Line l;
  l << "bounds";
  if (cx.bounds)
    l << std::vector<double>{cx.bounds->x0, cx.bounds->y0, cx.bounds->x1, cx.bounds->y1};
  else
    l << rec::kUndef;
  out << l;
}

// FNV-1a over a string's characters (lib/complexes.mjs textDigest).
inline uint32_t text_digest(const std::string& s, uint32_t h = 2166136261u) {
  for (const unsigned char c : s) h = (h ^ c) * 16777619u;
  return h;
}

inline std::string box_line(const SiteBox& b) {
  return (rec::Line() << b.x0 << b.y0 << b.z0 << b.x1 << b.y1 << b.z1 << b.m << b.mode).str();
}

// Box lines (lib/complexes.mjs boxLines): the tag and length, then every box or the digests of
// every 32 boxes' lines.
inline void box_lines(rec::Out& out, const char* tag, const std::vector<SiteBox>& list, bool full = true) {
  out << (rec::Line() << tag << list.size());
  if (full) {
    for (const SiteBox& b : list) out << (rec::Line() << b.x0 << b.y0 << b.z0 << b.x1 << b.y1 << b.z1 << b.m << b.mode);
    return;
  }
  for (size_t k = 0; k < list.size(); k += 32) {
    uint32_t h = 2166136261u;
    for (size_t i = k; i < list.size() && i < k + 32; ++i) h = text_digest(box_line(list[i]) + "\n", h);
    out << (rec::Line() << k << h);
  }
}

// FNV-1a over a chunk's data (stages/chunk.mjs digest).
inline uint32_t chunk_digest(const std::vector<uint16_t>& a) {
  uint32_t h = 2166136261u;
  for (const uint16_t v : a) h = (h ^ v) * 16777619u;
  return h;
}

// A chunk filled as the ground pass would leave it under a surface at z = surface: rock below
// (with a sparse lattice of air cells) and air above (lib/complexes.mjs groundChunk).
inline ChunkBuffer ground_chunk(int lod, double cx, double cy, double cz, double surface) {
  ChunkBuffer ch(lod, cx, cy, cz);
  for (int k = 0; k < kP; ++k) {
    if (ch.wz(k) > surface) continue;
    for (int j = 0; j < kP; ++j)
      for (int i = 0; i < kP; ++i) ch.data[static_cast<size_t>(ChunkBuffer::index(i, j, k))] = (i * 7 + j * 13 + k * 5) % 11 == 0 ? 0 : MAT::ROCK;
  }
  return ch;
}

// Points of a complex worth a chunk (lib/complexes.mjs complexPoints).
inline std::vector<std::array<double, 3>> complex_points(const Complex& cx) {
  std::vector<std::array<double, 3>> pts;
  auto mid = [](const Rect& q) { return std::array<double, 2>{js::round((q.x0 + q.x1) / 2), js::round((q.y0 + q.y1) / 2)}; };
  for (const ComplexLevel& lv : cx.levels) {
    for (const ComplexRoom& q : lv.rooms) {
      const auto m = mid(q.rect());
      pts.push_back({m[0], m[1], lv.zf + 4});
    }
    for (const Rect& c : lv.corridors) {
      const auto m = mid(c);
      pts.push_back({m[0], m[1], lv.zf});
    }
  }
  for (const ComplexShaft& sh : cx.shafts) {
    const auto m = mid(sh.rect);
    pts.push_back({m[0], m[1], sh.z_low});
    pts.push_back({m[0], m[1], sh.z_high});
  }
  for (const ComplexLadder& L : cx.ladders) {
    const auto m = mid(L.rect);
    pts.push_back({m[0], m[1], L.z_top});
  }
  if (cx.tram) {
    for (const TramStation& s : cx.tram->stations) pts.push_back({s.track.x, s.track.y, cx.tram->z});
    for (const Rect& c : cx.tram->segs) {
      const auto m = mid(c);
      pts.push_back({m[0], m[1], cx.tram->z});
    }
  }
  return pts;
}

}  // namespace svx::city::test
