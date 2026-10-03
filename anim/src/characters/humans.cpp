#include "svx/anim/characters/humans.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

#include "svx/anim/voxel/sculpt.hpp"

namespace svx::anim {

namespace {

struct Side {
  f64 sx;
  i32 clav, ua, fa, hand, th, sn, ft, toe;
};
constexpr Side kSides[2] = {
    {-1.0, H::clavicleL, H::upperarmL, H::forearmL, H::handL, H::thighL, H::shinL, H::footL, H::toeL},
    {1.0, H::clavicleR, H::upperarmR, H::forearmR, H::handR, H::thighR, H::shinR, H::footR, H::toeR},
};

struct Limb {
  const Side* d = nullptr;
  Shape shoulder, upper, fore, palm, thumb, thigh, shin, foot, toe;
};

// A number as a 32-bit hash seed: modulo 2^32, as the original's hash takes it.
inline i32 wrap_seed(i64 x) { return static_cast<i32>(static_cast<u32>(x)); }

// Face features painted on the head surface (eyes, brows, mouth), one voxel each.
class FaceCells {
 public:
  FaceCells(Sculptor& sc, const Skeleton& skel, f64 k) : sc_(sc), s_(sc.s), k_(k) {
    zc_ = skel.rest_head[H::head].z + 0.1 * k;
    eye_z_ = cell(zc_ + 0.012 * k);
    eye_x_ = cell(0.034 * k);
  }
  void paint_eyes() {
    for (const f64 sx : {-1.0, 1.0}) dot(sx * eye_x_, eye_z_, Slot::Detail);
  }
  void paint(const HumanSpec& spec) {
    paint_eyes();
    // lips: darker skin, two voxels
    const f64 mouth_z = cell(zc_ - 0.05 * k_);
    for (const f64 x : {-0.5, 0.5}) {
      V3 p;
      if (front(x * s_, mouth_z, &p))
        sc_.paint(sphere(p, s_ * 0.45), spec.beard ? Slot::Hair : Slot::Skin, {.bones = {H::head}, .shade = spec.beard ? 0.7 : 0.72, .jitter = 0.0});
    }
  }

 private:
  Sculptor& sc_;
  f64 s_, k_, zc_ = 0.0, eye_z_ = 0.0, eye_x_ = 0.0;
  f64 cell(f64 v) const { return (std::floor(v / s_) + 0.5) * s_; }
  // The front-most solid cell of the head at (x, z).
  bool front(f64 x, f64 z, V3* out) const {
    const i64 i = static_cast<i64>(std::floor(x / s_)) - sc_.lo[0];
    const i64 kk = static_cast<i64>(std::floor(z / s_)) - sc_.lo[2];
    const i64 nx = sc_.dims[0], ny = sc_.dims[1];
    const i64 n = static_cast<i64>(sc_.cells.size());
    for (i64 j = ny - 1; j >= 0; --j) {
      const i64 idx = i + nx * (j + ny * kk);
      if (idx < 0 || idx >= n) continue;  // (off the lattice: nothing there)
      if (sc_.cells[size_t(idx)] != 0 && sc_.bone[size_t(idx)] == H::head) {
        *out = V3{x, (static_cast<f64>(j + sc_.lo[1]) + 0.5) * s_, z};
        return true;
      }
    }
    return false;
  }
  void dot(f64 x, f64 z, u8 slot) {
    V3 p;
    if (!front(x, z, &p)) return;
    sc_.paint(sphere(p, s_ * 0.45), slot, {.bones = {H::head}, .jitter = 0.02});
  }
};

// Number.prototype.toFixed for up to 4 digits: the decimal nearest to x, halves away from zero
// (exact: the original's keys, character for character).
std::string js_to_fixed(f64 x, int digits) {
  if (std::isnan(x)) return "NaN";
  if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
  if (digits < 0 || digits > 4 || std::abs(x) >= 1e15) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits < 0 ? 0 : digits, x);
    return buf;
  }
  std::string out;
  if (x < 0.0) {
    out = "-";
    x = -x;
  }
  int e = 0;
  const f64 fr = std::frexp(x, &e);                    // x = fr 2^e, fr in [0.5, 1)
  const u64 m = static_cast<u64>(std::ldexp(fr, 53));  // x = m 2^(e - 53)
  u64 p5 = 1, p10 = 1;
  for (int i = 0; i < digits; ++i) {
    p5 *= 5;
    p10 *= 10;
  }
  const u64 a = m * p5;  // x 10^digits = a 2^(e - 53 + digits), a < 2^63
  const int sh = e - 53 + digits;
  u64 n = 0;
  if (sh >= 0) n = a << sh;
  else if (sh > -64) n = (a + (u64(1) << (-sh - 1))) >> (-sh);
  out += std::to_string(n / p10);
  if (digits > 0) {
    const std::string f = std::to_string(n % p10);
    out += "." + std::string(size_t(digits) - f.size(), '0') + f;
  }
  return out;
}

}  // namespace

