// Water surfaces (the engine's water meshes, texture 0xFFFE): translucent, a fresnel mix of the
// water's body colour and the sky it reflects, moving ripples, a sun glint; falling sheets
// streak downward. Drawn after the opaque world with premultiplied blending and no depth
// writes. Prepended with frame.wgsl.

struct VertexIn {
  @location(0) position: vec3f,
  @location(1) normalAo: vec4f,
  @location(2) uv: vec2f,
  @location(3) packed: u32,
};

struct VertexOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
};

@vertex
fn vs(v: VertexIn) -> VertexOut {
  var o: VertexOut;
  var p = v.position;
  let t = frame.eye.w;
  // (a gentle swell on the tops, kept below the rim)
  if (v.normalAo.z > 0.5) {
    p.z += 0.01 * sin(p.x * 1.7 + t * 1.3) * sin(p.y * 1.3 - t * 1.1) - 0.012;
  }
  o.clip = frame.viewProj * vec4f(p, 1.0);
  o.world = p;
  o.normal = v.normalAo.xyz;
  return o;
}

@fragment
fn fs(i: VertexOut) -> @location(0) vec4f {
  let t = frame.eye.w;
  let flat_n = normalize(i.normal);
  var n = flat_n;
  var streak = 0.0;
  if (flat_n.z > 0.5) {
    let p = i.world.xy;
    let dx = 0.07 * cos(p.x * 3.1 + t * 1.7) + 0.05 * cos((p.x + p.y) * 5.3 - t * 2.3) + 0.03 * cos(p.x * 9.7 + p.y * 2.1 + t * 3.1);
    let dy = 0.07 * cos(p.y * 2.7 - t * 1.4) + 0.05 * cos((p.x - p.y) * 4.9 + t * 2.1) + 0.03 * cos(p.y * 8.9 - p.x * 1.7 - t * 2.7);
    n = normalize(vec3f(-dx, -dy, 1.0));
  } else if (flat_n.z > -0.5) {
    // a wall of water (falling, or standing higher than its neighbour): streaks running down
    let along = dot(i.world.xy, vec2f(-flat_n.y, flat_n.x));
    streak = 0.5 + 0.5 * sin(along * 23.0 + sin(along * 7.0) * 2.0 + (i.world.z + t * 3.0) * 6.0);
  }
  let toEye = frame.eye.xyz - i.world;
  let dist = length(toEye);
  let v = toEye / max(dist, 1e-3);
  let cosTheta = clamp(abs(dot(n, v)), 0.0, 1.0);
  let fresnel = 0.03 + 0.97 * pow(1.0 - cosTheta, 5.0);
  let r = reflect(-v, n);
  let sky = mix(frame.fog.rgb, frame.zenith.rgb, clamp(r.z * 1.5, 0.0, 1.0));
  let glint = pow(max(dot(r, frame.sun.xyz), 0.0), 180.0) * 4.0;
  let body = vec3f(0.035, 0.12, 0.13) * (0.7 + 0.3 * max(dot(flat_n, frame.sun.xyz), 0.0));
  var color = mix(body, sky, fresnel) + vec3f(glint);
  var alpha = mix(0.62, 0.96, fresnel);
  if (flat_n.z <= 0.5) {
    color = mix(color, vec3f(0.55, 0.65, 0.68), 0.25 * streak);
    alpha = 0.55 + 0.25 * streak;
  }
  // flash light (explosions, fire)
  let toFlash = frame.flash.xyz - i.world;
  let fd = length(toFlash);
  color += vec3f(1.0, 0.72, 0.4) * 0.15 * frame.flash.w / (1.0 + 0.2 * fd * fd);
  let fog = 1.0 - exp(-dist * frame.fog.w);
  color = mix(color, frame.fog.rgb, fog);
  return vec4f(color * alpha, alpha);
}
