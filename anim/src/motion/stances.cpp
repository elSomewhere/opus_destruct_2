#include "svx/anim/motion/stances.hpp"

#include <algorithm>

namespace svx::anim {

namespace {

Quat flat(f64 yaw = 0.0) { return qz(yaw); }

}  // namespace

StanceSample& blend_samples(const StanceSample& a, const StanceSample& b, f64 w, StanceSample& out) {
  out.pelvis_pos = vlerp(a.pelvis_pos, b.pelvis_pos, w);
  out.pelvis_rot = qnlerp(a.pelvis_rot, b.pelvis_rot, w);
  out.spine = qnlerp(a.spine, b.spine, w);
  out.chest = qnlerp(a.chest, b.chest, w);
  out.neck = qnlerp(a.neck, b.neck, w);
  out.head = qnlerp(a.head, b.head, w);
  for (size_t i = 0; i < 2; ++i) {
    const FootPose &fa = a.feet[i], &fb = b.feet[i];
    FootPose& fo = out.feet[i];
    fo.ankle = vlerp(fa.ankle, fb.ankle, w);
    fo.rot = qnlerp(fa.rot, fb.rot, w);
    fo.pole = vlerp(fa.pole, fb.pole, w);
    fo.toe = fa.toe + (fb.toe - fa.toe) * w;
    const std::optional<V3> ha = a.hands[i], hb = b.hands[i];
    out.hand_weight[i] = lerp(ha ? a.hand_weight[i] : 0.0, hb ? b.hand_weight[i] : 0.0, w);
    if (ha && hb) out.hands[i] = vlerp(*ha, *hb, w);
    else out.hands[i] = ha ? ha : hb;
  }
  out.turn = a.turn + (b.turn - a.turn) * w;
  return out;
}

StanceSample& kneel_sample(const Dims& d, StanceSample& out) {
  out.hand_weight = {1.0, 1.0};
  const f64 k = d.k;
  out.pelvis_pos = V3{0.01 * k, -0.03 * k, 0.56 * k};
  out.pelvis_rot = qeuler(-0.05, 0, -0.12);
  out.spine = qeuler(-0.08, 0, 0.05);
  out.chest = qeuler(0.02, 0, 0.05);
  out.neck = qx(0.04);
  out.head = qx(0.02);
  FootPose& l = out.feet[0];
  FootPose& r = out.feet[1];
  l.ankle = V3{-0.14 * k, 0.36 * k, d.ankle_h};
  l.rot = flat(0.15);
  l.pole = V3{-0.1, 1, 0.3};
  l.toe = 0;
  r.ankle = V3{0.15 * k, -0.42 * k, 0.1 * k};
  r.rot = qx(-1.2);
  r.pole = V3{0.1, 0.5, -1};
  r.toe = 1.1;
  out.hands = {V3{-0.13 * k, 0.34 * k, 0.53 * k}, V3{0.14 * k, 0.12 * k, 0.5 * k}};
  out.turn = 0.9;
  return out;
}

StanceSample& prone_sample(const Dims& d, f64 crawl, f64 phase, StanceSample& out) {
  out.hand_weight = {1.0, 1.0};
  const f64 k = d.k;
  const f64 s = sin(phase * kPi * 2.0);
  const f64 c = crawl;
  out.pelvis_pos = V3{0, 0, 0.13 * k};
  // Transfer weight across the shoulders without yawing the whole body across
  // the floor on every pull. This roll is about the prone body's long axis.
  out.pelvis_rot = qx(-kPi / 2.0 + 0.04) * qz(0.05 * s * c);
  out.spine = qeuler(0.1, 0, -0.1 * s * c);
  out.chest = qeuler(0.28, 0, -0.08 * s * c);
  out.neck = qx(0.38);
  out.head = qx(0.32);
  FootPose& l = out.feet[0];
  FootPose& r = out.feet[1];
  const f64 draw_l = std::max(0.0, s) * c, draw_r = std::max(0.0, -s) * c;
  l.ankle = V3{(-0.15 - 0.14 * draw_l) * k, (-0.86 + 0.3 * draw_l) * k, 0.09 * k};
  r.ankle = V3{(0.15 + 0.14 * draw_r) * k, (-0.86 + 0.3 * draw_r) * k, 0.09 * k};
  // The instep rests along the floor behind the shin. A vertical foot here
  // drove its toes through the ground while the ankle tried to hold its height.
  const Quat toes_down = qx(-kPi + .25);
  l.rot = qz(0.3 * draw_l) * toes_down;
  r.rot = qz(-0.3 * draw_r) * toes_down;
  l.pole = V3{-0.8 * draw_l, 0.1, -1};
  r.pole = V3{0.8 * draw_r, 0.1, -1};
  l.toe = 0.5;
  r.toe = 0.5;
  // elbows alternate forward while crawling
  out.hands = {V3{-0.17 * k, (0.6 + 0.12 * s * c) * k, 0.07 * k}, V3{0.17 * k, (0.6 - 0.12 * s * c) * k, 0.07 * k}};
  out.turn = 0.35;
  return out;
}

StanceSample& sit_sample(const Dims& d, const SeatModel& seat, f64 t, StanceSample& out) {
  out.hand_weight = {1.0, 1.0};
  const f64 k = d.k;
  const SitVariant v = seat.variant == SitVariant::Desk && !seat.desk ? SitVariant::Upright : seat.variant;
  const f64 hip_z = seat.pos.z + 0.1 * k;
  const f64 py = seat.pos.y + 0.03 * k;
  out.pelvis_pos = V3{seat.pos.x, py, hip_z + 0.045 * k};
  const f64 tilt = v == SitVariant::LeanBack ? 0.24 : v == SitVariant::ElbowsOnKnees ? -0.12 : v == SitVariant::Desk ? 0.02 : 0.1;
  out.pelvis_rot = qx(tilt);
  const f64 breathe = sin(t * 1.7) * 0.015;
  switch (v) {
    case SitVariant::LeanBack:
      out.spine = qx(0.1 + breathe);
      out.chest = qx(0.06);
      out.neck = qx(-0.18);
      out.head = qx(-0.08);
      break;
    case SitVariant::ElbowsOnKnees:
      out.spine = qx(-0.42 + breathe);
      out.chest = qx(-0.3);
      out.neck = qx(0.32);
      out.head = qx(0.25);
      break;
    case SitVariant::Desk:
      out.spine = qx(-0.2 + breathe);
      out.chest = qx(-0.12);
      out.neck = qx(-0.05);
      out.head = qx(-0.2);
      break;
    default:
      out.spine = qx(-0.06 + breathe);
      out.chest = qx(0.02);
      out.neck = qx(0);
      out.head = qx(0);
  }
  const f64 knee_y = py + 0.4 * k;
  FootPose& l = out.feet[0];
  FootPose& r = out.feet[1];
  const f64 ay = v == SitVariant::LeanBack ? knee_y + 0.12 * k : v == SitVariant::Desk ? knee_y + 0.02 * k : knee_y - 0.04 * k;
  l.ankle = V3{-0.14 * k, ay, d.ankle_h};
  r.ankle = V3{0.14 * k, ay, d.ankle_h};
  l.rot = flat(0.12);
  r.rot = flat(-0.12);
  l.pole = V3{-0.15, 1, 0.3};
  r.pole = V3{0.15, 1, 0.3};
  l.toe = 0;
  r.toe = 0;
  if (v == SitVariant::CrossLegs) {
    // the right leg over the left knee, the foot hanging past it
    r.ankle = V3{-0.1 * k, knee_y + 0.14 * k, hip_z + 0.02 * k};
    r.rot = qz(0.3) * qx(-0.5);
    r.pole = V3{0.25, 1, 1.2};
    l.ankle = V3{-0.1 * k, knee_y - 0.02 * k, d.ankle_h};
  }
  const V3 lap{0, py + 0.2 * k, hip_z + 0.1 * k};
  switch (v) {
    case SitVariant::LeanBack:
      if (seat.backrest) out.hands = {V3{-0.4 * k, py - 0.12 * k, hip_z + 0.3 * k}, V3{0.08 * k, py + 0.24 * k, hip_z + 0.1 * k}};
      else out.hands = {V3{-0.06 * k, lap.y, lap.z}, V3{0.06 * k, lap.y, lap.z}};
      break;
    case SitVariant::ElbowsOnKnees:
      out.hands = {V3{-0.04 * k, knee_y + 0.02 * k, hip_z + 0.06 * k}, V3{0.04 * k, knee_y + 0.04 * k, hip_z + 0.08 * k}};
      break;
    case SitVariant::Desk: {
      const f64 z = (seat.desk ? *seat.desk : hip_z + 0.3) + 0.035 * k;
      // typing / writing
      const f64 a = sin(t * 7.0) * 0.012 * k, b = sin(t * 6.3 + 1.0) * 0.012 * k;
      out.hands = {V3{-0.14 * k, py + 0.46 * k + a, z}, V3{0.14 * k, py + 0.44 * k + b, z}};
      break;
    }
    case SitVariant::CrossLegs:
      out.hands = {V3{-0.06 * k, knee_y - 0.02 * k, hip_z + 0.12 * k}, V3{0.02 * k, knee_y + 0.02 * k, hip_z + 0.14 * k}};
      break;
    default:
      out.hands = {V3{-0.14 * k, py + 0.24 * k, hip_z + 0.08 * k}, V3{0.14 * k, py + 0.24 * k, hip_z + 0.08 * k}};
  }
  out.turn = 0.6;
  return out;
}

StanceSample& ground_sample(const Dims& d, GroundVariant variant, f64 t, StanceSample& out) {
  out.hand_weight = {1.0, 1.0};
  const f64 k = d.k;
  const f64 breathe = sin(t * 1.6) * 0.015;
  FootPose& l = out.feet[0];
  FootPose& r = out.feet[1];
  switch (variant) {
    case GroundVariant::Cross:
      out.pelvis_pos = V3{0, 0, 0.16 * k};
      out.pelvis_rot = qx(0.15);
      out.spine = qx(-0.1 + breathe);
      out.chest = qx(-0.02);
      out.neck = qx(0.05);
      out.head = qx(0);
      l.ankle = V3{0.08 * k, 0.3 * k, 0.07 * k};
      r.ankle = V3{-0.09 * k, 0.36 * k, 0.09 * k};
      l.rot = qz(-1.3) * qeuler(0, -0.6, 0);
      r.rot = qz(1.3) * qeuler(0, 0.6, 0);
      l.pole = V3{-1, 0.25, 0.25};
      r.pole = V3{1, 0.25, 0.25};
      out.hands = {V3{-0.24 * k, 0.28 * k, 0.24 * k}, V3{0.24 * k, 0.28 * k, 0.24 * k}};
      break;
    case GroundVariant::LegsOut:
      out.pelvis_pos = V3{0, 0, 0.14 * k};
      out.pelvis_rot = qx(0.35);
      out.spine = qx(0.1 + breathe);
      out.chest = qx(0.05);
      out.neck = qx(-0.3);
      out.head = qx(-0.1);
      l.ankle = V3{-0.15 * k, 0.78 * k, 0.07 * k};
      r.ankle = V3{0.13 * k, 0.74 * k, 0.07 * k};
      l.rot = qz(0.3) * qx(0.9);
      r.rot = qz(-0.3) * qx(0.9);
      l.pole = V3{-0.2, 0.2, 1};
      r.pole = V3{0.2, 0.2, 1};
      out.hands = {V3{-0.22 * k, -0.28 * k, 0.03 * k}, V3{0.22 * k, -0.28 * k, 0.03 * k}};
      break;
    default:  // knees up, arms round them
      out.pelvis_pos = V3{0, 0, 0.14 * k};
      out.pelvis_rot = qx(0.28);
      out.spine = qx(-0.32 + breathe);
      out.chest = qx(-0.22);
      out.neck = qx(0.25);
      out.head = qx(0.1);
      l.ankle = V3{-0.12 * k, 0.42 * k, d.ankle_h};
      r.ankle = V3{0.12 * k, 0.42 * k, d.ankle_h};
      l.rot = flat(0.1);
      r.rot = flat(-0.1);
      l.pole = V3{-0.2, 0.3, 1};
      r.pole = V3{0.2, 0.3, 1};
      out.hands = {V3{-0.08 * k, 0.4 * k, 0.34 * k}, V3{0.08 * k, 0.4 * k, 0.36 * k}};
  }
  l.toe = 0;
  r.toe = 0;
  out.turn = 0.55;
  return out;
}

StanceSample& down_sample(const Dims& d, bool back, f64 t, StanceSample& out) {
  out.hand_weight = {1.0, 1.0};
  const f64 k = d.k;
  const f64 breathe = sin(t * 2.4) * 0.02;
  FootPose& l = out.feet[0];
  FootPose& r = out.feet[1];
  if (back) {
    out.pelvis_pos = V3{0, 0, 0.11 * k};
    out.pelvis_rot = qx(kPi / 2.0 - 0.06);
    out.spine = qx(-0.04 + breathe);
    out.chest = qx(-0.04);
    out.neck = qx(-0.35);
    out.head = qeuler(-0.15, 0, 0.3);
    l.ankle = V3{-0.16 * k, 0.84 * k, 0.08 * k};
    r.ankle = V3{0.12 * k, 0.5 * k, d.ankle_h};
    l.rot = qz(0.4) * qx(kPi / 2.0 - 0.3);
    r.rot = flat(-0.2);
    l.pole = V3{-0.3, 0.1, 1};
    r.pole = V3{0.3, 0.2, 1};
    out.hands = {V3{-0.5 * k, -0.3 * k, 0.05 * k}, V3{0.45 * k, -0.1 * k, 0.05 * k}};
  } else {
    out.pelvis_pos = V3{0, 0, 0.11 * k};
    out.pelvis_rot = qx(-kPi / 2.0 + 0.03);
    out.spine = qx(0.02 + breathe);
    out.chest = qx(0.06);
    out.neck = qeuler(0.2, 0, 0.9);
    out.head = qeuler(0.05, 0, 0.5);
    l.ankle = V3{-0.2 * k, -0.84 * k, 0.09 * k};
    r.ankle = V3{0.14 * k, -0.8 * k, 0.09 * k};
    l.rot = qx(-kPi / 2.0 - 0.35);
    r.rot = qx(-kPi / 2.0 - 0.35);
    l.pole = V3{-0.2, 0.1, -1};
    r.pole = V3{0.2, 0.1, -1};
    out.hands = {V3{-0.3 * k, 0.52 * k, 0.05 * k}, V3{0.28 * k, -0.05 * k, 0.05 * k}};
  }
  l.toe = 0.3;
  r.toe = 0.3;
  out.turn = 0.15;
  return out;
}

namespace {

// The graph of stance transitions: a transition is a blend of two neighbours.
struct Edges {
  Stance to[3];
  int n;
};
constexpr Edges kEdges[6] = {
    {{Stance::Kneel, Stance::Sit, Stance::Down}, 3},      // stand
    {{Stance::Stand, Stance::Prone, Stance::Ground}, 3},  // kneel
    {{Stance::Kneel, Stance::Down, Stance::Down}, 2},     // prone
    {{Stance::Stand, Stance::Stand, Stance::Stand}, 1},   // sit
    {{Stance::Kneel, Stance::Down, Stance::Down}, 2},     // ground
    {{Stance::Ground, Stance::Ground, Stance::Ground}, 1},  // down
};

}  // namespace

f64 transition_time(Stance from, Stance to) {
  // (a body has weight: dropping to a knee takes most of a second, getting up from the ground
  // longer; only falling is quick)
  using S = Stance;
  if (from == S::Stand && to == S::Kneel) return 0.7;
  if (from == S::Kneel && to == S::Stand) return 0.8;
  if (from == S::Kneel && to == S::Prone) return 1.15;
  if (from == S::Prone && to == S::Kneel) return 1.15;
  if (from == S::Stand && to == S::Sit) return 1.45;
  if (from == S::Sit && to == S::Stand) return 1.3;
  if (from == S::Kneel && to == S::Ground) return 1.0;
  if (from == S::Ground && to == S::Kneel) return 1.1;
  if (from == S::Stand && to == S::Down) return 0.55;
  if (from == S::Prone && to == S::Down) return 0.4;
  if (from == S::Down && to == S::Ground) return 1.2;
  if (from == S::Ground && to == S::Down) return 0.5;
  return 0.8;
}

std::vector<Stance> stance_route(Stance from, Stance to) {
  std::vector<Stance> path;
  if (from == to) return path;
  // falling goes straight down from anywhere
  if (to == Stance::Down) {
    path.push_back(Stance::Down);
    return path;
  }
  // (breadth first through the graph)
  int prev[6] = {-1, -1, -1, -1, -1, -1};
  bool seen[6] = {false, false, false, false, false, false};
  Stance q[6];
  int head = 0, tail = 0;
  q[tail++] = from;
  seen[int(from)] = true;
  while (head < tail) {
    const Stance s = q[head++];
    if (s == to) break;
    const Edges& e = kEdges[int(s)];
    for (int j = 0; j < e.n; ++j) {
      const Stance n = e.to[j];
      if (seen[int(n)]) continue;
      seen[int(n)] = true;
      prev[int(n)] = int(s);
      q[tail++] = n;
    }
  }
  for (int s = int(to); s >= 0 && s != int(from); s = prev[s]) path.insert(path.begin(), Stance(s));
  return path;
}

}  // namespace svx::anim
