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

constexpr u8 kPieceVersion = 1;
constexpr u32 kSessionMagic = 0x53534553;  // "SESS"
constexpr u8 kGroupVersion = 1;            // (an archived group's record)
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

std::vector<u8> World::piece_record(const Body& b) const {
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
    for (size_t L = 0; L < S.layer.size() && L < layer_specs_.size(); ++L) {
      if (S.layer[L].empty() || !layer_specs_[L].persistent) continue;
      const std::string& name = layer_specs_[L].name;
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

std::unique_ptr<Body> World::read_piece_record(const std::vector<u8>& rec) const {
  Rd in{rec};
  if (in.u8_() != kPieceVersion) return nullptr;
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

std::vector<u8> World::joint_record(size_t k) const {
  const Joint& j = rigid_.joints[k];
  const JointRec& r = jrecs_[k];
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

bool World::read_joint_record(Rd& in, JointRec* r, Joint* j) const {
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

void World::write_group(std::vector<u8>& out, const std::vector<const Body*>& bodies, const std::vector<size_t>& joints) const {
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
}

bool World::read_group(Rd& in, SessionDelta* s) const {
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
    if (!read_joint_record(in, &r, &j)) return false;
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
  return in.ok;
}

void World::add_group(SessionDelta& s) {
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
    bool ok = std::none_of(jrecs_.begin(), jrecs_.end(), [&](const JointRec& x) { return x.id == r.id; });
    for (const JointRec::End* E : {&r.a, &r.b})
      if (E->kind != JointAnchor::Kind::World)
        ok = ok && (E->piece > 0 ? rigid_.find(E->piece) != nullptr : E->piece == 0 && slot_of(E->grid) >= 0);
    if (!ok) continue;
    insert_joint(r, j);
    next_joint_ = std::max<JointId>(next_joint_, (j.id & 0x80000000u) ? next_joint_ : j.id + 1);
  }
  if (!jrecs_.empty()) update_joint_ends();
  announce_bodies();
}

std::vector<u8> World::session_entries() const {
  std::vector<u8> out;
  put32(out, kSessionMagic);
  put64(out, static_cast<u64>(steps_));
  put64(out, static_cast<u64>(next_id_));
  put32(out, next_joint_);
  // the pieces (announced: the ones the host knows), their joints (those of pieces not announced
  // yet are left out) and dead loads
  std::vector<const Body*> bodies;
  for (const auto& bp : rigid_.bodies)
    if (bp->announced) bodies.push_back(bp.get());
  std::vector<size_t> js;
  for (size_t k = 0; k < rigid_.joints.size(); ++k) {
    if (rigid_.joints[k].broken) continue;
    bool fresh = false;
    for (const JointRec::End* E : {&jrecs_[k].a, &jrecs_[k].b})
      if (E->piece > 0)
        if (const Body* b = rigid_.find(E->piece)) fresh = fresh || !b->announced;
    if (!fresh) js.push_back(k);
  }
  write_group(out, bodies, js);
  // the groups archived out of range (a streamed world's): as archived
  put32(out, static_cast<u32>(archived_groups_.size()));
  for (const auto& [key, g] : archived_groups_) {
    put64(out, key);
    put32(out, static_cast<u32>(g.chunks.size()));
    for (u64 c : g.chunks) put64(out, c);
    put32(out, static_cast<u32>(g.joints.size()));
    for (JointId j : g.joints) put32(out, j);
    const std::vector<u8> rec = archive_->get(key);
    put32(out, static_cast<u32>(rec.size()));
    out.insert(out.end(), rec.begin(), rec.end());
  }
  return out;
}

bool World::read_session(Rd& in, SessionDelta* s) const {
  if (in.u32_() != kSessionMagic) return false;
  s->steps = in.i64_();
  s->next_id = in.i64_();
  s->next_joint = in.u32_();
  if (!in.ok || s->steps < 0 || s->next_id < 1) return false;
  if (!read_group(in, s)) return false;
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
    if (rin.u8_() != kGroupVersion || !read_group(rin, &probe) || rin.p != a.record.size()) return false;
    s->archived.push_back(std::move(a));
  }
  return in.ok;
}

void World::apply_session(SessionDelta&& s) {
  // (the saved session's pieces and joints take the place of the ones there are: the level's
  // joints, made again by the host, are the saved ones now)
  std::vector<i64> ids;
  for (const auto& bp : rigid_.bodies) ids.push_back(bp->id);
  remove_bodies(ids, PieceEnd::Removed);
  pending_add_.clear();
  pending_retire_.clear();
  dead_loads_.clear();
  jrecs_.clear();
  rigid_.joints.clear();
  next_id_ = std::max(next_id_, s.next_id);
  next_joint_ = std::max<JointId>(next_joint_, s.next_joint);
  steps_ = s.steps;
  add_group(s);
  // (a streamed world's groups out of range: archived again, as they were)
  for (const auto& [key, g] : archived_groups_) archive_->erase(key);
  archived_groups_.clear();
  archived_joints_.clear();
  st_.archived_pieces = 0;
  if (source_)
    for (SessionDelta::Archived& a : s.archived) {
      if (a.chunks.empty()) continue;
      archive_record(a.key, a.record, region_of(a.chunks.front()));
      if (!archive_->has(a.key)) continue;
      Rd rin{a.record};
      rin.u8_();
      const u32 pieces = rin.u32_();
      for (JointId j : a.joints) archived_joints_.insert(j);
      archived_groups_[a.key] = ArchivedGroup{std::move(a.chunks), std::move(a.joints), pieces};
      st_.archived_pieces += pieces;
    }
}

// ---------------------------------------------------------------------------------------------
// Pieces out of range (a streamed world)

void World::archive_group(const std::vector<i64>& ids, const std::function<void(const Body&, const std::function<void(u64)>&)>& chunks_of) {
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
    for (const JointRec::End* E : {&jrecs_[k].a, &jrecs_[k].b})
      in = in || (E->piece > 0 && std::binary_search(ids.begin(), ids.end(), E->piece));
    if (!in) continue;
    js.push_back(k);
    jids.push_back(rigid_.joints[k].id);
  }
  std::vector<u8> rec;
  rec.push_back(kGroupVersion);
  write_group(rec, bodies, js);
  // (the chunks that must be resident for it to come back: its pieces' and its joints' anchors')
  std::vector<u64> chunks;
  const IVec3 lo = source_->chunk_lo(), hi = source_->chunk_hi();
  auto add = [&](u64 k) {
    const IVec3 c = unkey3(k);
    for (int a = 0; a < 3; ++a)
      if (c[a] < lo[a] || c[a] >= hi[a]) return;
    chunks.push_back(k);
  };
  for (const Body* b : bodies) chunks_of(*b, add);
  for (size_t k : js)
    for (const JointRec::End* E : {&jrecs_[k].a, &jrecs_[k].b})
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
  if (!archive_->has(key)) ++st_.forgotten_pieces;  // (no room at all: it is gone)
  else {
    for (JointId j : jids) archived_joints_.insert(j);
    archived_groups_[key] = ArchivedGroup{std::move(chunks), jids, static_cast<u32>(bodies.size())};
    st_.archived_pieces += static_cast<i64>(bodies.size());
  }
  // (out of the simulation: its joints quietly, its pieces as unloaded)
  for (size_t q = js.size(); q-- > 0;) {
    jrecs_.erase(jrecs_.begin() + static_cast<std::ptrdiff_t>(js[q]));
    rigid_.joints.erase(rigid_.joints.begin() + static_cast<std::ptrdiff_t>(js[q]));
  }
  std::vector<i64> gone;
  for (const Body* b : bodies) gone.push_back(b->id);
  remove_bodies(gone, PieceEnd::Unloaded);
}

void World::restore_groups() {
  if (archived_groups_.empty()) return;
  std::vector<u64> ready;
  for (const auto& [key, g] : archived_groups_)
    if (std::all_of(g.chunks.begin(), g.chunks.end(), [&](u64 c) { return generated_.count(c) > 0; })) ready.push_back(key);
  for (u64 key : ready) {
    const auto it = archived_groups_.find(key);
    for (JointId j : it->second.joints) archived_joints_.erase(j);
    st_.archived_pieces -= it->second.pieces;
    archived_groups_.erase(it);
    const std::vector<u8> rec = archive_->get(key);
    archive_->erase(key);
    Rd in{rec};
    SessionDelta s;
    if (in.u8_() != kGroupVersion || !read_group(in, &s)) continue;  // (checked when archived: never)
    add_group(s);
  }
}

void World::forget_group(u64 key) {
  const auto it = archived_groups_.find(key);
  if (it == archived_groups_.end()) return;
  // (its pieces are gone for good; its source's joints are made again with its grids)
  for (JointId j : it->second.joints) archived_joints_.erase(j);
  st_.archived_pieces -= it->second.pieces;
  st_.forgotten_pieces += it->second.pieces;
  archived_groups_.erase(it);
}

}  // namespace svx
