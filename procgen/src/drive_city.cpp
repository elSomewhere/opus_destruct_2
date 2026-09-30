// structvox game — an endless procedural city to drive through (drive_city.hpp).
//
// The city is a grid of cells of kP voxels (56 m); at each cell's lower edges run two roads -
// one along x, one along y - that cross at its corner. Every fourth road is an avenue (two lanes
// each way, a double centre line), the others streets (a lane each way and a parking strip at
// each kerb). A cell makes its two roads (up to the next junctions), its junction, its sidewalks
// with their lamps and trees, and the block inside them: one to four lots, each a building (an
// apartment block, shops with glass fronts, a house with a garden, an office tower, a warehouse)
// or a park or a car park. Everything is boxes of voxels (air boxes cut windows and doors) laid
// down in passes - lots, sidewalks, carriageways and their markings, street furniture - so
// that what overlaps between cells comes out the same whichever cell is drawn first. The same
// boxes make a chunk's voxels, its paint (the "paint" layer: facades, markings, cars' colours)
// and the far tier's coarse view.
#include "svx/procgen/drive_city.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "svx/game/materials.hpp"
#include "svx/game/paint.hpp"

namespace svx {

namespace {

inline u64 mix(u64 x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}
inline u64 hash2(u64 seed, i32 a, i32 b, u64 salt = 0) {
  return mix(seed ^ mix((static_cast<u64>(static_cast<u32>(a)) << 32 | static_cast<u32>(b)) ^ (salt * 0xD1B54A32D192ED03ull)));
}
inline f64 unit(u64 h) { return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0); }
inline i32 floordiv(i32 a, i32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// (voxels)
constexpr i32 kP = 448;                  // a cell: 56 m
constexpr i32 kLane = 28;                // 3.5 m
constexpr i32 kStreetCarriage = 46;      // half: a lane and a parking strip (5.75 m)
constexpr i32 kStreetWalk = 24;          // 3 m
constexpr i32 kAvenueCarriage = 56;      // half: two lanes (7 m)
constexpr i32 kAvenueWalk = 28;
constexpr i32 kRock = 10;                // rock under the ground's top two layers
constexpr i32 kTopChunk = 18;
constexpr i32 kExtent = 32000;           // chunks each way (the world's voxel keys reach 2^20)

// Where the boxes go.
class Sink {
 public:
  virtual ~Sink() = default;
  IVec3 lo, hi;  // (the region drawn: [lo, hi) - boxes outside it are skipped)
  virtual void cell(const IVec3& p, Vox v, u8 paint) = 0;
  // [a, b): voxels v (air: cut), painted
  void box(IVec3 a, IVec3 b, Vox v, Paint paint = Paint::None) {
    for (int k = 0; k < 3; ++k) {
      a[k] = std::max(a[k], lo[k]);
      b[k] = std::min(b[k], hi[k]);
      if (a[k] >= b[k]) return;
    }
    fill(a, b, v, static_cast<u8>(paint));
  }
  // a rough ball of voxels (a tree's crown): a share `density` of those within r of c
  void ball(const IVec3& c, i32 r, Vox v, Paint paint, f64 density, u64 seed) {
    for (int k = 0; k < 3; ++k)
      if (c[k] + r < lo[k] || c[k] - r >= hi[k]) return;
    for (i32 x = std::max(lo[0], c[0] - r); x <= std::min(hi[0] - 1, c[0] + r); ++x)
      for (i32 y = std::max(lo[1], c[1] - r); y <= std::min(hi[1] - 1, c[1] + r); ++y)
        for (i32 z = std::max(lo[2], c[2] - r); z <= std::min(hi[2] - 1, c[2] + r); ++z) {
          const i32 dx = x - c[0], dy = y - c[1], dz = z - c[2];
          if (dx * dx + dy * dy + dz * dz > r * r) continue;
          if (unit(mix(seed ^ (static_cast<u64>(static_cast<u32>(x)) * 73856093ull) ^ (static_cast<u64>(static_cast<u32>(y)) * 19349663ull) ^
                       (static_cast<u64>(static_cast<u32>(z)) * 83492791ull))) > density)
            continue;
          cell({x, y, z}, v, static_cast<u8>(paint));
        }
  }

 protected:
  virtual void fill(const IVec3& a, const IVec3& b, Vox v, u8 paint) {
    for (i32 x = a[0]; x < b[0]; ++x)
      for (i32 y = a[1]; y < b[1]; ++y)
        for (i32 z = a[2]; z < b[2]; ++z) cell({x, y, z}, v, paint);
  }
};

// A chunk's voxels, or its paint.
class ChunkSink final : public Sink {
 public:
  ChunkSink(const IVec3& cc, std::vector<Vox>* vox, std::vector<u8>* paint) : vox_(vox), paint_(paint) {
    lo = {cc[0] * kChunk, cc[1] * kChunk, cc[2] * kChunk};
    hi = {lo[0] + kChunk, lo[1] + kChunk, lo[2] + kChunk};
  }
  bool any = false;
  void cell(const IVec3& p, Vox v, u8 paint) override {
    const size_t i = static_cast<size_t>(((p[0] - lo[0]) * kChunk + (p[1] - lo[1])) * kChunk + (p[2] - lo[2]));
    if (vox_) {
      (*vox_)[i] = v;
      any = any || v != kAir;
    }
    if (paint_) {
      (*paint_)[i] = v == kAir ? 0 : paint;
      any = any || (v != kAir && paint != 0);
    }
  }

 protected:
  void fill(const IVec3& a, const IVec3& b, Vox v, u8 paint) override {
    // (a column at a time: z is the fastest index)
    for (i32 x = a[0]; x < b[0]; ++x)
      for (i32 y = a[1]; y < b[1]; ++y) {
        const size_t base = static_cast<size_t>(((x - lo[0]) * kChunk + (y - lo[1])) * kChunk);
        if (vox_) std::fill(vox_->begin() + static_cast<long>(base + size_t(a[2] - lo[2])), vox_->begin() + static_cast<long>(base + size_t(b[2] - lo[2])), v);
        if (paint_)
          std::fill(paint_->begin() + static_cast<long>(base + size_t(a[2] - lo[2])), paint_->begin() + static_cast<long>(base + size_t(b[2] - lo[2])),
                    v == kAir ? u8{0} : paint);
      }
    any = any || (vox_ && v != kAir) || (paint_ && v != kAir && paint != 0);
  }

 private:
  std::vector<Vox>* vox_;
  std::vector<u8>* paint_;
};

// The far tier's coarse view: a cell is solid where any solid box covers part of it, air only
// where an air box covers all of it (thin walls survive, windows do not punch through).
class CoarseSink final : public Sink {
 public:
  CoarseSink(const IVec3& clo, const IVec3& n, i32 f, std::vector<Vox>* out) : clo_(clo), n_(n), f_(f), out_(out) {
    lo = clo;
    hi = {clo[0] + n[0] * f, clo[1] + n[1] * f, clo[2] + n[2] * f};
  }
  void cell(const IVec3& p, Vox v, u8) override {
    if (v == kAir) return;
    set({(p[0] - clo_[0]) / f_, (p[1] - clo_[1]) / f_, (p[2] - clo_[2]) / f_}, v);
  }

 protected:
  void fill(const IVec3& a, const IVec3& b, Vox v, u8) override {
    i32 c0[3], c1[3];
    for (int q = 0; q < 3; ++q) {
      const f64 s = static_cast<f64>(a[q] - clo_[q]) / f_, e = static_cast<f64>(b[q] - clo_[q]) / f_;
      c0[q] = static_cast<i32>(v == kAir ? std::ceil(s) : std::floor(s));
      c1[q] = static_cast<i32>(v == kAir ? std::floor(e) : std::ceil(e));
      c0[q] = std::max(c0[q], 0);
      c1[q] = std::min(c1[q], n_[q]);
      if (c0[q] >= c1[q]) return;
    }
    for (i32 x = c0[0]; x < c1[0]; ++x)
      for (i32 y = c0[1]; y < c1[1]; ++y)
        for (i32 z = c0[2]; z < c1[2]; ++z) set({x, y, z}, v);
  }

 private:
  void set(const IVec3& c, Vox v) { (*out_)[(size_t(c[0]) * size_t(n_[1]) + size_t(c[1])) * size_t(n_[2]) + size_t(c[2])] = v; }
  IVec3 clo_, n_;
  i32 f_;
  std::vector<Vox>* out_;
};

const Vox kRockV = make_vox(MaterialId::Rock, true);
const Vox kAsphaltV = make_vox(mat::Asphalt, true);
const Vox kWalkV = make_vox(MaterialId::Concrete, true);
const Vox kSoilV = make_vox(MaterialId::Soil, true);
const Vox kLotAsphaltV = make_vox(mat::Asphalt, true);
const Vox kRcV = make_vox(MaterialId::Rc, false);
const Vox kBrickV = make_vox(MaterialId::Masonry, false);
const Vox kWoodV = make_vox(MaterialId::Wood, false);
const Vox kGlassV = make_vox(mat::Window, false);
const Vox kSteelV = make_vox(MaterialId::SteelSection, false);
const Vox kSheetV = make_vox(mat::Sheet, false);
const Vox kPlasticV = make_vox(mat::Plastic, false);
const Vox kLampV = make_vox(mat::Lamp, false);
const Vox kStoneV = make_vox(MaterialId::Stone, false);
const Vox kRebarV = make_vox(MaterialId::Rebar, false);

constexpr std::array<Paint, 7> kFacades = {Paint::Plaster, Paint::Cream, Paint::Terracotta, Paint::Sand, Paint::Slate, Paint::Ochre, Paint::Mint};

// A lot: [x0, x1) x [y0, y1) on the ground (its top at z = 0), and the sides it faces a road on.
struct Lot {
  i32 x0, x1, y0, y1;
  bool front_xm = false, front_xp = false, front_ym = false, front_yp = false;
};

class DriveCity final : public GameSource, public RoadNetwork {
 public:
  DriveCity(u64 seed, f64 h) : seed_(mix(seed ^ 0xC17EC17Eull)), h_(h) {}

  // ---- the chunks
  bool generate(const IVec3& cc, std::vector<Vox>& out) const override {
    out.assign(kChunkVox, kAir);
    ChunkSink s(cc, &out, nullptr);
    draw(s);
    return s.any;
  }
  bool generate_layer(const IVec3& cc, const std::string& layer, std::vector<u8>& out) const override {
    if (layer != "paint") return false;
    out.assign(kChunkVox, 0);
    ChunkSink s(cc, nullptr, &out);
    draw(s);
    return s.any;
  }
  bool coarse(const IVec3& lo, const IVec3& n, i32 f, std::vector<Vox>& out) const override {
    out.assign(size_t(n[0]) * size_t(n[1]) * size_t(n[2]), kAir);
    CoarseSink s(lo, n, f, &out);
    draw(s, true);
    return true;
  }
  IVec3 chunk_lo() const override { return {-kExtent, -kExtent, -1}; }
  IVec3 chunk_hi() const override { return {kExtent, kExtent, kTopChunk}; }
  // (a cell: its buildings come back whole)
  u64 region(const IVec3& c) const override { return key3(floordiv(c[0] * kChunk, kP), floordiv(c[1] * kChunk, kP), 0); }
  V3 spawn_pos() const override {
    // on the sidewalk of the street along x through the origin, a car's length from the junction
    const i32 y = -carriage(0) - 10;
    return V3{wx(carriage(0) + 120), wx(y), h_ * 0.5 + 0.02};
  }
  V3 spawn_dir() const override { return {1.0, 0.0, 0.0}; }
  const RoadNetwork* roads() const override { return this; }

  // ---- the roads
  void lanes_in(const V3& lo, const V3& hi, std::vector<Lane>& out) const override {
    const i32 vx0 = static_cast<i32>(std::floor(lo.x / h_)), vx1 = static_cast<i32>(std::ceil(hi.x / h_));
    const i32 vy0 = static_cast<i32>(std::floor(lo.y / h_)), vy1 = static_cast<i32>(std::ceil(hi.y / h_));
    // roads along x (lines y = j kP) and along y (x = i kP) through the box
    for (i32 j = floordiv(vy0 - 100, kP) + 1; j * kP <= vy1 + 100; ++j)
      for (i32 i = floordiv(vx0, kP) - 1; i * kP <= vx1; ++i)
        for (int d = 0; d < 2; ++d)
          for (int k = 0; k < lanes(j); ++k) {
            Lane l;
            if (lane(lane_id(0, d, k, j, i), &l)) out.push_back(l);
          }
    for (i32 i = floordiv(vx0 - 100, kP) + 1; i * kP <= vx1 + 100; ++i)
      for (i32 j = floordiv(vy0, kP) - 1; j * kP <= vy1; ++j)
        for (int d = 0; d < 2; ++d)
          for (int k = 0; k < lanes(i); ++k) {
            Lane l;
            if (lane(lane_id(1, d, k, i, j), &l)) out.push_back(l);
          }
  }
  bool lane(u64 id, Lane* out) const override {
    int axis, d, k;
    i32 road, seg;
    unpack(id, &axis, &d, &k, &road, &seg);
    if (k >= lanes(road)) return false;
    // (along its road, from the junction at seg to the one at seg + 1)
    const i32 s0 = seg * kP + carriage(seg), s1 = (seg + 1) * kP - carriage(seg + 1);
    const f64 across = road * kP + (d == 0 ? -1.0 : 1.0) * (k + 0.5) * kLane * (axis == 0 ? 1.0 : -1.0);
    const f64 a = d == 0 ? s0 : s1, b = d == 0 ? s1 : s0;
    Lane l;
    l.id = id;
    if (axis == 0) {
      l.a = V3{wx(a), wx(across), road_z()};
      l.b = V3{wx(b), wx(across), road_z()};
    } else {
      l.a = V3{wx(across), wx(a), road_z()};
      l.b = V3{wx(across), wx(b), road_z()};
    }
    l.width = kLane * h_;
    l.speed = avenue(road) ? 16.7 : 11.1;  // (60 km/h, 40 km/h)
    *out = l;
    return true;
  }
  void next(u64 id, std::vector<std::pair<u64, int>>& out) const override {
    int axis, d, k;
    i32 road, seg;
    unpack(id, &axis, &d, &k, &road, &seg);
    const bool right_lane = k == 0, left_lane = k == lanes(road) - 1;
    const i32 ahead = d == 0 ? seg + 1 : seg - 1;           // (the next segment of its road)
    const i32 cross = d == 0 ? seg + 1 : seg;               // (the road it crosses at its junction)
    out.push_back({lane_id(axis, d, std::min(k, lanes(road) - 1), road, ahead), 0});
    // heading + along x, right is - along y; heading + along y, right is + along x
    if (axis == 0) {
      if (right_lane) out.push_back({d == 0 ? lane_id(1, 1, 0, cross, road - 1) : lane_id(1, 0, 0, cross, road), 1});
      if (left_lane) out.push_back({d == 0 ? lane_id(1, 0, lanes(cross) - 1, cross, road) : lane_id(1, 1, lanes(cross) - 1, cross, road - 1), -1});
    } else {
      if (right_lane) out.push_back({d == 0 ? lane_id(0, 0, 0, cross, road) : lane_id(0, 1, 0, cross, road - 1), 1});
      if (left_lane) out.push_back({d == 0 ? lane_id(0, 1, lanes(cross) - 1, cross, road - 1) : lane_id(0, 0, lanes(cross) - 1, cross, road), -1});
    }
  }
  bool green(u64 id, f64 t) const override {
    int axis, d, k;
    i32 road, seg;
    unpack(id, &axis, &d, &k, &road, &seg);
    const i32 cross = d == 0 ? seg + 1 : seg;  // (its junction: this road and that one)
    const i32 ji = axis == 0 ? cross : road, jj = axis == 0 ? road : cross;
    const f64 cycle = 34.0;
    const f64 u = std::fmod(t + unit(hash2(seed_, ji, jj, 7)) * cycle, cycle);
    return axis == 0 ? u < 14.0 : (u >= 17.0 && u < 31.0);
  }
  void parking_in(const V3& lo, const V3& hi, std::vector<ParkingSpot>& out) const override {
    const i32 vx0 = static_cast<i32>(std::floor(lo.x / h_)), vx1 = static_cast<i32>(std::ceil(hi.x / h_));
    const i32 vy0 = static_cast<i32>(std::floor(lo.y / h_)), vy1 = static_cast<i32>(std::ceil(hi.y / h_));
    auto inside = [&](f64 x, f64 y) { return x >= lo.x && x <= hi.x && y >= lo.y && y <= hi.y; };
    // along the streets' kerbs (not the avenues'), clear of the junctions
    for (int axis = 0; axis < 2; ++axis) {
      const i32 a0 = axis == 0 ? vy0 : vx0, a1 = axis == 0 ? vy1 : vx1;  // (across)
      const i32 b0 = axis == 0 ? vx0 : vy0, b1 = axis == 0 ? vx1 : vy1;  // (along)
      for (i32 road = floordiv(a0 - 100, kP) + 1; road * kP <= a1 + 100; ++road) {
        if (avenue(road)) continue;
        for (i32 seg = floordiv(b0, kP) - 1; seg * kP <= b1; ++seg) {
          const i32 s0 = seg * kP + carriage(seg) + 72, s1 = (seg + 1) * kP - carriage(seg + 1) - 72;
          for (int side = 0; side < 2; ++side) {
            const f64 across = road * kP + (side == 0 ? -1.0 : 1.0) * (kLane + 9.0) * (axis == 0 ? 1.0 : -1.0);
            i32 n = 0;
            for (i32 s = s0; s + 40 <= s1; s += 48, ++n) {
              const f64 along = s + 20.0;
              ParkingSpot p;
              p.pos = axis == 0 ? V3{wx(along), wx(across), road_z()} : V3{wx(across), wx(along), road_z()};
              if (!inside(p.pos.x, p.pos.y)) continue;
              // (heading the way its side's traffic goes)
              const f64 pi = 3.14159265358979323846;
              p.yaw = axis == 0 ? (side == 0 ? 0.0 : pi) : (side == 0 ? 0.5 * pi : -0.5 * pi);
              p.id = hash2(seed_, road, seg, 1000 + static_cast<u64>(axis * 4 + side) * 1000 + static_cast<u64>(n));
              out.push_back(p);
            }
          }
        }
      }
    }
  }

  // ---- the walkways: the sidewalks from street corner to street corner, the crossings on the
  // zebras. A junction has four corners, where its zebras meet the sidewalks (on the zebras' centre
  // lines); a corner's walks are the sidewalks along its two roads and the crossings over them.
  void walks_in(const V3& lo, const V3& hi, std::vector<Walk>& out) const override {
    const i32 vx0 = static_cast<i32>(std::floor(lo.x / h_)), vx1 = static_cast<i32>(std::ceil(hi.x / h_));
    const i32 vy0 = static_cast<i32>(std::floor(lo.y / h_)), vy1 = static_cast<i32>(std::ceil(hi.y / h_));
    auto meets = [&](const Walk& w) {
      return std::max(w.a.x, w.b.x) >= lo.x && std::min(w.a.x, w.b.x) <= hi.x && std::max(w.a.y, w.b.y) >= lo.y && std::min(w.a.y, w.b.y) <= hi.y;
    };
    // (each walk once: with the junction at its a end)
    for (i32 i = floordiv(vx0, kP) - 1; i * kP <= vx1 + kP; ++i)
      for (i32 j = floordiv(vy0, kP) - 1; j * kP <= vy1 + kP; ++j)
        for (int k = 0; k < 8; ++k) {
          const int side = k & 1, kind = (k >> 2) & 1, axis = (k >> 1) & 1;
          const u64 id = kind == 0 ? walk_id(0, axis, side, axis == 0 ? j : i, axis == 0 ? i : j) : walk_id(1, axis, side, axis == 0 ? j : i, axis == 0 ? i : j);
          Walk w;
          if (walk(id, &w) && meets(w)) out.push_back(w);
        }
  }
  bool walk(u64 id, Walk* out) const override {
    int kind, axis, side;
    i32 road, seg;
    unpack_walk(id, &kind, &axis, &side, &road, &seg);
    const f64 z = 0.5 * h_;  // (the sidewalks' surface: the top of voxels z = 0)
    // (x, y) in voxels, the road's axis first: along, then across
    auto at = [&](f64 along, f64 across) { return axis == 0 ? V3{wx(along), wx(across), z} : V3{wx(across), wx(along), z}; };
    const f64 sg = side ? 1.0 : -1.0;
    Walk w;
    w.id = id;
    if (kind == 0) {
      // a sidewalk along the road, from junction seg to junction seg + 1, on its side
      const f64 across = road * kP + sg * corner(road);
      w.a = at(seg * kP + corner(seg), across);
      w.b = at((seg + 1) * kP - corner(seg + 1), across);
      const f64 in = sg * (line(road) - corner(road)) * h_;
      w.inset = axis == 0 ? V3{0.0, in, 0.0} : V3{in, 0.0, 0.0};
      w.width = (avenue(road) ? kAvenueWalk : kStreetWalk) * h_;
    } else {
      // a crossing over the road at junction seg, on its side of the junction
      const f64 along = seg * kP + sg * corner(seg);
      w.a = at(along, road * kP - corner(road));
      w.b = at(along, road * kP + corner(road));
      w.width = 24 * h_;
      w.crossing = true;
    }
    *out = w;
    return true;
  }
  void walk_next(u64 id, int end, std::vector<std::pair<u64, int>>& out) const override {
    i32 i, j;
    int sx, sy;
    corner_of(id, end, &i, &j, &sx, &sy);
    const std::pair<u64, int> at_corner[4] = {
        {walk_id(0, 0, sy > 0, j, sx > 0 ? i : i - 1), sx > 0 ? 0 : 1},  // (the sidewalk along x)
        {walk_id(0, 1, sx > 0, i, sy > 0 ? j : j - 1), sy > 0 ? 0 : 1},  // (the sidewalk along y)
        {walk_id(1, 0, sx > 0, j, i), sy > 0 ? 1 : 0},                  // (the crossing over the road along x)
        {walk_id(1, 1, sy > 0, i, j), sx > 0 ? 1 : 0},                  // (the crossing over the road along y)
    };
    for (const auto& w : at_corner)
      if (w.first != id || w.second != end) out.push_back(w);
  }
  bool walk_open(u64 id, f64 t) const override {
    int kind, axis, side;
    i32 road, seg;
    unpack_walk(id, &kind, &axis, &side, &road, &seg);
    if (kind == 0) return true;
    // (the junction's signals, as green(): a road's traffic stopped - the other road's going, the
    // way the people cross - for the first seconds of that)
    const i32 ji = axis == 0 ? seg : road, jj = axis == 0 ? road : seg;
    const f64 cycle = 34.0;
    const f64 u = std::fmod(t + unit(hash2(seed_, ji, jj, 7)) * cycle, cycle);
    return axis == 0 ? (u >= 17.0 && u < 21.0) : u < 4.0;
  }

 private:
  // ---- layout
  bool avenue(i32 road) const { return ((road % 4) + 4) % 4 == 0; }
  i32 lanes(i32 road) const { return avenue(road) ? 2 : 1; }
  i32 carriage(i32 road) const { return avenue(road) ? kAvenueCarriage : kStreetCarriage; }
  i32 half(i32 road) const { return avenue(road) ? kAvenueCarriage + kAvenueWalk : kStreetCarriage + kStreetWalk; }
  // (a voxel boundary in world metres: voxel p spans (p - 1/2) h .. (p + 1/2) h)
  f64 wx(f64 b) const { return (b - 0.5) * h_; }
  f64 road_z() const { return -0.5 * h_; }  // (the road's surface: the top of voxels z = -1)

  // A walkway's corners: off the road's centre line (the zebras' centre lines), and the walking
  // line along a sidewalk (towards the buildings, clear of the lamps and trees near the kerb).
  i32 corner(i32 road) const { return carriage(road) + 16; }
  i32 line(i32 road) const { return carriage(road) + (avenue(road) ? 20 : 19); }
  static u64 walk_id(int kind, int axis, int side, i32 road, i32 seg) {
    return static_cast<u64>(kind) | (static_cast<u64>(axis) << 1) | (static_cast<u64>(side & 1) << 2) | (static_cast<u64>(static_cast<u32>(road + (1 << 29)) & 0x3FFFFFFFu) << 3) |
           (static_cast<u64>(static_cast<u32>(seg + (1 << 29)) & 0x3FFFFFFFu) << 33);
  }
  static void unpack_walk(u64 id, int* kind, int* axis, int* side, i32* road, i32* seg) {
    *kind = static_cast<int>(id & 1);
    *axis = static_cast<int>((id >> 1) & 1);
    *side = static_cast<int>((id >> 2) & 1);
    *road = static_cast<i32>((id >> 3) & 0x3FFFFFFFu) - (1 << 29);
    *seg = static_cast<i32>((id >> 33) & 0x3FFFFFFFu) - (1 << 29);
  }
  // The corner at a walk's end: junction (i, j) and its quadrant (sx, sy: -1 / 1).
  static void corner_of(u64 id, int end, i32* i, i32* j, int* sx, int* sy) {
    int kind, axis, side;
    i32 road, seg;
    unpack_walk(id, &kind, &axis, &side, &road, &seg);
    const int sg = side ? 1 : -1;
    if (kind == 0) {
      // (a sidewalk: its road's junctions seg and seg + 1, on its side)
      const i32 g = end == 0 ? seg : seg + 1;
      const int e = end == 0 ? 1 : -1;
      if (axis == 0) *i = g, *j = road, *sx = e, *sy = sg;
      else *i = road, *j = g, *sx = sg, *sy = e;
    } else {
      // (a crossing: junction seg of its road, from the road's - side to its + side)
      const int e = end == 0 ? -1 : 1;
      if (axis == 0) *i = seg, *j = road, *sx = sg, *sy = e;
      else *i = road, *j = seg, *sx = e, *sy = sg;
    }
  }

  static u64 lane_id(int axis, int d, int k, i32 road, i32 seg) {
    return static_cast<u64>(axis) | (static_cast<u64>(d) << 1) | (static_cast<u64>(k & 3) << 2) | (static_cast<u64>(static_cast<u32>(road + (1 << 29)) & 0x3FFFFFFFu) << 4) |
           (static_cast<u64>(static_cast<u32>(seg + (1 << 29)) & 0x3FFFFFFFu) << 34);
  }
  static void unpack(u64 id, int* axis, int* d, int* k, i32* road, i32* seg) {
    *axis = static_cast<int>(id & 1);
    *d = static_cast<int>((id >> 1) & 1);
    *k = static_cast<int>((id >> 2) & 3);
    *road = static_cast<i32>((id >> 4) & 0x3FFFFFFFu) - (1 << 29);
    *seg = static_cast<i32>((id >> 34) & 0x3FFFFFFFu) - (1 << 29);
  }

  // ---- drawing: the cells that reach into the sink's region, pass by pass
  void draw(Sink& s, bool coarse_view = false) const {
    const i32 i0 = floordiv(s.lo[0] - 100, kP), i1 = floordiv(s.hi[0] + 100, kP);
    const i32 j0 = floordiv(s.lo[1] - 100, kP), j1 = floordiv(s.hi[1] + 100, kP);
    for (int pass = 0; pass < 4; ++pass)
      for (i32 i = i0; i <= i1; ++i)
        for (i32 j = j0; j <= j1; ++j) cell(i, j, pass, s, coarse_view);
  }

  // A cell: the roads along its lower edges (x at y = j kP, y at x = i kP) and its block.
  void cell(i32 i, i32 j, int pass, Sink& s, bool coarse_view) const {
    const i32 x0 = i * kP, y0 = j * kP;
    const i32 Ci = carriage(i), Cj = carriage(j), Hi = half(i), Hj = half(j);
    const i32 Hi1 = half(i + 1), Hj1 = half(j + 1);
    if (pass == 0) {
      // the ground: rock, the lots' ground (grass) up to the sidewalks' level
      s.box({x0, y0, -kRock - 2}, {x0 + kP, y0 + kP, -2}, kRockV);
      s.box({x0, y0, -2}, {x0 + kP, y0 + kP, 1}, kSoilV, Paint::Mint);
      block({x0 + Hi, x0 + kP - Hi1, y0 + Hj, y0 + kP - Hj1}, hash2(seed_, i, j, 1), s, coarse_view, avenue(i) || avenue(j) || avenue(i + 1) || avenue(j + 1));
    } else if (pass == 1) {
      // sidewalks (their kerbs a voxel up)
      s.box({x0, y0 - Hj, -2}, {x0 + kP, y0 + Hj, 1}, kWalkV);
      s.box({x0 - Hi, y0, -2}, {x0 + Hi, y0 + kP, 1}, kWalkV);
      if (!coarse_view) {
        s.box({x0, y0 - Cj - 1, 0}, {x0 + kP, y0 - Cj, 1}, kWalkV, Paint::Kerb);
        s.box({x0, y0 + Cj, 0}, {x0 + kP, y0 + Cj + 1, 1}, kWalkV, Paint::Kerb);
        s.box({x0 - Ci - 1, y0, 0}, {x0 - Ci, y0 + kP, 1}, kWalkV, Paint::Kerb);
        s.box({x0 + Ci, y0, 0}, {x0 + Ci + 1, y0 + kP, 1}, kWalkV, Paint::Kerb);
      }
    } else if (pass == 2) {
      // carriageways (and the junction), then their markings
      s.box({x0, y0 - Cj, -2}, {x0 + kP, y0 + Cj, 0}, kAsphaltV);
      s.box({x0, y0 - Cj, 0}, {x0 + kP, y0 + Cj, 1}, kAir);
      s.box({x0 - Ci, y0, -2}, {x0 + Ci, y0 + kP, 0}, kAsphaltV);
      s.box({x0 - Ci, y0, 0}, {x0 + Ci, y0 + kP, 1}, kAir);
      if (!coarse_view) {
        markings(0, j, i, s);
        markings(1, i, j, s);
      }
    } else if (!coarse_view) {
      furniture(0, j, i, s);
      furniture(1, i, j, s);
    }
  }

  // A road segment's markings: its centre line, lane lines, the edges of its parking strips,
  // and at each end a zebra crossing and the stop line before it.
  void markings(int axis, i32 road, i32 seg, Sink& s) const {
    const i32 c = road * kP, C = carriage(road);
    const i32 s0 = seg * kP + carriage(seg), s1 = (seg + 1) * kP - carriage(seg + 1);
    // (a box across [a0, a1) and along [b0, b1) on the road's surface)
    auto paint = [&](i32 a0, i32 a1, i32 b0, i32 b1, Paint p) {
      if (axis == 0)
        s.box({b0, a0, -1}, {b1, a1, 0}, kAsphaltV, p);
      else
        s.box({a0, b0, -1}, {a1, b1, 0}, kAsphaltV, p);
    };
    const i32 m0 = s0 + 36, m1 = s1 - 36;  // (the markings between the stop lines)
    if (avenue(road)) {
      paint(c - 2, c - 1, m0, m1, Paint::LineYellow);
      paint(c + 1, c + 2, m0, m1, Paint::LineYellow);
      for (i32 b = m0; b + 24 <= m1; b += 48) {
        paint(c - kLane - 1, c - kLane + 1, b, b + 24, Paint::LineWhite);
        paint(c + kLane - 1, c + kLane + 1, b, b + 24, Paint::LineWhite);
      }
    } else {
      for (i32 b = m0; b + 24 <= m1; b += 48) paint(c - 1, c + 1, b, b + 24, Paint::LineYellow);
      paint(c - kLane - 1, c - kLane, m0, m1, Paint::LineWhite);
      paint(c + kLane, c + kLane + 1, m0, m1, Paint::LineWhite);
    }
    // zebras (stripes along the road) and stop lines at both ends
    for (int end = 0; end < 2; ++end) {
      const i32 z0 = end == 0 ? s0 + 4 : s1 - 28, z1 = end == 0 ? s0 + 28 : s1 - 4;
      for (i32 a = c - C + 2; a + 4 <= c + C - 2; a += 8) paint(a, a + 4, z0, z1, Paint::LineWhite);
      // (a direction's stop line: + traffic stops at the far end, on its side)
      const bool plus = end == 1;
      const i32 lo_a = (plus == (axis == 0)) ? c - C : c, hi_a = (plus == (axis == 0)) ? c : c + C;
      const i32 l0 = end == 0 ? s0 + 32 : s1 - 36, l1 = l0 + 4;
      paint(lo_a, hi_a, l0, l1, Paint::LineWhite);
    }
  }

  // A road segment's sidewalks: street lamps every 28 m with trees between them.
  void furniture(int axis, i32 road, i32 seg, Sink& s) const {
    const i32 c = road * kP, C = carriage(road);
    const i32 s0 = seg * kP + half(seg) + 24, s1 = (seg + 1) * kP - half(seg + 1) - 24;
    for (int side = 0; side < 2; ++side) {
      const i32 sg = side == 0 ? -1 : 1;
      const i32 kerb = c + sg * (C + 4);        // (the lamps' line, near the kerb)
      auto at = [&](i32 across, i32 along, i32 z) { return axis == 0 ? IVec3{along, across, z} : IVec3{across, along, z}; };
      auto boxw = [&](i32 a0, i32 a1, i32 b0, i32 b1, i32 z0, i32 z1, Vox v, Paint p) {
        if (axis == 0)
          s.box({b0, a0, z0}, {b1, a1, z1}, v, p);
        else
          s.box({a0, b0, z0}, {a1, b1, z1}, v, p);
      };
      i32 n = 0;
      for (i32 b = s0; b <= s1; b += 112, ++n) {
        const u64 hh = hash2(seed_, road * 2 + side, seg * 64 + n, 3 + static_cast<u64>(axis));
        if (n % 2 == 0) {
          // a lamp: a pole with an arm out over the road and its lamp
          boxw(kerb, kerb + 1, b, b + 1, 1, 49, kSteelV, Paint::Graphite);
          const i32 a0 = std::min(kerb, kerb - sg * 12), a1 = std::max(kerb + 1, kerb - sg * 12 + 1);
          boxw(a0, a1, b, b + 1, 48, 49, kSteelV, Paint::Graphite);
          boxw(kerb - sg * 12 - 1, kerb - sg * 12 + 2, b - 1, b + 2, 47, 48, kLampV, Paint::None);
        } else if ((hh & 3) != 0) {
          // a tree: a trunk and a crown
          const i32 tc = kerb + sg * 6;
          boxw(tc, tc + 2, b, b + 2, 1, 22, kWoodV, Paint::None);
          const i32 r = 6 + static_cast<i32>(hh >> 8 & 3);
          s.ball(at(tc + 1, b + 1, 22 + r), r, kWoodV, Paint::Green, 0.42, hh);
        }
      }
    }
  }

  // ---- the blocks
  void block(const Lot& b, u64 hsh, Sink& s, bool coarse_view, bool busy) const {
    const i32 w = b.x1 - b.x0, d = b.y1 - b.y0;
    if (w < 64 || d < 64) return;
    const u64 r = mix(hsh);
    const int layout = static_cast<int>(r % 5);
    std::vector<Lot> lots;
    auto sides = [&](Lot l) {
      l.front_xm = l.x0 == b.x0;
      l.front_xp = l.x1 == b.x1;
      l.front_ym = l.y0 == b.y0;
      l.front_yp = l.y1 == b.y1;
      return l;
    };
    const i32 gap = 12;  // (an alley: 1.5 m)
    const i32 mx = b.x0 + w / 2, my = b.y0 + d / 2;
    if (layout <= 1) {
      // four lots
      lots.push_back(sides({b.x0, mx - gap / 2, b.y0, my - gap / 2}));
      lots.push_back(sides({mx + gap / 2, b.x1, b.y0, my - gap / 2}));
      lots.push_back(sides({b.x0, mx - gap / 2, my + gap / 2, b.y1}));
      lots.push_back(sides({mx + gap / 2, b.x1, my + gap / 2, b.y1}));
    } else if (layout == 2) {
      lots.push_back(sides({b.x0, mx - gap / 2, b.y0, b.y1}));
      lots.push_back(sides({mx + gap / 2, b.x1, b.y0, b.y1}));
    } else if (layout == 3) {
      lots.push_back(sides({b.x0, b.x1, b.y0, my - gap / 2}));
      lots.push_back(sides({b.x0, b.x1, my + gap / 2, b.y1}));
    } else {
      lots.push_back(sides(b));
    }
    for (size_t k = 0; k < lots.size(); ++k) lot(lots[k], mix(hsh ^ (k + 1) * 0x9E3779B97F4A7C15ull), s, coarse_view, busy, lots.size() == 1);
  }

  void lot(const Lot& L, u64 hsh, Sink& s, bool coarse_view, bool busy, bool whole) const {
    const f64 r = unit(hsh);
    const Paint facade = kFacades[size_t(mix(hsh ^ 0xFACADE) % kFacades.size())];
    const i32 w = L.x1 - L.x0, d = L.y1 - L.y0;
    const bool small = w < 120 || d < 120;
    // what stands on it
    if (whole && r < 0.22) return park(L, hsh, s);
    if (whole && r < 0.4) return car_park(L, hsh, s);
    if (whole && busy && r < 0.75) return tower(L, hsh, s, coarse_view);
    if (whole) return warehouse(L, hsh, s, coarse_view);
    if (r < 0.34) return apartment(L, hsh, facade, s, coarse_view);
    if (r < 0.6) return shops(L, hsh, facade, s, coarse_view);
    if (r < 0.84 || small) return house(L, hsh, facade, s, coarse_view);
    return park(L, hsh, s);
  }

  // (a lot's sides: which faces the road; x or y first)
  static i32 front_count(const Lot& L) { return int(L.front_xm) + int(L.front_xp) + int(L.front_ym) + int(L.front_yp); }

  // Walls around [x0, x1) x [y0, y1) from z0 up h, t thick, with windows every `pitch` (sill
  // and head above z0) cut and glazed; a door on each front side on the ground floor.
  void walls(i32 x0, i32 x1, i32 y0, i32 y1, i32 z0, i32 hgt, i32 t, Vox v, Paint p, i32 pitch, i32 sill, i32 head, i32 win, Sink& s,
             bool coarse_view) const {
    s.box({x0, y0, z0}, {x1, y0 + t, z0 + hgt}, v, p);
    s.box({x0, y1 - t, z0}, {x1, y1, z0 + hgt}, v, p);
    s.box({x0, y0, z0}, {x0 + t, y1, z0 + hgt}, v, p);
    s.box({x1 - t, y0, z0}, {x1, y1, z0 + hgt}, v, p);
    if (coarse_view || pitch <= 0) return;
    // windows: glass in the outer layer, air behind it
    for (i32 x = x0 + pitch / 2; x + win <= x1 - pitch / 3; x += pitch) {
      s.box({x, y0, z0 + sill}, {x + win, y0 + t, z0 + head}, kAir);
      s.box({x, y0, z0 + sill}, {x + win, y0 + 1, z0 + head}, kGlassV);
      s.box({x, y1 - t, z0 + sill}, {x + win, y1, z0 + head}, kAir);
      s.box({x, y1 - 1, z0 + sill}, {x + win, y1, z0 + head}, kGlassV);
    }
    for (i32 y = y0 + pitch / 2; y + win <= y1 - pitch / 3; y += pitch) {
      s.box({x0, y, z0 + sill}, {x0 + t, y + win, z0 + head}, kAir);
      s.box({x0, y, z0 + sill}, {x0 + 1, y + win, z0 + head}, kGlassV);
      s.box({x1 - t, y, z0 + sill}, {x1, y + win, z0 + head}, kAir);
      s.box({x1 - 1, y, z0 + sill}, {x1, y + win, z0 + head}, kGlassV);
    }
  }

  // An apartment block: a concrete frame (columns, slabs) with brick walls, windows, a door on
  // the street, a parapet on its roof.
  void apartment(const Lot& L, u64 hsh, Paint facade, Sink& s, bool coarse_view) const {
    const i32 storeys = 3 + static_cast<i32>(mix(hsh ^ 11) % 5);
    const i32 x0 = L.x0 + 2, x1 = L.x1 - 2, y0 = L.y0 + 2, y1 = L.y1 - 2;
    const i32 H = 26;  // (storey: 24 + a slab of 2)
    for (i32 k = 0; k < storeys; ++k) {
      const i32 z0 = 1 + k * H;
      walls(x0, x1, y0, y1, z0, 24, 2, kBrickV, facade, 28, 7, 19, 10, s, coarse_view);
      // columns (with a bar through each), then the slab over the storey
      for (i32 x : {x0, (x0 + x1) / 2 - 1, x1 - 3})
        for (i32 y : {y0, (y0 + y1) / 2 - 1, y1 - 3}) {
          s.box({x, y, z0}, {x + 3, y + 3, z0 + 24}, kRcV, facade);
          if (!coarse_view) s.box({x + 1, y + 1, z0}, {x + 2, y + 2, z0 + H}, kRebarV);
        }
      s.box({x0, y0, z0 + 24}, {x1, y1, z0 + H}, kRcV, Paint::None);
    }
    s.box({x0, y0, 1 + storeys * H}, {x1, y0 + 1, 5 + storeys * H}, kBrickV, facade);
    s.box({x0, y1 - 1, 1 + storeys * H}, {x1, y1, 5 + storeys * H}, kBrickV, facade);
    s.box({x0, y0, 1 + storeys * H}, {x0 + 1, y1, 5 + storeys * H}, kBrickV, facade);
    s.box({x1 - 1, y0, 1 + storeys * H}, {x1, y1, 5 + storeys * H}, kBrickV, facade);
    if (!coarse_view) doors(L, x0, x1, y0, y1, 2, s);
  }

  // A door on each front: a 1 m gap, 2.2 m high, in the middle.
  void doors(const Lot& L, i32 x0, i32 x1, i32 y0, i32 y1, i32 t, Sink& s) const {
    const i32 mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    if (L.front_ym) s.box({mx - 4, y0, 1}, {mx + 4, y0 + t, 19}, kAir);
    if (L.front_yp) s.box({mx - 4, y1 - t, 1}, {mx + 4, y1, 19}, kAir);
    if (L.front_xm) s.box({x0, my - 4, 1}, {x0 + t, my + 4, 19}, kAir);
    if (L.front_xp) s.box({x1 - t, my - 4, 1}, {x1, my + 4, 19}, kAir);
  }

  // Shops: two storeys, their street fronts glass between concrete piers, an awning over them.
  void shops(const Lot& L, u64 hsh, Paint facade, Sink& s, bool coarse_view) const {
    const i32 x0 = L.x0 + 1, x1 = L.x1 - 1, y0 = L.y0 + 1, y1 = L.y1 - 1;
    walls(x0, x1, y0, y1, 1, 24, 2, kBrickV, facade, 0, 0, 0, 0, s, coarse_view);
    s.box({x0, y0, 25}, {x1, y1, 27}, kRcV);
    walls(x0, x1, y0, y1, 27, 24, 2, kBrickV, facade, 28, 7, 19, 10, s, coarse_view);
    s.box({x0, y0, 51}, {x1, y1, 53}, kRcV);
    s.box({x0, y0, 53}, {x1, y0 + 1, 56}, kBrickV, facade);
    s.box({x0, y1 - 1, 53}, {x1, y1, 56}, kBrickV, facade);
    s.box({x0, y0, 53}, {x0 + 1, y1, 56}, kBrickV, facade);
    s.box({x1 - 1, y0, 53}, {x1, y1, 56}, kBrickV, facade);
    if (coarse_view) return;
    const Paint awning = kCarPaints[size_t(mix(hsh ^ 0xA7) % kCarPaints.size())];
    // (each front: glass between piers every 5 m, a door, an awning)
    auto front = [&](bool along_x, i32 fixed, i32 out, i32 a0, i32 a1) {
      for (i32 a = a0 + 3; a + 36 <= a1 - 3; a += 40) {
        if (along_x) {
          s.box({a, std::min(fixed, fixed + out * 2), 3}, {a + 36, std::max(fixed, fixed + out * 2) + 1, 21}, kAir);
          s.box({a, fixed, 3}, {a + 36, fixed + 1, 21}, kGlassV);
        } else {
          s.box({std::min(fixed, fixed + out * 2), a, 3}, {std::max(fixed, fixed + out * 2) + 1, a + 36, 21}, kAir);
          s.box({fixed, a, 3}, {fixed + 1, a + 36, 21}, kGlassV);
        }
      }
      if (along_x)
        s.box({a0, out < 0 ? fixed - 10 : fixed + 1, 22}, {a1, out < 0 ? fixed : fixed + 11, 23}, kPlasticV, awning);
      else
        s.box({out < 0 ? fixed - 10 : fixed + 1, a0, 22}, {out < 0 ? fixed : fixed + 11, a1, 23}, kPlasticV, awning);
    };
    if (L.front_ym) front(true, y0, 1, x0, x1);
    if (L.front_yp) front(true, y1 - 1, -1, x0, x1);
    if (L.front_xm) front(false, x0, 1, y0, y1);
    if (L.front_xp) front(false, x1 - 1, -1, y0, y1);
    // (the fronts face out: their awnings over the sidewalk)
    (void)hsh;
  }

  // A house: brick, two storeys, timber floors, a pitched roof of tiles; set back behind its
  // garden's low wall.
  void house(const Lot& L, u64 hsh, Paint facade, Sink& s, bool coarse_view) const {
    const i32 set = 24;
    const i32 x0 = L.x0 + (L.front_xm ? set : 8), x1 = L.x1 - (L.front_xp ? set : 8);
    const i32 y0 = L.y0 + (L.front_ym ? set : 8), y1 = L.y1 - (L.front_yp ? set : 8);
    if (x1 - x0 < 48 || y1 - y0 < 48) return;
    walls(x0, x1, y0, y1, 1, 24, 2, kBrickV, facade, 24, 7, 19, 8, s, coarse_view);
    s.box({x0, y0, 25}, {x1, y1, 26}, kWoodV);
    walls(x0, x1, y0, y1, 26, 22, 2, kBrickV, facade, 24, 6, 17, 8, s, coarse_view);
    // the roof: along its longer side, stepped in a voxel a voxel up
    const bool along_x = (x1 - x0) >= (y1 - y0);
    const i32 span = along_x ? (y1 - y0) : (x1 - x0);
    for (i32 k = 0; 2 * k < span; ++k) {
      const i32 z = 48 + k * 2 / 3;
      if (along_x)
        s.box({x0 - 1, y0 + k - 1, z}, {x1 + 1, y1 - k + 1, z + 1}, kWoodV, Paint::Terracotta);
      else
        s.box({x0 + k - 1, y0 - 1, z}, {x1 - k + 1, y1 + 1, z + 1}, kWoodV, Paint::Terracotta);
    }
    if (coarse_view) return;
    doors(L, x0, x1, y0, y1, 2, s);
    // the garden's wall on its fronts, with a gate
    const Paint stone = (mix(hsh) & 1) ? Paint::Sand : Paint::None;
    auto garden = [&](bool along_x2, i32 fixed, i32 a0, i32 a1) {
      const i32 mid = (a0 + a1) / 2;
      for (int part = 0; part < 2; ++part) {
        const i32 b0 = part == 0 ? a0 : mid + 6, b1 = part == 0 ? mid - 6 : a1;
        if (along_x2)
          s.box({b0, fixed, 1}, {b1, fixed + 2, 6}, kStoneV, stone);
        else
          s.box({fixed, b0, 1}, {fixed + 2, b1, 6}, kStoneV, stone);
      }
    };
    if (L.front_ym) garden(true, L.y0 + 1, L.x0 + 1, L.x1 - 1);
    if (L.front_yp) garden(true, L.y1 - 3, L.x0 + 1, L.x1 - 1);
    if (L.front_xm) garden(false, L.x0 + 1, L.y0 + 1, L.y1 - 1);
    if (L.front_xp) garden(false, L.x1 - 3, L.y0 + 1, L.y1 - 1);
  }

  // An office tower: a concrete frame and core behind a glass curtain wall with steel mullions.
  void tower(const Lot& L, u64 hsh, Sink& s, bool coarse_view) const {
    const i32 storeys = 8 + static_cast<i32>(mix(hsh ^ 21) % 9);
    const i32 x0 = L.x0 + 16, x1 = L.x1 - 16, y0 = L.y0 + 16, y1 = L.y1 - 16;
    if (x1 - x0 < 80 || y1 - y0 < 80) return;
    const i32 H = 28;
    const i32 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    for (i32 k = 0; k < storeys; ++k) {
      const i32 z0 = 1 + k * H;
      // curtain wall
      s.box({x0, y0, z0}, {x1, y0 + 1, z0 + 26}, kGlassV);
      s.box({x0, y1 - 1, z0}, {x1, y1, z0 + 26}, kGlassV);
      s.box({x0, y0, z0}, {x0 + 1, y1, z0 + 26}, kGlassV);
      s.box({x1 - 1, y0, z0}, {x1, y1, z0 + 26}, kGlassV);
      if (!coarse_view) {
        for (i32 x = x0; x < x1; x += 12) {
          s.box({x, y0, z0}, {x + 1, y0 + 1, z0 + 26}, kSteelV, Paint::Graphite);
          s.box({x, y1 - 1, z0}, {x + 1, y1, z0 + 26}, kSteelV, Paint::Graphite);
        }
        for (i32 y = y0; y < y1; y += 12) {
          s.box({x0, y, z0}, {x0 + 1, y + 1, z0 + 26}, kSteelV, Paint::Graphite);
          s.box({x1 - 1, y, z0}, {x1, y + 1, z0 + 26}, kSteelV, Paint::Graphite);
        }
      }
      // columns every 6 m inside it, the core (walls), the slab
      for (i32 x = x0 + 4; x + 3 <= x1 - 4; x += 48)
        for (i32 y = y0 + 4; y + 3 <= y1 - 4; y += 48) s.box({x, y, z0}, {x + 3, y + 3, z0 + 26}, kRcV);
      s.box({cx - 14, cy - 14, z0}, {cx + 14, cy - 12, z0 + 26}, kRcV);
      s.box({cx - 14, cy + 12, z0}, {cx + 14, cy + 14, z0 + 26}, kRcV);
      s.box({cx - 14, cy - 14, z0}, {cx - 12, cy + 14, z0 + 26}, kRcV);
      s.box({cx + 12, cy - 14, z0}, {cx + 14, cy + 14, z0 + 26}, kRcV);
      s.box({x0, y0, z0 + 26}, {x1, y1, z0 + H}, kRcV, Paint::Slate);
    }
    // (the plaza around it)
    if (!coarse_view) {
      s.box({L.x0, L.y0, 0}, {L.x1, L.y1, 1}, kWalkV, Paint::Sand);
      s.box({x0, y0, 0}, {x1, y1, 1}, kWalkV);
    }
  }

  // A warehouse: a steel frame clad in sheet metal, big doors on its fronts.
  void warehouse(const Lot& L, u64 hsh, Sink& s, bool coarse_view) const {
    const i32 x0 = L.x0 + 8, x1 = L.x1 - 8, y0 = L.y0 + 8, y1 = L.y1 - 8;
    const i32 top = 56;
    const Paint cladding = std::array<Paint, 4>{Paint::Slate, Paint::Ochre, Paint::Silver, Paint::NavyBlue}[mix(hsh ^ 5) % 4];
    walls(x0, x1, y0, y1, 1, top, 1, kSheetV, cladding, 0, 0, 0, 0, s, coarse_view);
    for (i32 x = x0; x + 2 <= x1; x += 48) {
      s.box({x, y0, 1}, {x + 2, y0 + 2, top + 2}, kSteelV, Paint::Graphite);
      s.box({x, y1 - 2, 1}, {x + 2, y1, top + 2}, kSteelV, Paint::Graphite);
      s.box({x, y0, top}, {x + 2, y1, top + 2}, kSteelV, Paint::Graphite);
    }
    s.box({x1 - 2, y0, 1}, {x1, y0 + 2, top + 2}, kSteelV, Paint::Graphite);
    s.box({x1 - 2, y1 - 2, 1}, {x1, y1, top + 2}, kSteelV, Paint::Graphite);
    s.box({x0, y0, top + 2}, {x1, y1, top + 3}, kSheetV, cladding);
    if (coarse_view) return;
    const i32 mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    if (L.front_ym) s.box({mx - 20, y0, 1}, {mx + 20, y0 + 2, 36}, kAir);
    if (L.front_yp) s.box({mx - 20, y1 - 2, 1}, {mx + 20, y1, 36}, kAir);
    if (L.front_xm) s.box({x0, my - 20, 1}, {x0 + 2, my + 20, 36}, kAir);
    if (L.front_xp) s.box({x1 - 2, my - 20, 1}, {x1, my + 20, 36}, kAir);
  }

  // A park: grass, paths across it, trees, a low wall around it.
  void park(const Lot& L, u64 hsh, Sink& s) const {
    const i32 mx = (L.x0 + L.x1) / 2, my = (L.y0 + L.y1) / 2;
    s.box({L.x0, my - 8, 0}, {L.x1, my + 8, 1}, kWalkV, Paint::Sand);
    s.box({mx - 8, L.y0, 0}, {mx + 8, L.y1, 1}, kWalkV, Paint::Sand);
    for (int k = 0; k < 6; ++k) {
      const u64 t = mix(hsh ^ (static_cast<u64>(k) + 1) * 0x51);
      const i32 x = L.x0 + 24 + static_cast<i32>(t % u64(std::max(1, L.x1 - L.x0 - 48)));
      const i32 y = L.y0 + 24 + static_cast<i32>((t >> 20) % u64(std::max(1, L.y1 - L.y0 - 48)));
      if (std::abs(x - mx) < 16 || std::abs(y - my) < 16) continue;
      s.box({x, y, 1}, {x + 3, y + 3, 30}, kWoodV);
      s.ball({x + 1, y + 1, 36}, 10, kWoodV, Paint::Green, 0.38, t);
    }
    auto wall = [&](i32 a0, i32 a1, i32 b0, i32 b1) { s.box({a0, b0, 1}, {a1, b1, 5}, kBrickV, Paint::None); };
    wall(L.x0, mx - 12, L.y0, L.y0 + 2);
    wall(mx + 12, L.x1, L.y0, L.y0 + 2);
    wall(L.x0, mx - 12, L.y1 - 2, L.y1);
    wall(mx + 12, L.x1, L.y1 - 2, L.y1);
    wall(L.x0, L.x0 + 2, L.y0, my - 12);
    wall(L.x0, L.x0 + 2, my + 12, L.y1);
    wall(L.x1 - 2, L.x1, L.y0, my - 12);
    wall(L.x1 - 2, L.x1, my + 12, L.y1);
  }

  // A car park: asphalt with its bays marked.
  void car_park(const Lot& L, u64 hsh, Sink& s) const {
    s.box({L.x0, L.y0, -1}, {L.x1, L.y1, 1}, kLotAsphaltV);
    const bool along_x = (L.x1 - L.x0) >= (L.y1 - L.y0);
    (void)hsh;
    if (along_x) {
      for (i32 x = L.x0 + 8; x < L.x1 - 8; x += 22) {
        s.box({x, L.y0 + 8, 0}, {x + 1, L.y0 + 48, 1}, kLotAsphaltV, Paint::LineWhite);
        s.box({x, L.y1 - 48, 0}, {x + 1, L.y1 - 8, 1}, kLotAsphaltV, Paint::LineWhite);
      }
    } else {
      for (i32 y = L.y0 + 8; y < L.y1 - 8; y += 22) {
        s.box({L.x0 + 8, y, 0}, {L.x0 + 48, y + 1, 1}, kLotAsphaltV, Paint::LineWhite);
        s.box({L.x1 - 48, y, 0}, {L.x1 - 8, y + 1, 1}, kLotAsphaltV, Paint::LineWhite);
      }
    }
  }

  u64 seed_;
  f64 h_;
};

}  // namespace

std::unique_ptr<GameSource> make_drive_city(u64 seed, f64 h) { return std::make_unique<DriveCity>(seed, h); }

}  // namespace svx