const char* style_name(TopStyle v) {
  switch (v) {
    case TopStyle::Tshirt: return "tshirt";
    case TopStyle::Longsleeve: return "longsleeve";
    case TopStyle::Jacket: return "jacket";
    case TopStyle::Hoodie: return "hoodie";
    case TopStyle::Uniform: return "uniform";
    case TopStyle::Tanktop: return "tanktop";
  }
  return "";
}
const char* style_name(BottomStyle v) {
  switch (v) {
    case BottomStyle::Trousers: return "trousers";
    case BottomStyle::Shorts: return "shorts";
    case BottomStyle::Uniform: return "uniform";
  }
  return "";
}
const char* style_name(ShoeStyle v) {
  switch (v) {
    case ShoeStyle::Shoes: return "shoes";
    case ShoeStyle::Sneakers: return "sneakers";
    case ShoeStyle::Boots: return "boots";
  }
  return "";
}
const char* style_name(HairStyle v) {
  switch (v) {
    case HairStyle::None: return "none";
    case HairStyle::Buzz: return "buzz";
    case HairStyle::Short: return "short";
    case HairStyle::Long: return "long";
    case HairStyle::Ponytail: return "ponytail";
    case HairStyle::Bun: return "bun";
  }
  return "";
}
const char* style_name(HeadGear v) {
  switch (v) {
    case HeadGear::None: return "none";
    case HeadGear::Helmet: return "helmet";
    case HeadGear::HelmetGoggles: return "helmetGoggles";
    case HeadGear::Cap: return "cap";
    case HeadGear::Beanie: return "beanie";
    case HeadGear::Beret: return "beret";
    case HeadGear::Balaclava: return "balaclava";
  }
  return "";
}

