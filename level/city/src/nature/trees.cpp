// svx_city — voxel_city nature/trees.js.
#include "nature/trees.hpp"

#include <algorithm>
#include <cmath>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "svx/base/types.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

namespace {

constexpr double kPi = 3.141592653589793;

// ---- tables

// TREE_KINDS: size ranges per kind (metres): height and crown radius.
constexpr std::array<TreeKindSpec, kTreeKindCount> kKinds = {{
    {"oak", TreeKind::Oak, {8, 15}, {2.8, 4.8}},
    {"maple", TreeKind::Maple, {10, 17}, {2.4, 3.8}},
    {"autumn", TreeKind::Autumn, {10, 17}, {2.4, 3.8}},
    {"street", TreeKind::Street, {7, 12}, {2.2, 3.4}},
    {"birch", TreeKind::Birch, {8, 16}, {1.6, 2.8}},
    {"rowan", TreeKind::Rowan, {4.5, 8}, {1.6, 2.6}},
    {"blossom", TreeKind::Blossom, {4, 7}, {2.0, 3.2}},
    {"willow", TreeKind::Willow, {7, 12}, {3.0, 4.6}},
    {"poplar", TreeKind::Poplar, {15, 25}, {1.4, 2.1}},
    {"pine", TreeKind::Pine, {12, 22}, {2.2, 3.6}},
    {"spruce", TreeKind::Spruce, {12, 26}, {1.9, 3.2}},
    {"dwarfpine", TreeKind::Dwarfpine, {1.6, 3.5}, {1.2, 2.4}},
    {"juniper", TreeKind::Juniper, {1.2, 3.2}, {0.5, 0.9}},
    {"acacia", TreeKind::Acacia, {5, 8}, {3.0, 4.5}},
    {"jungle", TreeKind::Jungle, {15, 26}, {4.0, 6.0}},
    {"palm", TreeKind::Palm, {7, 12}, {2.5, 3.5}},
    {"cactus", TreeKind::Cactus, {2.0, 4.5}, {0.8, 1.4}},
    {"shrub", TreeKind::Shrub, {0.8, 1.8}, {0.8, 1.6}},
    {"hazel", TreeKind::Hazel, {2.2, 4.5}, {1.6, 2.6}},
    {"aspen", TreeKind::Aspen, {12, 22}, {1.8, 2.8}},
    {"alder", TreeKind::Alder, {8, 16}, {2.2, 3.4}},
    {"larch", TreeKind::Larch, {14, 26}, {2.2, 3.6}},
    {"shrubDry", TreeKind::ShrubDry, {0.5, 1.1}, {0.5, 1.1}},
    {"fern", TreeKind::Fern, {0.4, 0.9}, {0.5, 0.9}},
    {"berry", TreeKind::Berry, {0.2, 0.45}, {0.3, 0.6}},
    {"log", TreeKind::Log, {0.35, 0.6}, {2.5, 6}},
    {"stump", TreeKind::Stump, {0.3, 0.7}, {0.25, 0.45}},
    {"snag", TreeKind::Snag, {5, 12}, {0.2, 0.4}},
}};

constexpr int kKindCount = static_cast<int>(TreeKind::Unknown);
static_assert(kKindCount == kTreeKindCount);

// PALETTE: summer foliage [shadow, body, sunlit] per kind (none: the kinds without foliage of
// their own).
struct PaletteEntry {
  TreeKind kind;
  LeafPalette p;
};
constexpr PaletteEntry kPalette[] = {
    {TreeKind::Oak, {MAT::LEAVES_DARK, MAT::LEAVES, MAT::LEAVES_LIGHT}},
    {TreeKind::Maple, {MAT::LEAVES_DEEP, MAT::LEAVES_DARK, MAT::LEAVES}},
    {TreeKind::Street, {MAT::LEAVES_DARK, MAT::LEAVES, MAT::LEAVES_LIGHT}},
    {TreeKind::Birch, {MAT::LEAVES, MAT::LEAVES_BIRCH, MAT::LEAVES_LIGHT}},
    {TreeKind::Rowan, {MAT::LEAVES_DARK, MAT::LEAVES, MAT::LEAVES_LIGHT}},
    {TreeKind::Blossom, {MAT::LEAVES_DARK, MAT::LEAVES, MAT::LEAVES_LIGHT}},
    {TreeKind::Willow, {MAT::LEAVES, MAT::LEAVES_BIRCH, MAT::LEAVES_WILLOW}},
    {TreeKind::Poplar, {MAT::LEAVES_DEEP, MAT::LEAVES_DARK, MAT::LEAVES}},
    {TreeKind::Pine, {MAT::LEAVES_SPRUCE, MAT::LEAVES_PINE, MAT::LEAVES_PINE_LIGHT}},
    {TreeKind::Spruce, {MAT::LEAVES_DEEP, MAT::LEAVES_SPRUCE, MAT::LEAVES_PINE}},
    {TreeKind::Dwarfpine, {MAT::LEAVES_SPRUCE, MAT::LEAVES_PINE, MAT::LEAVES_PINE_LIGHT}},
    {TreeKind::Juniper, {MAT::LEAVES_DEEP, MAT::LEAVES_SPRUCE, MAT::LEAVES_PINE}},
    {TreeKind::Jungle, {MAT::LEAVES_DEEP, MAT::LEAVES_JUNGLE, MAT::LEAVES}},
    {TreeKind::Shrub, {MAT::LEAVES_DARK, MAT::BUSH, MAT::LEAVES}},
    {TreeKind::ShrubDry, {MAT::SHRUB_DRY, MAT::SHRUB_DRY, MAT::GRASS_STRAW}},
    {TreeKind::Fern, {MAT::LEAVES_DARK, MAT::FERN, MAT::FERN}},
    {TreeKind::Berry, {MAT::BLUEBERRY, MAT::BLUEBERRY, MAT::LEAVES_DARK}},
    {TreeKind::Hazel, {MAT::LEAVES_DARK, MAT::LEAVES, MAT::LEAVES_LIGHT}},
    {TreeKind::Aspen, {MAT::LEAVES, MAT::LEAVES_BIRCH, MAT::LEAVES_LIGHT}},
    {TreeKind::Alder, {MAT::LEAVES_DEEP, MAT::LEAVES_DARK, MAT::LEAVES}},
    {TreeKind::Larch, {MAT::LEAVES, MAT::LEAVES_LIGHT, MAT::LEAVES_SPRING}},
};

const LeafPalette* palette(TreeKind kind) {
  for (const PaletteEntry& e : kPalette)
    if (e.kind == kind) return &e.p;
  return nullptr;
}

// PALETTE[t.kind] ?? PALETTE[t.kind === "autumn" ? "maple" : "oak"]
const LeafPalette* model_palette(TreeKind kind) {
  if (const LeafPalette* p = palette(kind)) return p;
  return palette(kind == TreeKind::Autumn ? TreeKind::Maple : TreeKind::Oak);
}

// Trees with shapes of their own (SHAPES: no clusters).
bool is_shape(TreeKind k) { return k == TreeKind::Acacia || k == TreeKind::Cactus || k == TreeKind::Palm; }

// ---- model building

// Wild trees: how much further broadleaves lean (x their species' lean, at amp 1).
constexpr double kWildLean = 1.5;
// Largest lean of a wild snag and tilt of a windthrown log: yaw-table indices (16.3, 14.3 degrees).
constexpr double kSnagTilt = 5;
constexpr double kThrowTilt = 4;

// The fields a model reads (JS spreads the record into copies with another amp or r).
struct TreeIn {
  double x, y, z, h, r;
  TreeKind kind;
  double seed;
  bool open, wild;
  std::optional<double> amp;
  std::optional<int> yaw;
  std::optional<Yaw> tilt;
  std::optional<bool> plate;
};

TreeIn tree_in(const Tree& t) { return {t.x, t.y, t.z, t.h, t.r, t.kind, t.seed, t.open, t.wild, t.amp, t.yaw, t.tilt, t.plate}; }

// Lean and lopsidedness of a wild tree (0: none, 1: full); 0 for every other tree.
double amp_of(const TreeIn& t) { return t.wild ? t.amp.value_or(1) : 0; }

// A small seeded generator (mulberry32).
class Prng {
 public:
  explicit Prng(int32_t seed) : a_(static_cast<uint32_t>(seed)) {}
  double next() {
    a_ += 0x6d2b79f5u;
    uint32_t t = (a_ ^ (a_ >> 15)) * (1u | a_);
    t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
  }
  double range(double lo, double hi) { return lo + (hi - lo) * next(); }
  double int_(double lo, double hi) { return lo + std::floor(next() * (hi - lo + 1)); }

