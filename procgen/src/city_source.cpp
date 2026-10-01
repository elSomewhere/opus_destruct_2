// structvox procgen — the city generator as a game's world (svx/procgen/city_source.hpp).
#include "svx/procgen/city_source.hpp"

#include <algorithm>
#include <cmath>

#define JSON_NOEXCEPTION 1
#include "json.hpp"
#include "svx/procgen/city_materials.hpp"

namespace svx {

namespace {

template <class T>
i64 vec_bytes(const std::vector<T>& v) {
  return static_cast<i64>(v.capacity() * sizeof(T));
}

i64 chunk_bytes(const city::ChunkData& d) {
  return vec_bytes(d.vox) + vec_bytes(d.look) + vec_bytes(d.water) + vec_bytes(d.seams) + static_cast<i64>(sizeof(d));
}

std::array<int, 3> arr(const IVec3& v) { return {v[0], v[1], v[2]}; }

}  // namespace

CityWorldSource::CityWorldSource(std::shared_ptr<const city::Export> e) : export_(std::move(e)) {
  register_city_materials();
  appearances_ = city_appearances();
  roads_ = make_city_roads(export_->world());
  std::array<int, 3> lo{}, hi{};
  export_->extent(lo, hi);
  lo_ = IVec3{lo[0], lo[1], lo[2]};
  hi_ = IVec3{hi[0], hi[1], hi[2]};
  std::array<double, 3> p{}, d{};
  export_->spawn(p, d);
  spawn_pos_ = V3{p[0], p[1], p[2]};
  spawn_dir_ = V3{d[0], d[1], d[2]};
}

std::shared_ptr<const city::ChunkData> CityWorldSource::chunk_data(const IVec3& c) const {
  const u64 k = key3(c[0], c[1], c[2]);
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = recent_at_.find(k);
    if (it != recent_at_.end()) {
      recent_.splice(recent_.begin(), recent_, it->second);
      return it->second->second;
    }
  }
  // (made outside the lock: another thread asking for the same chunk makes the same one)
  auto d = std::make_shared<city::ChunkData>();
  export_->chunk(c[0], c[1], c[2], *d);
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = recent_at_.find(k);
  if (it != recent_at_.end()) return it->second->second;
  recent_.emplace_front(k, d);
  recent_at_[k] = recent_.begin();
  while (recent_.size() > kRecent) {
    recent_at_.erase(recent_.back().first);
    recent_.pop_back();
  }
  return d;
}

bool CityWorldSource::generate(const IVec3& c, std::vector<Vox>& out) const {
  const std::shared_ptr<const city::ChunkData> d = chunk_data(c);
  if (!d->any || d->vox.empty()) return false;
  out.assign(d->vox.begin(), d->vox.end());
  return true;
}

void CityWorldSource::column_range(i32 cx, i32 cy, i32* z_lo, i32* z_hi, Vox* below) const {
  uint8_t b = 0;
  export_->column_range(cx, cy, z_lo, z_hi, &b);
  *below = static_cast<Vox>(b);
}

bool CityWorldSource::generate_seams(const IVec3& c, std::vector<u8>& out) const {
  const std::shared_ptr<const city::ChunkData> d = chunk_data(c);
  if (d->seams.empty()) return false;
  out.assign(d->seams.begin(), d->seams.end());
  return true;
}

u64 CityWorldSource::region(const IVec3& c) const { return export_->region(c[0], c[1]); }

bool CityWorldSource::generate_layer(const IVec3& c, const std::string& layer, std::vector<u8>& out) const {
  if (layer != "look" && layer != "water") return false;
  const std::shared_ptr<const city::ChunkData> d = chunk_data(c);
  const std::vector<uint8_t>& v = layer == "look" ? d->look : d->water;
  if (v.empty()) return false;
  out.assign(v.begin(), v.end());
  return true;
}

std::vector<SourceGrid> CityWorldSource::grids(const IVec3& c) const {
  std::vector<city::GridInfo> gs;
  export_->grids(c[0], c[1], c[2], gs);
  std::vector<SourceGrid> out;
  out.reserve(gs.size());
  std::lock_guard<std::mutex> lock(mu_);
  for (const city::GridInfo& g : gs) {
    SourceGrid s;
    s.id = g.id;
    s.origin = V3{g.origin[0], g.origin[1], g.origin[2]};
    s.rot = Quat{g.rot[0], g.rot[1], g.rot[2], g.rot[3]};
    s.voxel_size = g.voxel_size;
    s.priority = g.priority;
    out.push_back(s);
    const auto it = home_at_.find(g.id);
    if (it != home_at_.end()) homes_.erase(it->second);
    homes_.emplace_front(g.id, IVec3{g.home[0], g.home[1], g.home[2]});
    home_at_[g.id] = homes_.begin();
    while (homes_.size() > kHomes) {
      home_at_.erase(homes_.back().first);
      homes_.pop_back();
    }
  }
  return out;
}

