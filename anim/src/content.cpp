#include "svx/anim/content.hpp"

#include <cmath>

#define JSON_NOEXCEPTION 1
#include "json.hpp"

namespace svx::anim {

namespace {

using Json = nlohmann::json;

constexpr const char* kTissueNames[kTissueCount] = {"soft", "flesh", "bone", "metal"};
constexpr const char* kHoldStyles[] = {"free", "aimed", "shouldered"};
constexpr const char* kImpacts[] = {"edge", "point", "blunt"};

Json rle(const std::vector<u8>& v) {
  Json out = Json::array();
  for (size_t i = 0; i < v.size();) {
    size_t j = i;
    while (j < v.size() && v[j] == v[i]) ++j;
    out.push_back(v[i]);
    out.push_back(j - i);
    i = j;
  }
  return out;
}
Json v3(const V3& v) { return Json::array({v.x, v.y, v.z}); }
Json quat(const Quat& q) { return Json::array({q.x, q.y, q.z, q.w}); }

// (typed reads that never throw: the first wrong field is the error)
struct Reader {
  std::string* error;
  bool ok = true;
  void fail(const std::string& what) {
    if (ok && error) *error = what;
    ok = false;
  }
  const Json* at(const Json& j, const char* key, bool required = true) {
    if (!j.is_object()) {
      fail("not an object");
      return nullptr;
    }
    const auto it = j.find(key);
    if (it == j.end()) {
      if (required) fail(std::string("missing \"") + key + "\"");
      return nullptr;
    }
    return &*it;
  }
  f64 number(const Json& j, const char* key, f64 fallback, f64 lo, f64 hi, bool required = false) {
    const Json* v = at(j, key, required);
    if (!v) return fallback;
    if (!v->is_number() || !std::isfinite(v->get<f64>()) || v->get<f64>() < lo || v->get<f64>() > hi) {
      fail(std::string("\"") + key + "\" out of range");
      return fallback;
    }
    return v->get<f64>();
  }
  std::string string(const Json& j, const char* key, const std::string& fallback = {}, bool required = false) {
    const Json* v = at(j, key, required);
    if (!v) return fallback;
    if (!v->is_string()) {
      fail(std::string("\"") + key + "\" is not text");
      return fallback;
    }
    return v->get<std::string>();
  }
  bool flag(const Json& j, const char* key, bool fallback) {
    const Json* v = at(j, key, false);
    if (!v) return fallback;
    if (!v->is_boolean()) {
      fail(std::string("\"") + key + "\" is not true or false");
      return fallback;
    }
    return v->get<bool>();
  }
  V3 vector(const Json& j, const char* key, const V3& fallback = {}) {
    const Json* v = at(j, key, false);
    if (!v) return fallback;
    if (!v->is_array() || v->size() != 3) {
      fail(std::string("\"") + key + "\" is not [x, y, z]");
      return fallback;
    }
    V3 out;
    for (int a = 0; a < 3; ++a) {
      if (!(*v)[size_t(a)].is_number() || !std::isfinite((*v)[size_t(a)].get<f64>())) fail(std::string("\"") + key + "\" is not finite");
      else out[a] = (*v)[size_t(a)].get<f64>();
    }
    return out;
  }
  Quat rotation(const Json& j, const char* key, const Quat& fallback = {}) {
    const Json* v = at(j, key, false);
    if (!v) return fallback;
    if (!v->is_array() || v->size() != 4) {
      fail(std::string("\"") + key + "\" is not [x, y, z, w]");
      return fallback;
    }
    f64 q[4];
    for (int a = 0; a < 4; ++a) {
      if (!(*v)[size_t(a)].is_number() || !std::isfinite((*v)[size_t(a)].get<f64>())) {
        fail(std::string("\"") + key + "\" is not finite");
        return fallback;
      }
      q[a] = (*v)[size_t(a)].get<f64>();
    }
    return Quat{q[0], q[1], q[2], q[3]};
  }
  std::vector<std::string> strings(const Json& j, const char* key) {
    std::vector<std::string> out;
    const Json* v = at(j, key, false);
    if (!v) return out;
    if (!v->is_array()) {
      fail(std::string("\"") + key + "\" is not a list");
      return out;
    }
    for (const Json& s : *v) {
      if (!s.is_string()) {
        fail(std::string("\"") + key + "\" holds a value that is not text");
        return {};
      }
      out.push_back(s.get<std::string>());
    }
    return out;
  }
  // a run-length coded per-cell array of `cells` values, each at most `max`
  std::vector<u8> runs(const Json& j, const char* key, size_t cells, u8 max, bool required) {
    const Json* v = at(j, key, required);
    if (!v) return {};
    std::vector<u8> out;
    if (!v->is_array() || v->size() % 2) {
      fail(std::string("\"") + key + "\" is not [value, run, ...]");
      return {};
    }
    for (size_t i = 0; i < v->size(); i += 2) {
      const Json &a = (*v)[i], &n = (*v)[i + 1];
      if (!a.is_number_unsigned() || !n.is_number_unsigned() || a.get<u64>() > max || n.get<u64>() > cells - out.size()) {
        fail(std::string("\"") + key + "\" holds a bad run");
        return {};
      }
      out.insert(out.end(), size_t(n.get<u64>()), u8(a.get<u64>()));
    }
    if (out.size() != cells) fail(std::string("\"") + key + "\" does not cover the part");
    return out;
  }
};

Json model_object(const VoxelModel& m, const HumanoidBuild* build) {
  Json j;
  j["format"] = "svx-anim-voxel-model";
  j["version"] = 2;
  j["name"] = m.name;
  j["rig"] = build ? "humanoid" : "prop";
  if (build) j["build"] = {{"height", build->height}, {"shoulders", build->shoulders}, {"hips", build->hips}, {"girth", build->girth}};
  j["voxel_size"] = m.voxel_size;
  Json tissues = Json::array();
  for (Tissue t : m.tissue) tissues.push_back(kTissueNames[size_t(t)]);
  j["tissues"] = tissues;
  Json parts = Json::array();
  for (const VoxelPart& p : m.parts) {
    Json q;
    q["bone"] = p.bone;
    q["origin"] = Json::array({p.origin[0], p.origin[1], p.origin[2]});
    q["dims"] = Json::array({p.dims[0], p.dims[1], p.dims[2]});
    q["cells"] = rle(p.cells);
    if (!p.shade.empty()) q["shade"] = rle(p.shade);
    if (!p.tissue.empty()) q["tissue"] = rle(p.tissue);
    parts.push_back(q);
  }
  j["parts"] = parts;
  return j;
}

ModelPtr model_from(const Json& j, Reader& r, HumanoidBuild* build_out) {
  if (r.string(j, "format", {}, true) != "svx-anim-voxel-model") r.fail("not a voxel model");
  if (r.number(j, "version", 0, 1, 2, true) != 2) r.fail("unknown voxel model version");
  const std::string rig = r.string(j, "rig", {}, true);
  HumanoidBuild build;
  SkeletonPtr sk;
  if (rig == "humanoid") {
    if (const Json* b = r.at(j, "build", false)) {
      build.height = r.number(*b, "height", 1, .5, 2);
      build.shoulders = r.number(*b, "shoulders", 1, .5, 2);
      build.hips = r.number(*b, "hips", 1, .5, 2);
      build.girth = r.number(*b, "girth", 1, .5, 2);
    }
    sk = humanoid_skeleton(build);
  } else if (rig == "prop") {
    sk = prop_skeleton();
  } else {
    r.fail("\"rig\" is neither humanoid nor prop");
  }
  const f64 s = r.number(j, "voxel_size", 0, 1e-4, 1, true);
  std::array<Tissue, kSlotCount> tissues = default_tissues();
  if (const Json* t = r.at(j, "tissues", false)) {
    if (!t->is_array() || t->size() != kSlotCount) r.fail("\"tissues\" is not one per slot");
    else
      for (size_t i = 0; i < kSlotCount; ++i) {
        bool found = false;
        for (int k = 0; k < kTissueCount; ++k)
          if ((*t)[i].is_string() && (*t)[i].get<std::string>() == kTissueNames[k]) {
            tissues[i] = Tissue(k);
            found = true;
          }
        if (!found) r.fail("\"tissues\" names an unknown tissue");
      }
  }
  std::vector<VoxelPart> parts;
  const Json* ps = r.at(j, "parts", true);
  if (ps && !ps->is_array()) r.fail("\"parts\" is not a list");
  if (ps && ps->is_array() && sk)
    for (const Json& q : *ps) {
      VoxelPart p;
      p.bone = i32(r.number(q, "bone", 0, 0, sk->count - 1, true));
      const V3 o = r.vector(q, "origin"), d = r.vector(q, "dims");
      size_t cells = 1;
      for (int a = 0; a < 3; ++a) {
        if (o[a] != std::floor(o[a]) || std::abs(o[a]) > 4096 || d[a] != std::floor(d[a]) || d[a] < 1 || d[a] > 1024) r.fail("a part's box is not whole cells");
        p.origin[size_t(a)] = i32(o[a]);
        p.dims[size_t(a)] = i32(std::max(1.0, d[a]));
        cells *= size_t(p.dims[size_t(a)]);
      }
      if (cells > (size_t(1) << 26)) r.fail("a part is too large");
      if (!r.ok) break;
      p.cells = r.runs(q, "cells", cells, kSlotCount, true);
      p.shade = r.runs(q, "shade", cells, 255, false);
      p.tissue = r.runs(q, "tissue", cells, kTissueCount, false);
      p.count = p.initial_count = i32(std::count_if(p.cells.begin(), p.cells.end(), [](u8 c) { return c != 0; }));
      parts.push_back(std::move(p));
    }
  if (!r.ok) return {};
  auto m = std::make_shared<VoxelModel>(sk, s, std::move(parts), r.string(j, "name", "model"));
  m->tissue = tissues;
  if (build_out) *build_out = build;
  return m;
}

Json prop_object(const Prop& p) {
  Json j;
  j["format"] = "svx-anim-prop";
  j["version"] = 1;
  j["id"] = p.id;
  j["name"] = p.name;
  j["mass"] = p.mass;
  j["tags"] = p.tags;
  j["attachments"] = p.attachments;
  j["material"] = {{"density", p.material.density}, {"penetration", p.material.penetration}, {"fracture", p.material.fracture}};
  const PropHold& h = p.hold;
  Json hold;
  hold["style"] = kHoldStyles[size_t(h.style)];
  hold["in_hand"] = h.in_hand;
  hold["recoil"] = h.recoil;
  hold["kick_back"] = h.kick_back;
  hold["kick_pitch"] = h.kick_pitch;
  hold["shot_impulse"] = h.shot_impulse;
  hold["guard"] = h.guard;
  hold["guard_two_hands"] = h.guard_two_hands;
  auto carry = [](const CarryPose& c) {
    Json o;
    o["on"] = c.on;
    o["offset"] = v3(c.offset);
    if (c.palm) o["palm"] = *c.palm;
    return o;
  };
  hold["ready"] = carry(h.ready);
  hold["ready_at_aim"] = h.ready_at_aim;
  hold["two_hands"] = carry(h.two_hands);
  j["hold"] = hold;
  j["grip"] = v3(p.grip);
  j["support"] = v3(p.support);
  j["butt"] = v3(p.butt);
  j["tip"] = v3(p.tip);
  j["reload_point"] = v3(p.reload_point);
  j["hanging_rotation"] = quat(p.hanging_rotation);
  j["ready_pitch"] = p.ready_pitch;
  j["ready_roll"] = p.ready_roll;
  j["ready_butt_offset"] = v3(p.ready_butt_offset);
  j["one_handed"] = p.one_handed;
  Json sockets = Json::array();
  for (const PropSocket& s : p.sockets) sockets.push_back({{"id", s.id}, {"point", v3(s.point)}, {"rotation", quat(s.rotation)}, {"retention", s.retention}});
  j["sockets"] = sockets;
  Json points = Json::array();
  for (const NamedPropPoint& n : p.points) points.push_back({{"id", n.id}, {"point", v3(n.point)}});
  j["points"] = points;
  Json features = Json::array();
  for (const ContactFeature& f : p.features)
    features.push_back({{"id", f.id}, {"impact", kImpacts[size_t(f.impact)]}, {"a", v3(f.a)}, {"b", v3(f.b)}, {"radius", f.radius}, {"sharpness", f.sharpness}, {"normal", v3(f.normal)}});
  j["features"] = features;
  j["centre"] = v3(p.centre);
  j["inertia"] = v3(p.inertia);
  j["dimensions"] = v3(p.dimensions);
  j["model"] = model_object(*p.model, nullptr);
  return j;
}

PropPtr prop_from(const Json& j, Reader& r) {
  if (r.string(j, "format", {}, true) != "svx-anim-prop") r.fail("not a prop");
  if (r.number(j, "version", 0, 1, 1, true) != 1) r.fail("unknown prop version");
  auto p = std::make_shared<Prop>();
  p->id = r.string(j, "id", {}, true);
  if (p->id.empty() && r.ok) r.fail("a prop needs an id");
  p->name = r.string(j, "name", p->id);
  p->mass = r.number(j, "mass", 1, 1e-4, 1e4, true);
  p->tags = r.strings(j, "tags");
  p->attachments = r.strings(j, "attachments");
  if (const Json* m = r.at(j, "material", false)) {
    p->material.density = r.number(*m, "density", p->material.density, 1, 1e5);
    p->material.penetration = r.number(*m, "penetration", p->material.penetration, 0, 1e12);
    p->material.fracture = r.number(*m, "fracture", p->material.fracture, 0, 1e9);
  }
  if (const Json* h = r.at(j, "hold", false)) {
    PropHold& hold = p->hold;
    const std::string style = r.string(*h, "style", "free");
    bool known = false;
    for (int k = 0; k < 3; ++k)
      if (style == kHoldStyles[k]) {
        hold.style = HoldStyle(k);
        known = true;
      }
    if (!known) r.fail("unknown hold style");
    hold.in_hand = r.flag(*h, "in_hand", false);
    hold.recoil = r.number(*h, "recoil", hold.recoil, 0, 100);
    hold.kick_back = r.number(*h, "kick_back", hold.kick_back, 0, 100);
    hold.kick_pitch = r.number(*h, "kick_pitch", hold.kick_pitch, 0, 100);
    hold.shot_impulse = r.number(*h, "shot_impulse", hold.shot_impulse, 0, 1000);
    hold.guard = r.string(*h, "guard", hold.guard);
    hold.guard_two_hands = r.string(*h, "guard_two_hands");
    auto carry = [&](const char* key, CarryPose& c) {
      if (const Json* o = r.at(*h, key, false)) {
        c.on = r.flag(*o, "on", false);
        c.offset = r.vector(*o, "offset");
        if (r.at(*o, "palm", false)) c.palm = r.number(*o, "palm", 0, -10, 10);
      }
    };
    carry("ready", hold.ready);
    hold.ready_at_aim = r.flag(*h, "ready_at_aim", false);
    carry("two_hands", hold.two_hands);
  }
  p->grip = r.vector(j, "grip");
  p->support = r.vector(j, "support");
  p->butt = r.vector(j, "butt");
  p->tip = r.vector(j, "tip");
  p->reload_point = r.vector(j, "reload_point");
  p->hanging_rotation = r.rotation(j, "hanging_rotation", p->hanging_rotation);
  p->ready_pitch = r.number(j, "ready_pitch", p->ready_pitch, -10, 10);
  p->ready_roll = r.number(j, "ready_roll", p->ready_roll, -10, 10);
  p->ready_butt_offset = r.vector(j, "ready_butt_offset", p->ready_butt_offset);
  p->one_handed = r.flag(j, "one_handed", false);
  if (const Json* ss = r.at(j, "sockets", false); ss && ss->is_array())
    for (const Json& s : *ss) p->sockets.push_back({r.string(s, "id", {}, true), r.vector(s, "point"), r.rotation(s, "rotation"), r.number(s, "retention", 350, 0, 1e7)});
  if (const Json* ns = r.at(j, "points", false); ns && ns->is_array())
    for (const Json& n : *ns) p->points.push_back({r.string(n, "id", {}, true), r.vector(n, "point")});
  if (const Json* fs = r.at(j, "features", false); fs && fs->is_array())
    for (const Json& f : *fs) {
      ContactFeature c;
      c.id = r.string(f, "id", {}, true);
      const std::string impact = r.string(f, "impact", "blunt");
      bool known = false;
      for (int k = 0; k < 3; ++k)
        if (impact == kImpacts[k]) {
          c.impact = ImpactClass(k);
          known = true;
        }
      if (!known) r.fail("unknown impact class");
      c.a = r.vector(f, "a");
      c.b = r.vector(f, "b");
      c.radius = r.number(f, "radius", c.radius, 0, 10);
      c.sharpness = r.number(f, "sharpness", 0, 0, 1);
      c.normal = r.vector(f, "normal", c.normal);
      p->features.push_back(std::move(c));
    }
  p->centre = r.vector(j, "centre");
  p->inertia = r.vector(j, "inertia", V3{1, 1, 1});
  p->dimensions = r.vector(j, "dimensions");
  if (const Json* m = r.at(j, "model", true)) p->model = model_from(*m, r, nullptr);
  if (!r.ok || !p->model) return {};
  for (int a = 0; a < 3; ++a)
    if (!(p->inertia[a] > 0)) r.fail("\"inertia\" must be positive");
  return r.ok ? p : PropPtr{};
}

}  // namespace

std::string model_json(const VoxelModel& m, const HumanoidBuild* build) { return model_object(m, build).dump(); }

ModelPtr read_model(std::string_view json, std::string* error, HumanoidBuild* build) {
  const Json j = Json::parse(json, nullptr, false);
  Reader r{error};
  if (j.is_discarded()) {
    r.fail("not JSON");
    return {};
  }
  return model_from(j, r, build);
}

std::string prop_json(const Prop& p) { return prop_object(p).dump(); }

PropPtr read_prop(std::string_view json, std::string* error) {
  const Json j = Json::parse(json, nullptr, false);
  Reader r{error};
  if (j.is_discarded()) {
    r.fail("not JSON");
    return {};
  }
  return prop_from(j, r);
}

std::vector<PropPtr> read_props(std::string_view json, std::string* error) {
  const Json j = Json::parse(json, nullptr, false);
  Reader r{error};
  if (j.is_discarded() || !j.is_array()) {
    r.fail("not a list of props");
    return {};
  }
  std::vector<PropPtr> out;
  for (const Json& e : j) {
    PropPtr p = prop_from(e, r);
    if (!p) return {};
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace svx::anim