 private:
  uint32_t a_;
};

const Yaw& yaw_at(double i) {
  const std::vector<Yaw>& y = yaws();
  // (JS reads undefined there and throws on its fields)
  if (!(i >= 0 && i < static_cast<double>(y.size()))) SVX_FAIL("trees: no such yaw");
  return y[static_cast<size_t>(i)];
}

// A lean of up to `max` voxels in an exact direction of the yaw table: x, y (the offset of the
// top), ux, uy (the unit direction).
struct Lean {
  double x, y, ux, uy;
};
Lean lean_of(Prng& R, double max) {
  const Yaw& Y = yaw_at(R.int_(0, static_cast<double>(yaws().size()) - 1));
  const double m = max * R.next();
  return {(Y.c / Y.r) * m, (Y.s / Y.r) * m, Y.c / Y.r, Y.s / Y.r};
}

using Parts = std::vector<TreePart>;

TreePart blob(double x, double y, double z, double rx, double rz, double mix) {
  // (the lumpy surface reaches ~15% past the ellipsoid)
  const double pad = 2 + 0.16 * js::max(rx, rz);
  TreePart p;
  p.k = TreePartKind::Blob;
  p.blob = {x, y, z, rx, rz, mix};
  p.bb = {std::floor(x - rx - pad), std::floor(y - rx - pad), std::floor(z - rz - pad), std::ceil(x + rx + pad), std::ceil(y + rx + pad), std::ceil(z + rz + pad)};
  return p;
}

// limb's extra fields
struct LimbExtra {
  bool trunk = false, twig = false, birch = false, moss = false, stump = false;
  bool has_foot = false;
  double foot = 0;
};

TreePart limb(double ax, double ay, double az, double bx, double by, double bz, double r0, double r1, uint16_t mat, const LimbExtra& extra = {}) {
  const double r = js::max(r0, r1) + 1;
  const double dx = bx - ax;
  const double dy = by - ay;
  const double dz = bz - az;
  const double L2 = js::or_(dx * dx + dy * dy + dz * dz, 1);
  TreePart p;
  p.k = TreePartKind::Limb;
  p.limb = {ax, ay, az, dx, dy, dz, L2, r0, r1, extra.foot};
  p.mat = mat;
  p.trunk = extra.trunk;
  p.twig = extra.twig;
  p.birch = extra.birch;
  p.moss = extra.moss;
  p.stump = extra.stump;
  p.has_foot = extra.has_foot;
  p.bb = {std::floor(js::min(ax, bx) - r), std::floor(js::min(ay, by) - r), std::floor(js::min(az, bz) - r),
          std::ceil(js::max(ax, bx) + r),  std::ceil(js::max(ay, by) + r),  std::ceil(js::max(az, bz) + r)};
  return p;
}

constexpr LimbExtra kTrunk = {true, false, false, false, false, false, 0};
constexpr LimbExtra kTwig = {false, true, false, false, false, false, 0};

// A cluster of a crown: centre, radius, the limb's azimuth, its palette mix.
struct Cluster {
  double x, y, z, rb, az, mix;
};

// Satellite clusters round a cluster c: smaller lumps pushed out and up from its surface, so crowns
// end in an irregular, lumpy outline instead of a ball. (JS: c.mix ?? R.next() - every caller's
// cluster has its mix.)
void satellites(Parts& parts, Prng& R, const Cluster& c, double flat, double n, double size0 = 0.42, double size1 = 0.62, double up = 0.35) {
  for (double q = 0; q < n; q += 1) {
    const double a = R.range(0, kPi * 2);
    const double e0 = R.range(-0.25, 1);
    const double e1 = R.range(-0.1, 0.35);
    const double e = e0 * up + (1 - up) * e1;
    const double d = c.rb * R.range(0.55, 0.85);
    const double rs = c.rb * R.range(size0, size1);
    parts.push_back(blob(c.x + js::cos(a) * d * js::cos(e), c.y + js::sin(a) * d * js::cos(e), c.z + js::sin(e) * d * flat, rs, rs * flat, c.mix));
  }
}

// Trunk radius (voxels) of a tree h voxels tall.
double trunk_r(const TreeIn& t, double k = 60) { return js::max(0.7, t.h / k); }

// A deciduous crown on a forking trunk: fork height (fraction), limb count, limb length and
// cluster size (fractions of r), cluster flatness (rz / rx), top cluster size, filler clusters,
// lean, twin (the chance of a wild tree's twin leaders). (JS's elev is unread.)
struct LeafOpts {
  std::array<double, 2> fork, limbs, spread, blob;
  double flat, top;
  double fill = 0;  // (o.fill ?? 0)
  double lean;
  double twin = 0;              // (o.twin ?? 0)
  std::optional<double> trunk;  // (o.trunk ?? trunkR(t))
  uint16_t bark = MAT::BARK;    // (o.bark ?? MAT.BARK)
  bool flare = true;            // (o.flare !== false)
  double sat = 3;               // (o.sat ?? 3)
};

struct Base {
  double x, y, z, h, r;
};

void broadleaf(const TreeIn& t, Prng& R, Parts& parts, const LeafOpts& o, const Base& base) {
  const bool wild = t.wild;
  const double amp = amp_of(t);
  const double h = base.h;
  // (the clusters and their satellites reach ~1.5 of this: t.r stays the crown's visual radius)
  const double r = base.r * 0.72;
  const double la = R.range(0, kPi * 2);
  const double cl = js::cos(la);
  const double sl = js::sin(la);
  const double lean = o.lean * (1 + kWildLean * amp) * h * R.next();
  // (wild: the fork anywhere from a little lower to a little higher)
  const double fork0 = wild ? o.fork[0] - 0.06 : o.fork[0];
  const double fork1 = wild ? o.fork[1] + 0.08 : o.fork[1];
  const double hf = h * R.range(fork0, fork1);
  const double tr = o.trunk ? *o.trunk : trunk_r(t);
  const XYZ F = {base.x + cl * lean * (hf / h), base.y + sl * lean * (hf / h), base.z + hf};
  const uint16_t bark = o.bark;
  if (wild) {
    // a trunk that sweeps: upright at the foot, leaning above
    const XYZ M = {base.x + (F.x - base.x) * 0.3, base.y + (F.y - base.y) * 0.3, base.z + hf * 0.5};
    parts.push_back(limb(base.x, base.y, base.z - 1, M.x, M.y, M.z, tr * 1.15, tr * 0.97, bark, kTrunk));
    parts.push_back(limb(M.x, M.y, M.z, F.x, F.y, F.z, tr * 0.97, tr * 0.8, bark, kTrunk));
  } else {
    parts.push_back(limb(base.x, base.y, base.z - 1, F.x, F.y, F.z, tr * 1.15, tr * 0.8, bark, kTrunk));
  }
  if (o.flare) parts.push_back(limb(base.x, base.y, base.z - 1, base.x, base.y, base.z + 2, tr * 1.7, tr * 1.1, bark, kTrunk));
  const double n = R.int_(o.limbs[0], o.limbs[1] + (wild ? 1 : 0));
  const double a0 = R.range(0, kPi * 2);
  // wild: a lopsided crown (longer limbs, bigger clusters on the lit side it leans to), flatter or rounder
  const double A = wild ? amp * R.range(0.12, 0.36) : 0;
  const double flat = wild ? o.flat * R.range(0.88, 1.12) : o.flat;
  std::vector<Cluster> crown;
  // the top cluster sits under the tree's height; the limb clusters fill the crown below it
  const double rt = r * o.top;
  const XYZ T = {F.x + cl * lean * 0.4, F.y + sl * lean * 0.4, base.z + h - rt * flat * 0.9};
  for (double i = 0; i < n; i += 1) {
    const double az = a0 + (i / n) * kPi * 2 + R.range(-0.45, 0.45);
    const double ca = js::cos(az);
    const double sa = js::sin(az);
    const double e = R.next();
    const double lit = A * (ca * cl + sa * sl);
    const double L = r * R.range(o.spread[0], o.spread[1]) * (1.1 - 0.25 * e) * (1 + lit);
    const XYZ E = {F.x + ca * L, F.y + sa * L, F.z + (T.z - F.z) * (0.28 + 0.42 * e) * R.range(0.9, 1.1)};
    const double rb = r * R.range(o.blob[0], o.blob[1]) * (1 + lit * 0.5);
    parts.push_back(limb(F.x, F.y, F.z, E.x, E.y, E.z, tr * 0.72, js::max(0.5, tr * 0.32), bark));
    crown.push_back({E.x, E.y, E.z, rb, az, 0});
  }
  if (wild && R.next() < o.twin) {
    // twin leaders: the crown forks across the lean, one top a little lower
    const double d = r * R.range(0.22, 0.4);
    const XYZ Q0 = {T.x - sl * d, T.y + cl * d, T.z};
    const double x1 = T.x + sl * d * 0.8;
    const double y1 = T.y - cl * d * 0.8;
    const XYZ Q1 = {x1, y1, T.z - h * R.range(0.04, 0.12)};
    for (const XYZ& Q : {Q0, Q1}) {
      parts.push_back(limb(F.x, F.y, F.z, Q.x, Q.y, Q.z, tr * 0.6, js::max(0.5, tr * 0.26), bark));
      crown.push_back({Q.x, Q.y, Q.z, rt * 0.8, la, 0});
    }
  } else {
    // the leader and the top cluster
    parts.push_back(limb(F.x, F.y, F.z, T.x, T.y, T.z, tr * 0.7, js::max(0.5, tr * 0.3), bark));
    crown.push_back({T.x, T.y, T.z, rt, la, 0});
  }
  // filler clusters between neighbouring limbs, higher up and further in (no hollows)
  for (double k = 0; k < o.fill && n > 1; k += 1) {
    const Cluster a = crown[static_cast<size_t>(std::fmod(k, n))];
    const Cluster b = crown[static_cast<size_t>(std::fmod(k + 1, n))];
    const double mx = (a.x + b.x) / 2;
    const double my = (a.y + b.y) / 2;
    const double mz = (a.z + b.z) / 2;
    const double f = R.range(0.35, 0.65);
    crown.push_back({F.x + (mx - F.x) * 0.65, F.y + (my - F.y) * 0.65, mz + (T.z - mz) * f, (a.rb + b.rb) * 0.4, (a.az + b.az) / 2, 0});
  }
  for (Cluster& c : crown) {
    c.mix = R.next();
    parts.push_back(blob(c.x, c.y, c.z, c.rb, c.rb * flat, c.mix));
    satellites(parts, R, c, flat, o.sat);
    // twigs from the cluster centre outwards (the bare crown in winter)
    for (int q = 0; q < 3; ++q) {
      const double ta = c.az + R.range(-1.4, 1.4);
      const double tl = c.rb * R.range(0.6, 0.95);
      const double tz = c.z + R.range(-0.2, 0.6) * c.rb * flat;
      parts.push_back(limb(c.x, c.y, c.z, c.x + js::cos(ta) * tl, c.y + js::sin(ta) * tl, tz, 0.55, 0.4, bark, kTwig));
    }
  }
}

void broadleaf(const TreeIn& t, Prng& R, Parts& parts, const LeafOpts& o) { broadleaf(t, R, parts, o, {t.x, t.y, t.z, t.h, t.r}); }

LeafOpts leaf(std::array<double, 2> fork, std::array<double, 2> limbs, std::array<double, 2> spread, std::array<double, 2> blob_, double flat, double top,
              double fill, double lean) {
  LeafOpts o;
  o.fork = fork;
  o.limbs = limbs;
  o.spread = spread;
  o.blob = blob_;
  o.flat = flat;
  o.top = top;
  o.fill = fill;
  o.lean = lean;
  return o;
}

void build_oak(const TreeIn& t, Prng& R, Parts& parts) {
  LeafOpts o = leaf({0.26, 0.38}, {3, 5}, {0.45, 0.68}, {0.42, 0.56}, 0.75, 0.5, 2, 0.06);
  o.twin = 0.3;
  o.trunk = trunk_r(t, 45);
  broadleaf(t, R, parts, o);
}

void build_maple(const TreeIn& t, Prng& R, Parts& parts) {
  LeafOpts o = leaf({0.32, 0.45}, {4, 5}, {0.35, 0.55}, {0.4, 0.52}, 0.95, 0.55, 2, 0.03);
  o.twin = 0.22;
  o.bark = R.next() < 0.5 ? MAT::BARK_GREY : MAT::BARK;
  broadleaf(t, R, parts, o);
}

void build_street(const TreeIn& t, Prng& R, Parts& parts) {
  broadleaf(t, R, parts, leaf({0.4, 0.5}, {4, 5}, {0.35, 0.5}, {0.45, 0.55}, 0.9, 0.55, 1, 0.01));
}

void build_blossom(const TreeIn& t, Prng& R, Parts& parts) {
  LeafOpts o = leaf({0.22, 0.32}, {4, 6}, {0.55, 0.85}, {0.36, 0.48}, 0.62, 0.4, 2, 0.07);
  o.twin = 0.3;
  o.bark = MAT::BARK;
  broadleaf(t, R, parts, o);
}

void build_jungle(const TreeIn& t, Prng& R, Parts& parts) {
  LeafOpts o = leaf({0.6, 0.72}, {4, 6}, {0.6, 0.9}, {0.35, 0.5}, 0.55, 0.45, 3, 0.04);
  o.twin = 0.2;
  o.trunk = trunk_r(t, 40);
  broadleaf(t, R, parts, o);
}

void build_willow(const TreeIn& t, Prng& R, Parts& parts) {
  LeafOpts o = leaf({0.28, 0.38}, {4, 6}, {0.5, 0.75}, {0.4, 0.52}, 0.7, 0.45, 2, 0.12);
  o.twin = 0.25;
  o.trunk = trunk_r(t, 38);
  broadleaf(t, R, parts, o);
  // hanging curtains of leaves round every cluster
  const size_t n = parts.size();
  for (size_t i = 0; i < n; ++i) {
    if (parts[i].k != TreePartKind::Blob || !(parts[i].blob.rx > t.r * 0.3)) continue;
    const TreeBlob p = parts[i].blob;
    TreePart c;
    c.k = TreePartKind::Curtain;
    c.curtain = {p.x, p.y, p.z, p.rx, p.rz, js::max(4, p.z - p.rz - t.z - t.h * 0.08), p.mix};
    c.bb = {std::floor(p.x - p.rx - 2), std::floor(p.y - p.rx - 2), std::floor(t.z + t.h * 0.06), std::ceil(p.x + p.rx + 2), std::ceil(p.y + p.rx + 2), std::ceil(p.z)};
    parts.push_back(c);
  }
}

void build_rowan(const TreeIn& t, Prng& R, Parts& parts) {
  // two or three thin stems from the foot, each with a small open crown
  const double n = R.int_(1, 3);
  for (double s = 0; s < n; s += 1) {
    const double a = R.range(0, kPi * 2);
    const double off = n > 1 ? R.range(0.5, 1.2) : 0;
    const double hh = t.h * R.range(0.8, 1);
    LeafOpts o = leaf({0.45, 0.6}, {2, 3}, {0.3, 0.5}, {0.38, 0.52}, 0.9, 0.45, 0, n > 1 ? 0.14 : 0.05);
    o.sat = 2;
    o.trunk = trunk_r(t, 70);
    o.flare = false;
    broadleaf(t, R, parts, o, {t.x + js::cos(a) * off, t.y + js::sin(a) * off, t.z, hh, t.r * (n > 1 ? 0.8 : 1)});
  }
}

void build_poplar(const TreeIn& t, Prng& R, Parts& parts) {
  const double tr = trunk_r(t, 70);
  // (wild: the column leans a little)
  std::optional<Lean> L;
  if (t.wild) L = lean_of(R, amp_of(t) * 0.03 * t.h);
  parts.push_back(limb(t.x, t.y, t.z - 1, L ? t.x + L->x * 0.9 : t.x, L ? t.y + L->y * 0.9 : t.y, t.z + t.h * 0.9, tr * 1.1, tr * 0.5, MAT::BARK_GREY, kTrunk));
  const double n = R.int_(4, 6);
  for (double k = 0; k < n; k += 1) {
    const double f = 0.2 + (0.8 * (k + 0.5)) / n;
    const double rb = t.r * (1 - js::abs(f - 0.45) * 0.9) * R.range(0.85, 1.05);
    const double x = t.x + R.range(-0.6, 0.6);
    const double y = t.y + R.range(-0.6, 0.6);
    const double mix = R.next();
    parts.push_back(blob(L ? x + L->x * f : x, L ? y + L->y * f : y, t.z + t.h * f, js::max(1.2, rb), t.h * 0.13, mix));
  }
}

void build_birch(const TreeIn& t, Prng& R, Parts& parts) {
  // one slender stem, or two or three from the foot leaning apart
  const double amp = amp_of(t);
  const double n = R.next() < 0.3 ? R.int_(2, 3) : 1;
  // (wild: stems lean further and bow, upright at the foot)
  const bool bow = t.wild;
  auto bowed = [](double f) { return f <= 0.5 ? 0.6 * f : 0.3 + 1.4 * (f - 0.5); };
  for (double s = 0; s < n; s += 1) {
    const double a = R.range(0, kPi * 2);
    const double lean = (n > 1 ? R.range(0.06, 0.13 + 0.06 * amp) : R.range(0, 0.05 + 0.05 * amp)) * t.h;
    const double hh = t.h * (s == 0 ? 1 : R.range(0.75, 0.95));
    const XYZ top = {t.x + js::cos(a) * lean, t.y + js::sin(a) * lean, t.z + hh};
    const double tr = trunk_r(t, 75);
    LimbExtra stem;
    stem.trunk = true;
    stem.birch = true;
    if (bow) {
      const XYZ M = {t.x + (top.x - t.x) * 0.3, t.y + (top.y - t.y) * 0.3, t.z + hh * 0.5};
      parts.push_back(limb(t.x, t.y, t.z - 1, M.x, M.y, M.z, tr, tr * 0.72, MAT::BARK_BIRCH, stem));
      LimbExtra upper = stem;
      upper.has_foot = true;
      upper.foot = t.z - 1;
      parts.push_back(limb(M.x, M.y, M.z, top.x, top.y, top.z - t.r * 0.4, tr * 0.72, tr * 0.45, MAT::BARK_BIRCH, upper));
    } else {
      parts.push_back(limb(t.x, t.y, t.z - 1, top.x, top.y, top.z - t.r * 0.4, tr, tr * 0.45, MAT::BARK_BIRCH, stem));
    }
    const double m = R.int_(5, 8);
    for (double k = 0; k < m; k += 1) {
      const double f = R.range(0.42, 0.95);
      const double g = bow ? bowed(f) : f;
      const double cx = t.x + (top.x - t.x) * g;
      const double cy = t.y + (top.y - t.y) * g;
      const double cz = t.z + hh * f;
      const double ba = R.range(0, kPi * 2);
      const double off = t.r * R.range(0.2, 0.55) * (1.15 - f * 0.5);
      const double rb = t.r * R.range(0.3, 0.44);
      const double bx = cx + js::cos(ba) * off;
      const double by = cy + js::sin(ba) * off;
      const double bz = cz - rb * 0.25;
      parts.push_back(limb(cx, cy, cz - rb * 0.3, bx, by, bz, 0.6, 0.45, MAT::BARK_BIRCH));
      const double mix = R.next();
      parts.push_back(blob(bx, by, bz, rb, rb * 1.2, mix));
      satellites(parts, R, {bx, by, bz, rb, 0, mix}, 1.2, 2, 0.4, 0.6, 0.1);
    }
    const double mix = R.next();
    parts.push_back(blob(top.x, top.y, top.z - t.r * 0.35, t.r * 0.32, t.r * 0.45, mix));
  }
}

void build_pine(const TreeIn& t0, Prng& R, Parts& parts) {
  // a long bare trunk (grey below, orange above) with a gentle S, flat clumps of needles on top
  const double amp = amp_of(t0);
  TreeIn t = t0;
  t.r = t0.r * 0.78;
  const double tr = trunk_r(t, 55);
  const double a = R.range(0, kPi * 2);
  const double bend = R.range(0.01, 0.04 + 0.03 * amp) * t.h;
  // wild: the whole pine leans (the rock and the edge of the wood), its clumps crowding to the lit side
  std::optional<Lean> L;
  if (t.wild) L = lean_of(R, amp * 0.05 * t.h);
  const double A = L ? amp * R.range(0.2, 0.5) : 0;
  XYZ M = {t.x + js::cos(a) * bend, t.y + js::sin(a) * bend, t.z + t.h * 0.5};
  XYZ T = {t.x - js::cos(a) * bend * 0.3, t.y - js::sin(a) * bend * 0.3, t.z + t.h * 0.97};
  if (L) {
    M.x += L->x * 0.35;
    M.y += L->y * 0.35;
    T.x += L->x;
    T.y += L->y;
  }
  parts.push_back(limb(t.x, t.y, t.z - 1, M.x, M.y, M.z, tr * 1.1, tr * 0.8, MAT::BARK, kTrunk));
  parts.push_back(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + 2, tr * 1.5, tr, MAT::BARK, kTrunk));
  parts.push_back(limb(M.x, M.y, M.z, T.x, T.y, T.z, tr * 0.8, tr * 0.35, MAT::BARK_PINE, kTrunk));
  // (a young or crowded pine keeps its needles lower down, an old one only near the top)
  const double c0 = t.open ? R.range(0.48, 0.58) : R.range(0.6, 0.7);
  const double n = R.int_(6, 9);
  for (double k = 0; k < n; k += 1) {
    // more clumps higher up; the lower ones reach further out on stout limbs
    const double q = js::sqrt((k + R.range(0.1, 0.9)) / n);
    const double f = c0 + (0.95 - c0) * q;
    const double cx = t.x + (T.x - t.x) * f;
    const double cy = t.y + (T.y - t.y) * f;
    const double cz = t.z + t.h * f;
    const double ba = R.range(0, kPi * 2);
    const double cb = js::cos(ba);
    const double sb = js::sin(ba);
    const double off = t.r * R.range(0.2, 0.5) * (1.3 - q * 0.7) * (L ? 1 + A * (cb * L->ux + sb * L->uy) : 1);
    const double rb = t.r * R.range(0.38, 0.55) * (1.1 - q * 0.3);
    const double bx = cx + cb * off;
    const double by = cy + sb * off;
    const double bz = cz + rb * 0.25;
    parts.push_back(limb(cx, cy, cz - 1, bx, by, bz, tr * 0.5, 0.6, MAT::BARK_PINE));
    const double mix = R.next();
    parts.push_back(blob(bx, by, bz, rb, rb * 0.55, mix));
    // (a clump of needles is a ragged tuft, not a plate)
    satellites(parts, R, {bx, by, bz, rb, 0, mix}, 0.6, 3, 0.4, 0.6, 0.3);
  }
  // the crown's rounded top
  const double mix = R.next();
  parts.push_back(blob(T.x, T.y, T.z - t.r * 0.1, t.r * 0.5, t.r * 0.32, mix));
}

