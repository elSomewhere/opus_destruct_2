#include "svx/anim/body/humanoid.hpp"

#include <algorithm>

namespace svx::anim {

const std::array<i32, kBodyCount> kBodyBone = {H::pelvis, H::spine,  H::chest,  H::head,   H::upperarmL, H::forearmL, H::handL, H::upperarmR,
                                               H::forearmR, H::handR, H::thighL, H::shinL, H::footL,     H::thighR,   H::shinR, H::footR};

const std::array<i32, kBodyCount> kBodyParent = {-1,        B::pelvis,   B::spine,  B::chest,  B::chest,  B::upperarmL, B::forearmL, B::chest,
                                                 B::upperarmR, B::forearmR, B::pelvis, B::thighL, B::shinL, B::pelvis,    B::thighR,   B::shinR};

const std::array<Region, kBodyCount> kRegion = {Region::Trunk, Region::Trunk, Region::Trunk, Region::Neck, Region::ArmL, Region::ArmL,
                                                Region::ArmL,  Region::ArmR,  Region::ArmR,  Region::ArmR, Region::LegL, Region::LegL,
                                                Region::LegL,  Region::LegR,  Region::LegR,  Region::LegR};

namespace {

// Segment mass fractions (Winter): head+neck, trunk thirds, arm, leg segments.
constexpr f64 kMass[kBodyCount] = {0.142, 0.139, 0.216, 0.081, 0.028, 0.016, 0.006, 0.028, 0.016, 0.006, 0.1, 0.0465, 0.0145, 0.1, 0.0465, 0.0145};

// Joint muscle frequency (rad/s) and damping ratio by child body.
constexpr f64 kMuscle[kBodyCount][2] = {
    {0, 0},
    {15, 0.95},  // lower back
    {15, 0.95},  // upper back
    {13, 1.0},   // neck
    // The shoulder yields to momentum; the distal joints actively carry the hand.
    // Giving the wrist the shoulder's slow response made it trail every turn and
    // oscillate through action releases. Tone still controls injury and ragdoll.
    {16, 1.05},  // shoulder
    {19, 1.05},  // elbow
    {28, 1.15},  // wrist
    {16, 1.05},
    {19, 1.05},
    {28, 1.15},
    {17, 0.95},  // hip
    {18, 0.95},  // knee
    {16, 0.95},  // ankle
    {17, 0.95},
    {18, 0.95},
    {16, 0.95},
};

// Where a foot's sole rests relative to its ankle (rest model space, per unit height).
constexpr f64 kSoleDrop = 0.085;

f64 clamp_v(f64 v, f64 max = 25.0) { return v > max ? max : v < -max ? -max : v; }

}  // namespace

HumanoidBody::HumanoidBody(SkeletonPtr sk, const CollisionWorld* collision, const HumanoidBodyOptions& o)
    : skeleton(std::move(sk)), system(collision), extras_(skeleton) {
  const std::vector<V3>& rh = skeleton->rest_head;
  const std::vector<V3>& rt = skeleton->rest_tail;
  k = rh[H::pelvis].z / 0.97;
  const f64 girth = o.girth;
  const f64 total = o.mass > 0.0 ? o.mass : 75.0 * k * k * k * girth;
  total_mass = total;
  system.max_substep = 1.0 / 480.0;
  system.margin = 0.02 * k;
  tone.fill(1.0f);
  hold_weight.fill(1.0f);
  auto mid = [](const V3& a, const V3& b, f64 t) { return vlerp(a, b, t); };

  // ---- bodies: centre of mass, inertia, spheres (rest model space) ----
  std::array<V3, kBodyCount> coms{}, inert{};
  std::array<std::vector<Sphere>, kBodyCount> sph;
  auto rod = [](f64 m, const V3& a, const V3& b, f64 r) {
    // a solid rod's inertia about its centre, the diagonal in the model frame
    const V3 d = b - a;
    const f64 L = hypot3(d.x, d.y, d.z);
    const V3 u = vnorm(d);
    const f64 perp = (m * (3.0 * r * r + L * L)) / 12.0;
    const f64 along = 0.5 * m * r * r;
    return V3{perp + (along - perp) * u.x * u.x, perp + (along - perp) * u.y * u.y, perp + (along - perp) * u.z * u.z};
  };
  auto box = [](f64 m, f64 w, f64 d, f64 h) { return V3{(m * (d * d + h * h)) / 12.0, (m * (w * w + h * h)) / 12.0, (m * (w * w + d * d)) / 12.0}; };
  struct At {
    V3 at;
    f64 r;
  };
  const f64 g = girth;
  for (i32 i = 0; i < kBodyCount; ++i) {
    const f64 m = kMass[i] * total;
    V3 com, I;
    std::vector<At> spheres;
    switch (i) {
      case B::pelvis: {
        com = V3{0, 0, rh[H::pelvis].z + 0.01 * k};
        I = box(m, 0.32 * k * g, 0.2 * k * g, 0.2 * k);
        spheres = {{V3{rh[H::thighL].x * 0.8, 0, rh[H::thighL].z + 0.03 * k}, 0.1 * k * g}, {V3{rh[H::thighR].x * 0.8, 0, rh[H::thighR].z + 0.03 * k}, 0.1 * k * g}};
        break;
      }
      case B::spine: {
        com = mid(rh[H::spine], rh[H::chest], 0.5);
        I = box(m, 0.3 * k * g, 0.19 * k * g, 0.15 * k);
        spheres = {{V3{0, 0, com.z}, 0.115 * k * g}};
        break;
      }
      case B::chest: {
        com = mid(rh[H::chest], rh[H::neck], 0.45);
        I = box(m, 0.36 * k * g, 0.21 * k * g, 0.25 * k);
        spheres = {{V3{0, 0.005 * k, rh[H::chest].z + 0.07 * k}, 0.125 * k * g},
                   {V3{rh[H::upperarmL].x * 0.55, -0.01 * k, rh[H::upperarmL].z - 0.03 * k}, 0.085 * k * g},
                   {V3{rh[H::upperarmR].x * 0.55, -0.01 * k, rh[H::upperarmR].z - 0.03 * k}, 0.085 * k * g}};
        break;
      }
      case B::head: {
        com = mid(rh[H::neck], rt[H::head], 0.6);
        I = box(m, 0.17 * k, 0.2 * k, 0.24 * k);
        spheres = {{V3{0, 0.02 * k, rh[H::head].z + 0.1 * k}, 0.1 * k}};
        break;
      }
      default: {
        const i32 bone = kBodyBone[size_t(i)];
        const V3 a = rh[size_t(bone)];
        const bool is_foot = i == B::footL || i == B::footR;
        const bool is_hand = i == B::handL || i == B::handR;
        const V3 end = is_foot ? rt[size_t(bone == H::footL ? H::toeL : H::toeR)] : rt[size_t(bone)];
        if (is_foot) {
          const f64 x = a.x;
          com = V3{x, (a.y + end.y) * 0.5 - 0.01 * k, 0.045 * k};
          I = box(m, 0.09 * k, 0.24 * k, 0.08 * k);
          spheres = {{V3{x, a.y - 0.035 * k, 0.04 * k}, 0.04 * k}, {V3{x, end.y - 0.075 * k, 0.034 * k}, 0.034 * k}, {V3{x, end.y - 0.02 * k, 0.022 * k}, 0.022 * k}};
        } else if (is_hand) {
          com = mid(a, end, 0.4);
          I = rod(m, a, end, 0.04 * k);
          spheres = {{mid(a, end, 0.45), 0.045 * k}};
        } else {
          const bool upper = i == B::upperarmL || i == B::upperarmR || i == B::thighL || i == B::thighR;
          const bool leg = i >= B::thighL;
          const f64 r = (leg ? (upper ? 0.075 : 0.052) : upper ? 0.048 : 0.042) * k * g;
          com = mid(a, end, 0.43);
          I = rod(m, a, end, r);
          spheres = {{mid(a, end, upper ? 0.3 : 0.25), r}, {mid(a, end, upper ? 0.72 : 0.7), r * (leg ? 0.85 : 0.92)}};
        }
      }
    }
    coms[size_t(i)] = com;
    inert[size_t(i)] = I;
    for (const At& s : spheres) sph[size_t(i)].push_back(Sphere{s.at - com, s.r});
  }
  for (i32 i = 0; i < kBodyCount; ++i) {
    auto body = std::make_unique<RigidBody>(kMass[i] * total, inert[size_t(i)], coms[size_t(i)], Quat{}, sph[size_t(i)]);
    body->friction = i == B::footL || i == B::footR ? 1.0 : 0.75;
    if (i >= B::upperarmL) {
      // limbs resist spinning about their length
      const i32 bone = kBodyBone[size_t(i)];
      const V3 tip = i == B::footL || i == B::footR ? rh[size_t(bone == H::footL ? H::toeL : H::toeR)] : rt[size_t(bone)];
      body->long_axis = vnorm(tip - rh[size_t(bone)]);
      body->twist_damping = 25.0;
    }
    parts[size_t(i)] = system.add(std::move(body));
    com_local[size_t(i)] = coms[size_t(i)] - rh[size_t(kBodyBone[size_t(i)])];
  }

  // ---- joints ----
  const V3 fwd{0, 1, 0};
  const V3 up{0, 0, 1};
  const f64 D = kPi / 180.0;
  for (i32 i = 1; i < kBodyCount; ++i) {
    const i32 p = kBodyParent[size_t(i)];
    const i32 bone = kBodyBone[size_t(i)];
    // the joint: at the child bone's head (the head body turns at the neck's base)
    const V3 at = i == B::head ? rh[H::neck] : rh[size_t(bone)];
    const V3 tail = i == B::head ? rt[H::head] : i == B::footL ? rh[H::toeL] : i == B::footR ? rh[H::toeR] : rt[size_t(bone)];
    const bool left = rh[size_t(bone)].x < 0.0;
    const f64 side = left ? -1.0 : 1.0;
    V3 z = vnorm(tail - at);
    JointOptions jo;
    // anatomical swing limits for a frame whose y is forward and x = y x z
    struct Anat {
      f64 fwd, back, out, in;
    };
    auto anat = [&](const V3& x_axis, const Anat& a) {
      // +x of the frame is outward if it points to the body's side
      const bool out_pos = x_axis.x * side > 0.0;
      SwingLimits s;
      s.y_pos = a.fwd * D;
      s.y_neg = a.back * D;
      s.x_pos = (out_pos ? a.out : a.in) * D;
      s.x_neg = (out_pos ? a.in : a.out) * D;
      return s;
    };
    auto swing_of = [&](f64 y_pos, f64 y_neg, f64 x_pos, f64 x_neg) {
      SwingLimits s;
      s.y_pos = y_pos * D;
      s.y_neg = y_neg * D;
      s.x_pos = x_pos * D;
      s.x_neg = x_neg * D;
      return s;
    };
    struct Frame {
      Quat q;
      V3 x;
    };
    auto frame_of = [](const V3& z_axis, const V3& y_hint) {
      const V3 x = vnorm(cross(y_hint, z_axis));
      const V3 y = cross(z_axis, x);
      return Frame{qfrom_basis(x, y, z_axis), x};
    };
    Quat frame;
    switch (i) {
      case B::spine:
      case B::chest:
      case B::head: {
        z = up;
        frame = frame_of(up, fwd).q;
        if (i == B::head) {
          jo.swing = swing_of(55, 50, 40, 40);
          jo.twist = std::make_pair(-70 * D, 70 * D);
        } else {
          jo.swing = i == B::spine ? swing_of(45, 25, 28, 28) : swing_of(35, 25, 22, 22);
          jo.twist = i == B::spine ? std::make_pair(-25 * D, 25 * D) : std::make_pair(-30 * D, 30 * D);
        }
        break;
      }
      case B::upperarmL:
      case B::upperarmR: {
        const Frame f = frame_of(z, fwd);
        frame = f.q;
        jo.swing = anat(f.x, Anat{160, 55, 150, 45});
        jo.twist = std::make_pair(-85 * D, 85 * D);
        break;
      }
      case B::forearmL:
      case B::forearmR: {
        // a hinge: the forearm swings forward (positive about x)
        jo.kind = JointKind::Hinge;
        const V3 upper = rh[size_t(bone)] - rh[size_t(kBodyBone[size_t(p)])];
        const V3 x = vnorm(cross(upper, fwd));
        const V3 zz = vnorm(z - x * (x.x * z.x + x.y * z.y + x.z * z.z));
        frame = qfrom_basis(x, cross(zz, x), zz);
        jo.hinge = std::make_pair(-3 * D, 150 * D);
        break;
      }
      case B::handL:
      case B::handR: {
        frame = frame_of(z, fwd).q;
        jo.swing = swing_of(70, 70, 35, 35);
        jo.twist = std::make_pair(-80 * D, 80 * D);
        break;
      }
      case B::thighL:
      case B::thighR: {
        const Frame f = frame_of(z, fwd);
        frame = f.q;
        jo.swing = anat(f.x, Anat{125, 40, 55, 30});
        jo.twist = std::make_pair(-45 * D, 45 * D);
        break;
      }
      case B::shinL:
      case B::shinR: {
        // a hinge: the shin swings back (negative about +x)
        jo.kind = JointKind::Hinge;
        const V3 x{1, 0, 0};
        const V3 zz = vnorm(z - x * z.x);
        frame = qfrom_basis(x, cross(zz, x), zz);
        jo.hinge = std::make_pair(-150 * D, 3 * D);
        break;
      }
      default: {
        // ankle: z along the foot, x across it
        const V3 x{1, 0, 0};
        const V3 zz = vnorm(z - x * z.x);
        frame = qfrom_basis(x, cross(zz, x), zz);
        // +y of this frame points down and back: toes down is a tilt towards +y
        jo.swing = swing_of(50, 30, 28, 28);
        jo.twist = std::make_pair(-30 * D, 30 * D);
      }
    }
    jo.anchor_a = at - coms[size_t(p)];
    jo.anchor_b = at - coms[size_t(i)];
    jo.frame_a = frame;
    jo.frame_b = frame;
    joints[size_t(i)] = system.add_joint(std::make_unique<Joint>(parts[size_t(p)], parts[size_t(i)], jo));
  }
  // muscle stiffness from the inertia each joint moves
  for (i32 i = 1; i < kBodyCount; ++i) {
    const V3 jp = joint_point(i);
    f64 I = 0.0;
    for (i32 c : subtree(i)) {
      const V3 d = coms[size_t(c)] - jp;
      const V3& ii = inert[size_t(c)];
      I += kMass[c] * total * (d.x * d.x + d.y * d.y + d.z * d.z) + (ii.x + ii.y + ii.z) / 3.0;
    }
    const f64 w = kMuscle[i][0], zeta = kMuscle[i][1];
    inertia_at[size_t(i)] = static_cast<f32>(I);
    base_stiffness[size_t(i)] = static_cast<f32>(I * w * w);
    base_damping[size_t(i)] = static_cast<f32>(2.0 * zeta * w * I);
    joints[size_t(i)]->eff_inertia = I;
  }
  // bodies that must not pass through each other
  auto pair = [&](i32 a, i32 b) {
    RigidBody* A = parts[size_t(a)];
    RigidBody* Bb = parts[size_t(b)];
    for (size_t sa = 0; sa < A->spheres.size(); ++sa)
      for (size_t sb = 0; sb < Bb->spheres.size(); ++sb) system.pairs.push_back(SpherePair{A, static_cast<i32>(sa), Bb, static_cast<i32>(sb)});
  };
  // (not the hands, nor the forearms and the head: a hand goes to the face, over the head, to a
  // wound, and pressing against its own body there would shove it about)
  for (i32 arm : {B::forearmL, B::forearmR, B::upperarmL, B::upperarmR}) {
    for (i32 t : {B::pelvis, B::spine, B::chest, B::head}) {
      if ((arm == B::upperarmL || arm == B::upperarmR) && t == B::chest) continue;
      if ((arm == B::forearmL || arm == B::forearmR) && t == B::head) continue;
      pair(arm, t);
    }
  }
  pair(B::forearmL, B::forearmR);
  for (i32 a : {B::thighL, B::shinL, B::footL})
    for (i32 b : {B::thighR, B::shinR, B::footR}) pair(a, b);
  for (i32 a : {B::forearmL, B::forearmR})
    for (i32 b : {B::thighL, B::thighR, B::shinL, B::shinR}) pair(a, b);

  // ---- assists ----
  RigidBody* pel = parts[B::pelvis];
  const V3 pelvis_head = rh[H::pelvis] - coms[B::pelvis];
  support = system.attach(pel, pelvis_head);
  support->axes[0] = false;
  support->axes[1] = false;
  steer = system.attach(pel, pelvis_head);
  steer->axes[2] = false;
  upright = system.orienter(pel);
  chest_turn = system.orienter(parts[B::chest]);
  auto foot_local = [&](i32 b) { return rh[size_t(kBodyBone[size_t(b)])] - coms[size_t(b)]; };
  feet = {system.attach(parts[B::footL], foot_local(B::footL)), system.attach(parts[B::footR], foot_local(B::footR))};
  feet_turn = {system.orienter(parts[B::footL]), system.orienter(parts[B::footR])};
  hands = {system.attach(parts[B::handL], foot_local(B::handL)), system.attach(parts[B::handR], foot_local(B::handR))};
  hands_turn = {system.orienter(parts[B::handL]), system.orienter(parts[B::handR])};
}

std::vector<i32> HumanoidBody::subtree(i32 i) const {
  std::vector<i32> out{i};
  for (i32 c = i + 1; c < kBodyCount; ++c)
    if (std::find(out.begin(), out.end(), kBodyParent[size_t(c)]) != out.end()) out.push_back(c);
  return out;
}

V3 HumanoidBody::joint_point(i32 i) const {
  const std::vector<V3>& rh = skeleton->rest_head;
  return i == B::head ? rh[H::neck] : rh[size_t(kBodyBone[size_t(i)])];
}

f64 HumanoidBody::sole_drop() const { return kSoleDrop * k; }

// ---- pose <-> bodies -------------------------------------------------------------------------

void HumanoidBody::set_from_pose(const WorldPose& world, const WorldPose* prev, f64 dt) {
  for (i32 i = 0; i < kBodyCount; ++i) {
    RigidBody& b = *parts[size_t(i)];
    const i32 bone = kBodyBone[size_t(i)];
    const V3 x = body_at(world, i);
    b.x = x;
    const Quat q = world.q[size_t(bone)];
    b.q = q;
    if (prev && dt > 0.0) {
      const V3 xp = body_at(*prev, i);
      b.v = V3{clamp_v((x.x - xp.x) / dt), clamp_v((x.y - xp.y) / dt), clamp_v((x.z - xp.z) / dt)};
      const V3 e = qerror(q, prev->q[size_t(bone)]);
      b.w = V3{clamp_v(e.x / dt, 30.0), clamp_v(e.y / dt, 30.0), clamp_v(e.z / dt, 30.0)};
    } else {
      b.v = V3{};
      b.w = V3{};
    }
  }
  system.wake();
}

V3 HumanoidBody::body_at(const WorldPose& world, i32 i) const {
  const i32 bone = kBodyBone[size_t(i)];
  return rotate(world.q[size_t(bone)], com_local[size_t(i)]) + world.p[size_t(bone)];
}

void HumanoidBody::track(const WorldPose& target, const Pose& local, const WorldPose* prev, f64 dt, f64 max_rate) {
  const std::vector<Quat>& q = target.q;
  for (i32 i = 1; i < kBodyCount; ++i) {
    Joint& j = *joints[size_t(i)];
    const size_t pb = size_t(kBodyBone[size_t(kBodyParent[size_t(i)])]), cb = size_t(kBodyBone[size_t(i)]);
    const Quat& pa = q[pb];
    const Quat& ch = q[cb];
    if (prev && dt > 0.0) {
      // the relative rotation's rate, in the parent's frame
      const Quat prev_rel = max_rate < 30 ? j.target : conj(prev->q[pb]) * prev->q[cb];
      j.target = conj(pa) * ch;
      if (max_rate < 30) {
        const f64 angle = norm(qerror(j.target, prev_rel));
        if (angle > max_rate * dt) j.target = qslerp(prev_rel, j.target, max_rate * dt / angle);
      }
      // R = qA^-1 qB turns at w (in A's frame) when R(t + dt) = exp(w dt) R(t)
      const V3 d = qerror(j.target, prev_rel);
      const f64 vx = clamp_v(d.x / dt, 30.0), vy = clamp_v(d.y / dt, 30.0), vz = clamp_v(d.z / dt, 30.0);
      // the plan's angular acceleration at the joint (smoothed: frame differences are noisy)
      V3& acc = target_acc_[size_t(i)];
      const f64 a = 1.0 - exp(-dt * 25.0);
      acc.x += (clamp_v((vx - j.target_vel.x) / dt, 200.0) - acc.x) * a;
      acc.y += (clamp_v((vy - j.target_vel.y) / dt, 200.0) - acc.y) * a;
      acc.z += (clamp_v((vz - j.target_vel.z) / dt, 200.0) - acc.z) * a;
      j.target_vel = V3{vx, vy, vz};
    } else {
      j.target = conj(pa) * ch;
      j.target_vel = V3{};
      target_acc_[size_t(i)] = V3{};
    }
  }
  // shoulders move with the clavicles
  const Quat chest_q = q[H::chest];
  const V3 chest_p = target.p[H::chest];
  for (const auto& [bi, bone] : {std::pair<i32, i32>{B::upperarmL, H::upperarmL}, std::pair<i32, i32>{B::upperarmR, H::upperarmR}}) {
    Joint& j = *joints[size_t(bi)];
    const V3 d = rotate(conj(chest_q), target.p[size_t(bone)] - chest_p);
    const V3& com = com_local[B::chest];
    // anchor = (head offset in the chest's rest frame) - (com - chest head); it follows the
    // clavicle smoothly (a shrug moving the joint itself in one frame would snap the arm)
    const f64 f = prev && dt > 0.0 ? 1.0 - exp(-dt * 18.0) : 1.0;
    j.anchor_a.x += (d.x - com.x - j.anchor_a.x) * f;
    j.anchor_a.y += (d.y - com.y - j.anchor_a.y) * f;
    j.anchor_a.z += (d.z - com.z - j.anchor_a.z) * f;
  }
  extras_.copy_from(local);
}

void HumanoidBody::refresh_mass() {
  total_mass = 0;
  for (const auto* b : parts) total_mass += b->mass;
  for (i32 i = 1; i < kBodyCount; ++i) {
    f64 I = 0;
    const V3 jp = joint_point(i);
    for (i32 c : subtree(i)) {
      const auto& b = *parts[size_t(c)];
      const V3 d = skeleton->rest_head[size_t(kBodyBone[size_t(c)])] + com_local[size_t(c)] - jp;
      I += b.mass * dot(d, d) + (1 / b.inv_i.x + 1 / b.inv_i.y + 1 / b.inv_i.z) / 3;
    }
    const f64 w = kMuscle[i][0], zeta = kMuscle[i][1];
    inertia_at[size_t(i)] = f32(I);
    base_stiffness[size_t(i)] = f32(I * w * w);
    base_damping[size_t(i)] = f32(2 * zeta * w * I);
    joints[size_t(i)]->eff_inertia = I;
  }
}

void HumanoidBody::apply_tone() {
  for (i32 i = 1; i < kBodyCount; ++i) {
    Joint& j = *joints[size_t(i)];
    const f64 t = std::max(0.0, static_cast<f64>(tone[size_t(i)]));
    j.stiffness = base_stiffness[size_t(i)] * t;
    // a tense joint is damped more; a limp one heavily by its tissue (a dead limb swings and
    // settles, it does not flap)
    j.damping = base_damping[size_t(i)] * (0.5 + 0.5 * std::min(1.5, t) + 0.5 * std::max(0.0, 1.0 - t));
  }
}

void HumanoidBody::compensate_gravity() {
  const f64 g = system.gravity;
  // subtree mass and weighted centre, children before parents
  f64 m[kBodyCount], cx[kBodyCount], cy[kBodyCount], cz[kBodyCount];
  for (i32 i = kBodyCount - 1; i >= 0; --i) {
    const RigidBody& b = *parts[size_t(i)];
    m[i] = b.mass;
    cx[i] = b.x.x * b.mass;
    cy[i] = b.x.y * b.mass;
    cz[i] = b.x.z * b.mass;
  }
  for (i32 i = kBodyCount - 1; i >= 1; --i) {
    const i32 p = kBodyParent[size_t(i)];
    m[p] += m[i];
    cx[p] += cx[i];
    cy[p] += cy[i];
    cz[p] += cz[i];
  }
  for (i32 i = 1; i < kBodyCount; ++i) {
    Joint& j = *joints[size_t(i)];
    const f64 t = std::min(1.0, std::max(0.0, static_cast<f64>(tone[size_t(i)])));
    const f64 hw = hold_weight[size_t(i)] * t;
    j.feed = V3{};
    // the torque that swings the limb along the plan: I alpha, in the world
    const f64 ff = feed_forward * t * inertia_at[size_t(i)];
    if (ff > 0.0) j.feed = rotate(j.a->q, target_acc_[size_t(i)]) * ff;
    if (hw <= 0.0) continue;
    const V3 jp = parts[size_t(i)]->point(j.anchor_b);
    const f64 mm = m[i];
    const f64 rx = cx[i] / mm - jp.x, ry = cy[i] / mm - jp.y;
    // torque on B that cancels gravity's: r x (0, 0, m g)
    const f64 f = mm * g * hw;
    j.feed.x += ry * f;
    j.feed.y += -rx * f;
  }
}

void HumanoidBody::write_pose(WorldPose& out) const {
  const std::vector<V3>& rh = skeleton->rest_head;
  for (i32 i = 0; i < kBodyCount; ++i) {
    const RigidBody& b = *parts[size_t(i)];
    const size_t bone = size_t(kBodyBone[size_t(i)]);
    out.q[bone] = b.q;
    out.p[bone] = b.x - rotate(b.q, com_local[size_t(i)]);
  }
  // root: under the pelvis
  const Quat pq = out.q[H::pelvis];
  out.p[H::root] = out.p[H::pelvis] - rotate(pq, rh[H::pelvis]);
  out.q[H::root] = pq;
  // neck: from the chest, turned part of the way to the head
  const Quat cq = out.q[H::chest];
  const Quat neck_q = qnlerp(cq, out.q[H::head], 0.45);
  out.q[H::neck] = neck_q;
  out.p[H::neck] = out.p[H::chest] + rotate(cq, rh[H::neck] - rh[H::chest]);
  // the head hangs from the physical neck base (keeps it on the neck)
  out.p[H::head] = out.p[H::neck] + rotate(neck_q, rh[H::head] - rh[H::neck]);
  // clavicles: the plan's local rotation on the chest
  for (i32 c : {H::clavicleL, H::clavicleR}) {
    out.q[size_t(c)] = cq * extras_.r[size_t(c)];
    out.p[size_t(c)] = out.p[H::chest] + rotate(cq, rh[size_t(c)] - rh[H::chest]);
  }
  // toes on the feet
  for (const auto& [t, f] : {std::pair<i32, i32>{H::toeL, H::footL}, std::pair<i32, i32>{H::toeR, H::footR}}) {
    const Quat fq = out.q[size_t(f)];
    out.q[size_t(t)] = fq * extras_.r[size_t(t)];
    out.p[size_t(t)] = out.p[size_t(f)] + rotate(fq, rh[size_t(t)] - rh[size_t(f)]);
  }
}

// ---- sensing ---------------------------------------------------------------------------------

V3 HumanoidBody::com() const {
  f64 x = 0.0, y = 0.0, z = 0.0;
  for (const RigidBody* b : parts) {
    x += b->x.x * b->mass;
    y += b->x.y * b->mass;
    z += b->x.z * b->mass;
  }
  return V3{x / total_mass, y / total_mass, z / total_mass};
}

V3 HumanoidBody::com_velocity() const {
  f64 x = 0.0, y = 0.0, z = 0.0;
  for (const RigidBody* b : parts) {
    x += b->v.x * b->mass;
    y += b->v.y * b->mass;
    z += b->v.z * b->mass;
  }
  return V3{x / total_mass, y / total_mass, z / total_mass};
}

void HumanoidBody::shove(const V3& dv, const f64* share) {
  for (i32 i = 0; i < kBodyCount; ++i) {
    const f64 s = share ? share[i] : 1.0;
    RigidBody& b = *parts[size_t(i)];
    b.v.x += dv.x * s;
    b.v.y += dv.y * s;
    b.v.z += dv.z * s;
  }
  system.wake();
}

void HumanoidBody::spheres_of(const WorldPose* pose, std::vector<Obstacle>& out, i32 owner) const {
  for (i32 i = 0; i < kBodyCount; ++i) {
    const RigidBody& b = *parts[size_t(i)];
    if (b.ghost || b.gone) continue;
    Quat q = b.q;
    V3 com = b.x;
    if (pose) {
      q = pose->q[size_t(kBodyBone[size_t(i)])];
      com = body_at(*pose, i);
    }
    for (const Sphere& s : b.spheres) {
      Obstacle o;
      o.c = com + rotate(q, s.c);
      o.r = s.r;
      o.owner = owner;
      o.part = i;
      o.v = b.v;
      out.push_back(o);
    }
  }
}

i32 HumanoidBody::nearest_part(const V3& p) const {
  i32 best = B::chest;
  f64 bd = kInf;
  for (i32 i = 0; i < kBodyCount; ++i) {
    const RigidBody& b = *parts[size_t(i)];
    for (const Sphere& s : b.spheres) {
      const V3 c = b.point(s.c);
      const f64 d = hypot3(c.x - p.x, c.y - p.y, c.z - p.z) - s.r;
      if (d < bd) {
        bd = d;
        best = i;
      }
    }
  }
  return best;
}

i32 HumanoidBody::body_of_bone(i32 bone) {
  for (i32 i = 0; i < kBodyCount; ++i)
    if (kBodyBone[size_t(i)] == bone) return i;
  if (bone == H::neck) return B::head;
  if (bone == H::clavicleL || bone == H::clavicleR) return B::chest;
  if (bone == H::toeL) return B::footL;
  if (bone == H::toeR) return B::footR;
  return B::pelvis;
}

i64 HumanoidBody::memory_bytes() const {
  return static_cast<i64>(sizeof(*this)) + system.memory_bytes() - static_cast<i64>(sizeof(RigidSystem)) +
         static_cast<i64>((extras_.t.capacity() + 0) * sizeof(V3) + extras_.r.capacity() * sizeof(Quat));
}

}  // namespace svx::anim
