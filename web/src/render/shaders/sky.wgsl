// Sky gradient + sun, drawn first as a fullscreen triangle without depth writes.
// Prepended with frame.wgsl.

struct SkyOut {
  @builtin(position) clip: vec4f,
  @location(0) ndc: vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> SkyOut {
  let p = vec2f(f32((vi << 1u) & 2u), f32(vi & 2u)) * 2.0 - 1.0;
  var o: SkyOut;
  o.clip = vec4f(p, 0.0, 1.0);
  o.ndc = p;
  return o;
}

@fragment
fn fs(i: SkyOut) -> @location(0) vec4f {
  let dir = normalize(frame.forward.xyz + i.ndc.x * frame.right.w * frame.right.xyz + i.ndc.y * frame.up.w * frame.up.xyz);
  let t = dir.z;
  var c = mix(frame.fog.rgb, frame.zenith.rgb, smoothstep(0.0, 0.55, t));
  if (t < 0.0) {
    c = frame.fog.rgb * mix(1.0, 0.55, smoothstep(0.0, -0.5, t));
  }
  let s = max(dot(dir, frame.sun.xyz), 0.0);
  c += vec3f(1.0, 0.88, 0.65) * (pow(s, 900.0) * 8.0 + pow(s, 12.0) * 0.18);
  return vec4f(c, 1.0);
}