// A wild cone (spruce, juniper): its axis leans with the trunk (sheared from the foot up to the
// top offset L) and it is lopsided, fuller by up to A on the lit side it leans to.
void wild_cone(TreePart& cone, const TreeIn& t, const Lean& L, double A) {
  const double r = t.r * (1 + A) + 3;
  cone.bb.x0 = std::floor(js::min(t.x, t.x + L.x) - r);
  cone.bb.y0 = std::floor(js::min(t.y, t.y + L.y) - r);
  cone.bb.x1 = std::ceil(js::max(t.x, t.x + L.x) + r);
  cone.bb.y1 = std::ceil(js::max(t.y, t.y + L.y) + r);
  cone.lean = true;
  TreeCone& c = cone.cone;
  c.lx = L.x;
  c.ly = L.y;
  c.zf = t.z;
  c.hh = t.h;
  c.ax = L.ux;
  c.ay = L.uy;
  c.asym = A;
}

void build_spruce(const TreeIn& t, Prng& R, Parts& parts) {
  const double tr = trunk_r(t, 70);
  // (wild: a slight lean and a lopsided cone)
  std::optional<Lean> L;
  if (t.wild) L = lean_of(R, amp_of(t) * 0.035 * t.h);
  const double A = L ? amp_of(t) * R.range(0.06, 0.22) : 0;
  parts.push_back(limb(t.x, t.y, t.z - 1, L ? t.x + L->x : t.x, L ? t.y + L->y : t.y, t.z + t.h, tr * 1.1, 0.5, MAT::BARK, kTrunk));
  // crowded spruces lose their lowest whorls
  const double z0 = t.z + t.h * (t.open ? R.range(0.02, 0.06) : R.range(0.1, 0.22));
  const double tier = R.int_(4, 6);
  const double lobes = R.int_(5, 7);
  const double phase = R.range(0, 6.3);
  TreePart cone;
  cone.k = TreePartKind::Cone;
  cone.cone = {t.x, t.y, z0, t.z + t.h, z0, t.r, tier, lobes, phase, 0, 0, 0, 0, 0, 0, 0};
  cone.bb = {std::floor(t.x - t.r - 3), std::floor(t.y - t.r - 3), std::floor(z0 - 1), std::ceil(t.x + t.r + 3), std::ceil(t.y + t.r + 3), std::ceil(t.z + t.h + 2)};
  if (L) wild_cone(cone, t, *L, A);
  parts.push_back(cone);
}

void build_juniper(const TreeIn& t, Prng& R, Parts& parts) {
  std::optional<Lean> L;
  if (t.wild) L = lean_of(R, amp_of(t) * 0.05 * t.h);
  const double A = L ? amp_of(t) * R.range(0.08, 0.25) : 0;
  const double phase = R.range(0, 6.3);
  TreePart cone;
  cone.k = TreePartKind::Cone;
  cone.cone = {t.x, t.y, t.z, t.z + t.h, t.z, t.r, 3, 4, phase, 0, 0, 0, 0, 0, 0, 0};
  cone.bb = {std::floor(t.x - t.r - 3), std::floor(t.y - t.r - 3), t.z - 1, std::ceil(t.x + t.r + 3), std::ceil(t.y + t.r + 3), std::ceil(t.z + t.h + 2)};
  if (L) wild_cone(cone, t, *L, A);
  parts.push_back(cone);
}

void build_dwarfpine(const TreeIn& t, Prng& R, Parts& parts) {
  // krummholz: low sprawling mounds on crooked stems
  const double n = R.int_(2, 4);
  for (double k = 0; k < n; k += 1) {
    const double a = R.range(0, kPi * 2);
    const double off = t.r * R.range(0.1, 0.6);
    const double rb = t.r * R.range(0.45, 0.75);
    const double x = t.x + js::cos(a) * off;
    const double y = t.y + js::sin(a) * off;
    parts.push_back(limb(t.x, t.y, t.z - 1, x, y, t.z + t.h * 0.4, 0.8, 0.5, MAT::BARK));
    const double rz = js::max(1.2, t.h * R.range(0.35, 0.55));
    const double mix = R.next();
    parts.push_back(blob(x, y, t.z + t.h * 0.45, rb, rz, mix));
  }
}

void build_shrub(const TreeIn& t0, Prng& R, Parts& parts) {
  TreeIn t = t0;
  t.r = t0.r * 0.75;
  const double n = R.int_(2, 4);
  for (double k = 0; k < n; k += 1) {
    const double a = R.range(0, kPi * 2);
    const double off = t.r * R.range(0, 0.45);
    const double rb = t.r * R.range(0.5, 0.8);
    const double rz = js::max(0.8, t.h * R.range(0.45, 0.6));
    const double mix = R.next();
    parts.push_back(blob(t.x + js::cos(a) * off, t.y + js::sin(a) * off, t.z + rz * 0.55, rb, rz, mix));
    satellites(parts, R, {t.x + js::cos(a) * off, t.y + js::sin(a) * off, t.z + rz * 0.55, rb, 0, mix}, rz / rb, 1, 0.45, 0.65, 0.2);
  }
}

void build_hazel(const TreeIn& t, Prng& R, Parts& parts) {
  // a clump of thin stems fanning out from the foot, a leafy dome on top
  const double n = R.int_(4, 7);
  for (double k = 0; k < n; k += 1) {
    const double a = (k / n) * kPi * 2 + R.range(-0.4, 0.4);
    const double out = t.r * R.range(0.35, 0.75);
    const XYZ top = {t.x + js::cos(a) * out, t.y + js::sin(a) * out, t.z + t.h * R.range(0.65, 0.95)};
    parts.push_back(limb(t.x + js::cos(a) * 0.6, t.y + js::sin(a) * 0.6, t.z - 1, top.x, top.y, top.z, 0.7, 0.5, MAT::BARK_GREY));
    const double rb = t.r * R.range(0.4, 0.55);
    const double mix = R.next();
    parts.push_back(blob(top.x, top.y, top.z - rb * 0.2, rb, rb * 0.8, mix));
    satellites(parts, R, {top.x, top.y, top.z - rb * 0.2, rb, 0, mix}, 0.8, 1, 0.45, 0.6, 0.2);
  }
}

void build_aspen(const TreeIn& t, Prng& R, Parts& parts) {
  // a straight pale grey-green stem, a small rounded crown high up
  const double tr = trunk_r(t, 70);
  const double lean = R.range(0, 0.03 + 0.03 * amp_of(t)) * t.h;
  const double a = R.range(0, kPi * 2);
  const XYZ top = {t.x + js::cos(a) * lean, t.y + js::sin(a) * lean, t.z + t.h};
  parts.push_back(limb(t.x, t.y, t.z - 1, top.x, top.y, top.z - t.r * 0.3, tr, tr * 0.4, MAT::BARK_GREY, kTrunk));
  const double m = R.int_(8, 12);
  for (double k = 0; k < m; k += 1) {
    const double f = R.range(0.4, 0.96);
    const double cx = t.x + (top.x - t.x) * f;
    const double cy = t.y + (top.y - t.y) * f;
    const double cz = t.z + t.h * f;
    const double ba = R.range(0, kPi * 2);
    const double off = t.r * R.range(0.2, 0.6) * (1.25 - f * 0.6);
    const double rb = t.r * R.range(0.36, 0.5);
    const double bx = cx + js::cos(ba) * off;
    const double by = cy + js::sin(ba) * off;
    parts.push_back(limb(cx, cy, cz - rb * 0.4, bx, by, cz, 0.6, 0.45, MAT::BARK_GREY));
    const double mix = R.next();
    parts.push_back(blob(bx, by, cz, rb, rb * 0.95, mix));
    satellites(parts, R, {bx, by, cz, rb, 0, mix}, 0.95, 2, 0.4, 0.55, 0.25);
  }
}

