// structvox — bond kinematics and internal forces, linear or corotational.
//
// State vector u (6 per cell): translation from rest (m) and the cell's rotation from rest as
// a world-frame rotation vector (rad). Corotational bond jump for bond i -> j along axis a
// (the prototype's computeKinematics, interface.js:618, without its incremental bookkeeping):
//   attachments a_i = x_i + R_i rho, a_j = x_j - R_j rho with rho = (h/2) e_a;
//   bond frame = slerp midpoint of the two cell rotations; translation jump
//   t = R_mid^T (a_j - a_i); rotation jump w = R_mid^T R_i log(R_i^T R_j).
// Internal forces are J^T sigma with the attachment lever arms (the prototype's f_int, which
// also omits frame-derivative terms). In the small-rotation limit both reduce exactly to the
// linear operator apply_stiffness().
#pragma once

#include <span>

#include <array>

#include "svx/solve/lattice.hpp"

namespace svx {

using Quat = std::array<f64, 4>;  // x, y, z, w
using Vec3 = std::array<f64, 3>;

Quat quat_from_rotvec(const f64* th);
void rotvec_from_quat(const Quat& q, f64* th);
Quat quat_mul(const Quat& a, const Quat& b);
Quat quat_conj(const Quat& q);
Vec3 quat_rotate(const Quat& q, const Vec3& v);

// Generalized jump (svx order) of bond (i -> nbr[axis][i]) for state u.
Vec6 bond_jump(const Lattice& L, int axis, i32 i, const f64* u, bool corot);

// f_int (6 n): internal generalized forces (+ support springs); Dirichlet rows zeroed.
void internal_forces(const Lattice& L, const f64* u, bool corot, f64* f_int);
// The same for the listed cells only (bitwise the values internal_forces gives them, with
// corotational or gathered linear kinematics); other entries are left untouched.
void internal_forces_cells(const Lattice& L, const f64* u, bool corot, std::span<const i32> cells, f64* f_int);

// Contributions of the single bond (i -> nbr[axis][i]) to f_int at its two cells (no masking).
void bond_internal_forces(const Lattice& L, int axis, i32 i, const f64* u, bool corot, f64 fi[6], f64 fj[6]);

// u <- u (+) du: translations add; rotations compose (R <- exp(dth) R) when corot.
void apply_increment(const Lattice& L, f64* u, const f64* du, f64 scale, bool corot);

// Largest cell rotation angle in u (rad).
f64 max_rotation(const Lattice& L, const f64* u);

// Linearization frames of every bond for state u (Lattice::frame), so the operator becomes
// the corotated tangent. clear_frames returns to the rest-configuration operator.
void compute_frames(Lattice& L, const f64* u);
void clear_frames(Lattice& L);

// Frames when the state is rotated by more than `threshold` rad anywhere, else the rest
// operator (the two agree to O(threshold)). Returns true if frames are in use.
bool update_frames(Lattice& L, const f64* u, f64 threshold);

}  // namespace svx
