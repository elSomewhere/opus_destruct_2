// Voxel characters (svx_anim character vertex format: anim/src/voxel/mesh.ts), blob shadows /
// decals, and instanced voxel bits (blood drops, casings, chips). Prepended with frame.wgsl.
//
// Characters are rigidly skinned: every vertex belongs to one bone, so voxels stay cubes.
// Colours come from the instance's palette (16 slots) times the voxel's shade.

struct Instance {
  boneBase: u32,
  palette: u32,
  light: f32,    // Doom sector light 0..1 (1 in procedural worlds)
  opacity: f32,  // dithered fade
  tint: vec4f,   // rgb tint, a = amount (hit flash)
};

@group(1) @binding(0) var<storage, read> bones: array<mat4x4f>;
@group(1) @binding(1) var<storage, read> instances: array<Instance>;
@group(1) @binding(2) var<storage, read> palettes: array<vec4f>;

struct CharIn {
  @location(0) position: vec3f,
  @location(1) normalAo: vec4f,
  @location(2) packed: u32, // bone (8) | slot (4) | shade (8)
  @builtin(instance_index) inst: u32,
};

struct CharOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
  @location(2) color: vec3f,
  @location(3) ao: f32,
  @location(4) @interpolate(flat) inst: u32,
};

@vertex
fn vsChar(v: CharIn) -> CharOut {
  let I = instances[v.inst];
  let bone = v.packed & 0xffu;
  let m = bones[I.boneBase + bone];
  let world = m * vec4f(v.position, 1.0);
  var o: CharOut;
  o.clip = frame.viewProj * world;
  o.world = world.xyz;
  o.normal = (m * vec4f(v.normalAo.xyz, 0.0)).xyz;
  let slot = (v.packed >> 8u) & 0xfu;
  let shade = f32((v.packed >> 12u) & 0xffu) / 128.0;
  o.color = palettes[I.palette * 16u + slot].rgb * shade;
  o.ao = v.normalAo.w * 0.5 + 0.5;
  o.inst = v.inst;
  return o;
}

fn bayer4c(p: vec2u) -> f32 {
  var m = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
  return (m[(p.y % 4u) * 4u + (p.x % 4u)] + 0.5) / 16.0;
}

// The world shader's lighting model (world.wgsl), so characters sit in the same light.
fn litColor(base: vec3f, world: vec3f, nIn: vec3f, aoIn: f32, sector: f32) -> vec3f {
  let n = normalize(nIn);
  let dist = length(frame.eye.xyz - world);
  let level = sector * sector;
  let diminish = clamp(1.3 - dist * 0.025 * (1.1 - sector), 0.3, 1.0);
  let hemi = mix(0.6, 1.0, n.z * 0.5 + 0.5);
  let contrast = 1.0 - 0.12 * abs(n.x);
  let sun = max(dot(n, frame.sun.xyz), 0.0) * smoothstep(0.85, 1.0, sector);
  let ao = mix(0.3, 1.0, aoIn * aoIn);
  let shade = (0.75 * hemi * contrast + 0.6 * sun) * ao;
  var color = base * level * diminish * shade;
  let toFlash = frame.flash.xyz - world;
  let fd = length(toFlash);
  let facing = max(dot(n, toFlash / max(fd, 1e-3)), 0.0);
  color += base * vec3f(1.0, 0.72, 0.4) * frame.flash.w * facing / (1.0 + 0.2 * fd * fd);
  let fog = 1.0 - exp(-dist * frame.fog.w);
  return mix(color, frame.fog.rgb, fog);
}

@fragment
fn fsChar(i: CharOut) -> @location(0) vec4f {
  let I = instances[i.inst];
  if (I.opacity < 0.999 && I.opacity < bayer4c(vec2u(i.clip.xy))) {
    discard;
  }
  var color = litColor(i.color, i.world, i.normal, i.ao, I.light);
  color = mix(color, I.tint.rgb, I.tint.a);
  return vec4f(color, 1.0);
}

// ---- decals: blob shadows and blood splats, quads on a surface -----------------------------

struct DecalIn {
  @builtin(vertex_index) vid: u32,
  @location(0) center: vec4f,  // xyz, w = radius
  @location(1) normal: vec4f,  // xyz unit normal, w = kind (0 soft blob, 1 splat)
  @location(2) color: vec4f,   // rgb, a = strength
};

