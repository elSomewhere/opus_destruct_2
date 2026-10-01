// svx_city — the public view of the export's materials (svx/city/materials.hpp).
#include "svx/city/materials.hpp"

#include "svx/materials.hpp"
#include "voxel/materials.hpp"

namespace svx::city {

const std::vector<PhysicsClass>& physics_classes() {
  static const std::vector<PhysicsClass> all = [] {
    std::vector<PhysicsClass> v;
    for (const SvxClass& c : svx_classes()) {
      PhysicsClass p;
      p.name = c.name;
      p.id = c.id;
      if (c.own) {
        const SvxOwnMaterial& o = *c.own;
        p.own = true;
        p.E = o.E;
        p.G = o.G;
        p.rho = o.rho;
        p.ft = o.ft;
        p.fb = o.fb;
        p.fc = o.fc;
        p.cohesion = o.cohesion;
        p.friction = o.friction;
        p.Gf = o.Gf;
        p.frag = o.frag;
        p.frag_noise = o.frag_noise;
        p.crush = o.crush.value_or(0.0);
        p.grip = o.grip.value_or(0.0);
      }
      v.push_back(std::move(p));
    }
    return v;
  }();
  return all;
}

const std::vector<Look>& looks() {
  static const std::vector<Look> all = [] {
    std::vector<Look> v;
    for (int cls : svx_look_classes()) {
      const std::vector<int>& list = svx_looks(cls);
      for (size_t k = 0; k < list.size(); ++k) {
        const MaterialInfo& m = material_info(list[k]);
        Look l;
        l.class_id = cls;
        l.look = static_cast<int>(k);
        l.material = m.name;
        l.rgb = m.rgb;
        l.noise = m.noise;
        l.opacity = m.opacity;
        l.emissive = m.emissive;
        l.transparent = m.transparent;
        l.glow = m.glow;
        l.flora = svx_classify(list[k]).flora;
        v.push_back(std::move(l));
      }
    }
    return v;
  }();
  return all;
}

}  // namespace svx::city
