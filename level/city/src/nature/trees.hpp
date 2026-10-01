// svx_city — procedural trees and small plants (voxel_city nature/trees.js), point-sampled so
// they render at every LOD.
//
//   Tree { x, y, z (the first voxel above ground), h (height), r (crown radius), kind, seed,
//          look (world/season tree_look) }
//
// Every tree is built once from its seed into a model (tree_model): a trunk that tapers, flares
// at the foot and leans a little, limbs forking off it with twigs at their ends, and foliage as a
// handful of clusters (ellipsoids with a lumpy, gappy surface) at the ends of the limbs. Species
// differ in structure:
//
//   oak        low fork, few long spreading limbs, a broad irregular crown
//   maple      (beech, ash; alias "autumn") a tall oval crown on steep limbs
//   street     (lime, plane) a regular oval crown on a clean trunk
//   birch      slender white stems (often two or three), small airy clusters
//   rowan      a few thin stems, an open crown, berries late in the year
//   blossom    (cherry, apple) a low fork, flat spreading crown
//   willow     a leaning trunk, a dome with hanging curtains of leaves
//   poplar     a tall narrow column
//   pine       (Scots pine) a long bare trunk, orange at the top, flat clumps of needles on top
//   spruce     a dense narrow cone of drooping whorls, irregular branch lobes
//   dwarfpine  low sprawling mounds (krummholz); juniper a small dark spire
//   shrub      a few overlapping clumps; fern, berry, log, stump, snag
//   hazel      a clump of thin stems fanning out, a leafy dome on top
//   aspen      a straight pale stem, a small rounded crown high up
//   alder      a dark egg-shaped crown, often two stems (by the water)
//   larch      an airy cone of soft needles in tiers of tufts, gold in autumn, bare in winter
//   acacia, palm, cactus: shapes of their own (no model), jungle
//
// Wild trees (Tree::wild: the organic vegetation of the angled world) grow less regularly: trunks
// lean further and sweep, forks sit higher or lower, crowns are lopsided towards the light,
// broadleaves now and then carry twin leaders, conifers lean and grow lopsided cones, snags lean
// and fallen logs follow the ground or rest on their root plate. Every new direction and tilt is an
// exact rotation of the yaw table (core/placement), never a new sin or cos. Tree::amp (0..1,
// default 1) scales the lean and lopsidedness; Tree::reach (voxels) is the emitter's search
// margin, which the tree keeps within (tree_model: less lean, then a smaller crown).
//
// Foliage is shaded by exposure; the seasonal look replaces the palette on some or all clusters,
// strips broadleaves to their limbs and twigs, and lays snow on upper surfaces. Up close the shell
// is lumpy with gaps; coarse LODs draw smooth, slightly inflated clusters.
//
// Threads: a Tree is immutable once its emitter has made it (its model is made once, on first use,
// by whichever thread asks: Lazy); rasterize_tree writes only the chunk it is given.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "core/cache.hpp"
#include "core/placement.hpp"
#include "voxel/chunk.hpp"
#include "world/season.hpp"