struct DecalOut {
  @builtin(position) clip: vec4f,
  @location(0) uv: vec2f,
  @location(1) color: vec4f,
  @location(2) @interpolate(flat) kind: f32,
  @location(3) seed: f32,
};

@vertex
fn vsDecal(d: DecalIn) -> DecalOut {
  var corners = array<vec2f, 6>(vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));
  let c = corners[d.vid];
  let n = d.normal.xyz;
  let a = select(vec3f(1.0, 0.0, 0.0), vec3f(0.0, 0.0, 1.0), abs(n.z) < 0.9);
  let t = normalize(cross(a, n));
  let b = cross(n, t);
  let p = d.center.xyz + n * 0.012 + (t * c.x + b * c.y) * d.center.w;
  var o: DecalOut;
  o.clip = frame.viewProj * vec4f(p, 1.0);
  o.uv = c;
  o.color = d.color;
  o.kind = d.normal.w;
  o.seed = fract(d.center.x * 12.9898 + d.center.y * 78.233 + d.center.z * 37.719);
  return o;
}

fn hash2(p: vec2f) -> f32 {
  return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453);
}

@fragment
fn fsDecal(i: DecalOut) -> @location(0) vec4f {
  let r = length(i.uv);
  var a = 0.0;
  if (i.kind < 0.5) {
    a = (1.0 - smoothstep(0.35, 1.0, r)) * i.color.a;
  } else {
    // voxel-ish splat: blocky cells, ragged edge
    let cell = floor(i.uv * 5.0);
    let n = hash2(cell + vec2f(i.seed * 91.0, i.seed * 13.0));
    let edge = 0.55 + 0.45 * n;
    a = select(0.0, i.color.a, r < edge);
  }
  if (a <= 0.003) {
    discard;
  }
  // premultiplied: rgb * a over the scene (shadows: black)
  return vec4f(i.color.rgb * a, a);
}

// ---- voxel bits: small instanced cubes ----------------------------------------------------

struct BitIn {
  @builtin(vertex_index) vid: u32,
  @location(0) pos: vec4f,   // xyz, w = half size
  @location(1) rot: vec4f,   // quaternion
  @location(2) color: vec4f, // linear rgb, a = light
};

struct BitOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
  @location(2) color: vec3f,
  @location(3) light: f32,
};

fn qrot(q: vec4f, v: vec3f) -> vec3f {
  let t = 2.0 * cross(q.xyz, v);
  return v + q.w * t + cross(q.xyz, t);
}

@vertex
fn vsBit(b: BitIn) -> BitOut {
  // 36 vertices: 6 faces x 2 triangles, CCW from outside
  let face = b.vid / 6u;
  let corner = b.vid % 6u;
  var quad = array<vec2f, 6>(vec2f(-1.0, -1.0), vec2f(1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, -1.0), vec2f(1.0, 1.0), vec2f(-1.0, 1.0));
  var ns = array<vec3f, 6>(vec3f(1.0, 0.0, 0.0), vec3f(-1.0, 0.0, 0.0), vec3f(0.0, 1.0, 0.0), vec3f(0.0, -1.0, 0.0), vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, -1.0));
  var us = array<vec3f, 6>(vec3f(0.0, 1.0, 0.0), vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, 1.0), vec3f(1.0, 0.0, 0.0), vec3f(1.0, 0.0, 0.0), vec3f(0.0, 1.0, 0.0));
  var vs = array<vec3f, 6>(vec3f(0.0, 0.0, 1.0), vec3f(0.0, 1.0, 0.0), vec3f(1.0, 0.0, 0.0), vec3f(0.0, 0.0, 1.0), vec3f(0.0, 1.0, 0.0), vec3f(1.0, 0.0, 0.0));
  let q = quad[corner];
  let n = ns[face];
  let local = (n + us[face] * q.x + vs[face] * q.y) * b.pos.w;
  let world = b.pos.xyz + qrot(b.rot, local);
  var o: BitOut;
  o.clip = frame.viewProj * vec4f(world, 1.0);
  o.world = world;
  o.normal = qrot(b.rot, n);
  o.color = b.color.rgb;
  o.light = b.color.a;
  return o;
}

@fragment
fn fsBit(i: BitOut) -> @location(0) vec4f {
  return vec4f(litColor(i.color, i.world, i.normal, 1.0, i.light), 1.0);
}
