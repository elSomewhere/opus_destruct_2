// structvox — the session's pieces and joints in records (docs/CORE.md §4): in deltas (a saved
// session comes back with its rubble, its machines and the world's clock) and in the streaming
// archive (a piece asleep in chunks that go out of range is kept with them, in the archive's
// fixed budget, and comes back with them).
//
// A piece's record holds all of it: its shapes (lattices, voxels, their fragments, broken faces
// and junctions, persistent layers by name), its fragments, its pose, motion and sleep. Its mass
// properties and collision samples are made again from them (body_refresh: the same bits), its
// bond graph when it is next needed. A joint's record holds its anchors (a piece's by the piece's
// id: pieces keep their ids), its settings, its drive and the solver's state.
#include <algorithm>
#include <cmath>
#include <memory>

#include "archive.hpp"
#include "bytes.hpp"
#include "svx/world/world.hpp"
#include "world_internal.hpp"

namespace svx {

using world_detail::put32;
using world_detail::put3;
using world_detail::put64;
using world_detail::putf;
using world_detail::putq;
using world_detail::Rd;

namespace {

constexpr u8 kPieceVersion = 2;            // (1: no speed limit)
constexpr u32 kSessionMagic = 0x53534553;  // "SESS"
// (an archived group's record; the session of a delta's trailer v3 is a group of v1, of v4 v2)
// v1: pieces, joints, dead loads. v2: the joints' collide flags and break angles; the wheels.
// v3: the joints' latches.
// (4: a wheel's material)
constexpr u8 kGroupVersion = 4;
// (a wheel saved before its material was: the tyre it then always became - the standard preset
// 16 of that time, now the id a host's tyre material keeps)
constexpr u8 kLegacyWheelMaterial = 16;
constexpr i64 kMaxCells = i64(1) << 24;    // (a piece's box at most: 16 M cells)

// Runs of equal values (a piece's box is mostly air): count, value.
template <class T>
void put_runs(std::vector<u8>& out, const std::vector<T>& v) {
  size_t runs_at = out.size();
  put32(out, 0);
  u32 runs = 0;
  for (size_t i = 0; i < v.size();) {
    size_t j = i + 1;
    while (j < v.size() && v[j] == v[i]) ++j;
    put32(out, static_cast<u32>(j - i));
    if constexpr (sizeof(T) == 1) out.push_back(static_cast<u8>(v[i]));
    else put32(out, static_cast<u32>(v[i]));
    ++runs;
    i = j;
  }
  for (int k = 0; k < 4; ++k) out[runs_at + size_t(k)] = static_cast<u8>(runs >> (8 * k));
}

template <class T>
bool read_runs(Rd& in, size_t n, std::vector<T>* v) {
  const u32 runs = in.u32_();
  if (!in.ok || runs > n) return false;
  v->clear();
  v->reserve(n);
  for (u32 r = 0; r < runs; ++r) {
    const u32 len = in.u32_();
    T x;
    if constexpr (sizeof(T) == 1) x = static_cast<T>(in.u8_());
    else x = static_cast<T>(in.u32_());
    if (!in.ok || len == 0 || len > n - v->size()) return false;
    v->insert(v->end(), len, x);
  }
  return v->size() == n;
}

bool finite_v(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite_q(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

void put_m3(std::vector<u8>& out, const M3& m) {
  for (f64 x : m.m) putf(out, x);
}
M3 read_m3(Rd& in) {
  M3 m;
  for (f64& x : m.m) x = in.f64_();
  return m;
}
bool finite_m3(const M3& m) {
  for (f64 x : m.m)
    if (!std::isfinite(x)) return false;
  return true;
}

}  // namespace

std::vector<u8> World::Impl::piece_record(const Body& b) const {
  std::vector<u8> out;
  out.push_back(kPieceVersion);
  put64(out, static_cast<u64>(b.id));
  put64(out, static_cast<u64>(b.parent));
  out.push_back(static_cast<u8>((b.asleep ? 1 : 0) | (b.keep ? 2 : 0)));
  put3(out, b.x);
  putq(out, b.q);
  put3(out, b.v);
  put3(out, b.w);
  put32(out, static_cast<u32>(b.still));
  put32(out, static_cast<u32>(b.held));
  putf(out, b.sleep_ema);
  putf(out, b.age);
  putf(out, b.max_speed);
  put32(out, static_cast<u32>(b.shapes.size()));
  for (const BodyShape& S : b.shapes) {
    out.push_back(S.xf.identity ? 1 : 0);
    if (!S.xf.identity) {
      put3(out, S.xf.off);
      putq(out, S.xf.q);
      put_m3(out, S.xf.R);
    }
    put32(out, S.grid);
    putf(out, S.h);
    put32(out, static_cast<u32>(S.priority));
    for (int a = 0; a < 3; ++a) put32(out, static_cast<u32>(S.lo[size_t(a)]));
    for (int a = 0; a < 3; ++a) put32(out, static_cast<u32>(S.dim[size_t(a)]));
    put_runs(out, S.vox);
    put_runs(out, S.frag);
    put_runs(out, S.brk);
    // (the persistent layers it has values in, by name)
    u8 nl = 0;
    const size_t nl_at = out.size();
    out.push_back(0);
    for (size_t L = 0; L < S.layer.size() && L < ext_.layers.size(); ++L) {
      if (S.layer[L].empty() || !ext_.layers[L].persistent) continue;
      const std::string& name = ext_.layers[L].name;
      out.push_back(static_cast<u8>(std::min<size_t>(name.size(), 255)));
      out.insert(out.end(), name.begin(), name.begin() + static_cast<long>(std::min<size_t>(name.size(), 255)));
      put_runs(out, S.layer[L]);
      ++nl;
    }
    out[nl_at] = nl;
    put32(out, static_cast<u32>(S.jbrk.size()));
    for (u64 j : S.jbrk) put64(out, j);
  }
  put32(out, static_cast<u32>(b.frags.size()));
  for (const BodyFrag& f : b.frags) {
    put3(out, f.com);
    putf(out, f.mass);
    put_m3(out, f.inertia);
    out.push_back(static_cast<u8>(f.mat));
    put32(out, static_cast<u32>(f.count));
    putf(out, f.strength);
    put32(out, f.shape);
  }
  return out;
}

std::unique_ptr<Body> World::Impl::read_piece_record(const std::vector<u8>& rec) const {
  Rd in{rec};
  const u8 version = in.u8_();
  if (version < 1 || version > kPieceVersion) return nullptr;
  auto b = std::make_unique<Body>();
  b->id = in.i64_();
  b->parent = in.i64_();
  const u8 flags = in.u8_();
  b->x = in.v3();
  b->q = in.q4();
  b->v = in.v3();
  b->w = in.v3();
  b->still = in.i32_();
  b->held = in.i32_();
  b->sleep_ema = in.f64_();
  b->age = in.f64_();
  if (version >= 2) {
    b->max_speed = in.f64_();
    if (!std::isfinite(b->max_speed) || b->max_speed < 0.0 || b->max_speed > 1000.0) return nullptr;
  }
  const f64 qn = b->q.x * b->q.x + b->q.y * b->q.y + b->q.z * b->q.z + b->q.w * b->q.w;
  if (!in.ok || b->id <= 0 || (flags & ~3u) != 0 || !in_range(b->x) || !finite_q(b->q) || !(qn > 0.5 && qn < 2.0) || !finite_v(b->v) ||
      !finite_v(b->w) || !std::isfinite(b->sleep_ema) || !std::isfinite(b->age))
    return nullptr;
  b->asleep = (flags & 1) != 0;
  b->keep = (flags & 2) != 0;
  const u32 ns = in.u32_();
  if (!in.ok || ns == 0 || ns > 256) return nullptr;
  b->shapes.resize(ns);
  for (BodyShape& S : b->shapes) {
    const u8 ident = in.u8_();
    if (ident > 1) return nullptr;
    if (!ident) {
      S.xf.identity = false;
      S.xf.off = in.v3();
      S.xf.q = in.q4();
      S.xf.R = read_m3(in);
      S.xf.Rt = transpose(S.xf.R);
      if (!finite_v(S.xf.off) || !finite_q(S.xf.q) || !finite_m3(S.xf.R)) return nullptr;
    }
    S.grid = in.u32_();
    S.h = in.f64_();
    S.priority = in.i32_();
    for (int a = 0; a < 3; ++a) S.lo[size_t(a)] = in.i32_();
    i64 cells = 1;
    for (int a = 0; a < 3; ++a) {
      S.dim[size_t(a)] = in.i32_();
      if (S.dim[size_t(a)] < 1 || S.dim[size_t(a)] > 4096) return nullptr;
      cells *= S.dim[size_t(a)];
    }
    if (!in.ok || !std::isfinite(S.h) || !(S.h >= grid_.h / 64.0) || !(S.h <= grid_.h * 64.0) || cells > kMaxCells) return nullptr;
    for (int a = 0; a < 3; ++a)
      if (std::abs(static_cast<i64>(S.lo[size_t(a)])) > kVoxelLimit) return nullptr;
    const size_t n = static_cast<size_t>(cells);
    if (!read_runs(in, n, &S.vox) || !read_runs(in, n, &S.frag) || !read_runs(in, n, &S.brk)) return nullptr;
    S.count = 0;
    for (Vox v : S.vox) {
      if (vox_anchored(v)) return nullptr;  // (a piece has no supports)
      S.count += vox_solid(v) ? 1 : 0;
    }
    const u8 nl = in.u8_();
    for (u8 k = 0; k < nl && in.ok; ++k) {
      const u8 len = in.u8_();
      if (!in.need(len)) return nullptr;
      const std::string name(rec.begin() + static_cast<long>(in.p), rec.begin() + static_cast<long>(in.p + len));
      in.p += len;
      std::vector<u8> vals;
      if (!read_runs(in, n, &vals)) return nullptr;
      const int L = grid_.layer_index(name);
      if (L >= 0 && L < kMaxLayers) S.layer[size_t(L)] = std::move(vals);  // (a layer this world does not have: dropped)
    }
    const u32 nj = in.u32_();
    if (!in.ok || u64(nj) * 8 > rec.size()) return nullptr;
    S.jbrk.resize(nj);
    for (u64& j : S.jbrk) j = in.u64_();
    if (!std::is_sorted(S.jbrk.begin(), S.jbrk.end())) return nullptr;
  }
  const u32 nf = in.u32_();
  if (!in.ok || nf == 0 || u64(nf) * 100 > rec.size()) return nullptr;
  b->frags.resize(nf);
  for (BodyFrag& f : b->frags) {
    f.com = in.v3();
    f.mass = in.f64_();
    f.inertia = read_m3(in);
    const u8 mat = in.u8_();
    f.count = in.i32_();
    f.strength = in.f64_();
    const u32 shape = in.u32_();
    if (!in.ok || !finite_v(f.com) || !std::isfinite(f.mass) || f.mass < 0.0 || !finite_m3(f.inertia) || mat >= 128 || f.count < 0 ||
        !std::isfinite(f.strength) || shape >= ns)
      return nullptr;
    f.mat = static_cast<MaterialId>(mat);
    f.shape = static_cast<u16>(shape);
  }
  for (const BodyShape& S : b->shapes)
    for (u32 fr : S.frag)
      if (fr > nf) return nullptr;
  if (!in.ok || in.p != rec.size()) return nullptr;
  // (what the record does not hold: made again)
  const V3 x = b->x, v = b->v, w = b->w;
  const Quat q = b->q;
  body_refresh(*b, grid_.h, cfg_.rigid.max_points);
  if (!(b->mass > 0.0)) return nullptr;
  b->x = x;
  b->q = q;
  b->v = b->v_pre = v;
  b->w = b->w_pre = w;
  b->was_asleep = b->asleep;
  b->graph_dirty = true;
  b->announced = false;
  b->refresh_box();
  return b;
}

std::vector<u8> World::Impl::joint_record(size_t k) const {
  const Joint& j = rigid_.joints[k];
  const JointRec& r = att_.joints[k];
  std::vector<u8> out;
  put32(out, j.id);
  out.push_back(static_cast<u8>(j.type));
  out.push_back(j.limited ? 1 : 0);
  for (f64 x : {j.min_length, j.max_length, j.stiffness, j.damping, j.lower, j.upper, j.break_force, j.break_torque}) putf(out, x);
  out.push_back(static_cast<u8>(j.drive.kind));
  for (f64 x : {j.drive.speed, j.drive.max, j.drive.target, j.drive.target2, j.drive.period, j.drive.phase, j.drive.stiffness}) putf(out, x);
  putq(out, j.rel);
  put3(out, j.lin);
  put3(out, j.ang);
  for (f64 x : {j.axial, j.limit, j.motor}) putf(out, x);
  put3(out, j.force);
  put3(out, j.torque);
  putf(out, j.value);
  out.push_back(j.collide ? 1 : 0);
  putf(out, j.break_angle);
  putf(out, j.latch);
  out.push_back(j.latched ? 1 : 0);
  for (const JointRec::End* E : {&r.a, &r.b}) {
    out.push_back(static_cast<u8>(E->kind));
    put32(out, E->grid);
    for (int a = 0; a < 3; ++a) put32(out, static_cast<u32>(E->voxel[size_t(a)]));
    put3(out, E->point);
    put3(out, E->axis);
    put3(out, E->ref);
    put64(out, static_cast<u64>(E->piece));
    put32(out, static_cast<u32>(E->shape));
  }
  return out;
}

bool World::Impl::read_joint_record(Rd& in, JointRec* r, Joint* j, u8 version) const {
  j->id = in.u32_();
  const u8 type = in.u8_(), lim = in.u8_();
  f64* fs[] = {&j->min_length, &j->max_length, &j->stiffness, &j->damping, &j->lower, &j->upper, &j->break_force, &j->break_torque};
  for (f64* x : fs) *x = in.f64_();
  const u8 dk = in.u8_();
  f64* ds[] = {&j->drive.speed, &j->drive.max, &j->drive.target, &j->drive.target2, &j->drive.period, &j->drive.phase, &j->drive.stiffness};
  for (f64* x : ds) *x = in.f64_();
  j->rel = in.q4();
  j->lin = in.v3();
  j->ang = in.v3();
  j->axial = in.f64_();
  j->limit = in.f64_();
  j->motor = in.f64_();
  j->force = in.v3();
  j->torque = in.v3();
  j->value = in.f64_();
  if (version >= 2) {
    const u8 col = in.u8_();
    j->break_angle = in.f64_();
    if (col > 1 || !std::isfinite(j->break_angle) || j->break_angle < 0.0) return false;
    j->collide = col != 0;
  }
  if (version >= 3) {
    j->latch = in.f64_();
    const u8 lat = in.u8_();
    if (lat > 1 || !std::isfinite(j->latch) || j->latch < 0.0) return false;
    j->latched = lat != 0;
  }
  if (!in.ok || j->id == 0 || type > static_cast<u8>(JointType::Distance) || lim > 1 || dk > static_cast<u8>(JointDrive::Kind::Oscillate)) return false;
  for (f64* x : fs)
    if (!std::isfinite(*x)) return false;
  for (f64* x : ds)
    if (!std::isfinite(*x)) return false;
  if (!finite_q(j->rel) || !finite_v(j->lin) || !finite_v(j->ang) || !std::isfinite(j->axial) || !std::isfinite(j->limit) ||
      !std::isfinite(j->motor) || !finite_v(j->force) || !finite_v(j->torque) || !std::isfinite(j->value))
    return false;
  j->type = static_cast<JointType>(type);
  j->limited = lim != 0;
  j->drive.kind = static_cast<JointDrive::Kind>(dk);
  r->id = j->id;
  for (JointRec::End* E : {&r->a, &r->b}) {
    const u8 kind = in.u8_();
    E->grid = in.u32_();
    for (int a = 0; a < 3; ++a) E->voxel[size_t(a)] = in.i32_();
    E->point = in.v3();
    E->axis = in.v3();
    E->ref = in.v3();
    E->piece = in.i64_();
    E->shape = in.i32_();
    if (!in.ok || kind > static_cast<u8>(JointAnchor::Kind::Piece) || !finite_v(E->point) || !finite_v(E->axis) || !finite_v(E->ref)) return false;
    E->kind = static_cast<JointAnchor::Kind>(kind);
  }
  return true;
}

std::vector<u8> World::Impl::wheel_record(size_t k) const {
  const Wheel& w = rigid_.wheels[k];
  const WheelRec& r = att_.wheels[k];
  std::vector<u8> out;
  put32(out, w.id);
  put32(out, r.group);
  put32(out, r.tag);
  out.push_back(static_cast<u8>(r.material));
  for (f64 x : {w.radius, w.width, w.rest, w.travel, w.stiffness, w.damping, w.inertia, w.grip, w.break_force, w.drive, w.brake, w.steer, w.spin,
                w.angle, w.length})
    putf(out, x);
  const JointRec::End& E = r.mount;
  out.push_back(static_cast<u8>(E.kind));
  put32(out, E.grid);
  for (int a = 0; a < 3; ++a) put32(out, static_cast<u32>(E.voxel[size_t(a)]));
  put3(out, E.point);
  put3(out, E.axis);
  put3(out, E.ref);
  put64(out, static_cast<u64>(E.piece));
  put32(out, static_cast<u32>(E.shape));
  return out;
}

bool World::Impl::read_wheel_record(Rd& in, WheelRec* r, Wheel* w, u8 version) const {
  w->id = in.u32_();
  r->id = w->id;
  r->group = in.u32_();
  r->tag = in.u32_();
  const u8 m = version >= 4 ? in.u8_() : kLegacyWheelMaterial;
  if (m >= kMaxMaterials) return false;
  r->material = static_cast<MaterialId>(m);
  f64* fs[] = {&w->radius, &w->width, &w->rest, &w->travel, &w->stiffness, &w->damping, &w->inertia, &w->grip, &w->break_force, &w->drive,
               &w->brake, &w->steer, &w->spin, &w->angle, &w->length};
  for (f64* x : fs) *x = in.f64_();
  JointRec::End& E = r->mount;
  const u8 kind = in.u8_();
  E.grid = in.u32_();
  for (int a = 0; a < 3; ++a) E.voxel[size_t(a)] = in.i32_();
  E.point = in.v3();
  E.axis = in.v3();
  E.ref = in.v3();
  E.piece = in.i64_();
  E.shape = in.i32_();
  if (!in.ok || w->id == 0 || kind != static_cast<u8>(JointAnchor::Kind::Grid)) return false;
  for (f64* x : fs)
    if (!std::isfinite(*x)) return false;
  if (!(w->radius > 0.0) || w->inertia <= 0.0 || w->travel < 0.0 || w->rest < 0.0 || !finite_v(E.point) || !finite_v(E.axis) || !finite_v(E.ref))
    return false;
  E.kind = JointAnchor::Kind::Grid;
  return true;
}

void World::Impl::write_group(std::vector<u8>& out, const std::vector<const Body*>& bodies, const std::vector<size_t>& joints,
                        const std::vector<size_t>& wheels) const {
  put32(out, static_cast<u32>(bodies.size()));
  for (const Body* b : bodies) {
    const std::vector<u8> rec = piece_record(*b);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  put32(out, static_cast<u32>(joints.size()));
  for (size_t k : joints) {
    const std::vector<u8> rec = joint_record(k);
    out.insert(out.end(), rec.begin(), rec.end());
  }
  // (the sleeping pieces' dead loads: what they rest on keeps carrying them)
  u32 nd = 0;
  const size_t nd_at = out.size();
  put32(out, 0);
  for (const Body* b : bodies) {
    const auto it = dead_loads_.find(b->id);
    if (it == dead_loads_.end() || it->second.empty()) continue;
    put64(out, static_cast<u64>(b->id));
    u32 n = 0;
    const size_t n_at = out.size();
    put32(out, 0);
    for (const DeadLoad& d : it->second) {
      if (!live(d.vox.grid)) continue;
      put32(out, id_of(d.vox.grid));
      for (int a = 0; a < 3; ++a) put32(out, static_cast<u32>(d.vox.p[size_t(a)]));
      put3(out, d.p);
      put3(out, d.F);
      ++n;
    }
    for (int k = 0; k < 4; ++k) out[n_at + size_t(k)] = static_cast<u8>(n >> (8 * k));
    ++nd;
  }
  for (int k = 0; k < 4; ++k) out[nd_at + size_t(k)] = static_cast<u8>(nd >> (8 * k));
  // (v2) the wheels on its pieces
  put32(out, static_cast<u32>(wheels.size()));
  for (size_t k : wheels) {
    const std::vector<u8> rec = wheel_record(k);
    out.insert(out.end(), rec.begin(), rec.end());
  }
}

bool World::Impl::read_group(Rd& in, SessionDelta* s, u8 version) const {
  const u32 np = in.u32_();
  if (!in.ok || u64(np) * 8 > in.b.size()) return false;
  for (u32 k = 0; k < np; ++k) {
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz)) return false;
    const std::vector<u8> rec(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    std::unique_ptr<Body> b = read_piece_record(rec);
    if (!b) return false;
    s->pieces.push_back(std::move(b));
  }
  std::sort(s->pieces.begin(), s->pieces.end(), [](const std::unique_ptr<Body>& a, const std::unique_ptr<Body>& b) { return a->id < b->id; });
  for (size_t k = 1; k < s->pieces.size(); ++k)
    if (s->pieces[k]->id == s->pieces[k - 1]->id) return false;
  const u32 nj = in.u32_();
  if (!in.ok || u64(nj) * 64 > in.b.size()) return false;
  for (u32 k = 0; k < nj; ++k) {
    JointRec r;
    Joint j;
    if (!read_joint_record(in, &r, &j, version)) return false;
    s->joints.push_back({r, j});
  }
  std::sort(s->joints.begin(), s->joints.end(), [](const auto& a, const auto& b) { return a.second.id < b.second.id; });
  for (size_t k = 1; k < s->joints.size(); ++k)
    if (s->joints[k].second.id == s->joints[k - 1].second.id) return false;
  const u32 nd = in.u32_();
  if (!in.ok || u64(nd) * 12 > in.b.size()) return false;
  for (u32 k = 0; k < nd; ++k) {
    SessionDelta::Dead d;
    d.piece = in.i64_();
    const u32 n = in.u32_();
    if (!in.ok || u64(n) * 64 > in.b.size()) return false;
    for (u32 q = 0; q < n; ++q) {
      SessionDelta::Dead::Load l;
      l.grid = in.u32_();
      for (int a = 0; a < 3; ++a) l.voxel[size_t(a)] = in.i32_();
      l.p = in.v3();
      l.F = in.v3();
      if (!in.ok || !finite_v(l.p) || !finite_v(l.F)) return false;
      d.loads.push_back(l);
    }
    s->dead.push_back(std::move(d));
  }
  if (version >= 2) {
    const u32 nw = in.u32_();
    if (!in.ok || u64(nw) * 64 > in.b.size()) return false;
    for (u32 k = 0; k < nw; ++k) {
      WheelRec r;
      Wheel w;
      if (!read_wheel_record(in, &r, &w, version)) return false;
      s->wheels.push_back({r, w});
    }
    std::sort(s->wheels.begin(), s->wheels.end(), [](const auto& a, const auto& b) { return a.second.id < b.second.id; });
    for (size_t k = 1; k < s->wheels.size(); ++k)
      if (s->wheels[k].second.id == s->wheels[k - 1].second.id) return false;
  }
  return in.ok;
}

void World::Impl::add_group(SessionDelta& s) {
  for (std::unique_ptr<Body>& b : s.pieces) {
    if (rigid_.find(b->id)) continue;  // (never expected: ids are the world's)
    next_id_ = std::max(next_id_, b->id + 1);
    rigid_.add(std::move(b));
  }
  for (SessionDelta::Dead& d : s.dead) {
    if (!rigid_.find(d.piece)) continue;
    auto& list = dead_loads_[d.piece];
    for (const auto& l : d.loads) {
      const i32 g = slot_of(l.grid);
      if (g >= 0) list.push_back({GVox{l.voxel, static_cast<u16>(g)}, l.p, l.F});
    }
  }
  for (auto& [r, j] : s.joints) {
    // (an end on a grid that is not there, or on a piece that is not: the joint is gone)
    bool ok = std::none_of(att_.joints.begin(), att_.joints.end(), [&](const JointRec& x) { return x.id == r.id; });
    for (const JointRec::End* E : {&r.a, &r.b})
      if (E->kind != JointAnchor::Kind::World)
        ok = ok && (E->piece > 0 ? rigid_.find(E->piece) != nullptr : E->piece == 0 && slot_of(E->grid) >= 0);
    if (!ok) continue;
    insert_joint(r, j);
    att_.next_joint = std::max<JointId>(att_.next_joint, (j.id & 0x80000000u) ? att_.next_joint : j.id + 1);
  }
  if (!att_.joints.empty()) update_joint_ends();
  for (auto& [r, w] : s.wheels) {
    // (on a piece that is not there, or a grid that is not: it is gone)
    const JointRec::End& E = r.mount;
    const bool ok = std::none_of(att_.wheels.begin(), att_.wheels.end(), [&](const WheelRec& x) { return x.id == r.id; }) &&
                    (E.piece > 0 ? rigid_.find(E.piece) != nullptr : E.piece == 0 && slot_of(E.grid) >= 0);
    if (!ok) continue;
    insert_wheel(r, w);
    att_.next_wheel = std::max<WheelId>(att_.next_wheel, w.id + 1);
  }
  if (!att_.wheels.empty()) update_wheel_mounts();
  announce_bodies();
}

std::vector<u8> World::Impl::session_entries() const {
  std::vector<u8> out;
  put32(out, kSessionMagic);
  put64(out, static_cast<u64>(steps_));
  put64(out, static_cast<u64>(next_id_));
  put32(out, att_.next_joint);
  put32(out, att_.next_wheel);
  // the pieces (announced: the ones the host knows), their joints (those of pieces not announced
  // yet are left out) and dead loads
  std::vector<const Body*> bodies;
  for (const auto& bp : rigid_.bodies)
    if (bp->announced) bodies.push_back(bp.get());
  std::vector<size_t> js;
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    if (rigid_.joints[k].broken) continue;
    bool fresh = false;
    for (const JointRec::End* E : {&att_.joints[k].a, &att_.joints[k].b})
      if (E->piece > 0)
        if (const Body* b = rigid_.find(E->piece)) fresh = fresh || !b->announced;
    if (!fresh) js.push_back(k);
  }
  // (and the wheels: on announced pieces, or still on a grid's voxel)
  std::vector<size_t> ws;
  for (size_t k = 0; k < rigid_.wheels.size(); ++k) {
    if (rigid_.wheels[k].broken) continue;
    const i64 on = att_.wheels[k].mount.piece;
    if (on > 0) {
      const Body* b = rigid_.find(on);
      if (!b || !b->announced) continue;
    }
    ws.push_back(k);
  }
  write_group(out, bodies, js, ws);
  // the groups archived out of range (a streamed world's): as archived
  put32(out, static_cast<u32>(strm_.archived_groups.size()));
  for (const auto& [key, g] : strm_.archived_groups) {
    put64(out, key);
    put32(out, static_cast<u32>(g.chunks.size()));
    for (u64 c : g.chunks) put64(out, c);
    put32(out, static_cast<u32>(g.joints.size()));
    for (JointId j : g.joints) put32(out, j);
    const std::vector<u8> rec = strm_.archive->get(key);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  // (v6) the articulations, then those archived out of range
  put32(out, static_cast<u32>(arts_.size()));
  for (const auto& a : arts_) {
    const std::vector<u8> rec = articulation_record(*a);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  put32(out, static_cast<u32>(strm_.archived_arts.size()));
  for (const auto& [key, chunks] : strm_.archived_arts) {
    put64(out, key);
    put32(out, static_cast<u32>(chunks.size()));
    for (u64 c : chunks) put64(out, c);
    const std::vector<u8> rec = strm_.archive->get(key);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  return out;
}

bool World::Impl::read_session(Rd& in, SessionDelta* s, u32 version) const {
  if (in.u32_() != kSessionMagic) return false;
  s->steps = in.i64_();
  s->next_id = in.i64_();
  s->next_joint = in.u32_();
  if (version >= 4) s->next_wheel = in.u32_();
  if (!in.ok || s->steps < 0 || s->next_id < 1) return false;
  if (!read_group(in, s, version >= 7 ? 4 : version >= 5 ? 3 : version >= 4 ? 2 : 1)) return false;
  const u32 na = in.u32_();
  if (!in.ok || u64(na) * 20 > in.b.size()) return false;
  for (u32 k = 0; k < na; ++k) {
    SessionDelta::Archived a;
    a.key = in.u64_();
    const u32 nc = in.u32_();
    if (!in.ok || (a.key >> 62) != 3 || u64(nc) * 8 > in.b.size()) return false;
    for (u32 q = 0; q < nc; ++q) a.chunks.push_back(in.u64_());
    const u32 nj = in.u32_();
    if (!in.ok || u64(nj) * 4 > in.b.size()) return false;
    for (u32 q = 0; q < nj; ++q) a.joints.push_back(in.u32_());
    const u32 sz = in.u32_();
    if (!in.ok || !in.need(sz)) return false;
    a.record.assign(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
    in.p += sz;
    // (checked now: nothing is applied from a malformed delta)
    Rd rin{a.record};
    SessionDelta probe;
    const u8 gv = rin.u8_();
    if (gv < 1 || gv > kGroupVersion || !read_group(rin, &probe, gv) || rin.p != a.record.size()) return false;
    s->archived.push_back(std::move(a));
  }
  if (version >= 6) {
    const u32 nl = in.u32_();
    if (!in.ok || u64(nl) * 4 > in.b.size()) return false;
    for (u32 k = 0; k < nl; ++k) {
      const u32 sz = in.u32_();
      if (!in.ok || !in.need(sz)) return false;
      const std::vector<u8> rec(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
      in.p += sz;
      Rd rin{rec};
      ArticulationSaved a;
      if (!read_articulation_record(rin, &a) || rin.p != rec.size()) return false;
      s->articulations.push_back(std::move(a));
    }
    const u32 na2 = in.u32_();
    if (!in.ok || u64(na2) * 16 > in.b.size()) return false;
    for (u32 k = 0; k < na2; ++k) {
      SessionDelta::ArchivedArticulation a;
      a.key = in.u64_();
      const u32 nc = in.u32_();
      if (!in.ok || (a.key >> 61) != 7 || u64(nc) * 8 > in.b.size()) return false;
      for (u32 q = 0; q < nc; ++q) a.chunks.push_back(in.u64_());
      const u32 sz = in.u32_();
      if (!in.ok || !in.need(sz)) return false;
      a.record.assign(in.b.begin() + static_cast<long>(in.p), in.b.begin() + static_cast<long>(in.p + sz));
      in.p += sz;
      Rd rin{a.record};
      ArticulationSaved probe;
      if (!read_articulation_record(rin, &probe) || rin.p != a.record.size()) return false;
      s->archived_articulations.push_back(std::move(a));
    }
  }
  return in.ok;
}

void World::Impl::apply_session(SessionDelta&& s) {
  // (the saved session's pieces and joints take the place of the ones there are: the level's
  // joints, made again by the host, are the saved ones now)
  clear_articulations();
  std::vector<i64> ids;
  for (const auto& bp : rigid_.bodies) ids.push_back(bp->id);
  remove_bodies(ids, PieceEnd::Removed);
  pw_.pending_add.clear();
  pw_.pending_retire.clear();
  dead_loads_.clear();
  att_.joints.clear();
  rigid_.joints.clear();
  att_.wheels.clear();
  rigid_.wheels.clear();
  next_id_ = std::max(next_id_, s.next_id);
  att_.next_joint = std::max<JointId>(att_.next_joint, s.next_joint);
  att_.next_wheel = std::max<WheelId>(att_.next_wheel, s.next_wheel);
  steps_ = s.steps;
  add_group(s);
  // (a streamed world's groups out of range: archived again, as they were)
  for (const auto& [key, g] : strm_.archived_groups) strm_.archive->erase(key);
  strm_.archived_groups.clear();
  strm_.archived_joints.clear();
  st_.archived_pieces = 0;
  if (strm_.source)
    for (SessionDelta::Archived& a : s.archived) {
      if (a.chunks.empty()) continue;
      archive_record(a.key, a.record, region_of(a.chunks.front()));
      if (!strm_.archive->has(a.key)) continue;
      Rd rin{a.record};
      rin.u8_();
      const u32 pieces = rin.u32_();
      for (JointId j : a.joints) strm_.archived_joints.insert(j);
      strm_.archived_groups[a.key] = ArchivedGroup{std::move(a.chunks), std::move(a.joints), pieces};
      st_.archived_pieces += pieces;
    }
  // the articulations (their ids as they were), and those archived out of range
  for (ArticulationSaved& a : s.articulations) restore_articulation(std::move(a));
  if (strm_.source)
    for (SessionDelta::ArchivedArticulation& a : s.archived_articulations) {
      if (a.chunks.empty()) continue;
      archive_record(a.key, a.record, region_of(a.chunks.front()));
      if (!strm_.archive->has(a.key)) continue;
      strm_.archived_arts[a.key] = std::move(a.chunks);
      ++st_.archived_articulations;
    }
}

// ---------------------------------------------------------------------------------------------
// Pieces out of range (a streamed world)

void World::Impl::archive_group(const std::vector<i64>& ids, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of) {
  std::vector<const Body*> bodies;
  for (i64 id : ids)
    if (const Body* b = rigid_.find(id); b && b->announced) bodies.push_back(b);
  if (bodies.empty()) return;
  // (its joints: those with an end on one of its pieces - a group is closed under joints)
  std::vector<size_t> js;
  std::vector<JointId> jids;
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    if (rigid_.joints[k].broken) continue;
    bool in = false;
    for (const JointRec::End* E : {&att_.joints[k].a, &att_.joints[k].b})
      in = in || (E->piece > 0 && std::binary_search(ids.begin(), ids.end(), E->piece));
    if (!in) continue;
    js.push_back(k);
    jids.push_back(rigid_.joints[k].id);
  }
  // (and the wheels on its pieces: an assembly goes whole)
  std::vector<size_t> ws;
  for (size_t k = 0; k < rigid_.wheels.size(); ++k)
    if (!rigid_.wheels[k].broken && att_.wheels[k].mount.piece > 0 && std::binary_search(ids.begin(), ids.end(), att_.wheels[k].mount.piece)) ws.push_back(k);
  std::vector<u8> rec;
  rec.push_back(kGroupVersion);
  write_group(rec, bodies, js, ws);
  // (the chunks that must be resident for it to come back: its pieces' and its joints' anchors')
  std::vector<u64> chunks;
  const IVec3 lo = strm_.source->chunk_lo(), hi = strm_.source->chunk_hi();
  auto add = [&](u64 k) {
    const IVec3 c = unkey3(k);
    for (int a = 0; a < 3; ++a)
      if (c[a] < lo[a] || c[a] >= hi[a]) return;
    chunks.push_back(k);
  };
  for (const Body* b : bodies) chunks_of(*b, add);
  for (size_t k : js)
    for (const JointRec::End* E : {&att_.joints[k].a, &att_.joints[k].b})
      if (E->kind == JointAnchor::Kind::Grid && E->piece == 0 && E->grid == kWorldGrid) {
        const IVec3 c = chunk_of(E->voxel);
        add(key3(c[0], c[1], c[2]));
      }
  std::sort(chunks.begin(), chunks.end());
  chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
  const V3 at = bodies.front()->x;
  const IVec3 home = chunk_of(world_detail::voxel_of(at, grid_.h));
  const u64 key = (3ull << 62) | static_cast<u64>(bodies.front()->id);
  archive_record(key, rec, region_of(key3(home[0], home[1], home[2])));
  if (!strm_.archive->has(key)) ++st_.forgotten_pieces;  // (no room at all: it is gone)
  else {
    for (JointId j : jids) strm_.archived_joints.insert(j);
    strm_.archived_groups[key] = ArchivedGroup{std::move(chunks), jids, static_cast<u32>(bodies.size())};
    st_.archived_pieces += static_cast<i64>(bodies.size());
  }
  // (out of the simulation: its joints and wheels quietly, its pieces as unloaded)
  for (size_t q = ws.size(); q-- > 0;) {
    att_.wheels.erase(att_.wheels.begin() + static_cast<std::ptrdiff_t>(ws[q]));
    rigid_.wheels.erase(rigid_.wheels.begin() + static_cast<std::ptrdiff_t>(ws[q]));
  }
  for (size_t q = js.size(); q-- > 0;) {
    att_.joints.erase(att_.joints.begin() + static_cast<std::ptrdiff_t>(js[q]));
    rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(js[q]));
  }
  std::vector<i64> gone;
  for (const Body* b : bodies) gone.push_back(b->id);
  remove_bodies(gone, PieceEnd::Unloaded);
}

void World::Impl::restore_groups() {
  if (strm_.archived_groups.empty()) return;
  std::vector<u64> ready;
  for (const auto& [key, g] : strm_.archived_groups)
    if (std::all_of(g.chunks.begin(), g.chunks.end(), [&](u64 c) { return strm_.generated.count(c) > 0; })) ready.push_back(key);
  for (u64 key : ready) {
    const auto it = strm_.archived_groups.find(key);
    for (JointId j : it->second.joints) strm_.archived_joints.erase(j);
    st_.archived_pieces -= it->second.pieces;
    strm_.archived_groups.erase(it);
    const std::vector<u8> rec = strm_.archive->get(key);
    strm_.archive->erase(key);
    Rd in{rec};
    SessionDelta s;
    const u8 gv = in.u8_();
    if (gv < 1 || gv > kGroupVersion || !read_group(in, &s, gv)) continue;  // (checked when archived: never)
    add_group(s);
  }
}

void World::Impl::forget_group(u64 key) {
  if (key & (1ull << 61)) {
    forget_articulation(key);  // (an articulation's record, not a group's)
    return;
  }
  const auto it = strm_.archived_groups.find(key);
  if (it == strm_.archived_groups.end()) return;
  // (its pieces are gone for good; its source's joints are made again with its grids)
  for (JointId j : it->second.joints) strm_.archived_joints.erase(j);
  st_.archived_pieces -= it->second.pieces;
  st_.forgotten_pieces += it->second.pieces;
  strm_.archived_groups.erase(it);
}

}  // namespace svx
