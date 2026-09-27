#include "svx/mech/bond.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "svx/base/dmath.hpp"

namespace svx {

namespace {

constexpr f64 kEps = 1e-12;

// Prototype archetypes.js profile table (createInterfaceProfile options).
constexpr ProfileParams kProfiles[static_cast<int>(Profile::Count)] = {
    {"solid", 1.0, 1.0, 1.0, 1.0, 1.0, 0.08, 5.0, 8.0},
    {"monolithic_joint", 0.95, 0.9, 0.95, 0.9, 0.85, 0.08, 4.0, 6.5},
    {"weak_joint", 0.45, 0.35, 0.3, 0.3, 0.45, 0.03, 1.0, 1.6},
    {"brace", 4.0, 0.25, 0.35, 0.35, 1.1, 0.15, 6.0, 8.5},
};

// continuum_to_voxel.js seriesGeneralizedStiffness (lengths are the half cells).
f64 series(f64 len, f64 modA, f64 propA, f64 modB, f64 propB, f64 continuity) {
  const f64 sa = std::max(kEps, std::abs(modA * propA));
  const f64 sb = std::max(kEps, std::abs(modB * propB));
  const f64 compliance = len / sa + len / sb;
  if (compliance <= kEps) return 0.0;
  return std::max(0.0, continuity) / compliance;
}

// safeGeneralizedThreshold(capacity, stiffness) with fallback 1e-9.
f64 safe_threshold(f64 cap, f64 k) {
  const f64 kk = std::abs(k), c = std::abs(cap);
  if (kk <= kEps || c <= kEps) return 1e-9;
  return std::max(c / kk, 1e-9);
}

// safeSofteningEnd(yieldDelta, fractureWork, capacity).
f64 softening_end(f64 y, f64 work, f64 cap) {
  const f64 c = std::abs(cap), w = std::max(0.0, work);
  if (c <= kEps || w <= kEps) return std::max(y * 1.001, y + 1e-9);
  return std::max(y * 1.001, 2.0 * w / c);
}

}  // namespace

const ProfileParams& profile_params(Profile p) {
  const int i = static_cast<int>(p);
  SVX_ASSERT(i >= 0 && i < static_cast<int>(Profile::Count));
  return kProfiles[i];
}

Profile profile_from_name(const char* name, bool* ok) {
  for (int i = 0; i < static_cast<int>(Profile::Count); ++i)
    if (std::strcmp(kProfiles[i].name, name) == 0) {
      if (ok) *ok = true;
      return static_cast<Profile>(i);
    }
  if (ok) *ok = false;
  return Profile::Solid;
}

f64 st_venant_rect(f64 a, f64 b) {
  if (a < b) std::swap(a, b);
  if (a <= 0.0 || b <= 0.0) return 0.0;
  const f64 r = b / a;
  const f64 beta = (1.0 / 3.0) * (1.0 - 0.63 * r + 0.052 * dm::ipow(r, 5));
  return beta * a * b * b * b;
}

Section cell_section(int axis, const std::array<f64, 3>& effDims, f64 h, const SectionFixes& fixes) {
  const int t1 = (axis + 1) % 3;
  const int t2 = (axis + 2) % 3;
  // Correct assignment: s1 is the extent along t1, s2 the extent along t2.
  f64 s1 = effDims[t1] * h;
  f64 s2 = effDims[t2] * h;
  if (!fixes.fix_y_axes && axis == 1) {
    // Prototype: Y edges use cross dims (sx, sz) in that order although t1 = z, t2 = x.
    s1 = effDims[0] * h;
    s2 = effDims[2] * h;
  }
  Section s{};
  s.A = s1 * s2;
  s.As = (5.0 / 6.0) * s.A;
  s.I1 = s1 * s2 * s2 * s2 / 12.0;  // rotation about t1: fibres spread along t2
  s.I2 = s2 * s1 * s1 * s1 / 12.0;  // rotation about t2: fibres spread along t1
  s.c1 = 0.5 * s2;
  s.c2 = 0.5 * s1;
  s.J = fixes.st_venant_j ? st_venant_rect(s1, s2) : s.A * (s1 * s1 + s2 * s2) / 12.0;
  s.rt = 0.5 * std::sqrt(s1 * s1 + s2 * s2);
  return s;
}

Profile default_profile(MaterialId a, MaterialId b) { return a == b ? Profile::Solid : Profile::WeakJoint; }

BondModel make_bond(int axis, MaterialId matA, const std::array<f64, 3>& effA, MaterialId matB,
                    const std::array<f64, 3>& effB, f64 h, Profile profile, const SectionFixes& fixes,
                    const LawParams& law) {
  const Material& A = material(matA);
  const Material& B = material(matB);
  const Section sa = cell_section(axis, effA, h, fixes);
  const Section sb = cell_section(axis, effB, h, fixes);
  const ProfileParams& pp = profile_params(profile);
  const f64 L = 0.5 * h;  // half lattice pitch

  BondModel m{};
  m.profile = profile;
  m.residual = law.game ? 0.0 : pp.residual;
  m.k[0] = series(L, A.E, sa.A, B.E, sb.A, pp.axial);
  m.k[1] = series(L, A.G, sa.As, B.G, sb.As, pp.shear);
  m.k[2] = m.k[1];
  m.k[3] = series(L, A.G, sa.J, B.G, sb.J, pp.torsion);
  m.k[4] = series(L, A.E, sa.I1, B.E, sb.I1, pp.bend);
  m.k[5] = series(L, A.E, sa.I2, B.E, sb.I2, pp.bend);

  const f64 s = pp.strength;
  m.cap.Nt = s * std::min(A.ft * sa.A, B.ft * sb.A);
  m.cap.Nc = s * std::min(A.fc * sa.A, B.fc * sb.A);
  m.cap.V = s * std::min(A.tau * sa.As, B.tau * sb.As);
  m.cap.M1 = s * std::min(A.fb * sa.I1 / std::max(sa.c1, L), B.fb * sb.I1 / std::max(sb.c1, L));
  m.cap.M2 = s * std::min(A.fb * sa.I2 / std::max(sa.c2, L), B.fb * sb.I2 / std::max(sb.c2, L));
  m.cap.T = s * std::min(A.taut * sa.J / std::max(sa.rt, L), B.taut * sb.J / std::max(sb.rt, L));

  // Fracture energies (minimum toughness) scaled by the strength continuity, per
  // normalizeInterfaceProfile: fractureContinuity.modeI/II default to strengthContinuity.
  m.area_min = std::max(kEps, std::min(sa.A, sb.A));
  const f64 gI = std::min(A.GfI, B.GfI) * s;
  const f64 gII = std::min(A.GfII, B.GfII) * s;
  const f64 workI = gI * m.area_min, workII = gII * m.area_min;

  const f64 caps6[6] = {m.cap.Nt, m.cap.V, m.cap.V, m.cap.T, m.cap.M1, m.cap.M2};
  if (!law.game) {
    const f64 kon = pp.kappa_on;
    const f64 kbr = std::max(kon * 1.001, pp.kappa_br);
    const f64 ratio = kbr / kon;
    m.onset.open = safe_threshold(m.cap.Nt, m.k[0]) * kon;
    m.onset.comp = safe_threshold(m.cap.Nc, m.k[0]) * kon;
    m.brk.open = std::max(m.onset.open * ratio, softening_end(m.onset.open, workI, m.cap.Nt));
    m.brk.comp = std::max(m.onset.comp * ratio, softening_end(m.onset.comp, workI, m.cap.Nc));
    m.onset.comp6[0] = m.brk.comp6[0] = 0.0;
    for (int q = 1; q < 6; ++q) {
      m.onset.comp6[q] = safe_threshold(caps6[q], m.k[q]) * kon;
      m.brk.comp6[q] = std::max(m.onset.comp6[q] * ratio, softening_end(m.onset.comp6[q], workII, caps6[q]));
    }
  } else {
    // Game mode: onset at the fragility-scaled capacity; break from the fracture energy
    // (triangular softening => dissipated work = G_f * A independent of the cell size).
    const f64 F = std::max(1e-6, law.fragility) * law.kappa_on_game;
    const f64 rmin = std::max(1.001, law.min_ductility);
    // triangular softening from peak P at y = P / k to zero at 2 w / P, with the peak
    // lowered where needed so that break / onset >= rmin (regularized size effect)
    auto tri = [&](f64 cap, f64 k, f64 work, f64* on, f64* br) {
      f64 P = std::abs(cap) * F;
      if (k > kEps && work > kEps) P = std::min(P, std::sqrt(2.0 * work * k / rmin));
      *on = safe_threshold(P, k);
      *br = softening_end(*on, work, P);
    };
    const f64 tI = workI * law.toughness, tII = workII * law.toughness;
    tri(m.cap.Nt, m.k[0], tI, &m.onset.open, &m.brk.open);
    tri(m.cap.Nc, m.k[0], tI, &m.onset.comp, &m.brk.comp);
    m.onset.comp6[0] = m.brk.comp6[0] = 0.0;
    for (int q = 1; q < 6; ++q) tri(caps6[q], m.k[q], tII, &m.onset.comp6[q], &m.brk.comp6[q]);
  }
  return m;
}

f64 bond_utilization(const Vec6& F, const BondCapacity& cap) {
  f64 u = 0.0;
  const f64 un = F[0] >= 0.0 ? F[0] / cap.Nt : -F[0] / cap.Nc;
  u = std::max(u, un);
  u = std::max(u, std::abs(F[1]) / cap.V);
  u = std::max(u, std::abs(F[2]) / cap.V);
  u = std::max(u, std::abs(F[3]) / cap.T);
  u = std::max(u, std::abs(F[4]) / cap.M1);
  u = std::max(u, std::abs(F[5]) / cap.M2);
  return u;
}

}  // namespace svx