namespace svx::city {

// The kinds of TREE_KINDS, in its key order; Unknown is any other kind (JS: a kind string the
// tables do not know: no model, the oak's palette).
enum class TreeKind : uint8_t {
  Oak,
  Maple,
  Autumn,
  Street,
  Birch,
  Rowan,
  Blossom,
  Willow,
  Poplar,
  Pine,
  Spruce,
  Dwarfpine,
  Juniper,
  Acacia,
  Jungle,
  Palm,
  Cactus,
  Shrub,
  Hazel,
  Aspen,
  Alder,
  Larch,
  ShrubDry,
  Fern,
  Berry,
  Log,
  Stump,
  Snag,
  Unknown,
};

// A kind's size ranges (metres): height and crown radius (TREE_KINDS[kind]).
struct TreeKindSpec {
  std::string_view name;
  TreeKind kind;
  std::array<double, 2> h;
  std::array<double, 2> r;
};

// TREE_KINDS, in its key order.
constexpr int kTreeKindCount = 28;
const std::array<TreeKindSpec, kTreeKindCount>& tree_kinds();
// TREE_KINDS[name], or null (callers fall back as JS does: TREE_KINDS[kind] ?? TREE_KINDS.oak).
const TreeKindSpec* tree_kind_spec(std::string_view name);
const TreeKindSpec& tree_kind_spec(TreeKind kind);  // (not Unknown)
// The kind of a name (Unknown for any name TREE_KINDS does not hold), and back ("" for Unknown).
TreeKind tree_kind(std::string_view name);
std::string_view tree_kind_name(TreeKind kind);
// EVERGREEN: kinds that keep their needles (the season only lays snow on them). (world/season's
// own list, for its looks, holds the dead wood and dry shrubs too.)
bool tree_evergreen(TreeKind kind);

// Parts of a model.
enum class TreePartKind : uint8_t { Blob = 0, Limb = 1, Cone = 2, Curtain = 3, Fern = 4, Plate = 5 };

// A foliage cluster: an ellipsoid (radius rx across, rz up) with a lumpy, gappy surface; mix
// decides which palette it takes in a turning season.
struct TreeBlob {
  double x, y, z, rx, rz, mix;
};
// A tapered segment from a to a + d (radius r0 to r1): a trunk, limb or twig.
struct TreeLimb {
  double ax, ay, az, dx, dy, dz, L2, r0, r1;
  double foot;  // (has_foot: where a bowed birch stem's dark foot is measured from)
};
// A conifer cone (spruce, juniper): whorls every `tier` voxels, `lobes` irregular branch lobes; a
// wild one leans with its trunk (the axis sheared by lx, ly from zf over hh) and is lopsided.
struct TreeCone {
  double x, y, z0, z1, z_live, r, tier, lobes, phase;
  double lx, ly, zf, hh, ax, ay, asym;  // (lean)
};
// Willow curtains hanging from a cluster's underside.
struct TreeCurtain {
  double x, y, z, rx, rz, drop, mix;
};
// A fern: n fronds arching out from the crown.
struct TreeFern {
  double x, y, z, r, h, n, phase;
};
// A windthrow's root plate: a ragged disc (normal n, radius r, half thickness th).
struct TreePlate {
  double x, y, z, nx, ny, nz, r, th;
};

struct TreePart {
  TreePartKind k = TreePartKind::Blob;
  // (limbs) trunk: part of a stem; twig: drawn bare only, up close; birch: white bark with dark
  // marks and foot; moss: a log's mossy back; stump: a cut top. (cones) lean: a wild cone.
  bool trunk = false, twig = false, birch = false, moss = false, stump = false, has_foot = false, lean = false;
  uint16_t mat = 0;  // (limbs) bark
  Box3 bb;           // the voxels it may write (inclusive)
  union {
    TreeBlob blob;
    TreeLimb limb;
    TreeCone cone;
    TreeCurtain curtain;
    TreeFern fern;
    TreePlate plate;
  };
  TreePart() : blob{} {}
};

// A tree's model: its parts (foliage first: limbs only show where the leaves leave a gap), their
// bounds (none without parts) and its summer palette [shadow, body, sunlit].
struct TreeModel {
  std::vector<TreePart> parts;
  bool has_bb = false;
  Box3 bb;
  const LeafPalette* pal = nullptr;
};

// A tree record. Its emitter sets the fields, then bb = tree_bounds(t); nothing changes it after.
struct Tree {
  double x = 0, y = 0, z = 0;  // z: the first voxel above ground
  double h = 0;                // height (voxels)
  double r = 0;                // crown radius (voxels; a log's length)
  TreeKind kind = TreeKind::Oak;
  double seed = 0;             // (a uint32 hash; any number is taken as JS takes it)
  std::optional<TreeLook> look;  // the seasonal look (none: summer's)
  bool open = false;           // open-grown (pines and spruces keep lower branches, larches tiers)
  bool under = false;          // (the forest's understory: carried for its emitters, unread here)
  bool wild = false;           // the angled world's organic growth
  std::optional<double> amp;   // (wild) lean and lopsidedness, 0..1 (none: 1)
  std::optional<double> reach; // (wild) the emitter's search margin the tree keeps within
  // (wild logs) their lie: a yaw-table index, a tilt along the ground (s > 0 where the +u end
  // lies higher), windthrown on a root plate; each drawn from the seed when not given
  std::optional<int> yaw;
  std::optional<Yaw> tilt;
  std::optional<bool> plate;
  Box3 bb;  // (t.bb: tree_bounds, set by the emitter)

  // (the model, made on first use: tree_model)
  Lazy<TreeModel> model;
};

// The tree's model (treeModel), built once from its seed. A wild tree keeps within the reach its
// emitter allows (Tree::reach): if it would not, it leans and lops less, then its crown (a log: its
// length) shrinks - a pure function of the tree still.
const TreeModel& tree_model(const Tree& t);
// The same, made afresh (not kept on the tree).
TreeModel build_tree_model(const Tree& t);
// The voxels a tree may write (treeBounds): its model's bounds, a box round the shapes of their
// own (acacia, cactus, palm), its foot for a tree without parts.
Box3 tree_bounds(const Tree& t);
// Rasterize a tree into a chunk (rasterizeTree; only into air). Its look chooses the palette, bare
// branches and snow; without one, `snow` (0..1) is the snow cover (a snowy crown sheds its leaves).
void rasterize_tree(ChunkBuffer& chunk, const Tree& t, double snow = 0);

}  // namespace svx::city