ModelPtr sculpt_human(const HumanSpec& spec) {
  const SkeletonPtr skel = humanoid_skeleton(spec.build);
  const Skeleton& sk = *skel;
  const f64 s = spec.voxel_size;
  const f64 k = spec.build.height;
  const f64 g = spec.build.girth;
  const f64 sh = spec.build.shoulders;
  const f64 hp = spec.build.hips;
  Sculptor sc(skel, s, V3{-0.62 * k, -0.45 * k, 0.0}, V3{0.62 * k, 0.5 * k, 1.95 * k}, spec.seed);
  auto hd = [&](i32 b) { return sk.rest_head[size_t(b)]; };
  auto tl = [&](i32 b) { return sk.rest_tail[size_t(b)]; };
  auto at = [&](i32 b, f64 t) { return vlerp(hd(b), tl(b), t); };
  auto off = [](const V3& p, f64 x, f64 y, f64 z) { return V3{p.x + x, p.y + y, p.z + z}; };
  const bool fem = spec.female;
  const V3 up{0, 0, 1}, down{0, 0, -1};
  const AddOptions hard{.organic = false};

  // ---- body shapes (skin), kept to dress them
  const V3 pelvis_c{0.0, -0.012 * k, 0.945 * k};
  const Shape pelvis_s = ellipsoid(pelvis_c, V3{(fem ? 0.16 : 0.15) * hp * g, 0.105 * g, 0.105 * k});
  std::vector<Shape> glutes;
  for (const Side& d : kSides) glutes.push_back(ellipsoid(V3{d.sx * 0.068 * hp, -0.052 * g, 0.9 * k}, V3{0.078 * hp * g, 0.072 * g, 0.088 * k}));
  const Shape crotch = ellipsoid(V3{0.0, -0.005, 0.875 * k}, V3{0.075 * hp * g, 0.07 * g, 0.06 * k});
  const Shape abdomen = ellipsoid(V3{0.0, -0.006 * k, 1.13 * k}, V3{(fem ? 0.125 : 0.14) * g, 0.097 * g, 0.105 * k});
  const Shape chest_s = ellipsoid(V3{0.0, -0.012 * k, 1.315 * k}, V3{0.162 * sh * g, 0.108 * g, 0.14 * k});
  const Shape yoke = capsule(V3{-0.125 * sh, -0.02 * k, 1.41 * k}, V3{0.125 * sh, -0.02 * k, 1.41 * k}, 0.064 * g);
  const Shape neck_s = capsule(off(hd(H::neck), 0, 0, -0.02 * k), tl(H::neck), 0.05 * g * (fem ? 0.9 : 1.0));
  const V3 head_c = off(hd(H::head), 0, 0.012 * k, 0.1 * k);
  const Shape head_s = ellipsoid(head_c, V3{0.082 * k, 0.097 * k, 0.11 * k});
  const Shape jaw_s = ellipsoid(off(hd(H::head), 0, 0.035 * k, 0.052 * k), V3{0.068 * k, 0.07 * k, 0.058 * k});

  const Shape hips_s = smooth_union({pelvis_s, glutes[0], glutes[1], crotch}, 0.04);
  sc.add(hips_s, H::pelvis, Slot::Skin);
  sc.add(abdomen, H::spine, Slot::Skin);
  std::vector<Shape> torso{chest_s, yoke};
  if (fem)
    for (const Side& d : kSides) torso.push_back(ellipsoid(V3{d.sx * 0.058 * sh, 0.075 * g, 1.295 * k}, V3{0.058 * g, 0.048 * g, 0.052 * k}));
  const Shape torso_s = smooth_union(torso, 0.05);
  sc.add(torso_s, H::chest, Slot::Skin);
  sc.add(neck_s, H::neck, Slot::Skin);
  sc.add(head_s, H::head, Slot::Skin);
  sc.add(jaw_s, H::head, Slot::Skin);

  std::vector<Limb> limbs;
  for (const Side& d : kSides) {
    Limb l;
    l.d = &d;
    l.shoulder = sphere(hd(d.ua), 0.058 * g * (fem ? 0.9 : 1.0));
    l.upper = capsule(hd(d.ua), tl(d.ua), 0.05 * g * (fem ? 0.88 : 1.0), 0.042 * g * (fem ? 0.9 : 1.0));
    l.fore = capsule(hd(d.fa), tl(d.fa), 0.042 * g * (fem ? 0.9 : 1.0), 0.032 * g);
    const V3 hand_dir = vnorm(tl(d.hand) - hd(d.hand));
    const Quat hand_q = qfrom_to(V3{0, 0, -1}, hand_dir);
    l.palm = box(at(d.hand, 0.45), V3{0.021 * k, 0.037 * k, 0.07 * k}, 0.012 * k, hand_q);
    l.thumb = capsule(at(d.hand, 0.12), at(d.hand, 0.45) + V3{-d.sx * 0.004, 0.045 * k, 0.0}, 0.014 * k);
    l.thigh = smooth_union({capsule(hd(d.th), tl(d.th), 0.085 * g * hp, 0.058 * g),
                            ellipsoid(at(d.th, 0.38) + V3{0.0, 0.012, 0.0}, V3{0.072 * g, 0.076 * g, 0.15 * k})},
                           0.04);
    l.shin = smooth_union({capsule(hd(d.sn), tl(d.sn), 0.054 * g, 0.038 * g),
                           ellipsoid(at(d.sn, 0.3) + V3{0.0, -0.022, 0.0}, V3{0.05 * g, 0.052 * g, 0.1 * k})},
                          0.04);
    const V3 ank = hd(d.ft);
    l.foot = box(V3{ank.x, 0.03 * k, 0.042 * k}, V3{0.042 * k, 0.088 * k, 0.042 * k}, 0.02 * k);
    l.toe = box(V3{hd(d.toe).x, 0.148 * k, 0.026 * k}, V3{0.04 * k, 0.034 * k, 0.026 * k}, 0.012 * k);
    limbs.push_back(std::move(l));
  }
  for (const Limb& l : limbs) {
    sc.add(l.shoulder, l.d->ua, Slot::Skin);
    sc.add(l.upper, l.d->ua, Slot::Skin);
    sc.add(l.fore, l.d->fa, Slot::Skin);
    sc.add(l.palm, l.d->hand, Slot::Skin);
    sc.add(l.thumb, l.d->hand, Slot::Skin);
    sc.add(l.thigh, l.d->th, Slot::Skin);
    sc.add(l.shin, l.d->sn, Slot::Skin);
    sc.add(l.foot, l.d->ft, Slot::Skin);
    sc.add(l.toe, l.d->toe, Slot::Skin);
  }

  // ---- clothes
  const f64 cloth = 0.011;
  const u8 bottom_slot = Slot::Bottom;
  const f64 shorts_z = 0.62 * k;
  // trousers / shorts: hips, and the legs down to the shoe tops
  sc.add(clip(inflate(hips_s, cloth), V3{0, 0, 0.8 * k}, up), H::pelvis, bottom_slot);
  const f64 shoe_top = spec.shoes == ShoeStyle::Boots ? 0.225 * k : 0.1 * k;
  for (const Limb& l : limbs) {
    const f64 leg_bottom = spec.bottom == BottomStyle::Shorts ? shorts_z : shoe_top - 0.02 * k;
    sc.add(clip(inflate(l.thigh, cloth * (spec.bottom == BottomStyle::Uniform ? 1.6 : 1.1)), V3{0, 0, leg_bottom}, up), l.d->th, bottom_slot);
    if (spec.bottom != BottomStyle::Shorts)
      sc.add(clip(inflate(l.shin, cloth * (spec.bottom == BottomStyle::Uniform ? 1.6 : 1.2)), V3{0, 0, leg_bottom}, up), l.d->sn, bottom_slot);
  }

  // shirts
  const u8 top_slot = Slot::Top;
  const TopStyle top = spec.top;
  const f64 top_bulk = top == TopStyle::Jacket || top == TopStyle::Hoodie ? 0.024 : top == TopStyle::Uniform ? 0.016 : 0.01;
  const f64 waist = 0.985 * k;
  sc.add(clip(inflate(abdomen, top_bulk), V3{0, 0, waist}, up), H::spine, top_slot);
  sc.add(inflate(torso_s, top_bulk), H::chest, top_slot);
  // the shirt hangs over the waistband
  sc.add(clip(clip(inflate(hips_s, top_bulk + 0.004), V3{0, 0, 0.97 * k}, up), V3{0, 0, 1.05 * k}, down), H::pelvis, top_slot);
  for (const Limb& l : limbs) {
    if (top == TopStyle::Tanktop) continue;
    const f64 sleeve_end = top == TopStyle::Tshirt ? 0.55 : 1.0;
    sc.add(inflate(l.shoulder, top_bulk), l.d->ua, top_slot);
    const V3 upper_end = at(l.d->ua, sleeve_end);
    const V3 n = vnorm(tl(l.d->ua) - hd(l.d->ua));
    sc.add(clip(inflate(l.upper, top_bulk * 0.9), upper_end, V3{-n.x, -n.y, -n.z}), l.d->ua, top_slot);
    if (top != TopStyle::Tshirt) {
      const V3 cuff = at(l.d->fa, 0.94);
      const V3 nf = vnorm(tl(l.d->fa) - hd(l.d->fa));
      sc.add(clip(inflate(l.fore, top_bulk * 0.8), cuff, V3{-nf.x, -nf.y, -nf.z}), l.d->fa, top_slot);
    }
  }
  if (top == TopStyle::Jacket) {
    // an open front showing the shirt, and a collar
    sc.paint(box(V3{0, 0.12 * k, 1.2 * k}, V3{0.035 * k, 0.05, 0.22 * k}), Slot::Top2, {.bones = {H::spine, H::chest, H::pelvis}});
    sc.add(clip(shell(inflate(capsule(hd(H::neck), tl(H::neck), 0.05 * g), 0.022), 0.02), V3{0, 0, 1.5 * k}, down), H::chest, top_slot, hard);
  }
  if (top == TopStyle::Hoodie) {
    sc.add(ellipsoid(V3{0, -0.1 * k, 1.47 * k}, V3{0.1 * k, 0.05 * k, 0.05 * k}), H::chest, Slot::Top, hard);
    sc.paint(box(V3{0, 0.12 * k, 1.1 * k}, V3{0.08 * k, 0.03, 0.05 * k}), Slot::Top2, {.bones = {H::spine}});
  }
  if (top == TopStyle::Uniform) {
    // breast pockets and a collar
    for (const Side& d : kSides) sc.add(box(V3{d.sx * 0.075 * sh, 0.1 * g + 0.012, 1.33 * k}, V3{0.035, 0.012, 0.035}), H::chest, top_slot, hard);
    sc.add(clip(shell(inflate(capsule(hd(H::neck), tl(H::neck), 0.05 * g), 0.02), 0.018), V3{0, 0, 1.49 * k}, down), H::chest, top_slot, hard);
  }

  // shoes / boots
  for (const Limb& l : limbs) {
    const u8 slot = Slot::Shoes;
    sc.add(inflate(l.foot, 0.01), l.d->ft, slot);
    sc.add(inflate(l.toe, 0.01), l.d->toe, slot);
    const V3 ft = hd(l.d->ft);
    if (spec.shoes == ShoeStyle::Boots) {
      sc.add(clip(capsule(ft, off(ft, 0, 0.005, 0.15 * k), 0.052 * g), V3{0, 0, shoe_top}, down), l.d->sn, slot);
      sc.paint(box(V3{ft.x, 0.02, shoe_top - 0.012 * k}, V3{0.07, 0.1, 0.013 * k}), Slot::GearDark, {.only = {Slot::Shoes}});
    } else {
      sc.add(clip(capsule(ft, off(ft, 0, 0.01, 0.03 * k), 0.046 * g), V3{0, 0, shoe_top}, down), l.d->sn, slot);
    }
    // soles
    const u8 sole = spec.shoes == ShoeStyle::Sneakers ? Slot::Accent : Slot::GearDark;
    sc.paint(box(V3{ft.x, 0.05, 0.012}, V3{0.08, 0.2, 0.017}), sole, {.only = {Slot::Shoes}});
  }

  // gloves, or skin hands
  if (spec.gloves)
    for (const Limb& l : limbs) sc.paint(inflate(l.palm, 0.02), Slot::GearDark, {.bones = {l.d->hand}});

  // ---- head: face, hair, headgear
  FaceCells face(sc, sk, k);
  // ears
  for (const Side& d : kSides)
    sc.add(ellipsoid(off(head_c, d.sx * 0.08 * k, -0.012 * k, -0.01 * k), V3{0.018 * k, 0.03 * k, 0.038 * k}), H::head, Slot::Skin);
  // nose
  sc.add(box(off(head_c, 0, 0.097 * k, -0.018 * k), V3{s * 0.9, s * 0.6, s * 0.95}), H::head, Slot::Skin, {.shade = 1.04});
  const Shape hair_top = clip(inflate(head_s, 0.016), off(head_c, 0, 0, 0.018 * k), V3{0, 0.55, 1});
  const Shape hair_back = clip(clip(inflate(head_s, 0.014), off(head_c, 0, -0.02 * k, 0), V3{0, -1, 0.25}), off(head_c, 0, 0, -0.07 * k), up);
  switch (spec.hair) {
    case HairStyle::Buzz:
      sc.paint(clip(inflate(head_s, 0.02), off(head_c, 0, 0, 0.03 * k), V3{0, 0.5, 1}), Slot::Hair, {.only = {Slot::Skin}, .bones = {H::head}});
      break;
    case HairStyle::Short:
      sc.add(hair_top, H::head, Slot::Hair, hard);
      sc.add(hair_back, H::head, Slot::Hair, hard);
      break;
    case HairStyle::Long:
      sc.add(hair_top, H::head, Slot::Hair, hard);
      sc.add(hair_back, H::head, Slot::Hair, hard);
      sc.add(clip(ellipsoid(off(head_c, 0, -0.045 * k, -0.07 * k), V3{0.098 * k, 0.07 * k, 0.13 * k}), off(head_c, 0, 0.0, 0), V3{0, -1, 0}), H::head, Slot::Hair,
             hard);
      for (const Side& d : kSides)
        sc.add(box(off(head_c, d.sx * 0.083 * k, 0.0, -0.05 * k), V3{0.016 * k, 0.05 * k, 0.07 * k}, 0.01), H::head, Slot::Hair, hard);
      break;
    case HairStyle::Ponytail:
      sc.add(hair_top, H::head, Slot::Hair, hard);
      sc.add(hair_back, H::head, Slot::Hair, hard);
      sc.add(capsule(off(head_c, 0, -0.1 * k, 0.02 * k), off(head_c, 0, -0.14 * k, -0.12 * k), 0.028 * k, 0.018 * k), H::head, Slot::Hair, hard);
      break;
    case HairStyle::Bun:
      sc.add(hair_top, H::head, Slot::Hair, hard);
      sc.add(hair_back, H::head, Slot::Hair, hard);
      sc.add(sphere(off(head_c, 0, -0.09 * k, 0.06 * k), 0.04 * k), H::head, Slot::Hair, hard);
      break;
    case HairStyle::None:
      break;
  }
  if (spec.beard)
    sc.paint(clip(inflate(jaw_s, 0.02), off(head_c, 0, 0, -0.035 * k), V3{0, 0.15, -1}), Slot::Hair, {.only = {Slot::Skin}, .bones = {H::head}});
  // eyes, brows, mouth (painted on the face surface after the hair)
  face.paint(spec);

  auto helmet_shape = [&](f64 grow) {
    return clip(clip(inflate(head_s, grow), off(head_c, 0, 0.1 * k, 0.012 * k), V3{0, 0.35, 1}), off(head_c, 0, -0.1 * k, -0.045 * k), V3{0, -0.35, 1});
  };
  switch (spec.headgear) {
    case HeadGear::Helmet:
    case HeadGear::HelmetGoggles: {
      const Shape outer = helmet_shape(0.032);
      sc.add(subtract(outer, inflate(head_s, 0.004)), H::head, Slot::Gear, hard);
      // brim lip and cover seams
      sc.paint(clip(clip(inflate(head_s, 0.04), off(head_c, 0, 0, 0.022 * k), down), off(head_c, 0, 0, -0.01 * k), up), Slot::GearDark, {.only = {Slot::Gear}});
      if (spec.headgear == HeadGear::HelmetGoggles) {
        sc.add(box(off(head_c, 0, 0.1 * k, 0.05 * k), V3{0.07 * k, 0.018, 0.02 * k}, 0.008), H::head, Slot::GearDark, hard);
        for (const Side& d : kSides)
          sc.add(box(off(head_c, d.sx * 0.035 * k, 0.118 * k, 0.05 * k), V3{0.022 * k, 0.012, 0.016 * k}), H::head, Slot::Metal, hard);
      }
      // chin strap
      for (const Side& d : kSides)
        sc.paint(box(off(head_c, d.sx * 0.075 * k, 0.02 * k, -0.06 * k), V3{0.012, 0.015, 0.05 * k}), Slot::GearDark, {.bones = {H::head}});
      break;
    }
    case HeadGear::Cap:
      sc.add(subtract(clip(inflate(head_s, 0.02), off(head_c, 0, 0, 0.03 * k), V3{0, 0.2, 1}), inflate(head_s, 0.002)), H::head, Slot::Accent, hard);
      sc.add(box(off(head_c, 0, 0.12 * k, 0.045 * k), V3{0.07 * k, 0.05 * k, 0.008 + s * 0.3}, 0.01, qfrom_to(V3{0, 0, 1}, vnorm(V3{0, -0.15, 1}))), H::head,
             Slot::Accent, {.organic = false, .shade = 0.85});
      break;
    case HeadGear::Beanie:
      sc.add(subtract(clip(inflate(head_s, 0.024), off(head_c, 0, 0, 0.015 * k), V3{0, 0.3, 1}), inflate(head_s, 0.002)), H::head, Slot::Accent, hard);
      sc.paint(clip(clip(inflate(head_s, 0.03), off(head_c, 0, 0, 0.045 * k), V3{0, 0.3, -1}), off(head_c, 0, 0, 0.015 * k), V3{0, 0.3, 1}), Slot::Accent,
               {.shade = 0.8});
      break;
    case HeadGear::Beret:
      sc.add(subtract(clip(ellipsoid(off(head_c, -0.02 * k, -0.005 * k, 0.05 * k), V3{0.1 * k, 0.11 * k, 0.07 * k}), off(head_c, 0, 0, 0.045 * k), V3{0.3, 0.1, 1}),
                      inflate(head_s, 0.002)),
             H::head, Slot::Accent, hard);
      break;
    case HeadGear::Balaclava:
      sc.paint(inflate(head_s, 0.03), Slot::GearDark, {.only = {Slot::Skin, Slot::Hair}, .bones = {H::head}});
      sc.paint(inflate(jaw_s, 0.03), Slot::GearDark, {.only = {Slot::Skin, Slot::Hair, Slot::Accent}, .bones = {H::head}});
      sc.paint(inflate(neck_s, 0.02), Slot::GearDark, {.bones = {H::neck}});
      // the eye slot
      sc.paint(box(off(head_c, 0, 0.1 * k, 0.012 * k), V3{0.06 * k, 0.03, 0.022 * k}), Slot::Skin, {.only = {Slot::GearDark}, .bones = {H::head}});
      face.paint_eyes();
      break;
    case HeadGear::None:
      break;
  }
  if (spec.glasses) sc.add(box(off(head_c, 0, 0.106 * k, 0.012 * k), V3{0.066 * k, s * 0.45, s * 0.5}), H::head, Slot::Detail, hard);

  // ---- gear
  if (spec.belt) {
    const Shape band = clip(clip(inflate(pelvis_s, 0.02), V3{0, 0, 1.0 * k}, up), V3{0, 0, 1.045 * k}, down);
    sc.add(subtract(band, inflate(pelvis_s, 0.0)), H::pelvis, Slot::GearDark, hard);
    sc.add(box(V3{0, 0.115 * g + 0.012, 1.022 * k}, V3{0.022, 0.01, 0.018}), H::pelvis, Slot::Metal, hard);
  }
  if (spec.vest) {
    const V3 vest_c{0, -0.008 * k, 1.28 * k};
    const Shape vest_body = box(vest_c, V3{0.158 * sh * g, 0.128 * g, 0.135 * k}, 0.04 * k);
    sc.add(subtract(vest_body, inflate(chest_s, -0.004)), H::chest, Slot::Gear, hard);
    // the lower part of the vest, over the abdomen, belongs to the spine
    sc.add(clip(subtract(box(V3{0, -0.006 * k, 1.13 * k}, V3{0.145 * g, 0.12 * g, 0.045 * k}, 0.03), inflate(abdomen, -0.004)), V3{0, 0, 1.08 * k}, up), H::spine,
           Slot::Gear, hard);
    // shoulder straps
    for (const Side& d : kSides) sc.add(box(V3{d.sx * 0.095 * sh, -0.01 * k, 1.425 * k}, V3{0.035, 0.1 * g, 0.025}), H::chest, Slot::Gear, hard);
    // reload_point pouches and a radio
    for (const f64 x : {-0.07, 0.0, 0.07}) sc.add(box(V3{x * sh, 0.14 * g + 0.012, 1.215 * k}, V3{0.03, 0.022, 0.048}, 0.008), H::chest, Slot::GearDark, hard);
    sc.add(box(V3{0.1 * sh, 0.13 * g + 0.01, 1.36 * k}, V3{0.022, 0.018, 0.04}, 0.006), H::chest, Slot::GearDark, hard);
    sc.add(box(V3{0.1 * sh, 0.13 * g + 0.012, 1.41 * k}, V3{0.006, 0.006, 0.03}), H::chest, Slot::Metal, hard);
  }
  if (spec.backpack) {
    sc.add(box(V3{0, -0.19 * g, 1.27 * k}, V3{0.12 * sh, 0.065, 0.16 * k}, 0.035), H::chest, Slot::Gear, {.organic = false, .shade = 0.95});
    sc.add(box(V3{0, -0.25 * g, 1.2 * k}, V3{0.09 * sh, 0.02, 0.07 * k}, 0.015), H::chest, Slot::GearDark, hard);
  }
  if (spec.kneepads)
    for (const Limb& l : limbs) sc.add(box(off(hd(l.d->sn), 0, 0.058 * g, -0.01), V3{0.042, 0.018, 0.048}, 0.012), l.d->sn, Slot::GearDark, hard);
  if (spec.vest || spec.belt) {
    // a thigh pouch / holster on the right leg
    sc.add(box(at(H::thighR, 0.35) + V3{0.07 * g, 0.0, 0.0}, V3{0.022, 0.045, 0.06}, 0.01), H::thighR, Slot::GearDark, hard);
  }

  if (spec.camo) {
    const i32 seed = wrap_seed(i64(spec.seed) * 7 + 3), seed1 = wrap_seed(i64(seed) + 1);
    sc.paint_fn([seed, seed1](f64 x, f64 y, f64 z, u8 slot, i32, i32, i32) -> i32 {
      if (slot != Slot::Top && slot != Slot::Bottom && slot != Slot::Top2 && slot != Slot::Bottom2) return -1;
      const f64 n = value_noise(x, y, z, 0.055, seed) * 0.7 + value_noise(x, y, z, 0.022, seed1) * 0.3;
      const bool is_top = slot == Slot::Top || slot == Slot::Top2;
      if (n > 0.6) return is_top ? Slot::Top2 : Slot::Bottom2;
      if (n < 0.3) return is_top ? Slot::Bottom2 : Slot::Top2;
      return is_top ? Slot::Top : Slot::Bottom;
    });
  }

  // ---- joints
  sc.joint(hd(H::spine), 0.1 * g, {H::pelvis, H::spine});
  sc.joint(hd(H::chest), 0.1 * g, {H::spine, H::chest});
  sc.joint(hd(H::neck), 0.055 * g, {H::chest, H::neck});
  sc.joint(hd(H::head), 0.05 * g, {H::neck, H::head});
  for (const Side& d : kSides) {
    sc.joint(hd(d.ua), 0.058 * g, {H::chest, d.ua});
    sc.joint(hd(d.fa), 0.046 * g, {d.ua, d.fa});
    sc.joint(hd(d.hand), 0.034 * g, {d.fa, d.hand});
    sc.joint(hd(d.th), 0.08 * g, {H::pelvis, d.th});
    sc.joint(hd(d.sn), 0.058 * g, {d.th, d.sn});
    sc.joint(hd(d.ft), 0.048 * g, {d.sn, d.ft});
    sc.joint(hd(d.toe), 0.03 * g, {d.ft, d.toe});
  }
  return sc.finish({.name = spec.name});
}