void build_alder(const TreeIn& t, Prng& R, Parts& parts) {
  // a dark egg-shaped crown on one to three stems (by the water)
  const double n = R.next() < 0.35 ? 2 : 1;
  for (double k = 0; k < n; k += 1) {
    const double a = R.range(0, kPi * 2);
    const double off = n > 1 ? R.range(0.6, 1.2) : 0;
    LeafOpts o = leaf({0.35, 0.5}, {3, 4}, {0.3, 0.45}, {0.42, 0.55}, 1.05, 0.6, 1, n > 1 ? 0.1 : 0.04);
    o.twin = n > 1 ? 0 : 0.12;
    o.bark = MAT::BARK_GREY;
    o.trunk = trunk_r(t, 60);
    const double hh = t.h * (k != 0 ? R.range(0.75, 0.9) : 1);
    broadleaf(t, R, parts, o, {t.x + js::cos(a) * off, t.y + js::sin(a) * off, t.z, hh, t.r * (n > 1 ? 0.8 : 1)});
  }
}

void build_larch(const TreeIn& t, Prng& R, Parts& parts) {
  // an airy cone of soft needles in tiers of tufts on slightly drooping limbs (gold in autumn, bare in winter)
  const double tr = trunk_r(t, 60);
  // (wild: it leans, its tiers reaching further on the lit side)
  std::optional<Lean> W;
  if (t.wild) W = lean_of(R, amp_of(t) * 0.04 * t.h);
  const double A = W ? amp_of(t) * R.range(0.15, 0.4) : 0;
  parts.push_back(limb(t.x, t.y, t.z - 1, W ? t.x + W->x : t.x, W ? t.y + W->y : t.y, t.z + t.h, tr * 1.1, 0.5, MAT::BARK, kTrunk));
  parts.push_back(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + 2, tr * 1.5, tr, MAT::BARK, kTrunk));
  const double z0 = t.h * (t.open ? R.range(0.08, 0.15) : R.range(0.25, 0.35));
  const double tiers = R.int_(7, 10);
  for (double k = 0; k < tiers; k += 1) {
    const double f = (z0 + ((t.h * 0.95 - z0) * (k + R.range(0, 0.6))) / tiers) / t.h;
    const double rk = t.r * js::pow(1 - f, 0.85) * R.range(0.85, 1.1) + 1;
    const double n = R.int_(3, 5);
    const double a0 = R.range(0, kPi * 2);
    for (double q = 0; q < n; q += 1) {
      const double a = a0 + (q / n) * kPi * 2 + R.range(-0.3, 0.3);
      const double ca = js::cos(a);
      const double sa = js::sin(a);
      const double cz = t.z + t.h * f;
      // (the trunk at this height)
      const double ax = W ? t.x + W->x * f : t.x;
      const double ay = W ? t.y + W->y * f : t.y;
      const double L = rk * R.range(0.6, 0.95) * (W ? 1 + A * (ca * W->ux + sa * W->uy) : 1);
      const XYZ E = {ax + ca * L, ay + sa * L, cz - L * 0.15};
      parts.push_back(limb(ax, ay, cz, E.x, E.y, E.z, 0.7, 0.45, MAT::BARK));
      const double rb = js::max(1.2, rk * R.range(0.35, 0.5));
      const double mix = R.next();
      parts.push_back(blob(E.x - ca * rb * 0.4, E.y - sa * rb * 0.4, E.z, rb, rb * 0.55, mix));
    }
  }
  const double mix = R.next();
  parts.push_back(blob(W ? t.x + W->x : t.x, W ? t.y + W->y : t.y, t.z + t.h - 1.5, 1.4, 2.2, mix));
}

void build_berry(const TreeIn& t, Prng& R, Parts& parts) {
  const double mix = R.next();
  parts.push_back(blob(t.x, t.y, t.z, js::max(1, t.r), js::max(0.8, t.h), mix));
}

void build_fern(const TreeIn& t, Prng& R, Parts& parts) {
  const double n = R.int_(5, 8);
  const double phase = R.range(0, 6.3);
  TreePart p;
  p.k = TreePartKind::Fern;
  p.fern = {t.x, t.y, t.z, js::max(3, t.r), js::max(2, t.h), n, phase};
  p.bb = {std::floor(t.x - t.r - 2), std::floor(t.y - t.r - 2), t.z, std::ceil(t.x + t.r + 2), std::ceil(t.y + t.r + 2), t.z + std::ceil(t.h) + 2};
  parts.push_back(p);
}

// A wild fallen log (t.r its length, t.h its thickness), turned to an exact yaw (t.yaw, or one of
// its own): lying along the ground (t.tilt, s > 0 where its +u end lies higher), or windthrown
// (t.plate): lifted at its root end on the root plate it tore out of the ground, at an exact tilt
// of the yaw table.
void wild_log(const TreeIn& t, Prng& R, Parts& parts) {
  const Yaw& Y = yaw_at(t.yaw ? static_cast<double>(*t.yaw) : R.int_(0, 2 * kYawQuarter - 1));
  const double ux = Y.c / Y.r;
  const double uy = Y.s / Y.r;
  const double rr = js::max(1.5, t.h);
  const bool plate = t.plate ? *t.plate : R.next() < 0.35;
  Yaw T;
  if (plate)
    T = yaw_at(js::max(1, js::round(amp_of(t) * R.int_(1, kThrowTilt))));
  else
    T = t.tilt ? *t.tilt : yaws()[0];
  const double c = T.c / T.r;
  const double s = T.s / T.r;
  const double hl = (t.r / 2) * c;
  // (from the broken crown end to the root end; a windthrow rests its crown end on the ground)
  const double z0 = plate ? t.z + rr * 0.8 - 1 : t.z + rr - 1.25 - (t.r / 2) * s;
  const XYZ A = {t.x - ux * hl, t.y - uy * hl, z0};
  const XYZ B = {t.x + ux * hl, t.y + uy * hl, z0 + t.r * s};
  LimbExtra moss;
  moss.moss = true;
  parts.push_back(limb(A.x, A.y, A.z, B.x, B.y, B.z, rr * 0.8, rr, MAT::DEADWOOD, moss));
  if (!plate) return;
  // the root plate: a ragged disc of soil and roots square to the trunk, down into the ground
  const XYZ n = {ux * c, uy * c, s};
  const double th = js::max(1.5, rr * 0.45);
  const XYZ C = {B.x + n.x * th, B.y + n.y * th, B.z + n.z * th};
  const double rp = js::max(rr * 1.8, (C.z - t.z + 2) / c);
  // (its rim reaches out to 1.12 rp in lobes)
  auto e = [&](double k) { return rp * 1.12 * js::sqrt(js::max(0, 1 - k * k)) + th * js::abs(k) + 2; };
  TreePart p;
  p.k = TreePartKind::Plate;
  p.plate = {C.x, C.y, C.z, n.x, n.y, n.z, rp, th};
  p.bb = {std::floor(C.x - e(n.x)), std::floor(C.y - e(n.y)), std::floor(C.z - e(n.z)), std::ceil(C.x + e(n.x)), std::ceil(C.y + e(n.y)), std::ceil(C.z + e(n.z))};
  parts.push_back(p);
}

void build_log(const TreeIn& t, Prng& R, Parts& parts) {
  // a fallen trunk with moss on its back; t.r is its length, t.h its thickness
  if (t.wild) {
    wild_log(t, R, parts);
    return;
  }
  const double a = R.range(0, kPi);
  const double L = t.r;
  const double rr = js::max(1.5, t.h);
  const double x0 = t.x - (js::cos(a) * L) / 2;
  const double y0 = t.y - (js::sin(a) * L) / 2;
  LimbExtra moss;
  moss.moss = true;
  parts.push_back(limb(x0, y0, t.z + rr - 1, x0 + js::cos(a) * L, y0 + js::sin(a) * L, t.z + rr - 1.5, rr, rr * 0.8, MAT::DEADWOOD, moss));
}

void build_stump(const TreeIn& t, Prng&, Parts& parts) {
  const double rr = js::max(1.5, t.r);
  LimbExtra stump;
  stump.stump = true;
  parts.push_back(limb(t.x, t.y, t.z - 1, t.x, t.y, t.z + js::max(2, t.h), rr * 1.2, rr, MAT::BARK, stump));
}

// A unit direction of the yaw table.
Point2 dir_of(Prng& R) {
  const Yaw& Y = yaw_at(R.int_(0, static_cast<double>(yaws().size()) - 1));
  return {Y.c / Y.r, Y.s / Y.r};
}

// A wild snag: the dead trunk leans at an exact tilt of the yaw table, its broken limbs along it.
void wild_snag(const TreeIn& t, Prng& R, Parts& parts, double tr) {
  const Yaw& Y = yaw_at(R.int_(0, static_cast<double>(yaws().size()) - 1));
  const Yaw& T = yaw_at(js::round(amp_of(t) * R.int_(0, kSnagTilt)));
  const double ux = (Y.c / Y.r) * (T.s / T.r);
  const double uy = (Y.s / Y.r) * (T.s / T.r);
  const double uz = T.c / T.r;
  parts.push_back(limb(t.x, t.y, t.z - 1, t.x + ux * t.h, t.y + uy * t.h, t.z + uz * t.h, tr * 1.2, tr * 0.5, MAT::DEADWOOD, kTrunk));
  // (the loop's bound is drawn anew at every test, as JS's)
  for (double k = 0; k < R.int_(1, 3); k += 1) {
    const double f = R.range(0.4, 0.85);
    const double x = t.x + ux * t.h * f;
    const double y = t.y + uy * t.h * f;
    const double z = t.z + uz * t.h * f;
    const Point2 D = dir_of(R);
    const double L = R.range(3, 8);
    parts.push_back(limb(x, y, z, x + D.x * L, y + D.y * L, z + L * 0.5, 0.8, 0.5, MAT::DEADWOOD));
  }
}

void build_snag(const TreeIn& t, Prng& R, Parts& parts) {
  // a dead standing trunk, a few broken limbs
  const double tr = js::max(1, t.r);
  if (t.wild) {
    wild_snag(t, R, parts, tr);
    return;
  }
  const double bx = t.x + R.range(-1, 1);
  const double by = t.y + R.range(-1, 1);
  parts.push_back(limb(t.x, t.y, t.z - 1, bx, by, t.z + t.h, tr * 1.2, tr * 0.5, MAT::DEADWOOD, kTrunk));
  // (the loop's bound is drawn anew at every test, as JS's)
  for (double k = 0; k < R.int_(1, 3); k += 1) {
    const double z = t.z + t.h * R.range(0.4, 0.85);
    const double a = R.range(0, kPi * 2);
    const double L = R.range(3, 8);
    parts.push_back(limb(t.x, t.y, z, t.x + js::cos(a) * L, t.y + js::sin(a) * L, z + L * 0.5, 0.8, 0.5, MAT::DEADWOOD));
  }
}

// BUILD[t.kind] (autumn: maple's; shrubDry: shrub's).
void build(const TreeIn& t, Prng& R, Parts& parts) {
  switch (t.kind) {
    case TreeKind::Oak: return build_oak(t, R, parts);
    case TreeKind::Maple:
    case TreeKind::Autumn: return build_maple(t, R, parts);
    case TreeKind::Street: return build_street(t, R, parts);
    case TreeKind::Blossom: return build_blossom(t, R, parts);
    case TreeKind::Jungle: return build_jungle(t, R, parts);
    case TreeKind::Willow: return build_willow(t, R, parts);
    case TreeKind::Rowan: return build_rowan(t, R, parts);
    case TreeKind::Poplar: return build_poplar(t, R, parts);
    case TreeKind::Birch: return build_birch(t, R, parts);
    case TreeKind::Pine: return build_pine(t, R, parts);
    case TreeKind::Spruce: return build_spruce(t, R, parts);
    case TreeKind::Juniper: return build_juniper(t, R, parts);
    case TreeKind::Dwarfpine: return build_dwarfpine(t, R, parts);
    case TreeKind::Shrub:
    case TreeKind::ShrubDry: return build_shrub(t, R, parts);
    case TreeKind::Hazel: return build_hazel(t, R, parts);
    case TreeKind::Aspen: return build_aspen(t, R, parts);
    case TreeKind::Alder: return build_alder(t, R, parts);
    case TreeKind::Larch: return build_larch(t, R, parts);
    case TreeKind::Berry: return build_berry(t, R, parts);
    case TreeKind::Fern: return build_fern(t, R, parts);
    case TreeKind::Log: return build_log(t, R, parts);
    case TreeKind::Stump: return build_stump(t, R, parts);
    case TreeKind::Snag: return build_snag(t, R, parts);
    default: return;  // (acacia, palm, cactus: shapes of their own; unknown kinds)
  }
}

// A model: the tree's parts and bounds.
TreeModel build_model(const TreeIn& t) {
  TreeModel m;
  Prng R(js::to_int32(t.seed) ^ 0x7ee5);
  build(t, R, m.parts);
  for (const TreePart& p : m.parts) {
    const Box3& b = p.bb;
    if (m.has_bb) {
      m.bb = {js::min(m.bb.x0, b.x0), js::min(m.bb.y0, b.y0), js::min(m.bb.z0, b.z0), js::max(m.bb.x1, b.x1), js::max(m.bb.y1, b.y1), js::max(m.bb.z1, b.z1)};
    } else {
      m.bb = b;
      m.has_bb = true;
    }
  }
  // (foliage first: limbs only show where the leaves leave a gap; JS's stable sort on whether a
  // part is a limb - a consistent comparator, so any stable order is its order)
  std::stable_partition(m.parts.begin(), m.parts.end(), [](const TreePart& p) { return p.k != TreePartKind::Limb; });
  m.pal = model_palette(t.kind);
  return m;
}

