// Tyre marks on the ground (skids.ts): rubber laid down by sliding tyres, dark and translucent
// (premultiplied alpha), fading at their ends and with age.
// Prepended with frame.wgsl.

struct SkidIn {
  @location(0) posAlpha: vec4f, // xyz world position, w = opacity
};

struct SkidOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) alpha: f32,
};

@vertex
fn vs(v: SkidIn) -> SkidOut {
  var o: SkidOut;
  o.clip = frame.viewProj * vec4f(v.posAlpha.xyz, 1.0);
  o.world = v.posAlpha.xyz;
  o.alpha = v.posAlpha.w;
  return o;
}

@fragment
fn fs(i: SkidOut) -> @location(0) vec4f {
  let dist = length(frame.eye.xyz - i.world);
  let a = clamp(i.alpha, 0.0, 1.0) * exp(-dist * frame.fog.w);
  return vec4f(vec3f(0.01, 0.0095, 0.009) * a, a);
}