// ---- presets

std::string geometry_key(const HumanSpec& s) {
  const HumanoidBuild& b = s.build;
  std::string k = s.female ? "f" : "m";
  for (const f64 v : {b.height, b.shoulders, b.hips, b.girth}) k += ":" + js_to_fixed(v, 2);
  for (const char* name : {style_name(s.top), style_name(s.bottom), style_name(s.shoes), style_name(s.hair), style_name(s.headgear)}) k += std::string(":") + name;
  for (const bool f : {s.beard, s.glasses, s.vest, s.backpack, s.belt, s.gloves, s.kneepads, s.camo}) k += f ? ":1" : ":0";
  k += ":" + std::to_string(s.camo ? s.seed : 0);
  k += ":" + js_to_fixed(s.voxel_size, 4);
  return k;
}

HumanSpec soldier_spec(i32 seed, const HumanOptions& opts) {
  Rng r(static_cast<f64>(seed) * 2654435761.0 + 17.0);
  HumanSpec sp;
  const bool heavy = r.chance(0.3);
  sp.build.height = r.range(0.97, 1.05);
  sp.build.shoulders = r.range(1.0, 1.08);
  sp.build.hips = 1.0;
  sp.build.girth = heavy ? 1.1 : r.range(0.98, 1.04);
  sp.female = false;
  sp.top = TopStyle::Uniform;
  sp.bottom = BottomStyle::Uniform;
  sp.shoes = ShoeStyle::Boots;
  static constexpr HairStyle kHair[] = {HairStyle::Buzz, HairStyle::Short, HairStyle::None};
  sp.hair = r.pick(kHair);
  static constexpr HeadGear kGear[] = {HeadGear::Helmet, HeadGear::HelmetGoggles, HeadGear::Helmet, HeadGear::Balaclava, HeadGear::Beret};
  sp.headgear = r.pick(kGear);
  sp.beard = r.chance(0.2);
  sp.glasses = false;
  sp.vest = r.chance(0.85);
  sp.backpack = r.chance(0.4);
  sp.belt = true;
  sp.gloves = r.chance(0.7);
  sp.kneepads = r.chance(0.6);
  sp.camo = true;
  sp.voxel_size = opts.voxel_size;
  sp.seed = seed & 3;
  sp.name = "soldier-" + std::to_string(seed);
  return sp;
}