// How far (voxels) a model reaches from the tree's foot, horizontally.
double reach_of(const TreeModel& m, const TreeIn& t) {
  return m.has_bb ? js::max(t.x - m.bb.x0, m.bb.x1 - t.x, t.y - m.bb.y0, m.bb.y1 - t.y) : 0;
}

// ---- rasterization
//
// Every voxel is computed as the reference computes it: the same operations on the same values, in
// the same order. What the rasterizers below leave out is only work that cannot change a voxel -
// rows, columns and voxels a conservative bound puts where JS's own tests would skip them (a spare
// voxel beyond every bound), the integers the hashes take of the chunk's coordinates converted once
// per chunk instead of per voxel (and hashes sharing their first three inputs mixed once), values of
// one axis hoisted out of the loops over the others, and a cone's angle and lobes taken once per
// column and whorl instead of per voxel. (A voxel's value depends on nothing but itself: each part
// writes only into air, every voxel once, so the order the voxels are visited in is free.)

// hash32 (core/hash) on inputs already taken ToInt32, stage by stage: hash32(a, b, c, d) is
// in(in(in(first(a), b * kHashB), c * kHashC), d * kHashD). A chunk's first stages and products are
// made once per axis (Grid), a voxel's hashes - which differ in the seed only - share their first
// three stages, and a column's share its first two.
constexpr uint32_t kHashA = 0x9e3779b9u;
constexpr uint32_t kHashB = 0x27d4eb2du;
constexpr uint32_t kHashC = 0x165667b1u;
constexpr uint32_t kHashD = 0x61c88647u;
inline uint32_t first(int32_t a) { return mix32(static_cast<uint32_t>(a) ^ kHashA); }
inline uint32_t in(uint32_t h, uint32_t v) { return mix32(h ^ v); }
inline uint32_t salt_of(double v) { return static_cast<uint32_t>(js::to_int32(v)) * kHashD; }
// (h01: the hash as a fraction)
inline double fin(uint32_t h, uint32_t salt) { return mix32(h ^ salt) / 4294967296.0; }

// The seed's salts, ToInt32(seed + c) times kHashD for each c the rasterizers add.
struct Salts {
  uint32_t s0, s5, s9, s11, s13, s17, s21, s22, s31, s33, s35;
  explicit Salts(double seed)
      : s0(salt_of(seed)),
        s5(salt_of(seed + 5)),
        s9(salt_of(seed + 9)),
        s11(salt_of(seed + 11)),
        s13(salt_of(seed + 13)),
        s17(salt_of(seed + 17)),
        s21(salt_of(seed + 21)),
        s22(salt_of(seed + 22)),
        s31(salt_of(seed + 31)),
        s33(salt_of(seed + 33)),
        s35(salt_of(seed + 35)) {}
};

// A chunk's padded indices on each axis: their representatives' world coordinates w (0 x, 1 y,
// 2 z), and the hash stages of the integers JS's hashes take of them - w | 0, w >> 1, w >> 2, and of
// a limb's voxel centre c = w + 0.5: c | 0, (c - 0.5) | 0 and (c - 0.5) >> 1 - as the input they
// are: x first (its first stage), y second (times kHashB), z third (times kHashC); for a birch's
// marks, hashed (z, x, y), the other way round.
struct Grid {
  double s = 0, bx = 0, by = 0, bz = 0;  // (the chunk it is made for)
  double w[3][kP];
  uint32_t x0[kP], x1[kP], x2[kP], xc[kP], xm[kP];
  uint32_t y0[kP], y1[kP], y2[kP], yc[kP], ym[kP];
  uint32_t z0[kP], z1[kP], z2[kP], zc[kP], zm[kP];
  // (up close) the 2-voxel blocks: each index's block w >> 1, counted from index 0's on its axis, and
  // whether every block of the chunk has a place of its own (not where coordinates wrap past 2^31)
  uint8_t bk[3][kP];
  uint32_t hz[kP];
  bool blocky = false;
  void make(const ChunkBuffer& ch) {
    s = ch.s;
    bx = ch.bx;
    by = ch.by;
    bz = ch.bz;
    for (int i = 0; i < kP; ++i) {
      const double x = ch.wx(i), y = ch.wy(i), z = ch.wz(i);
      w[0][i] = x;
      w[1][i] = y;
      w[2][i] = z;
      x0[i] = first(js::to_int32(x));
      x1[i] = first(js::sar(x, 1));
      x2[i] = first(js::sar(x, 2));
      xc[i] = first(js::to_int32(x + 0.5));
      xm[i] = static_cast<uint32_t>(js::sar(x + 0.5 - 0.5, 1)) * kHashB;
      y0[i] = static_cast<uint32_t>(js::to_int32(y)) * kHashB;
      y1[i] = static_cast<uint32_t>(js::sar(y, 1)) * kHashB;
      y2[i] = static_cast<uint32_t>(js::sar(y, 2)) * kHashB;
      yc[i] = static_cast<uint32_t>(js::to_int32(y + 0.5)) * kHashB;
      ym[i] = static_cast<uint32_t>(js::sar(y + 0.5 - 0.5, 1)) * kHashC;
      z0[i] = static_cast<uint32_t>(js::to_int32(z)) * kHashC;
      z1[i] = static_cast<uint32_t>(js::sar(z, 1)) * kHashC;
      z2[i] = static_cast<uint32_t>(js::sar(z, 2)) * kHashC;
      zc[i] = static_cast<uint32_t>(js::to_int32(z + 0.5)) * kHashC;
      zm[i] = first(js::to_int32(z + 0.5 - 0.5));
      hz[i] = static_cast<uint32_t>(js::sar(z, 1));
    }
    blocky = true;
    for (int a = 0; a < 3; ++a) {
      const uint32_t b0 = static_cast<uint32_t>(js::sar(w[a][0], 1));
      for (int i = 0; i < kP; ++i) {
        const uint32_t b = static_cast<uint32_t>(js::sar(w[a][i], 1)) - b0;
        blocky = blocky && b < 18;
        bk[a][i] = static_cast<uint8_t>(b < 18 ? b : 0);
      }
    }
  }
};

// The grid of a chunk: made once per chunk geometry and thread (trees come chunk by chunk).
const Grid& grid_of(const ChunkBuffer& ch) {
  thread_local Grid g;
  thread_local bool made = false;
  if (!made || g.s != ch.s || g.bx != ch.bx || g.by != ch.by || g.bz != ch.bz) {
    g.make(ch);
    made = true;
  }
  return g;
}

// Narrows the padded indices [lo, hi] to those whose representatives lie in the world interval
// [a, b] on an axis whose index 0 lies at `base` (s apart). A bound that is no number narrows
// nothing.
inline void clip(double base, double s, double a, double b, int& lo, int& hi) {
  // (lo = max(lo, ceil(l)), hi = min(hi, floor(h)); within [lo, hi], both non-negative)
  const double l = (a - base) / s;
  const double h = (b - base) / s;
  if (l > lo) {
    if (l > hi) {
      lo = hi + 1;
    } else {
      const int c = static_cast<int>(l);
      lo = c < l ? c + 1 : c;
    }
  }
  if (h < hi) hi = h < lo ? lo - 1 : static_cast<int>(h);
}

// rasterizeTree's context: the look's snow, bare branches, palettes, holes.
struct Ctx {
  double snow;
  bool bare;
  const LeafPalette* pal;
  const LeafPalette* season;  // (null: none)
  double mix;
  uint16_t accent;  // (0: none)
  double holes;
  Salts salt;
};

// The palette of one cluster: the season's on `mix` of them, else summer's.
const LeafPalette& palette_of(double mix, const Ctx& ctx) { return ctx.season && mix < ctx.mix ? *ctx.season : *ctx.pal; }

// Up close, a cluster's noise and its shading's hash are those of the voxel's 2-voxel block
// (x >> 1, y >> 1, z >> 1): made once per block for its eight voxels, kept for a layer of blocks
// (a chunk spans 18 blocks a side) while it is the layer being drawn. (Per thread; a layer is
// valid while its generation is the current one.)
struct BlockCache {
  struct Entry {
    uint32_t stamp;
    double n, vh;
  };
  uint32_t gen = 0;
  Entry e[18][18] = {};
  // a new layer: every block made afresh
  void next() {
    if (++gen == 0) {
      for (auto& row : e)
        for (Entry& v : row) v.stamp = 0;
      gen = 1;
    }
  }
};

BlockCache& block_cache() {
  thread_local BlockCache blocks;
  return blocks;
}

void raster_blob(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  const TreeBlob& p = part.blob;
  const Box3& b = part.bb;
  const IdxRange ri = chunk.range_x(b.x0, b.x1);
  const IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const double s = chunk.s;
  const bool fine = s == 1;
  const double infl = fine ? 0 : s * 0.5;
  const double irx = 1 / js::pow(p.rx + infl, 2);
  const double irz = 1 / js::pow(p.rz + infl, 2);
  const double sirz = js::sqrt(irz);
  const LeafPalette& pal = palette_of(p.mix, ctx);
  const Salts& salt = ctx.salt;
  const double snow = ctx.snow;
  const bool bare = ctx.bare;
  const double holes = ctx.holes;
  const uint16_t accent = ctx.accent;
  // at q >= thr a voxel lies outside the cluster whatever its noise (|n| < 0.3 up close, 0 further out)
  const double thr = fine ? 1.3 : 1;
  const double base_x = chunk.bx + chunk.half;
  BlockCache& blocks = block_cache();
  const bool blocky = fine && g.blocky;
  uint32_t layer = 0;
  bool layered = false;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = g.w[2][k];
    const double dz = z + 0.5 - p.z;
    const double qz = dz * dz * irz;
    if (qz > 1.3) continue;
    const bool up = snow > 0 && dz > 0;
    const double dz2 = dz + s;
    const double qz2 = dz2 * dz2 * irz;
    // (sunlit tops, shaded undersides: the shading's part of this height)
    const double v0 = 0.5 + 0.42 * dz * sirz;
    const uint32_t zh = g.z1[k];
    const uint32_t zi = g.z0[k];
    if (blocky && (!layered || g.hz[k] != layer)) {
      blocks.next();
      layer = g.hz[k];
      layered = true;
    }
    for (int j = rj.lo; j <= rj.hi; ++j) {
      const double y = g.w[1][j];
      const double dy = y + 0.5 - p.y;
      const double dy2 = dy * dy;
      // (every voxel of the row at q >= thr; else those with |x + 0.5 - p.x| below the bound)
      if (dy2 * irx + qz >= thr) continue;
      int i0 = ri.lo;
      int i1 = ri.hi;
      const double hw = std::sqrt((thr - qz) / irx - dy2) + 1;
      clip(base_x, s, p.x - 0.5 - hw, p.x - 0.5 + hw, i0, i1);
      // (a bare cluster writes into its shell only: q > 0.68 + n, past 0.38 up close and 0.68 further
      // out; the hollow within, a voxel short of it, is skipped)
      int h0 = i1 + 1, h1 = i1;
      if (bare) {
        const double w2 = ((fine ? 0.38 : 0.68) - qz) / irx - dy2;
        if (w2 > 1) {
          h0 = i0;
          h1 = i1;
          const double hin = std::sqrt(w2) - 1;
          clip(base_x, s, p.x - 0.5 - hin, p.x - 0.5 + hin, h0, h1);
        }
      }
      const uint32_t yh = g.y1[j];
      const uint32_t yi = g.y0[j];
      BlockCache::Entry* brow = blocks.e[g.bk[1][j]];
      const uint32_t gen = blocks.gen;
      // (the row's voxels before its hollow and after it)
      const int segs[2][2] = {{i0, h0 <= h1 ? h0 - 1 : i1}, {h0 <= h1 ? h1 + 1 : i1 + 1, i1}};
      for (const auto& seg : segs) {
        for (int i = seg[0]; i <= seg[1]; ++i) {
          const int idx = i + j * kP + k * kP2;
          if (d[idx] != 0) continue;
          const double x = g.w[0][i];
          const double dx = x + 0.5 - p.x;
          const double q = (dx * dx + dy2) * irx + qz;
          if (q >= thr) continue;
          // (the hash of x >> 1, y >> 1, z >> 1: the noise's, and the shading's)
          double n = 0, vh = 0;
          if (fine) {
            BlockCache::Entry& e = brow[g.bk[0][i]];
            if (blocky && e.stamp == gen) {
              n = e.n;
              vh = e.vh;
            } else {
              const uint32_t ph = in(in(g.x1[i], yh), zh);
              // a lumpy surface of leaf tufts (2-voxel noise) up close
              n = (fin(ph, salt.s0) - 0.5) * 0.6;
              vh = fin(ph, salt.s9);
              if (blocky) e = {gen, n, vh};
            }
          }
          if (q >= 1 + n) continue;
          const bool shell = q > 0.68 + n;
          if (bare) {
            // leafless: a few twig voxels in the shell (the limbs and twigs are drawn anyway)
            if (!shell) continue;
            const uint32_t pv = in(in(g.x0[i], yi), zi);
            if (fin(pv, salt.s5) > (fine ? 0.07 : 0.16)) continue;
            d[idx] = up && fin(pv, salt.s11) < snow * 0.4 ? MAT::SNOW : MAT::TWIGS;
            continue;
          }
          // (the hash of x, y, z: holes, snow, berries)
          uint32_t pv = 0;
          bool hv = false;
          if (fine && shell) {
            pv = in(in(g.x0[i], yi), zi);
            hv = true;
            if (fin(pv, salt.s0) < holes) continue;
          }
          if (up && (dx * dx + dy2) * irx + qz2 >= 1 + n) {
            // snow where nothing of the cluster lies above
            if (!hv) {
              pv = in(in(g.x0[i], yi), zi);
              hv = true;
            }
            if (fin(pv, salt.s11) < snow * 0.85) {
              d[idx] = MAT::SNOW;
              continue;
            }
          }
          if (accent && shell) {
            if (!hv) pv = in(in(g.x0[i], yi), zi);
            if (fin(pv, salt.s13) < 0.06) {
              d[idx] = accent;
              continue;
            }
          }
          if (!fine) vh = fin(in(in(g.x1[i], yh), zh), salt.s9);
          // sunlit tops, shaded undersides and interior
          const double v = v0 + (vh - 0.5) * 0.45 - (shell ? 0 : 0.2);
          d[idx] = v < 0.34 ? pal[0] : v < 0.74 ? pal[1] : pal[2];
        }
      }
    }
  }
}

