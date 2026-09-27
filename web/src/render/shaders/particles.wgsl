// Camera-facing particle sprites (dust, chips, sparks, rocket flame and smoke).
// One instance per particle; premultiplied alpha, additive when size < 0.
// Prepended with frame.wgsl.

struct ParticleIn {
  @location(0) posSize: vec4f, // xyz position, w = radius (negative = additive)
  @location(1) color: vec4f,   // linear rgb, alpha
};

struct ParticleOut {
  @builtin(position) clip: vec4f,
  @location(0) corner: vec2f,
  @location(1) color: vec4f,
  @location(2) @interpolate(flat) additive: f32,
};

@vertex
fn vs(@builtin(vertex_index) vi: u32, p: ParticleIn) -> ParticleOut {
  var corners = array<vec2f, 6>(
    vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0),
    vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0),
  );
  let c = corners[vi];
  let size = abs(p.posSize.w);
  let world = p.posSize.xyz + (frame.right.xyz * c.x + frame.up.xyz * c.y) * size;
  let dist = length(p.posSize.xyz - frame.eye.xyz);
  var o: ParticleOut;
  o.clip = frame.viewProj * vec4f(world, 1.0);
  o.corner = c;
  o.color = vec4f(p.color.rgb, p.color.a * exp(-dist * frame.fog.w));
  o.additive = select(0.0, 1.0, p.posSize.w < 0.0);
  return o;
}

@fragment
fn fs(i: ParticleOut) -> @location(0) vec4f {
  let r2 = dot(i.corner, i.corner);
  if (r2 > 1.0) {
    discard;
  }
  let falloff = (1.0 - r2) * (1.0 - r2);
  let a = i.color.a * falloff;
  return vec4f(i.color.rgb * a, a * (1.0 - i.additive));
}
