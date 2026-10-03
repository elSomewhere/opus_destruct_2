#include "svx/anim/damage/state.hpp"
#include "svx/anim/body/humanoid.hpp"
namespace svx::anim {
namespace {
f64 clot_of(const PersistentWound& w) { return w.arterial ? std::max(.65, exp(-w.age / 600)) : std::max(.08, exp(-w.age / 90)); }
}  // namespace
void DamageState::configure(const CharacterProfile& p) {
  injuries_.cap = std::max(1, p.max_injuries);
  max_wounds_ = std::max(1, p.max_wounds);
  merge_distance_ = p.wound_merge_distance;
  stain_work_ = std::max<i64>(1, p.stain_work);
  fracture_limb_ = p.fracture_limb;
  fracture_slender_ = p.fracture_slender;
  fracture_trunk_ = p.fracture_trunk;
  shatter_limb_ = p.shatter_limb;
  shatter_trunk_ = p.shatter_trunk;
}
void DamageState::add_wound(const PersistentWound& w) {
  // A wound beside one already there on the same part, or one too many, joins it: the flow they
  // have now is kept (clotting is memoryless while it runs), the worse pain and opened vessel too.
  const bool full = i32(state_.wounds.size()) >= max_wounds_;
  PersistentWound* near = nullptr;
  f64 best = kInf;
  for (auto& o : state_.wounds)
    if (o.part == w.part && (o.arterial == w.arterial || full)) {
      const f64 d = vdist(o.rest, w.rest);
      if (d < best) {
        best = d;
        near = &o;
      }
    }
  if (near && (best < merge_distance_ || full)) {
    near->bleeding = near->bleeding * clot_of(*near) + w.bleeding;
    near->age = 0;
    near->arterial = near->arterial || w.arterial;
    near->pain = std::max(near->pain, w.pain);
    return;
  }
  state_.wounds.push_back(w);
}
const char* mobility_name(Mobility m) {
  constexpr const char* names[] = {"walk", "limp", "hobble", "kneel", "crawl", "immobile"};
  return size_t(m) < 6 ? names[size_t(m)] : "immobile";
}
void derive_mobility(Capabilities& c) {
  const f64 worst = std::min(c.legs[0].support, c.legs[1].support), best = std::max(c.legs[0].support, c.legs[1].support);
  const f64 left = c.arms[0].strength, right = c.arms[1].strength;
  c.crawl = left > .25 && right > .25 ? CrawlStyle::BothArms : left > .25 ? CrawlStyle::LeftArm : right > .25 ? CrawlStyle::RightArm : CrawlStyle::Scoot;
  const bool can_move = std::max({left, right, best}) > .12 && c.trunk > .08;
  c.mobility = c.fatal || c.consciousness < .15 || c.vigor < .08 || !can_move ? Mobility::Immobile
               : best < .22                                                   ? Mobility::Crawl
               : best < .4                                                    ? Mobility::Kneel
               : worst < .3                                                   ? Mobility::Hobble
               : worst < .97                                                  ? Mobility::Limp
                                                                              : Mobility::Walk;
  constexpr f64 speed[] = {8, 1.65, .65, .25, .32, 0};
  const f64 crawl_drive = c.crawl == CrawlStyle::Scoot ? best * .6 : std::max(left, right);
  c.max_speed = speed[size_t(c.mobility)] * c.vigor * (c.mobility == Mobility::Crawl ? crawl_drive : 1);
}
void derive_muscles(Capabilities& c) {
  for (i32 part = 0; part < kBodyCount; ++part)
    c.muscle[size_t(part)] = part >= B::thighR      ? c.legs[1].support
                             : part >= B::thighL    ? c.legs[0].support
                             : part >= B::upperarmR ? c.arms[1].strength
                             : part >= B::upperarmL ? c.arms[0].strength
                             : part == B::head      ? c.neck
                                                    : c.trunk;
}
void DamageState::reaction(i32 part, const V3& local, const V3& normal, HitKind kind, f64 force) {
  if (part < 0 || part >= 16 || force <= 0) return;
  ++revision_;
  Injury i;
  i.part = part;
  i.local = local;
  i.normal = normal;
  i.zone = zone_of_part(part);
  i.kind = kind;
  i.severity = clamp(.25 + .35 * force, 0.0, 1.0);
  i.lasting = i.severity * .45;
  i.hold_until = 1.6 + 2.6 * i.severity + (i.zone == Zone::Gut || i.zone == Zone::Chest ? 2 : 0);
  injuries_.add(i);
}
void DamageState::old_wound(i32 part, f64 severity) {
  ++revision_;
  Injury i;
  i.part = part;
  i.zone = zone_of_part(part);
  i.kind = HitKind::Bullet;
  i.severity = severity;
  i.lasting = severity;
  i.age = 30;
  i.hold_until = 0;
  injuries_.add(i);
}
void DamageState::lost(i32 part) {
  if (part >= 0 && part < 16) {
    state_.parts[size_t(part)].lost = true;
    state_.parts[size_t(part)].nerve = 0;
    if (part == 3) state_.cause = DeathCause::HighSpine;
    physical_wounds_ = true;
  }
  old_wound(part, 1);
}
void DamageState::update(f64 dt, i32 pressed_part) {
  injuries_.update(dt);
  const auto& i = injuries_;
  cap_.legs[0] = {1 - .45 * i.legL, 1 - .45 * i.legL, 1 - i.legL};
  cap_.legs[1] = {1 - .45 * i.legR, 1 - .45 * i.legR, 1 - i.legR};
  cap_.arms[0] = {1 - i.armL, 1 - i.armL, 1 - .65 * i.armL};
  cap_.arms[1] = {1 - i.armR, 1 - i.armR, 1 - .65 * i.armR};
  cap_.trunk = 1 - i.trunk;
  cap_.neck = 1 - i.head;
  cap_.pain = i.pain;
  cap_.max_speed = 8 * (1 - .5 * std::max(i.legL, i.legR)) * (1 - .3 * i.pain);
  if (physical_wounds_) {
    for (auto& p : state_.parts) p.bleeding = 0;
    f64 bleed = 0;
    for (auto& w : state_.wounds) {
      w.age += dt;
      const f64 rate = w.bleeding * clot_of(w) * (w.part == pressed_part ? .45 : 1);
      bleed += rate;
      state_.parts[size_t(w.part)].bleeding += rate;
    }
    state_.blood = std::max(0.0, state_.blood - bleed * dt);
    state_.adrenaline = std::max(0.0, state_.adrenaline - dt / 35);
    state_.shock = clamp(state_.shock + std::max(0.0, 3.5 - state_.blood) * dt * .012 - dt * .001, 0.0, 1.0);
    state_.consciousness = clamp((state_.blood - 1.7) / 1.8 * (1 - state_.shock * .75) * state_.breathing, 0.0, 1.0);
    if (state_.blood < 1.5) state_.cause = DeathCause::BloodLoss;
    physical_capabilities();
  }
  if (cap_.fatal) {
    cap_.consciousness = 0;
    cap_.max_speed = 0;
    cap_.mobility = Mobility::Immobile;
  }
  cap_.care.reset();
  if (const auto* w = injuries_.to_hold()) cap_.care = CareTarget{w->part, w->local, w->normal, w->age, w->hold_until, w->severity};
  if (physical_wounds_)
    for (const auto& w : state_.wounds) {
      const f64 clot = clot_of(w);
      // Care urgency is dimensionless on both paths. Active bleeding outranks a
      // transient pain reflex, and the worst open vessel gets the available
      // hand.
      const f64 urgency = w.bleeding > 0 ? 1 + w.bleeding * clot / .04 : w.pain;
      if (urgency > 0 && (!cap_.care || urgency > cap_.care->urgency)) cap_.care = CareTarget{w.part, w.rest, w.normal, w.age, 1e9, urgency, true};
    }
}
void DamageState::apply(const VoxelModel& model, const WorldPose& pose, const DamageDescriptor& d, const WoundMechanics& result) {
  ++revision_;
  const auto regions = anatomy_regions(*model.skeleton);
  std::array<f64, 16> energy{}, removed{}, bone_energy{};
  std::array<V3, 16> rest{};
  std::vector<f64> vital_energy(regions.size()), vital_removed(regions.size());
  for (const auto& t : result.tissue) {
    if (t.energy <= 0) continue;
    const size_t part = size_t(HumanoidBody::body_of_bone(t.bone));
    if (energy[part] == 0) rest[part] = t.rest;  // care and bleeding begin at the wound's entry
    energy[part] += t.energy;
    removed[part] += t.removed;
    if (t.bone_hit) bone_energy[part] += t.energy;
    for (size_t i = 0; i < regions.size(); ++i) {
      const auto& v = regions[i];
      if (HumanoidBody::body_of_bone(v.bone) != i32(part)) continue;
      const V3 delta = t.rest - v.centre;
      const f64 q =
          delta.x * delta.x / (v.radii.x * v.radii.x) + delta.y * delta.y / (v.radii.y * v.radii.y) + delta.z * delta.z / (v.radii.z * v.radii.z);
      if (q > 1) continue;
      vital_energy[i] += t.energy;
      vital_removed[i] += t.removed;
    }
  }
  const bool blunt = d.kind == DamageKind::Blunt || d.kind == DamageKind::Crush || (d.kind == DamageKind::Edge && d.alignment <= .15);
  auto fatal = [&](DeathCause cause) {
    if (state_.cause == DeathCause::None) state_.cause = cause;
  };
  for (size_t i = 0; i < 16; ++i)
    if (energy[i] > 0) {
      physical_wounds_ = true;
      state_.adrenaline = 1;
      auto& p = state_.parts[i];
      const bool limb = i >= 4;
      const f64 severity = clamp(energy[i] / (limb ? 350.0 : 1300.0), 0.0, 1.0);
      p.flesh = clamp(p.flesh - severity * (removed[i] > 0 ? .45 : .18), 0.0, 1.0);
      p.muscle = clamp(p.muscle - severity * (d.kind == DamageKind::Edge && !blunt ? .9 : .55), 0.0, 1.0);
      p.pain = clamp(p.pain + severity, 0.0, 1.0);
      // Blunt compression transmits the local part's load to bone. A penetrator
      // loads only the bone it actually reaches; energy carried onward cannot
      // fracture it.
      const f64 skeletal = blunt && bone_energy[i] > 0 ? energy[i] : bone_energy[i];
      const bool slender_bone = i == B::shinL || i == B::shinR || i == B::forearmL || i == B::forearmR;
      const f64 fracture = limb ? (slender_bone ? fracture_slender_ : fracture_limb_) : fracture_trunk_, shatter = limb ? shatter_limb_ : shatter_trunk_;
      if (skeletal > fracture) {
        const BoneState injury = skeletal > shatter || p.bone != BoneState::Intact ? BoneState::Shattered : BoneState::Fractured;
        p.bone = std::max(p.bone, injury);
      }
      PersistentWound w;
      w.part = i32(i);
      w.bone = kBodyBone[i];
      w.rest = rest[i] - model.skeleton->rest_head[size_t(w.bone)];
      w.normal = rotate(conj(pose.q[size_t(w.bone)]), vnorm(-d.direction));
      w.pain = severity;
      w.bleeding = removed[i] > 0 ? .0008 + severity * .006 : 0;
      for (size_t r = 0; r < regions.size(); ++r) {
        const auto& v = regions[r];
        if (HumanoidBody::body_of_bone(v.bone) != i32(i)) continue;
        const f64 deposited = vital_energy[r];
        const bool opened = vital_removed[r] > 0;
        if (deposited <= 0) continue;
        if (v.kind == VitalKind::Brain && deposited > (opened ? 15 : 180)) fatal(DeathCause::Brain);
        if (v.kind == VitalKind::Heart && deposited > (opened ? 25 : 350)) fatal(DeathCause::Heart);
        if (v.kind == VitalKind::Cord && deposited > (opened ? 8 : 90)) {
          for (size_t below = (v.bone == H::neck ? 4 : 10); below < 16; ++below) state_.parts[below].nerve = 0;
          if (v.bone == H::neck) fatal(DeathCause::HighSpine);
        }
        if (v.kind == VitalKind::Vessel && deposited > (opened ? 3 : 100)) {
          w.arterial = true;
          w.bleeding = std::max(w.bleeding, .035 + .03 * severity);
          p.vessel = 1;
        }
        if (v.kind == VitalKind::Lung) state_.breathing = std::max(.2, state_.breathing - deposited / (opened ? 300 : 600));
        if (v.kind == VitalKind::Liver && deposited > (opened ? 5 : 100)) w.bleeding = std::max(w.bleeding, .025 * clamp(deposited / 80, 0.0, 1.0));
      }
      add_wound(w);
      state_.shock = clamp(state_.shock + severity * .07, 0.0, 1.0);
    }
  // Massive trauma concerns the core of the body, not an energetic source
  // grazing a finger.
  if (d.kind == DamageKind::Crush && energy[0] + energy[1] + energy[2] + energy[3] > 1800) fatal(DeathCause::MassiveTrauma);
  update(0);
}
void DamageState::physical_capabilities() {
  cap_.vigor = clamp((state_.blood - 1.5) / 3.0, 0.0, 1.0) * (1 - state_.shock * .65);
  cap_.consciousness = state_.consciousness;
  cap_.fatal = state_.cause != DeathCause::None;
  cap_.pain = 0;
  for (size_t i = 0; i < 16; ++i) {
    const auto& p = state_.parts[i];
    const f64 skeletal = p.bone == BoneState::Shattered ? .04 : p.bone == BoneState::Fractured ? .25 : 1;
    cap_.muscle[i] = p.lost ? 0 : p.muscle * p.nerve * skeletal * cap_.vigor;
    cap_.paralysed[i] = p.nerve < .05;
    cap_.pain = std::max(cap_.pain, p.pain * (1 - state_.adrenaline * .6));
  }
  for (size_t side = 0; side < 2; ++side) {
    const size_t leg = side ? 13 : 10, arm = side ? 7 : 4;
    const f64 support = std::min({cap_.muscle[leg], cap_.muscle[leg + 1], cap_.muscle[leg + 2]});
    cap_.legs[side] = {support, support, std::min(support, state_.parts[leg].nerve)};
    const f64 strength = std::min({cap_.muscle[arm], cap_.muscle[arm + 1], cap_.muscle[arm + 2]});
    cap_.arms[side] = {strength, strength, strength * cap_.consciousness};
  }
  cap_.trunk = std::min({cap_.muscle[0], cap_.muscle[1], cap_.muscle[2]});
  cap_.neck = cap_.muscle[3];
  derive_mobility(cap_);
}
void DamageState::bleed(f64 dt, f64 time, const WorldPose& pose, GibSystem& effects) const {
  if (state_.blood <= 0) return;
  for (const auto& w : state_.wounds) {
    const f64 rate = w.bleeding * clot_of(w);
    if (rate <= 0) continue;
    const f64 frequency = clamp(rate * 600, 1.0, 25.0);
    const f64 beat = std::max(0.0, sin(time * 2 * kPi * 1.3)), pulse = beat * beat * beat * beat;
    if (i64(time * frequency) != i64((time + dt) * frequency))
      effects.spray(pose.p[size_t(w.bone)] + rotate(pose.q[size_t(w.bone)], w.rest), rotate(pose.q[size_t(w.bone)], w.normal), 1,
                    w.arterial ? .25 + 2 * pulse : .15, w.arterial ? .15 : .5);
  }
}
bool DamageState::stain(VoxelModel& model, const WorldPose& pose, f64 dt, f64 time) {
  // Clothing soaks slowly. Only surface cells take blood (VoxelPart::stain: what they are made of
  // stays); the cut's flesh and bone remain readable. Each wound looks only at the cells its stain
  // can reach, and a frame visits at most stain_work cells: the wounds take turns.
  if (state_.blood <= 0 || state_.wounds.empty() || i64(time * 2) == i64((time + dt) * 2)) return false;
  const f64 s = model.voxel_size;
  const size_t nw = state_.wounds.size();
  std::vector<u8> changed(model.parts.size(), 0);
  i64 work = 0;
  size_t done = 0;
  for (; done < nw && work < stain_work_; ++done) {
    const auto& w = state_.wounds[(stain_next_ + done) % nw];
    if (w.bleeding <= 0) continue;
    const V3 origin = model.skeleton->rest_head[size_t(w.bone)] + w.rest;
    const f64 radius = std::min(.14, .025 + std::sqrt(w.bleeding * w.age) * .22), down = std::min(.4, w.age * w.bleeding * 1.5);
    const V3 gravity = rotate(conj(pose.q[size_t(w.bone)]), V3{0, 0, -1});
    const f64 reach = radius + down + s;
    const u8 shade = u8(std::max(55.0, 100 - std::min(45.0, w.age * 2)));
    for (size_t pi = 0; pi < model.parts.size(); ++pi) {
      auto& p = model.parts[pi];
      if (p.bone != w.bone) continue;
      std::array<i32, 3> lo, hi;
      bool none = false;
      for (int a = 0; a < 3; ++a) {
        lo[size_t(a)] = std::max(0, i32(std::floor((origin[a] - reach) / s)) - p.origin[size_t(a)]);
        hi[size_t(a)] = std::min(p.dims[size_t(a)], i32(std::floor((origin[a] + reach) / s)) - p.origin[size_t(a)] + 1);
        none = none || lo[size_t(a)] >= hi[size_t(a)];
      }
      if (none) continue;
      for (i32 z = lo[2]; z < hi[2]; ++z)
        for (i32 y = lo[1]; y < hi[1]; ++y)
          for (i32 x = lo[0]; x < hi[0]; ++x) {
            ++work;
            const auto n = size_t(p.index(x, y, z));
            if (!p.cells[n]) continue;
            const Tissue t = model.tissue_at(p, n);
            if (t == Tissue::Flesh || t == Tissue::Bone) continue;
            const V3 at = model.cell_centre(x + p.origin[0], y + p.origin[1], z + p.origin[2]), d = at - origin;
            if (norm(d - gravity * clamp(dot(d, gravity), 0.0, down)) > radius) continue;
            bool surface = false;
            for (int axis = 0; axis < 3 && !surface; ++axis)
              for (int sign : {-1, 1}) {
                std::array<i32, 3> a{x, y, z};
                a[size_t(axis)] += sign;
                if (a[size_t(axis)] < 0 || a[size_t(axis)] >= p.dims[size_t(axis)] || !p.cells[size_t(p.index(a[0], a[1], a[2]))]) surface = true;
              }
            if (!surface) continue;
            if (p.stain.empty()) p.stain.assign(p.cells.size(), 0);
            if (!p.stain[n] || p.stain[n] > shade) {
              p.stain[n] = shade;
              changed[pi] = 1;
            }
          }
    }
  }
  stain_next_ = (stain_next_ + done) % nw;
  bool any = false;
  for (size_t pi = 0; pi < model.parts.size(); ++pi)
    if (changed[pi]) {
      ++model.parts[pi].version;
      any = true;
    }
  return any;
}
}  // namespace svx::anim
