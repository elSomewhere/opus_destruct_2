// structvox procgen — procgen/district.hpp: the district file (tools/procgen_ref/district.mjs,
// format "SVXD" 2) and its source.
#include "svx/procgen/district.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

namespace svx {

namespace {

struct Reader {
  std::vector<char> b;
  size_t p = 0;
  bool ok = true;
  bool need(size_t n) {
    if (p + n > b.size()) ok = false;
    return ok;
  }
  template <class T>
  T get() {
    T v{};
    if (!need(sizeof(T))) return v;
    std::memcpy(&v, b.data() + p, sizeof(T));
    p += sizeof(T);
    return v;
  }
  std::string str() {
    const u32 n = get<u32>();
    if (!need(n)) return {};
    std::string s(b.data() + p, n);
    p += n;
    return s;
  }
  // a run-length encoded 32^3 array: (value, LEB128 run) pairs
  std::vector<u8> rle() {
    const u32 n = get<u32>();
    std::vector<u8> out;
    if (!need(n)) return out;
    out.reserve(kChunkVox);
    const size_t end = p + n;
    while (p < end && ok) {
      const u8 v = static_cast<u8>(b[p++]);
      u32 run = 0;
      int shift = 0;
      for (;;) {
        if (p >= end) {
          ok = false;
          break;
        }
        const u8 c = static_cast<u8>(b[p++]);
        run |= static_cast<u32>(c & 127) << shift;
        shift += 7;
        if (!(c & 128)) break;
      }
      if (out.size() + run > static_cast<size_t>(kChunkVox)) {
        ok = false;
        break;
      }
      out.insert(out.end(), run, v);
    }
    if (out.size() != static_cast<size_t>(kChunkVox)) ok = false;
    return out;
  }
  std::vector<u8> layer() { return get<u8>() ? rle() : std::vector<u8>{}; }
  IVec3 ivec() {
    IVec3 c;
    for (int a = 0; a < 3; ++a) c[a] = get<i32>();
    return c;
  }
  V3 v3() {
    V3 v;
    v.x = get<f64>();
    v.y = get<f64>();
    v.z = get<f64>();
    return v;
  }
};

u64 k3(const IVec3& c) { return key3(c[0], c[1], c[2]); }

}  // namespace

bool read_district(const std::string& path, District* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  Reader r;
  r.b.assign(std::istreambuf_iterator<char>(f), {});
  if (r.b.size() < 8 || std::memcmp(r.b.data(), "SVXD", 4) != 0) return false;
  r.p = 4;
  if (r.get<u32>() != 2) return false;
  District d;
  d.h = r.get<f64>();
  const u32 nm = r.get<u32>();
  for (u32 i = 0; i < nm && r.ok; ++i) {
    District::Mat m;
    m.id = r.get<i32>();
    m.m.name = r.str();
    m.m.E = r.get<f64>();
    m.m.G = r.get<f64>();
    m.m.rho = r.get<f64>();
    m.m.ft = r.get<f64>();
    m.m.fb = r.get<f64>();
    m.m.fc = r.get<f64>();
    m.m.cohesion = r.get<f64>();
    m.m.friction = r.get<f64>();
    m.m.Gf = r.get<f64>();
    m.m.frag_x = r.get<f64>();
    m.m.frag_y = r.get<f64>();
    m.m.frag_z = r.get<f64>();
    m.m.frag_noise = r.get<f64>();
    d.mats.push_back(m);
  }
  d.lo = r.ivec();
  d.hi = r.ivec();
  d.focus = r.v3();
  const u32 ncol = r.get<u32>();
  for (u32 i = 0; i < ncol && r.ok; ++i) {
    District::Column c;
    c.cx = r.get<i32>();
    c.cy = r.get<i32>();
    c.z_lo = r.get<i32>();
    c.z_hi = r.get<i32>();
    c.region = r.get<u64>();
    d.columns.push_back(c);
  }
  const u32 nc = r.get<u32>();
  for (u32 i = 0; i < nc && r.ok; ++i) {
    const IVec3 c = r.ivec();
    District::ChunkRec rec;
    rec.vox = r.rle();
    rec.look = r.layer();
    rec.flora = r.layer();
    rec.water = r.layer();
    const u32 np = r.get<u32>();
    for (u32 k = 0; k < np && r.ok; ++k) {
      District::Prop p;
      p.index = r.get<u32>();
      p.vox = r.get<u8>();
      p.look = r.get<u8>();
      if (p.index >= static_cast<u32>(kChunkVox)) r.ok = false;
      rec.props.push_back(p);
    }
    d.chunks[k3(c)] = std::move(rec);
  }
  const u32 ng = r.get<u32>();
  for (u32 i = 0; i < ng && r.ok; ++i) {
    District::Grid g;
    g.g.id = r.get<u32>();
    g.g.origin = r.v3();
    g.g.rot.x = r.get<f64>();
    g.g.rot.y = r.get<f64>();
    g.g.rot.z = r.get<f64>();
    g.g.rot.w = r.get<f64>();
    g.g.voxel_size = r.get<f64>();
    g.g.priority = r.get<i32>();
    g.anchored = r.get<u8>() != 0;
    g.home = r.ivec();
    g.kind = r.str();
    const u32 n = r.get<u32>();
    for (u32 k = 0; k < n && r.ok; ++k) {
      District::GridChunk c;
      c.c = r.ivec();
      c.vox = r.rle();
      c.look = r.layer();
      c.flora = r.layer();
      g.chunks.push_back(std::move(c));
    }
    d.grids.push_back(std::move(g));
  }
  const u32 nl = r.get<u32>();
  for (u32 i = 0; i < nl && r.ok; ++i) {
    const V3 a = r.v3();
    const V3 b = r.v3();
    d.lanes.push_back({a, b});
  }
  const u32 nt = r.get<u32>();
  for (u32 i = 0; i < nt && r.ok; ++i) d.touches.push_back(r.v3());
  const u32 ncl = r.get<u32>();
  for (u32 i = 0; i < ncl && r.ok; ++i) {
    District::Class c;
    c.id = r.get<i32>();
    c.name = r.str();
    const u32 nl2 = r.get<u32>();
    for (u32 k = 0; k < nl2 && r.ok; ++k) c.looks.push_back(r.str());
    d.classes.push_back(std::move(c));
  }
  const u32 nf = r.get<u32>();
  for (u32 i = 0; i < nf && r.ok; ++i) d.flora.push_back(r.str());
  if (!r.ok || r.p != r.b.size()) return false;
  *out = std::move(d);
  return true;
}

DistrictSource::DistrictSource(std::shared_ptr<const District> d, DistrictOptions o) : d_(std::move(d)), o_(o) {
  for (size_t i = 0; i < d_->grids.size(); ++i) by_home_[k3(d_->grids[i].home)].push_back(i);
  for (size_t i = 0; i < d_->columns.size(); ++i) columns_[key3(d_->columns[i].cx, d_->columns[i].cy, 0)] = i;
  for (const District::Class& c : d_->classes)
    if (c.name == "foliage") foliage_ = c.id;
}

bool DistrictSource::generate(const IVec3& c, std::vector<Vox>& out) const {
  auto it = d_->chunks.find(k3(c));
  if (it == d_->chunks.end()) return false;
  const District::ChunkRec& r = it->second;
  out = r.vox;
  bool any = false;
  for (Vox v : out)
    if (v) {
      any = true;
      break;
    }
  if (o_.props == DistrictOptions::Props::Structure)
    for (const District::Prop& p : r.props)
      if (!out[p.index]) {
        out[p.index] = p.vox;
        any = true;
      }
  if (o_.flora && foliage_ >= 0 && !r.flora.empty())
    for (size_t i = 0; i < r.flora.size(); ++i)
      if (r.flora[i] && !out[i]) {
        out[i] = make_vox(static_cast<MaterialId>(foliage_), false);
        any = true;
      }
  return any;
}

void DistrictSource::column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const {
  auto it = columns_.find(key3(cx, cy, 0));
  if (it == columns_.end()) {
    *z_lo = *z_hi = d_->lo[2];
    *below = kAir;
    return;
  }
  const District::Column& c = d_->columns[it->second];
  *z_lo = c.z_lo;
  *z_hi = c.z_hi + 1;
  *below = make_vox(MaterialId::Rock, true);
}

u64 DistrictSource::region(const IVec3& c) const {
  auto it = columns_.find(key3(c[0], c[1], 0));
  if (it == columns_.end()) return ChunkSource::region(c);
  return d_->columns[it->second].region;
}

bool DistrictSource::generate_layer(const IVec3& c, const std::string& layer, std::vector<u8>& out) const {
  auto it = d_->chunks.find(k3(c));
  if (it == d_->chunks.end()) return false;
  const District::ChunkRec& r = it->second;
  if (layer == "water" && !r.water.empty()) {
    out = r.water;
    return true;
  }
  if (layer == "look") {
    bool any = !r.look.empty();
    if (any) out = r.look;
    if (o_.props == DistrictOptions::Props::Structure && !r.props.empty()) {
      if (!any) out.assign(kChunkVox, 0);
      for (const District::Prop& p : r.props) out[p.index] = p.look;
      any = true;
    }
    return any;
  }
  return false;
}

std::vector<SourceGrid> DistrictSource::grids(const IVec3& c) const {
  std::vector<SourceGrid> out;
  auto it = by_home_.find(k3(c));
  if (it != by_home_.end())
    for (size_t i : it->second) out.push_back(d_->grids[i].g);
  return out;
}

bool DistrictSource::generate_grid(u32 id, VoxelGrid& out) const {
  for (const District::Grid& g : d_->grids) {
    if (g.g.id != id) continue;
    out.h = d_->h;
    i64 n = 0;
    for (const District::GridChunk& c : g.chunks)
      for (i32 x = 0; x < kChunk; ++x)
        for (i32 y = 0; y < kChunk; ++y)
          for (i32 z = 0; z < kChunk; ++z) {
            const size_t i = static_cast<size_t>((x * kChunk + y) * kChunk + z);
            Vox q = c.vox[i];
            if (!q && o_.flora && foliage_ >= 0 && !c.flora.empty() && c.flora[i]) q = make_vox(static_cast<MaterialId>(foliage_), false);
            if (q) {
              out.set(c.c[0] * kChunk + x, c.c[1] * kChunk + y, c.c[2] * kChunk + z, q);
              ++n;
            }
          }
    out.compact();
    return n > 0;
  }
  return false;
}

std::vector<VoxelEdit> district_props(const District& d, const IVec3& c) {
  std::vector<VoxelEdit> out;
  auto it = d.chunks.find(k3(c));
  if (it == d.chunks.end()) return out;
  for (const District::Prop& p : it->second.props) {
    const i32 i = static_cast<i32>(p.index);
    VoxelEdit e;
    e.p = IVec3{c[0] * kChunk + (i >> 10), c[1] * kChunk + ((i >> 5) & 31), c[2] * kChunk + (i & 31)};
    e.v = p.vox;
    out.push_back(e);
  }
  return out;
}

}  // namespace svx