bool CityWorldSource::generate_grid(u32 id, VoxelGrid& out) const {
  IVec3 near{0, 0, 0};
  {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = home_at_.find(id);
    if (it != home_at_.end()) near = it->second->second;
  }
  city::GridInfo info;
  std::vector<city::GridChunk> chunks;
  if (!export_->grid(id, arr(near), &info, chunks) || chunks.empty()) return false;
  out.h = info.voxel_size;
  const int look = out.add_layer({"look", true, LayerBind::Solid});
  bool any = false;
  for (city::GridChunk& gc : chunks) {
    const IVec3 cc{gc.c[0], gc.c[1], gc.c[2]};
    if (gc.vox.size() != static_cast<size_t>(kChunkVox)) continue;
    bool solid = false;
    for (u8 v : gc.vox)
      if (v) {
        solid = true;
        break;
      }
    if (!solid) continue;
    any = true;
    out.insert_chunk(cc, std::vector<Vox>(gc.vox.begin(), gc.vox.end()));
    if (gc.look.size() == static_cast<size_t>(kChunkVox)) out.set_layer_values(look, cc, gc.look.data());
    if (gc.seams.size() == static_cast<size_t>(kChunkVox)) out.install_seams(cc, gc.seams);
  }
  if (any) out.compact();
  return any;
}

i64 CityWorldSource::memory_bytes() const {
  i64 b = export_->memory_bytes();
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& [k, d] : recent_) b += chunk_bytes(*d);
  b += static_cast<i64>(homes_.size() * (sizeof(std::pair<u32, IVec3>) + 48));
  return b;
}

void CityWorldSource::spawns_in(const V3& lo, const V3& hi, std::vector<SpawnRecord>& out) const {
  std::vector<city::SpawnInfo> rs;
  export_->spawns_in({lo.x, lo.y}, {hi.x, hi.y}, rs);
  for (const city::SpawnInfo& r : rs) {
    SpawnRecord s;
    s.id = r.id;
    s.kind = r.kind;
    s.pos = V3{r.pos[0], r.pos[1], r.pos[2]};
    s.yaw = r.yaw;
    s.context = r.context;
    out.push_back(std::move(s));
  }
}

void CityWorldSource::fire_materials(FireSystem& fire) const { set_city_fire_materials(fire); }

bool CityWorldSource::coarse(const IVec3& lo, const IVec3& n, i32 factor, std::vector<Vox>& out) const {
  std::vector<uint8_t> v;
  if (!export_->coarse(arr(lo), arr(n), factor, v)) return false;
  out.assign(v.begin(), v.end());
  return true;
}

bool CityWorldSource::coarse_water(const IVec3& lo, const IVec3& n, i32 factor, std::vector<i32>& out) const {
  std::vector<int32_t> v;
  if (!export_->coarse_water(arr(lo), arr(n), factor, v)) return false;
  out.assign(v.begin(), v.end());
  return true;
}

std::shared_ptr<CityWorldSource> make_city_world(const std::string& params_json, u64 seed, std::string* error) {
  using Json = nlohmann::json;
  const Json p = Json::parse(params_json.empty() ? std::string("{}") : params_json, nullptr, false);
  if (p.is_discarded() || !p.is_object()) {
    if (error) *error = "city parameters: not a JSON object";
    return nullptr;
  }
  city::WorldSpec spec;
  auto str = [&](const char* k, std::string* out) {
    if (!p.contains(k)) return true;
    if (!p[k].is_string()) {
      if (error) *error = std::string("city parameters: ") + k + ": expected a string";
      return false;
    }
    *out = p[k].get<std::string>();
    return true;
  };
  if (!str("preset", &spec.preset) || !str("size", &spec.size) || !str("season", &spec.season)) return nullptr;
  if (p.contains("config")) {
    if (!p["config"].is_object()) {
      if (error) *error = "city parameters: config: expected an object";
      return nullptr;
    }
    spec.overrides_json = p["config"].dump();
  }
  // (voxel_city's seed is a number: a preset's seed, kept within what a double holds exactly)
  spec.seed = static_cast<double>(seed & ((u64{1} << 53) - 1));
  std::shared_ptr<const city::Export> e = city::Export::make(spec, error);
  if (!e) return nullptr;
  return std::make_shared<CityWorldSource>(std::move(e));
}

}  // namespace svx
