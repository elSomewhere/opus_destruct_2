// svx_city tests — the chunk writer (voxel_city voxel/chunk.js) against the reference (stage
// "chunk" of tools/procgen_ref).
#include <doctest.h>

#include <functional>

#include "records.hpp"
#include "voxel/chunk.hpp"

using namespace svx::city;
using rec::Line;

namespace {

// FNV-1a over the values (stages/chunk.mjs digest).
uint32_t digest(const std::vector<uint16_t>& a) {
  uint32_t h = 2166136261u;
  for (const uint16_t v : a) h = (h ^ v) * 16777619u;
  return h;
}

std::string range(const IdxRange& r) { return r.lo <= r.hi ? js::cat(r.lo, ",", r.hi) : "e"; }

}  // namespace

TEST_CASE("city chunk: the writer is the reference's (stage chunk)") {
  rec::Samples r(7);
  rec::Out out;
  for (int lod = 0; lod <= 8; ++lod)
    for (int c = 0; c < 6; ++c) {
      const double cx = std::floor((r() - 0.5) * 8);
      const double cy = std::floor((r() - 0.5) * 8);
      const double cz = std::floor((r() - 0.3) * 4);
      ChunkBuffer ch(lod, cx, cy, cz);
      if (r() < 0.5) ch.track_isolated();
      const Box3 wb = ch.world_box();
      const Box3 core = chunk_core_box(lod, cx, cy, cz);
      out << (Line() << "c" << lod << cx << cy << cz << ch.s << ch.half << ch.bx << ch.by << ch.bz << wb.x0 << wb.y0 << wb.z0 << wb.x1 << wb.y1 << wb.z1
                     << core.x0 << core.y0 << core.z0 << core.x1 << core.y1 << core.z1 << ch.wx(0) << ch.wy(5) << ch.wz(33));
      const double span = kP * ch.s;
      auto coord = [&](double b0, const std::function<double(double)>& rep) {
        const double t = r();
        if (t < 0.4) return rep(std::floor(r() * (kP + 6)) - 3);
        if (t < 0.9) return b0 + std::floor((r() * 1.4 - 0.2) * span);
        if (t < 0.96) return b0 + std::floor(r() * span) + 0.5;
        if (t < 0.98) return js::kNaN;
        return r() < 0.5 ? js::kInf : -js::kInf;
      };
      auto px = [&] { return coord(wb.x0, [&](double i) { return ch.wx(i); }); };
      auto py = [&] { return coord(wb.y0, [&](double j) { return ch.wy(j); }); };
      auto pz = [&] { return coord(wb.z0, [&](double k) { return ch.wz(k); }); };
      for (int op = 0; op < 150; ++op) {
        const int kind = static_cast<int>(std::floor(r() * 9));
        if (kind == 0 || kind == 1) {
          const double x = px();
          const double y = py();
          const double z = pz();
          const auto m = static_cast<uint16_t>(std::floor(r() * 400));
          if (kind == 0)
            ch.set(x, y, z, m);
          else
            ch.set_if_air(x, y, z, m);
        } else if (kind == 2 || kind == 3) {
          const double x0 = px();
          const double y0 = py();
          const double z0 = pz();
          const double x1 = x0 + std::floor(r() * span * 0.6);
          const double y1 = y0 + std::floor(r() * span * 0.6);
          const double z1 = z0 + std::floor(r() * span * 0.6) - std::floor(span * 0.05);
          const auto m = static_cast<uint16_t>(std::floor(r() * 400));
          const int mode = static_cast<int>(std::floor(r() * 4));
          ch.fill_box(x0, y0, z0, x1, y1, z1, m, mode);
        } else if (kind == 4) {
          ch.isolating = r() < 0.5;
        } else if (kind == 5) {
          const double x = px();
          const double y = py();
          const double z = pz();
          const double d = std::floor((r() - 0.2) * span);
          out << (Line() << "r" << range(ch.range_x(x, x + d)) << range(ch.range_y(y - 7, y + d)) << range(ch.range_z(z, z + d + 3)));
        } else if (kind == 6) {
          const double x = px();
          const double y = py();
          const double z = pz();
          if (x == x && y == y && z == z) out << (Line() << "s" << ch.sample_world(x, y, z) << ch.sample_world(x + 0.25, y, z - 0.5));
        } else if (kind == 7) {
          const double x0 = wb.x0 + std::floor((r() * 2 - 0.6) * span);
          const double y0 = wb.y0 + std::floor((r() * 2 - 0.6) * span);
          const double z0 = wb.z0 + std::floor((r() * 2 - 0.6) * span);
          const double d = std::floor(r() * span * 0.3);
          out << (Line() << "t" << ch.touches(x0, y0, z0, x0 + d, y0 + d, z0 + d) << ch.touches_rect(Rect{x0, y0, x0 + d, y0 + 2 * d}, z0 - d, z0));
        } else {
          const int i = static_cast<int>(std::floor(r() * kP));
          const int j = static_cast<int>(std::floor(r() * kP));
          const int k = static_cast<int>(std::floor(r() * kP));
          out << (Line() << "g" << ch.get(i, j, k));
        }
        if (op % 25 == 24) {
          Line l;
          l << "d" << digest(ch.data);
          if (ch.iso_data())
            l << digest(ch.iso);
          else
            l << rec::kUndef;
          out << l;
        }
      }
      const int n = ch.count_non_air();
      Line l;
      l << "n" << n << ch.non_air << digest(ch.data);
      if (ch.iso_data())
        l << digest(ch.iso);
      else
        l << rec::kUndef;
      out << l;
    }
  CHECK(rec::record("chunk", out.text()) == rec::recorded_digest("chunk"));
}

TEST_CASE("city chunk: writes land on representatives only") {
  ChunkBuffer c0(0, 0, 0, 0);
  c0.set(5, 6, 7, 9);
  CHECK(c0.sample_world(5, 6, 7) == 9);
  CHECK(c0.get(6, 7, 8) == 9);
  c0.set(5.5, 6, 7, 3);  // (a typed array ignores a store at a non-integer index)
  CHECK(c0.count_non_air() == 1);
  ChunkBuffer c2(2, 1, 0, 0);  // s = 4, bx = 124
  CHECK(c2.bx == 124);
  c2.set(124 + 4 * 3 + 2, 2, 2, 7);  // the representative of padded (3, 1, 1)... (y = -4 + 4 + 2)
  CHECK(c2.get(3, 1, 1) == 7);
  c2.set(124 + 4 * 3 + 1, 2, 2, 8);  // not a representative
  CHECK(c2.count_non_air() == 1);
  c2.fill_box(-1e9, -1e9, -1e9, 1e9, 1e9, 1e9, 4);
  CHECK(c2.count_non_air() == kP3);
  CHECK(c2.range_x(js::kNaN, 5).empty());
}