Palette soldier_palette(i32 seed, std::optional<i32> scheme) {
  Rng r(static_cast<f64>(seed) * 40503.0 + 5.0);
  const i32 si = scheme ? *scheme : r.integer(0, static_cast<i32>(kCamoSchemes.size()) - 1);
  // (no such scheme: black, as in the original)
  const CamoScheme c = si >= 0 && si < static_cast<i32>(kCamoSchemes.size()) ? kCamoSchemes[size_t(si)] : CamoScheme{0, 0, 0, 0, 0, 0, 0};
  PaletteSpec p;
  p.skin = r.pick(kSkinTones);
  p.hair = r.pick(kHairColours);
  p.top = c.top;
  p.top2 = c.top2;
  p.bottom = c.bottom;
  p.bottom2 = c.bottom2;
  p.gear = c.gear;
  p.gear_dark = c.gear_dark;
  p.shoes = c.shoes;
  p.metal = 0x2a2c2e;
  p.furniture = 0x1e1f21;
  p.detail = 0x121212;
  p.accent = r.chance(0.5) ? 0x7a2a26 : 0x2b3b52;
  return make_palette(p);
}

HumanSpec civilian_spec(i32 seed, const HumanOptions& opts) {
  Rng r(static_cast<f64>(seed) * 2246822519.0 + 3.0);
  HumanSpec sp;
  const bool female = r.chance(0.5);
  const bool heavy = r.chance(0.2);
  if (female) {
    sp.build.height = r.range(0.9, 0.98);
    sp.build.shoulders = r.range(0.86, 0.92);
    sp.build.hips = r.range(1.02, 1.08);
    sp.build.girth = heavy ? 1.1 : r.range(0.92, 1.0);
  } else {
    sp.build.height = r.range(0.95, 1.04);
    sp.build.shoulders = r.range(0.96, 1.04);
    sp.build.hips = 1.0;
    sp.build.girth = heavy ? 1.14 : r.range(0.95, 1.03);
  }
  sp.female = female;
  const TopStyle tops[] = {TopStyle::Tshirt, TopStyle::Tshirt, TopStyle::Longsleeve, TopStyle::Jacket, TopStyle::Hoodie, female ? TopStyle::Tanktop : TopStyle::Tshirt};
  sp.top = r.pick(tops);
  sp.bottom = r.chance(0.25) ? BottomStyle::Shorts : BottomStyle::Trousers;
  static constexpr ShoeStyle kShoes[] = {ShoeStyle::Shoes, ShoeStyle::Sneakers, ShoeStyle::Sneakers};
  sp.shoes = r.pick(kShoes);
  if (female) {
    static constexpr HairStyle kHair[] = {HairStyle::Long, HairStyle::Ponytail, HairStyle::Bun, HairStyle::Short};
    sp.hair = r.pick(kHair);
  } else {
    static constexpr HairStyle kHair[] = {HairStyle::Short, HairStyle::Short, HairStyle::Buzz, HairStyle::None, HairStyle::Long};
    sp.hair = r.pick(kHair);
  }
  if (r.chance(0.2)) {
    static constexpr HeadGear kGear[] = {HeadGear::Cap, HeadGear::Beanie};
    sp.headgear = r.pick(kGear);
  } else {
    sp.headgear = HeadGear::None;
  }
  sp.beard = !female && r.chance(0.3);
  sp.glasses = r.chance(0.2);
  sp.vest = false;
  sp.backpack = r.chance(0.2);
  sp.belt = r.chance(0.4);
  sp.gloves = false;
  sp.kneepads = false;
  sp.camo = false;
  sp.voxel_size = opts.voxel_size;
  sp.seed = seed;
  sp.name = "civilian-" + std::to_string(seed);
  return sp;
}