void raster_limb(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  const TreeLimb& p = part.limb;
  const double s = chunk.s;
  // twigs vanish at a distance; thin limbs thicken a little so trunks still read
  if (part.twig && (s > 1 || !ctx.bare)) return;
  const double grow = s > 1 ? s * 0.35 : 0;
  if (!part.trunk && s > 2 && p.r0 < 1.2) return;
  const Box3& b = part.bb;
  const IdxRange ri = chunk.range_x(b.x0, b.x1);
  const IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const Salts& salt = ctx.salt;
  const double foot = part.has_foot ? p.foot : p.az;
  const bool snowy = ctx.snow > 0.2 && !part.trunk;
  // A row's voxels within reach of the limb's line (rm: its radius and a hundredth of a voxel to
  // spare), where a vx^2 + bq vx + cq <= 0 (vx = x - ax): the limb's own voxels among them. A row
  // whose own line passes the limb's further than rm (|vy dz - vz dy| / |(dy, dz)|) holds none. (A
  // limb nearly along x is taken row by row whole.)
  const double rm = js::max(p.r0, p.r1) + grow + 0.01;
  const double rm2 = rm * rm;
  const double a = 1 - p.dx * p.dx / p.L2;
  const bool lined = a > 1e-3 && a <= 1 && rm < 1e6;
  const double reach = rm * std::sqrt(p.dy * p.dy + p.dz * p.dz) * (1 + 1e-9);
  const double spare = 0.01 + 1e-9 * js::abs(p.ax);
  const double base_x = chunk.bx + chunk.half;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = g.w[2][k] + 0.5;
    const double vz = z - p.az;
    const double wz = z - 0.5;
    const uint32_t zc = g.zc[k];
    const uint32_t zm = g.zm[k];
    for (int j = rj.lo; j <= rj.hi; ++j) {
      const double y = g.w[1][j] + 0.5;
      const double vy = y - p.ay;
      if (js::abs(vy * p.dz - vz * p.dy) > reach) continue;
      int i0 = ri.lo;
      int i1 = ri.hi;
      if (lined) {
        const double c0 = vy * p.dy + vz * p.dz;
        const double bq = -2 * p.dx * c0 / p.L2;
        const double cq = vy * vy + vz * vz - c0 * c0 / p.L2 - rm2;
        const double disc = bq * bq - 4 * a * cq;
        if (disc < 0) continue;
        const double sq = std::sqrt(disc);
        clip(base_x, s, p.ax - 0.5 + (-bq - sq) / (2 * a) - spare, p.ax - 0.5 + (-bq + sq) / (2 * a) + spare, i0, i1);
      }
      const uint32_t yc = g.yc[j];
      const uint32_t ym = g.ym[j];
      for (int i = i0; i <= i1; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        const double x = g.w[0][i] + 0.5;
        const double vx = x - p.ax;
        double u = (vx * p.dx + vy * p.dy + vz * p.dz) / p.L2;
        u = u < 0 ? 0 : u > 1 ? 1 : u;
        const double ex = vx - p.dx * u;
        const double ey = vy - p.dy * u;
        const double ez = vz - p.dz * u;
        const double r = p.r0 + (p.r1 - p.r0) * u + grow;
        if (ex * ex + ey * ey + ez * ez > r * r) continue;
        uint16_t m = part.mat;
        // birch: short dark marks on the white bark, a dark rough foot
        if (part.birch && ((s == 1 && fin(in(in(zm, g.xm[i]), ym), salt.s0) < 0.13) || wz - foot < 3 + (fin(in(in(g.xc[i], yc), 0), salt.s0) < 0.5 ? 2 : 0)))
          m = MAT::BARK_BIRCH_MARK;
        else if (part.moss && ez > 0.3 * r && ez * ez > ex * ex + ey * ey)
          m = MAT::MOSS;
        else if (part.stump && wz >= p.az + p.dz - 1)
          m = MAT::WOOD_LIGHT;
        else if (snowy && ez > 0 && ez * ez > (ex * ex + ey * ey) * 0.5 && fin(in(in(g.xc[i], yc), zc), salt.s17) < ctx.snow * 0.6)
          m = MAT::SNOW;
        d[idx] = m;
      }
    }
  }
}

// A cone's whorl at one height (what JS's radius() computes of the height alone): in (rel within
// [0, 1]: else the radius is -1), the radius at coarse LODs, and up close env (1 - 0.4 pt) - the
// radius being that times the lobe term, plus 0.7 - and the whorl's index.
struct Whorl {
  bool in = false;
  double coarse = 0;
  double envpt = 0;
  double ti = 0;
};

Whorl whorl_at(const TreeCone& p, double H, double s, bool fine, double zz) {
  Whorl w;
  const double rel = (zz - p.z0) / H;
  if (rel < 0 || rel > 1) return w;
  w.in = true;
  const double env = p.r * js::pow(1 - rel, 1.05);
  if (!fine) {
    w.coarse = env * 0.9 + 0.8 + s * 0.5;
    return w;
  }
  const double fz = (zz - p.z0) / p.tier;
  w.ti = std::floor(fz);
  const double pt = fz - std::floor(fz);
  w.envpt = env * (1 - 0.4 * pt);
  return w;
}

// The lobes of whorl ti at angle a (irregular branch lobes round the stem): 0.8 + 0.2 sin(..) plus
// the noise of the whorl's sector of the angle, floor((a + 3.2) 1.6) (a within -pi and pi: the
// floor of a small positive number, 0 to 10).
int32_t sector_of(double a) {
  const double v = (a + 3.2) * 1.6;
  return v >= 0 && v < 1e9 ? static_cast<int32_t>(v) : js::to_int32(std::floor(v));
}
double lobe_noise(double ti, int32_t sector, uint32_t s0) {
  return (fin(in(in(first(js::to_int32(ti)), static_cast<uint32_t>(sector) * kHashB), 3 * kHashC), s0) - 0.5) * 0.3;
}
double lobe_of(const TreeCone& p, double ti, double a, uint32_t s0) { return 0.8 + 0.2 * js::sin(p.lobes * a + p.phase + ti * 2.3) + lobe_noise(ti, sector_of(a), s0); }

// A leaning cone's lobe noise, per whorl and sector (a chunk spans a few whorls; its voxels' angles
// differ, their sectors are 11).
struct LobeNoise {
  static constexpr int kWhorls = 48;
  double ti0 = 0;
  double v[kWhorls][11];
  bool made[kWhorls][11] = {};
  double get(double ti, int32_t sector, uint32_t s0) {
    const double w = ti - ti0;
    if (!(w >= 0 && w < kWhorls) || sector < 0 || sector > 10) return lobe_noise(ti, sector, s0);
    const int a = static_cast<int>(w);
    if (!made[a][sector]) {
      v[a][sector] = lobe_noise(ti, sector, s0);
      made[a][sector] = true;
    }
    return v[a][sector];
  }
};

// A column's lobes, kept per whorl (two: the snow looks one whorl up).
struct LobeCache {
  double ti[2] = {js::kNaN, js::kNaN};
  double lob[2] = {0, 0};
  int next = 0;
  double get(const TreeCone& p, double t, double a, uint32_t s0) {
    if (ti[0] == t) return lob[0];
    if (ti[1] == t) return lob[1];
    const int e = next;
    next ^= 1;
    ti[e] = t;
    lob[e] = lobe_of(p, t, a, s0);
    return lob[e];
  }
};

