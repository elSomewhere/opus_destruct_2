// svx_city — terrain/terrain.hpp (voxel_city terrain/terrain.js).
#include "terrain/terrain.hpp"

#include <array>
#include <cstring>

#include "core/hash.hpp"
#include "core/js.hpp"
#include "core/math.hpp"
#include "nature/biomes.hpp"
#include "world/fields.hpp"
#include "world/island.hpp"

namespace svx::city {

namespace {

// A config value read as a JS default parameter reads it (only undefined takes the default).
double arg(const Value& v, double def) { return v.is_undefined() ? def : v.to_number(); }

}  // namespace

TerrainCfg::TerrainCfg(const Value& t) {
  lowland_base = t["lowlandBase"].to_number();
  continent_scale = t["continentScale"].to_number();
  continent_amplitude = t["continentAmplitude"].to_number();
  hill_amplitude = t["hillAmplitude"].to_number();
  hill_scale = t["hillScale"].to_number();
  detail_amplitude = t["detailAmplitude"].to_number();
  detail_scale = t["detailScale"].to_number();
  mountain_uplift = t["mountainUplift"].to_number();
  mountain_base = t["mountainBase"].num(400);
  mountain_height = t["mountainHeight"].to_number();
  mountain_scale = t["mountainScale"].to_number();
  mountain_detail = t["mountainDetail"].to_number();
  mountain_detail_scale = t["mountainDetailScale"].to_number();
  valley_scale = t["valleyScale"].to_number();
  valley_depth = t["valleyDepth"].to_number();
  mountain_gain = arg(t["mountainGain"], 0.5);
  gully_depth = t["gullyDepth"].num(50);
  plateau_height = t["plateauHeight"].to_number();
  mesa_step = t["mesaStep"].to_number();
  canyon_scale = t["canyonScale"].to_number();
  canyon_depth = t["canyonDepth"].to_number();
  ravine_scale = t["ravineScale"].to_number();
  ravine_depth = t["ravineDepth"].to_number();
  rugged = t["rugged"].num(0);
  relief_knoll_scale = t["reliefKnollScale"].num(170);
  relief_hummock_scale = t["reliefHummockScale"].num(22);
  relief_knoll = t["reliefKnoll"].num(7);
  relief_hummock = t["reliefHummock"].num(1.3);
  relief_bump = t["reliefBump"].num(0.35);
  creek_scale = t["creekScale"].num(900);
  city_relief = t["cityRelief"].to_number();
  city_relief_scale = t["cityReliefScale"].to_number();
}

TerrainCtx::TerrainCtx(const Terrain& t)
    : terrain(&t), island(t.fields().island.get()), cfg(&t.cfg()), torus_r(t.chart().R) {}

double TerrainCtx::cliff() {
  if (cliff_ < 0) cliff_ = island->cliff(x * kVoxelSize, y * kVoxelSize);
  return cliff_;
}

void TerrainCtx::climate(double* t, double* m) {
  if (!clim_) {
    clim_t = terrain->fields().temperature(x, y);
    clim_m = terrain->fields().moisture(x, y);
    clim_ = true;
  }
  *t = clim_t;
  *m = clim_m;
}

double TerrainCtx::desert() {
  if (desert_ < 0) {
    double t, m;
    climate(&t, &m);
    desert_ = desertness(t, m);
  }
  return desert_;
}

FieldPoint TerrainCtx::to_field(double xm, double ym) const { return terrain->chart().to_field(xm, ym); }

Terrain::Terrain(const Value& config, std::shared_ptr<const Chart> chart, std::shared_ptr<const MacroFields> fields)
    : config_(config),
      chart_(std::move(chart)),
      fields_(std::move(fields)),
      cfg_(config["terrain"]),
      n_city_(derive_seed(config["seed"].to_number(), "terrain.city")),
      token_(std::make_shared<const char>(0)) {
  const double seed = config["seed"].to_number();
  sea_level = config["world"]["seaLevel"].to_number() * kVoxelsPerMeter;
  std::vector<const Landform*> sorted;
  for (const Landform& l : landforms().all()) sorted.push_back(&l);
  js::sort(sorted, [](const Landform* a, const Landform* b) { return a->order - b->order; });
  for (const Landform* l : sorted) {
    LandformState st;
    for (const std::string& name : l->noises) st.n.emplace_back(derive_seed(seed, "terrain." + name));
    forms_.push_back({l, std::move(st)});
  }
}

Terrain::~Terrain() = default;

size_t LandformMemo::CellHash::operator()(const Cell& c) const {
  uint64_t a, b;
  const double i = c.i + 0.0, j = c.j + 0.0;  // (-0 and 0 alike)
  std::memcpy(&a, &i, 8);
  std::memcpy(&b, &j, 8);
  uint64_t h = a * 0x9e3779b97f4a7c15ull;
  h ^= b + 0x632be59bd9b4e019ull + (h << 6) + (h >> 2);
  return static_cast<size_t>(h ^ (h >> 31));
}

LandformMemo& Terrain::memo() const {
  // This thread's memos, a few Terrains' (the least recently used recycled). A slot belongs to a
  // Terrain while it holds that Terrain's token (a weak reference: a Terrain made later at the
  // same address is another). The memos hold pure values only: recycling one costs time, never
  // changes a result.
  struct Slot {
    std::weak_ptr<const char> owner;
    std::unique_ptr<LandformMemo> m;
    uint64_t used = 0;
  };
  thread_local std::array<Slot, 4> slots;
  thread_local uint64_t tick = 0;
  ++tick;
  for (Slot& s : slots)
    if (s.m && !s.owner.owner_before(token_) && !token_.owner_before(s.owner)) {
      s.used = tick;
      return *s.m;
    }
  Slot* pick = &slots[0];
  for (Slot& s : slots)
    if (!s.m || (pick->m && s.used < pick->used)) pick = &s;
  pick->owner = token_;
  pick->m = std::make_unique<LandformMemo>();
  pick->used = tick;
  return *pick->m;
}

double Terrain::natural(TerrainCtx& c, double x, double y, double u, double fx, double fy, double fz, std::optional<double> prox,
                        double fw) const {
  c.x = x;
  c.y = y;
  c.fx = fx;
  c.fy = fy;
  c.fz = fz;
  c.fw = fw;
  c.u = u;
  c.h = 0;
  c.lowland = 0;
  c.ridge = 0;
  c.canyon = 0;
  c.ravine = 0;
  c.outcrop = 0;
  c.valley = 0;
  c.rough = 0;
  c.channel = 0;
  c.rugged_ = -1;
  c.stream.reset();
  c.clim_ = false;
  c.desert_ = -1;
  c.cliff_ = -1;
  // mountains fade out towards towns (foothills around each one)
  const double m = fields_->mountainness(x, y);
  c.prox = prox ? *prox : fields_->settlement_proximity(x, y);
  c.mountain = m > 0 ? m * (1 - c.prox) : 0;
  if (c.island) {
    // on an island the fells plunge into the sea: steep fjord walls and sea cliffs where the coast
    // is rocky, a long slope down to a beach elsewhere
    c.coast = c.island->coast(x * kVoxelSize, y * kVoxelSize);
    if (c.mountain > 0) c.mountain *= smoothstep(0, 500 + 1100 * (1 - c.cliff()), c.coast);
  }
  for (const Form& f : forms_) f.lf->apply(c, f.st);
  return c.h;
}

double Terrain::natural(double x, double y, double u, double fx, double fy, double fz, std::optional<double> prox, double fw) const {
  TerrainCtx c(*this);
  return natural(c, x, y, u, fx, fy, fz, prox, fw);
}

double Terrain::settlement_base(const Settlement& s) const {
  return s.base_h.get([&] {
    // (in a context of its own: the sample that first needs it keeps its own; docs/CITY.md §6)
    TerrainCtx c(*this);
    const FieldPoint f = chart_->to_field(s.x * kVoxelSize, s.y * kVoxelSize);
    natural(c, s.x, s.y, 1, f.x, f.y, f.z, std::nullopt, f.w);
    return js::max(2.0, c.lowland);
  });
}

double Terrain::city_meters(double fx, double fy, double fz, double fw, const Urban& ur) const {
  const TerrainCfg& t = cfg_;
  double base = 6;
  if (!ur.parts.empty()) {
    double sw = 0;
    double sb = 0;
    for (const UrbanPart& p : ur.parts) {
      sw += p.w;
      sb += p.w * settlement_base(*p.s);
    }
    base = sb / sw;
  } else if (ur.settlement) {
    base = settlement_base(*ur.settlement);
  }
  return base + t.city_relief * n_city_.fbmP(fx / t.city_relief_scale, fy / t.city_relief_scale, fz / t.city_relief_scale, fw / t.city_relief_scale, 2);
}

TerrainSample Terrain::sample(double x, double y, const Urban* urban, bool raw) const {
  Urban own;
  if (!urban) {
    own = fields_->urban(x, y);
    urban = &own;
  }
  const Urban& ur = *urban;
  const FieldPoint f = chart_->to_field(x * kVoxelSize, y * kVoxelSize);
  TerrainCtx c(*this);
  const double nat = natural(c, x, y, ur.u, f.x, f.y, f.z, ur.prox, f.w);
  const double mountain = c.mountain;
  const double canyon = c.canyon;
  const double ravine = c.ravine;
  const double outcrop = c.outcrop;
  const double rough = c.rough;
  // a landform's stream channel (canyon floor stream, ravine creek): only in open country
  const std::optional<Stream> stream = c.stream && ur.u < 0.06 ? c.stream : std::nullopt;
  double w = smoothstep(0.06, 0.32, ur.u);
  double city = w > 0 ? js::max(city_meters(f.x, f.y, f.z, f.w, ur), 2.0) : 0;
  if (w > 0 && c.island) {
    const double rise = c.island->town_rise;
    if (rise > 0) {
      // a town climbing the hillside from its harbour (Bergen): a waterfront 2.5 m above the sea
      // rising `townRise` m inland over about its radius, the town's own relief on top
      const TerrainCfg& t = cfg_;
      const double sR = js::max(250.0, (ur.settlement ? ur.settlement->radius : 4000) * kVoxelSize);
      const double relief =
          t.city_relief * n_city_.fbmP(f.x / t.city_relief_scale, f.y / t.city_relief_scale, f.z / t.city_relief_scale, f.w / t.city_relief_scale, 2);
      city = 2.5 + rise * (1 - js::exp(-js::max(0.0, c.coast) / (sR * 0.85))) + relief * smoothstep(20, 200, c.coast);
    } else {
      // an island town eases down to a waterfront 2.5 m above the sea within 350 m of the shore
      // and never fills the sea beyond it
      const double k = smoothstep(0, 350, c.coast);
      city = 2.5 + (city - 2.5) * k;
    }
    w *= smoothstep(-30, 4, c.coast);
  }
  double h = w > 0 ? lerp(nat, city, w) : nat;
  // harbour towns ease down to their lake (world hook; raw samples skip it)
  if (!raw && ur.prox > 0 && port_grade) h = port_grade(x, y, h);
  TerrainSample s;
  s.h = h * kVoxelsPerMeter;
  s.natural = nat * kVoxelsPerMeter;
  s.u = ur.u;
  s.core = ur.core;
  s.settlement = ur.settlement;
  s.grade = w;
  s.mountain = mountain;
  s.canyon = canyon;
  s.ravine = ravine;
  s.outcrop = ur.u < 0.08 ? outcrop : 0;
  s.stream = stream;
  s.rough = rough;
  s.rugged = c.rugged_ > 0 ? c.rugged_ : 0;
  s.coast = c.coast;
  return s;
}

double Terrain::height(double x, double y) const { return sample(x, y).h; }

}  // namespace svx::city