Palette civilian_palette(i32 seed) {
  Rng r(static_cast<f64>(seed) * 69069.0 + 11.0);
  PaletteSpec p;
  const u32 top = r.pick(kClothColours);
  p.skin = r.pick(kSkinTones);
  p.hair = r.pick(kHairColours);
  p.top = top;
  p.top2 = r.pick(kClothColours);
  p.bottom = r.pick(kTrouserColours);
  p.bottom2 = r.pick(kTrouserColours);
  p.shoes = r.pick(kShoeColours);
  static constexpr u32 kGear[] = {0x3b3f46, 0x6b4f2a, 0x1f3a5f, 0x5a2d2d};
  p.gear = r.pick(kGear);
  p.gear_dark = 0x222326;
  p.metal = 0x9a9a9a;
  p.furniture = 0x3a2a1c;
  p.detail = 0x15120f;
  static constexpr u32 kAccent[] = {0x8a3b35, 0xa04848, 0x7a3030, 0xd8d8d8, 0x2a2a2a, 0x3060a0};
  p.accent = r.pick(kAccent);
  return make_palette(p);
}

HumanSpec thug_spec(i32 seed, const HumanOptions& opts) {
  Rng r(static_cast<f64>(seed) * 2654435761.0 + 71.0);
  HumanSpec sp;
  const bool female = r.chance(0.15);
  if (female) {
    sp.build.height = r.range(0.94, 1.0);
    sp.build.shoulders = r.range(0.92, 0.98);
    sp.build.hips = r.range(1.0, 1.05);
    sp.build.girth = r.range(0.98, 1.06);
  } else {
    sp.build.height = r.range(0.98, 1.06);
    sp.build.shoulders = r.range(1.03, 1.1);
    sp.build.hips = 1.0;
    sp.build.girth = r.range(1.0, 1.12);
  }
  sp.female = female;
  static constexpr TopStyle kTops[] = {TopStyle::Hoodie, TopStyle::Hoodie, TopStyle::Jacket, TopStyle::Jacket, TopStyle::Tanktop, TopStyle::Tshirt};
  sp.top = r.pick(kTops);
  sp.bottom = BottomStyle::Trousers;
  static constexpr ShoeStyle kShoes[] = {ShoeStyle::Sneakers, ShoeStyle::Sneakers, ShoeStyle::Boots};
  sp.shoes = r.pick(kShoes);
  if (female) {
    static constexpr HairStyle kHair[] = {HairStyle::Ponytail, HairStyle::Short, HairStyle::Bun};
    sp.hair = r.pick(kHair);
  } else {
    static constexpr HairStyle kHair[] = {HairStyle::Buzz, HairStyle::Buzz, HairStyle::None, HairStyle::Short};
    sp.hair = r.pick(kHair);
  }
  static constexpr HeadGear kGear[] = {HeadGear::Beanie, HeadGear::Beanie, HeadGear::Cap, HeadGear::Balaclava, HeadGear::None, HeadGear::None};
  sp.headgear = r.pick(kGear);
  sp.beard = !female && r.chance(0.5);
  sp.glasses = false;
  sp.vest = false;
  sp.backpack = false;
  sp.belt = r.chance(0.6);
  sp.gloves = r.chance(0.45);
  sp.kneepads = false;
  sp.camo = false;
  sp.voxel_size = opts.voxel_size;
  sp.seed = seed;
  sp.name = "thug-" + std::to_string(seed);
  return sp;
}