// Conifer cone (spruce, juniper): whorls every few voxels that droop (each widest at its foot),
// their branches in irregular lobes round the stem; dead lower whorls in a crowded stand; snow on
// every whorl.
void raster_cone(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  const TreeCone& p = part.cone;
  const Box3& b = part.bb;
  IdxRange ri = chunk.range_x(b.x0, b.x1);
  IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const double s = chunk.s;
  const bool fine = s == 1;
  const double H = js::max(1, p.z1 - p.z0);
  const LeafPalette& pal = *ctx.pal;
  const Salts& salt = ctx.salt;
  const double snow = ctx.snow;
  // per height: in the cone's rows or not, the whorls at zz and zz + s (the snow's), the shading's
  // phase, and how far out its voxels may lie (the radius at the lobes' widest, its noise, the lean's
  // lopsidedness: past it, hr > edge)
  bool row[kP];
  Whorl W[kP], W2[kP];
  double pts[kP], bound[kP], ox[kP], oy[kP];
  // (a lean's lop lies within 1 -+ |asym|: positive below 1, when a row out of the cone - R = -1
  // times it - writes nothing, and the bounds hold)
  const bool bounded = !part.lean || js::abs(p.asym) < 0.999;
  const double lopmax = part.lean ? 1 + js::abs(p.asym) * 1.0001 + 1e-9 : 1;
  double bmax = 0;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double zz = g.w[2][k] + 0.5;
    row[k] = !(zz < p.z0 || zz > p.z1 + 1);
    if (!row[k]) continue;
    W[k] = whorl_at(p, H, s, fine, zz);
    if (!W[k].in && bounded) {
      row[k] = false;
      continue;
    }
    if (snow > 0) W2[k] = whorl_at(p, H, s, fine, zz + s);  // (read for the snow only)
    pts[k] = fine ? std::fmod((zz - p.z0) / p.tier, 1) : 0.5;
    // (a cone that leans: the axis at this height)
    ox[k] = part.lean ? p.x + p.lx * ((zz - p.zf) / p.hh) : p.x;
    oy[k] = part.lean ? p.y + p.ly * ((zz - p.zf) / p.hh) : p.y;
    // the radius at its widest (the lobes lie within 0.45 and 1.15), past which hr > edge (the
    // noise adds less than 0.55 up close)
    const double e = W[k].envpt;
    const double r = fine ? (e >= 0 ? e * 1.16 : e * 0.44) + 0.71 : W[k].coarse;
    bound[k] = W[k].in && bounded ? r * lopmax * 1.0000001 + (fine ? 0.56 : 1e-6) : js::kInf;
    if (bound[k] != bound[k])
      bmax = js::kInf;
    else if (bound[k] > bmax)
      bmax = bound[k];
  }
  // (the widest bound from each height up: past it, no voxel of the column higher up is the cone's)
  double above[kP];
  double widest = -js::kInf;
  for (int k = rk.hi; k >= rk.lo; --k) {
    if (row[k]) widest = bound[k] != bound[k] ? js::kInf : js::max(widest, bound[k]);
    above[k] = widest;
  }
  // a voxel of the cone: what JS computes once the voxel is air, with its angle and its lobes given
  // (c1, c0: the column's hashes of (x >> 1, y >> 1) and (x, y), their first two stages)
  auto voxel = [&](int idx, uint32_t c1, uint32_t c0, int k, double dx, double dy, double hr, double lob, double lob2) {
    const double lop = part.lean ? 1 + (p.asym * (dx * p.ax + dy * p.ay)) / js::max(hr, 1e-9) : 1;
    const double rad = !W[k].in ? -1 : fine ? W[k].envpt * lob + 0.7 : W[k].coarse;
    const double R = part.lean ? rad * lop : rad;
    // the leader: a thin spike over the top whorl
    if (R < 0) return;
    // (up close the edge lies within 0.55 of R)
    if (fine && hr > R + 0.55) return;
    const uint32_t ph = in(c1, g.z1[k]);
    const double edge = fine ? R + (fin(ph, salt.s0) - 0.5) * 1.1 : R;
    if (hr > edge) return;
    const bool shell = hr > edge - 1.5;
    uint32_t pv = 0;
    bool hv = false;
    if (fine && shell) {
      pv = in(c0, g.z0[k]);
      hv = true;
      if (fin(pv, salt.s0) < 0.12) return;
    }
    if (snow > 0) {
      const double rad2 = !W2[k].in ? -1 : fine ? W2[k].envpt * lob2 + 0.7 : W2[k].coarse;
      const double Ra = part.lean ? rad2 * lop : rad2;
      // (snow on the upper side of every whorl, the dark needles still showing beneath)
      if (hr > Ra - 0.6) {
        if (!hv) pv = in(c0, g.z0[k]);
        if (fin(pv, salt.s11) < snow * 0.7) {
          d[idx] = MAT::SNOW;
          return;
        }
      }
    }
    // outer needles lighter, the inside and the underside of a whorl dark
    const double v = 0.25 + 0.55 * (hr / js::max(1, R)) + 0.25 * pts[k] + (fin(ph, salt.s9) - 0.5) * 0.35;
    d[idx] = v < 0.42 ? pal[0] : v < 0.82 ? pal[1] : pal[2];
  };
  const double base_x = chunk.bx + chunk.half;
  const double base_y = chunk.by + chunk.half;
  if (!part.lean) {
    // column by column: the angle once per column, the lobes once per whorl
    if (bmax < 1e9) {
      clip(base_x, s, p.x - 0.5 - bmax - 1, p.x - 0.5 + bmax + 1, ri.lo, ri.hi);
      clip(base_y, s, p.y - 0.5 - bmax - 1, p.y - 0.5 + bmax + 1, rj.lo, rj.hi);
    }
    for (int j = rj.lo; j <= rj.hi; ++j) {
      const double y = g.w[1][j];
      const double dy = y + 0.5 - p.y;
      for (int i = ri.lo; i <= ri.hi; ++i) {
        const double x = g.w[0][i];
        const double dx = x + 0.5 - p.x;
        const double hr = js::sqrt(dx * dx + dy * dy);
        if (hr > bmax) continue;
        const uint32_t c1 = in(g.x1[i], g.y1[j]);
        const uint32_t c0 = in(g.x0[i], g.y0[j]);
        double a = 0;
        bool has_a = false;
        LobeCache lc;
        for (int k = rk.lo; k <= rk.hi; ++k) {
          if (hr > above[k]) break;
          if (!row[k] || hr > bound[k]) continue;
          const int idx = i + j * kP + k * kP2;
          if (d[idx] != 0) continue;
          double lob = 0, lob2 = 0;
          if (fine) {
            if (!has_a) {
              a = js::atan2(dy, dx);
              has_a = true;
            }
            if (W[k].in) lob = lc.get(p, W[k].ti, a, salt.s0);
            if (snow > 0 && W2[k].in) lob2 = lc.get(p, W2[k].ti, a, salt.s0);
          }
          voxel(idx, c1, c0, k, dx, dy, hr, lob, lob2);
        }
      }
    }
    return;
  }
  // a cone that leans: row by row (its axis moves with the height), within the bound round it; a
  // voxel past the widest radius its own lopsidedness allows needs no angle
  LobeNoise noise;
  for (int k = rk.lo; k <= rk.hi; ++k)
    if (row[k] && W[k].in) {
      noise.ti0 = W[k].ti;
      break;
    }
  for (int k = rk.lo; k <= rk.hi; ++k) {
    if (!row[k]) continue;
    int i0 = ri.lo, i1 = ri.hi, j0 = rj.lo, j1 = rj.hi;
    if (bound[k] < 1e9) {
      clip(base_x, s, ox[k] - 0.5 - bound[k] - 1, ox[k] - 0.5 + bound[k] + 1, i0, i1);
      clip(base_y, s, oy[k] - 0.5 - bound[k] - 1, oy[k] - 0.5 + bound[k] + 1, j0, j1);
    }
    for (int j = j0; j <= j1; ++j) {
      const double y = g.w[1][j];
      const double dy = y + 0.5 - oy[k];
      for (int i = i0; i <= i1; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        const double x = g.w[0][i];
        const double dx = x + 0.5 - ox[k];
        const double hr = js::sqrt(dx * dx + dy * dy);
        if (hr > bound[k]) continue;
        double lob = 0, lob2 = 0;
        if (fine) {
          const double e = W[k].envpt;
          if (bounded && e >= 0) {
            const double lop = 1 + (p.asym * (dx * p.ax + dy * p.ay)) / js::max(hr, 1e-9);
            if (lop > 0 && hr > (e * 1.16 + 0.71) * lop * 1.0000001 + 0.56) continue;
          }
          const double a = js::atan2(dy, dx);
          const int32_t sector = sector_of(a);
          if (W[k].in) lob = 0.8 + 0.2 * js::sin(p.lobes * a + p.phase + W[k].ti * 2.3) + noise.get(W[k].ti, sector, salt.s0);
          if (snow > 0 && W2[k].in)
            lob2 = W2[k].ti == W[k].ti && W[k].in ? lob : 0.8 + 0.2 * js::sin(p.lobes * a + p.phase + W2[k].ti * 2.3) + noise.get(W2[k].ti, sector, salt.s0);
        }
        voxel(idx, in(g.x1[i], g.y1[j]), in(g.x0[i], g.y0[j]), k, dx, dy, hr, lob, lob2);
      }
    }
  }
}

constexpr LeafPalette kTwigs = {MAT::TWIGS, MAT::TWIGS, MAT::TWIGS};

// Willow curtains: thin strands of leaves hanging from the underside of a cluster, longer
// towards its rim, swaying in small groups.
void raster_curtain(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  if (ctx.bare && chunk.s == 1) return;
  const TreeCurtain& p = part.curtain;
  const Box3& b = part.bb;
  const IdxRange ri = chunk.range_x(b.x0, b.x1);
  const IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const bool fine = chunk.s == 1;
  const LeafPalette& pal = ctx.bare ? kTwigs : palette_of(p.mix, ctx);
  const Salts& salt = ctx.salt;
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double y = g.w[1][j];
    const double dy = y + 0.5 - p.y;
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double x = g.w[0][i];
      const double dx = x + 0.5 - p.x;
      const double rr = js::sqrt(dx * dx + dy * dy) / p.rx;
      if (rr > 1.02 || rr < 0.5) continue;
      const double strand = fin(in(in(g.x0[i], g.y0[j]), 0), salt.s21);
      if (fine && strand > 0.45) continue;
      // hanging from the cluster's underside, longest at its rim
      const double top = p.z - p.rz * js::sqrt(js::max(0, 1 - rr * rr)) + 1;
      const double len = p.drop * (0.45 + 0.55 * fin(in(in(g.x1[i], g.y1[j]), kHashC), salt.s22)) * (0.3 + 0.7 * rr);
      const uint16_t m = strand < 0.1 ? pal[0] : strand < 0.3 ? pal[1] : pal[2];
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const double z = g.w[2][k];
        if (z > top || z < top - len) continue;
        const int idx = i + j * kP + k * kP2;
        if (d[idx] == 0) d[idx] = m;
      }
    }
  }
}

// A fern: fronds arching out from the crown in every direction.
void raster_fern(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  // (ferns die back in winter)
  if (chunk.s > 1 || ctx.bare) return;
  const TreeFern& p = part.fern;
  const Box3& b = part.bb;
  const IdxRange ri = chunk.range_x(b.x0, b.x1);
  const IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const LeafPalette& pal = ctx.season ? *ctx.season : *ctx.pal;
  for (int j = rj.lo; j <= rj.hi; ++j) {
    const double dy = g.w[1][j] + 0.5 - p.y;
    for (int i = ri.lo; i <= ri.hi; ++i) {
      const double dx = g.w[0][i] + 0.5 - p.x;
      const double hr = js::sqrt(dx * dx + dy * dy);
      if (hr > p.r) continue;
      // near a frond's axis?
      const double a = js::atan2(dy, dx) - p.phase;
      const double sector = (kPi * 2) / p.n;
      const double off = js::abs(std::fmod(std::fmod(a, sector) + sector * 1.5, sector) - sector / 2) * hr;
      if (off > 0.75 + hr * 0.12) continue;
      const double zf = p.z + js::round(p.h * js::sin(js::min(1, hr / p.r) * kPi * 0.85));
      const double zb = zf - (hr < 1.5 ? p.h : 0);
      const uint16_t m = ctx.snow > 0.3 ? static_cast<uint16_t>(MAT::SNOW) : off < 0.4 ? pal[0] : pal[1];
      for (int k = rk.lo; k <= rk.hi; ++k) {
        const double z = g.w[2][k];
        if (z < zb || z > zf) continue;
        const int idx = i + j * kP + k * kP2;
        if (d[idx] == 0) d[idx] = m;
      }
    }
  }
}

// A windthrow's root plate: a ragged disc (normal n, radius r, half thickness th) of soil threaded
// with roots, its face towards the trunk the mossy forest floor it lifted. Up close only.
void raster_plate(ChunkBuffer& chunk, const TreePart& part, const Ctx& ctx, const Grid& g) {
  if (chunk.s > 2) return;
  const TreePlate& p = part.plate;
  const Box3& b = part.bb;
  const IdxRange ri = chunk.range_x(b.x0, b.x1);
  const IdxRange rj = chunk.range_y(b.y0, b.y1);
  const IdxRange rk = chunk.range_z(b.z0, b.z1);
  if (ri.lo > ri.hi || rj.lo > rj.hi || rk.lo > rk.hi) return;
  uint16_t* d = chunk.data.data();
  const Salts& salt = ctx.salt;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = g.w[2][k];
    const double dz = z + 0.5 - p.z;
    for (int j = rj.lo; j <= rj.hi; ++j) {
      const double y = g.w[1][j];
      const double dy = y + 0.5 - p.y;
      for (int i = ri.lo; i <= ri.hi; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        const double x = g.w[0][i];
        const double dx = x + 0.5 - p.x;
        const double a = dx * p.nx + dy * p.ny + dz * p.nz;
        if (a > p.th || a < -p.th) continue;
        // (a lobed, ragged rim: soil falls away between the roots)
        const double rim = p.r * (0.82 + 0.3 * fin(in(in(g.x2[i], g.y2[j]), g.z2[k]), salt.s31));
        const double q2 = dx * dx + dy * dy + dz * dz - a * a;
        if (q2 > rim * rim) continue;
        const uint32_t pv = in(in(g.x0[i], g.y0[j]), g.z0[k]);
        if (q2 > rim * rim * 0.56 && fin(pv, salt.s35) < 0.35) continue;
        const double q = fin(pv, salt.s33);
        d[idx] = a < -p.th * 0.35 ? (q < 0.12 ? MAT::LICHEN : MAT::MOSS) : q < 0.2 ? MAT::BARK : q < 0.3 ? MAT::DEADWOOD : MAT::DIRT;
      }
    }
  }
}

// The voxels of a shape of its own (eachVoxel: every representative of its bounds, filling only
// air), height by height: at each, only within the rect cand(z, rect) gives (false: none) - every
// voxel fn could fill there, with room to spare.
template <class Cand, class F>
void each_voxel(ChunkBuffer& chunk, const Box3& bb, const Grid& g, Cand cand, F fn) {
  const IdxRange ri = chunk.range_x(bb.x0, bb.x1);
  const IdxRange rj = chunk.range_y(bb.y0, bb.y1);
  const IdxRange rk = chunk.range_z(bb.z0, bb.z1);
  uint16_t* d = chunk.data.data();
  const double s = chunk.s;
  const double base_x = chunk.bx + chunk.half;
  const double base_y = chunk.by + chunk.half;
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = g.w[2][k];
    Rect c;
    if (!cand(z, c)) continue;
    int i0 = ri.lo, i1 = ri.hi, j0 = rj.lo, j1 = rj.hi;
    clip(base_x, s, c.x0, c.x1, i0, i1);
    clip(base_y, s, c.y0, c.y1, j0, j1);
    for (int j = j0; j <= j1; ++j) {
      const double y = g.w[1][j];
      for (int i = i0; i <= i1; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        const uint16_t m = fn(i, j, k, g.w[0][i], y, z);
        if (m) d[idx] = m;
      }
    }
  }
}

// umbrella crown on a leaning, forking trunk
void shape_acacia(ChunkBuffer& chunk, const Tree& t, const Box3& bb, const Grid& g) {
  const double top = t.z + t.h;
  const double crown0 = top - 3;
  const double lean = static_cast<double>(js::shr(t.seed, 4) & 3) - 1.5;
  const uint32_t s0 = salt_of(t.seed);
  const uint32_t s5 = salt_of(t.seed + 5);
  // (the crown reaches out to t.r (0.75 t.r on top) plus its noise, < 0.75; the trunk 1.1 round its
  // axis, the fork 0.8 round its own)
  const double crown = js::max(t.r, t.r * 0.75) + 0.8;
  auto cand = [&](double z, Rect& c) {
    if (z >= t.z && z < crown0) {
      const double k = (z - t.z) / js::max(1, crown0 - t.z);
      const double ax = t.x + lean * k * 3;
      const double fx = k > 0.6 ? ax - (k - 0.6) * 8 : ax;
      c = {js::min(ax, fx) - 1.2, t.y - 1.2, js::max(ax, fx) + 1.2, t.y + 1.2};
      return true;
    }
    if (z >= crown0 && z <= top) {
      c = {t.x - crown, t.y - crown, t.x + crown, t.y + crown};
      return true;
    }
    return false;
  };
  each_voxel(chunk, bb, g, cand, [&](int i, int j, int k, double x, double y, double z) -> uint16_t {
    const double dx = x - t.x;
    const double dy = y - t.y;
    if (z >= t.z && z < crown0) {
      const double kk = (z - t.z) / js::max(1, crown0 - t.z);
      const double ax = lean * kk * 3;
      if (js::hypot(dx - ax, dy) <= 1.1) return MAT::BARK;
      if (kk > 0.6 && js::hypot(dx - ax + (kk - 0.6) * 8, dy) <= 0.8) return MAT::BARK;
      return 0;
    }
    if (z >= crown0 && z <= top) {
      const double hr = js::hypot(dx, dy);
      const double rr = t.r * (z == top ? 0.75 : 1) + (fin(in(in(g.x1[i], g.y1[j]), g.z0[k]), s0) - 0.5) * 1.5;
      return hr <= rr && fin(in(in(g.x0[i], g.y0[j]), g.z0[k]), s5) > 0.1 ? MAT::LEAVES_ACACIA : 0;
    }
    return 0;
  });
}

// saguaro: a column with one or two upturned arms
void shape_cactus(ChunkBuffer& chunk, const Tree& t, const Box3& bb, const Grid& g) {
  struct Arm {
    double ox, oy, z0;
  };
  Arm arms[2] = {};
  const int n = 1 + static_cast<int>(js::shr(t.seed, 3) & 1);
  for (int a = 0; a < n; ++a) {
    const double ang = static_cast<double>(js::shr(t.seed, 6 + a * 3) & 7) * (kPi / 4) + a * kPi;
    const double z0 = t.z + js::round(t.h * (0.35 + 0.15 * a));
    arms[a] = {js::round(js::cos(ang) * 4), js::round(js::sin(ang) * 4), z0};
  }
  const double up = js::round(t.h * 0.35);
  // (the column within 1.5 of its axis, the arms within 5 (|ox|, |oy| <= 4, then 1 round them))
  auto cand = [&](double z, Rect& c) {
    bool any = z >= t.z && z <= t.z + t.h;
    for (int q = 0; q < n; ++q) any = any || z == arms[q].z0 || z == arms[q].z0 + 1 || (z >= arms[q].z0 && z <= arms[q].z0 + up);
    c = {t.x - 5.5, t.y - 5.5, t.x + 5.5, t.y + 5.5};
    return any;
  };
  each_voxel(chunk, bb, g, cand, [&](int, int, int, double x, double y, double z) -> uint16_t {
    const double dx = x - t.x;
    const double dy = y - t.y;
    if (z >= t.z && z <= t.z + t.h && dx * dx + dy * dy <= 2) return MAT::CACTUS;
    for (int q = 0; q < n; ++q) {
      const Arm& a = arms[q];
      // horizontal elbow then an upright arm
      if (z == a.z0 || z == a.z0 + 1) {
        const double tt = (dx * a.ox + dy * a.oy) / 16;
        if (tt >= 0 && tt <= 1 && js::hypot(dx - a.ox * tt, dy - a.oy * tt) <= 1) return MAT::CACTUS;
      }
      if (z >= a.z0 && z <= a.z0 + up && js::hypot(dx - a.ox, dy - a.oy) <= 1) return MAT::CACTUS;
    }
    return 0;
  });
}

// straight thin trunk with drooping fronds
void shape_palm(ChunkBuffer& chunk, const Tree& t, const Box3& bb, const Grid& g) {
  const double top = t.z + t.h;
  const double lean_x = (static_cast<double>(js::shr(t.seed, 2) & 7) - 3.5) * 0.35;
  const double lean_y = (static_cast<double>(js::shr(t.seed, 5) & 7) - 3.5) * 0.35;
  const double nF = 7;
  const double turn = static_cast<double>(js::to_int32(t.seed) & 7) * 0.3;
  const double fx = t.x + lean_x * 6;
  const double fy = t.y + lean_y * 6;
  // JS's test of the trunk (hypot >= either of its arguments: the cheap test first)
  auto trunk_at = [&](double x, double y, double z) {
    const double k = (z - t.z) / t.h;
    const double cx = t.x + lean_x * k * k * 6;
    const double cy = t.y + lean_y * k * k * 6;
    return z >= t.z && z <= top && js::abs(x - cx) <= 0.9 && js::abs(y - cy) <= 0.9 && js::hypot(x - cx, y - cy) <= 0.9;
  };
  // the fronds of a column: its offset from the crown's centre, dr and the height they droop to
  // (want, and the voxel under it); `frond` tests its angle - JS's tests, of the column alone up to
  // the height
  struct Column {
    double dx, dy, dr, want;
  };
  auto column = [&](double x, double y) {
    const double dx = x - fx;
    const double dy = y - fy;
    Column c{dx, dy, js::hypot(dx, dy), js::kNaN};
    if (!(c.dr > t.r)) c.want = js::round(top + 1 - js::pow(c.dr / t.r, 2) * 4);
    return c;
  };
  auto frond = [&](const Column& c) {
    const double ang = js::atan2(c.dy, c.dx) + turn;
    const double f = std::fmod((ang / (2 * kPi)) * nF + nF, 1);
    return js::abs(f - 0.5) < 0.16 + 0.25 / js::max(1, c.dr);
  };
  // eachVoxel's function: the trunk, else the fronds of the column
  auto fn = [&](double x, double y, double z) -> uint16_t {
    if (trunk_at(x, y, z)) return MAT::BARK_PALM;
    if (z > top + 1 || z < top - 4) return 0;
    const Column c = column(x, y);
    if (c.dr > t.r) return 0;
    if (z != c.want && z != c.want - 1) return 0;
    return frond(c) ? MAT::PALM_FROND : 0;
  };
  const IdxRange ri = chunk.range_x(bb.x0, bb.x1);
  const IdxRange rj = chunk.range_y(bb.y0, bb.y1);
  const IdxRange rk = chunk.range_z(bb.z0, bb.z1);
  uint16_t* d = chunk.data.data();
  const double s = chunk.s;
  const double base_x = chunk.bx + chunk.half;
  const double base_y = chunk.by + chunk.half;
  const double base_z = chunk.bz + chunk.half;
  // the trunk: at each height of it, the voxels within 0.9 of its axis (every voxel there tested whole)
  for (int k = rk.lo; k <= rk.hi; ++k) {
    const double z = g.w[2][k];
    if (!(z >= t.z && z <= top)) continue;
    const double kk = (z - t.z) / t.h;
    const double cx = t.x + lean_x * kk * kk * 6;
    const double cy = t.y + lean_y * kk * kk * 6;
    int i0 = ri.lo, i1 = ri.hi, j0 = rj.lo, j1 = rj.hi;
    clip(base_x, s, cx - 1, cx + 1, i0, i1);
    clip(base_y, s, cy - 1, cy + 1, j0, j1);
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i) {
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        const uint16_t m = fn(g.w[0][i], g.w[1][j], z);
        if (m) d[idx] = m;
      }
  }
  // the fronds: in every column within t.r of the crown's centre, the two heights they droop to
  // (a voxel elsewhere in their rows is no frond, nor trunk but within 0.9 of its axis) - where the
  // chunk holds a row at their heights
  bool rows = false;
  for (int k = rk.lo; k <= rk.hi && !rows; ++k) rows = !(g.w[2][k] > top + 1 || g.w[2][k] < top - 4);
  if (!rows) return;
  int i0 = ri.lo, i1 = ri.hi, j0 = rj.lo, j1 = rj.hi;
  clip(base_x, s, fx - t.r - 0.1, fx + t.r + 0.1, i0, i1);
  clip(base_y, s, fy - t.r - 0.1, fy + t.r + 0.1, j0, j1);
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      const double x = g.w[0][i];
      const double y = g.w[1][j];
      const Column c = column(x, y);
      if (!(c.dr <= t.r)) continue;
      int fronds = -1;  // (the column's frond test, made once)
      for (const double z : {c.want - 1, c.want}) {
        if (z > top + 1 || z < top - 4) continue;
        // (the padded index of height z, if it is a representative)
        const double kd = (z - base_z) / s;
        if (!(kd >= rk.lo && kd <= rk.hi) || kd != std::floor(kd)) continue;
        const int k = static_cast<int>(kd);
        const int idx = i + j * kP + k * kP2;
        if (d[idx] != 0) continue;
        // (fn at a height the column's fronds droop to: the trunk, else its fronds)
        uint16_t m = MAT::BARK_PALM;
        if (!trunk_at(x, y, g.w[2][k])) {
          if (fronds < 0) fronds = frond(c) ? 1 : 0;
          m = fronds ? MAT::PALM_FROND : 0;
        }
        if (m) d[idx] = m;
      }
    }
}

}  // namespace

const std::array<TreeKindSpec, kTreeKindCount>& tree_kinds() { return kKinds; }

const TreeKindSpec* tree_kind_spec(std::string_view name) {
  for (const TreeKindSpec& k : kKinds)
    if (k.name == name) return &k;
  return nullptr;
}

const TreeKindSpec& tree_kind_spec(TreeKind kind) {
  if (static_cast<int>(kind) >= kKindCount) SVX_FAIL("trees: no such kind");
  return kKinds[static_cast<size_t>(kind)];
}

TreeKind tree_kind(std::string_view name) {
  const TreeKindSpec* k = tree_kind_spec(name);
  return k ? k->kind : TreeKind::Unknown;
}

std::string_view tree_kind_name(TreeKind kind) { return static_cast<int>(kind) < kKindCount ? kKinds[static_cast<size_t>(kind)].name : std::string_view(); }

bool tree_evergreen(TreeKind k) {
  return k == TreeKind::Pine || k == TreeKind::Spruce || k == TreeKind::Dwarfpine || k == TreeKind::Juniper || k == TreeKind::Jungle || k == TreeKind::Palm ||
         k == TreeKind::Cactus || k == TreeKind::Acacia;
}

TreeModel build_tree_model(const Tree& tree) {
  const TreeIn t = tree_in(tree);
  TreeModel m = build_model(t);
  if (t.wild && tree.reach) {
    for (int k = 1; k <= 7 && reach_of(m, t) > *tree.reach; ++k) {
      TreeIn u = t;
      if (k <= 2) {
        u.amp = 1 - k / 2.0;
      } else {
        u.amp = 0;
        u.r = t.r * js::pow(0.9, k - 2);
      }
      m = build_model(u);
    }
  }
  return m;
}

const TreeModel& tree_model(const Tree& t) {
  return t.model.get([&] { return build_tree_model(t); });
}

Box3 tree_bounds(const Tree& t) {
  if (is_shape(t.kind)) {
    const double r = std::ceil(t.r) + (t.kind == TreeKind::Palm ? 8 : 2);
    return {t.x - r, t.y - r, t.z - 1, t.x + r, t.y + r, t.z + t.h + 2};
  }
  const TreeModel& m = tree_model(t);
  return m.has_bb ? m.bb : Box3{t.x, t.y, t.z, t.x, t.y, t.z};
}

void rasterize_tree(ChunkBuffer& chunk, const Tree& t, double snow) {
  if (is_shape(t.kind)) {
    const Box3 bb = tree_bounds(t);
    if (chunk.touches(bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1)) {
      const Grid& g = grid_of(chunk);
      if (t.kind == TreeKind::Acacia)
        shape_acacia(chunk, t, bb, g);
      else if (t.kind == TreeKind::Cactus)
        shape_cactus(chunk, t, bb, g);
      else
        shape_palm(chunk, t, bb, g);
    }
    return;
  }
  const TreeModel& m = tree_model(t);
  const Box3& bb = m.bb;
  if (!m.has_bb || !chunk.touches(bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1)) return;
  const TreeLook* look = t.look ? &*t.look : nullptr;
  const bool airy = t.kind == TreeKind::Birch || t.kind == TreeKind::Pine || t.kind == TreeKind::Rowan || t.kind == TreeKind::ShrubDry;
  const Ctx ctx = {
      look ? look->snow : snow,
      look ? look->bare : snow > 0.4 && !tree_evergreen(t.kind),
      m.pal,
      look ? look->leaves : nullptr,
      look ? look->mix : 1,
      look ? look->accent : static_cast<uint16_t>(0),
      (airy ? 0.26 : 0.14) + (look ? look->holes : 0),
      Salts(t.seed),
  };
  const Grid& g = grid_of(chunk);
  for (const TreePart& p : m.parts) {
    const Box3& b = p.bb;
    if (!chunk.touches(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1)) continue;
    switch (p.k) {
      case TreePartKind::Blob: raster_blob(chunk, p, ctx, g); break;
      case TreePartKind::Limb: raster_limb(chunk, p, ctx, g); break;
      case TreePartKind::Cone: raster_cone(chunk, p, ctx, g); break;
      case TreePartKind::Curtain: raster_curtain(chunk, p, ctx, g); break;
      case TreePartKind::Fern: raster_fern(chunk, p, ctx, g); break;
      case TreePartKind::Plate: raster_plate(chunk, p, ctx, g); break;
    }
  }
}

}  // namespace svx::city