Palette thug_palette(i32 seed) {
  Rng r(static_cast<f64>(seed) * 40503.0 + 29.0);
  static constexpr u32 kDark[] = {0x1c1c1f, 0x2a2b2e, 0x33352f, 0x3d2a2a, 0x252c3a, 0x3b3b3b, 0x4a4032, 0x2f3a2c};
  static constexpr u32 kTop2[] = {0x8a1f1f, 0xb0b0b0, 0x1f1f1f, 0xc9a227, 0x3a6ea5};
  static constexpr u32 kBottom[] = {0x1d2430, 0x2a2e36, 0x39342c, 0x202020, 0x3a4a5c};
  static constexpr u32 kBottom2[] = {0x1d2430, 0x2a2e36};
  static constexpr u32 kShoes[] = {0xe8e8e8, 0x1a1a1a, 0x3a2a1c, 0xb03030};
  static constexpr u32 kGear[] = {0x1f1f1f, 0x2b2b2b, 0x4a3b2a};
  static constexpr u32 kAccent[] = {0xc9a227, 0x8a1f1f, 0x2a2a2a, 0xd8d8d8};
  PaletteSpec p;
  p.skin = r.pick(kSkinTones);
  p.hair = r.pick(kHairColours);
  p.top = r.pick(kDark);
  p.top2 = r.pick(kTop2);
  p.bottom = r.pick(kBottom);
  p.bottom2 = r.pick(kBottom2);
  p.shoes = r.pick(kShoes);
  p.gear = r.pick(kGear);
  p.gear_dark = 0x151515;
  p.metal = 0xb8b8b8;
  p.furniture = 0x3a2a1c;
  p.detail = 0x121212;
  p.accent = r.pick(kAccent);
  return make_palette(p);
}

HumanVariant make_thug(i32 seed, const HumanOptions& opts) {
  HumanSpec spec = thug_spec(seed, opts);
  ModelPtr model = sculpt_human(spec);
  return HumanVariant{std::move(model), thug_palette(seed), std::move(spec)};
}

HumanVariant make_soldier(i32 seed, const HumanOptions& opts) {
  HumanSpec spec = soldier_spec(seed, opts);
  ModelPtr model = sculpt_human(spec);
  return HumanVariant{std::move(model), soldier_palette(seed, opts.scheme), std::move(spec)};
}

HumanVariant make_civilian(i32 seed, const HumanOptions& opts) {
  HumanSpec spec = civilian_spec(seed, opts);
  ModelPtr model = sculpt_human(spec);
  return HumanVariant{std::move(model), civilian_palette(seed), std::move(spec)};
}

}  // namespace svx::anim
