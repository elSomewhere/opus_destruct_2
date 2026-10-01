// World geometry: chunk meshes and detached islands (28-byte protocol vertices).
// Prepended with frame.wgsl.

struct TexInfo {
  rect: vec4f,  // atlas texel position of texture texel (0,0); texture width, height (texels)
  extra: vec4f, // layer, atlas texels per texel, valid (1/0), unused
};

struct Object {
  model: mat4x4f,
  params: vec4f, // x = opacity (dithered), y = displaced by the fields (1/0), z = its voxel size (0: the frame's), w unused
};

// Displacement fields of running physics bubbles (fields.ts; v1 engines, v2 sends none): per
// field (origin.xyz, 1/h) and (size.xyz, active); rgba16float texels (w d, w), w = solid weight.
struct Fields {
  f: array<vec4f, 8>,
};

@group(0) @binding(1) var atlas: texture_2d_array<f32>;
@group(0) @binding(2) var atlasSampler: sampler;
@group(0) @binding(3) var<storage, read> texInfo: array<TexInfo>;
@group(0) @binding(4) var field0: texture_3d<f32>;
@group(0) @binding(5) var field1: texture_3d<f32>;
@group(0) @binding(6) var field2: texture_3d<f32>;
@group(0) @binding(7) var field3: texture_3d<f32>;
@group(0) @binding(8) var fieldSampler: sampler;
@group(0) @binding(9) var<uniform> fields: Fields;
@group(1) @binding(0) var<uniform> object: Object;

struct VertexIn {
  @location(0) position: vec3f,
  @location(1) normalAo: vec4f, // snorm8x4: normal xyz, w = AO (-1..1 -> 0..1)
  @location(2) uv: vec2f,       // texels
  @location(3) packed: u32,     // texture id (16) | light (8) | debug (8), little-endian
};

struct VertexOut {
  @builtin(position) clip: vec4f,
  @location(0) world: vec3f,
  @location(1) normal: vec3f,
  @location(2) uv: vec2f,
  @location(3) ao: f32,
  @location(4) light: f32,
  @location(5) debugValue: f32,
  @location(6) @interpolate(flat) tex: u32,
  @location(7) @interpolate(flat) debugId: u32, // the debug byte, not interpolated (fragment ids)
  // (the per-voxel variation: in the object's own frame and voxel size, so it stays on a moving grid's voxels)
  @location(8) local: vec3f,
  @location(9) localNormal: vec3f,
  @location(10) @interpolate(flat) cell: f32,
};

// Normalized texture coordinates of p in field k (w = 1 inside the field's texel box).
fn fieldCoord(k: u32, p: vec3f) -> vec4f {
  let a = fields.f[2u * k];
  let b = fields.f[2u * k + 1u];
  let t = (p - a.xyz) * a.w + 0.5; // texel i's centre sits at i + 0.5
  let inside = b.w > 0.5 && all(t >= vec3f(0.0)) && all(t <= b.xyz);
  return vec4f(t / max(b.xyz, vec3f(1.0)), select(0.0, 1.0, inside));
}

fn fieldValue(s: vec4f) -> vec3f {
  return select(vec3f(0.0), s.xyz / max(s.w, 1e-4), s.w > 1e-3);
}

// Solid-weighted trilinear displacement at p: the average over the solid voxels around a
// vertex (voxel corner), like the CPU mesher's corner average.
fn displacement(p: vec3f) -> vec3f {
  var d = vec3f(0.0);
  let c0 = fieldCoord(0u, p);
  if (c0.w > 0.5) { d += fieldValue(textureSampleLevel(field0, fieldSampler, c0.xyz, 0.0)); }
  let c1 = fieldCoord(1u, p);
  if (c1.w > 0.5) { d += fieldValue(textureSampleLevel(field1, fieldSampler, c1.xyz, 0.0)); }
  let c2 = fieldCoord(2u, p);
  if (c2.w > 0.5) { d += fieldValue(textureSampleLevel(field2, fieldSampler, c2.xyz, 0.0)); }
  let c3 = fieldCoord(3u, p);
  if (c3.w > 0.5) { d += fieldValue(textureSampleLevel(field3, fieldSampler, c3.xyz, 0.0)); }
  return d;
}

@vertex
fn vs(v: VertexIn) -> VertexOut {
  var p = v.position;
  if (object.params.y > 0.5) {
    p += displacement(p);
  }
  let world = object.model * vec4f(p, 1.0);
  var o: VertexOut;
  o.clip = frame.viewProj * world;
  o.world = world.xyz;
  o.normal = (object.model * vec4f(v.normalAo.xyz, 0.0)).xyz;
  o.uv = v.uv;
  o.ao = v.normalAo.w * 0.5 + 0.5;
  o.light = f32((v.packed >> 16u) & 0xffu) / 255.0;
  o.debugValue = f32(v.packed >> 24u);
  o.debugId = v.packed >> 24u;
  o.tex = v.packed & 0xffffu;
  o.local = p;
  o.localNormal = v.normalAo.xyz;
  o.cell = select(frame.zenith.w, object.params.z, object.params.z > 0.0);
  return o;
}

fn hash3(p: vec3f) -> f32 {
  var q = fract(p * vec3f(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yxz + 33.33);
  return fract((q.x + q.y) * q.z);
}

// Utilization heat map: blue -> cyan -> green -> yellow -> red.
fn heat(t: f32) -> vec3f {
  let c = clamp(t, 0.0, 1.0);
  return clamp(vec3f(1.5 - abs(4.0 * c - 3.0), 1.5 - abs(4.0 * c - 2.0), 1.5 - abs(4.0 * c - 1.0)), vec3f(0.0), vec3f(1.0));
}

// Rubble fragment: debug byte 1..254 (pseudo-random per fragment) -> a distinct colour.
// Golden-ratio hue steps, with saturation and brightness varied by the low bits so that
// neighbouring fragments of similar hue still separate.
fn fragmentColor(id: u32) -> vec3f {
  let hue = fract(f32(id) * 0.618034);
  let rgb = clamp(abs(fract(vec3f(hue) + vec3f(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, vec3f(0.0), vec3f(1.0));
  let sat = select(0.9, 0.6, (id & 1u) == 1u);
  let val = select(1.0, 0.72, (id & 2u) == 2u);
  return val * mix(vec3f(1.0), rgb, sat);
}

fn bayer4(p: vec2u) -> f32 {
  var m = array<f32, 16>(0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
  return (m[(p.y % 4u) * 4u + (p.x % 4u)] + 0.5) / 16.0;
}

@fragment
fn fs(i: VertexOut) -> @location(0) vec4f {
  // Derivatives of the unwrapped texel coordinates, taken in uniform control flow; they
  // drive mip selection so the manual wrap below causes no seams.
  let duvdx = dpdx(i.uv);
  let duvdy = dpdy(i.uv);
  let n = normalize(i.normal);

  var base = vec3f(0.0);
  var textured = false;
  if (i.tex < u32(frame.sun.w)) {
    let info = texInfo[i.tex];
    if (info.extra.z > 0.5) {
      let size = info.rect.zw;
      let wrapped = i.uv - floor(i.uv / size) * size;
      let atlasSize = vec2f(textureDimensions(atlas, 0));
      let s = info.extra.y / atlasSize;
      let uv = info.rect.xy / atlasSize + wrapped * s;
      base = textureSampleGrad(atlas, atlasSampler, uv, i32(info.extra.x), duvdx * s, duvdy * s).rgb;
      textured = true;
    }
  }
  // Glossy surfaces: car paint (paint slots 32..45: its clear coat), glazing (glass 9, a car's
  // windows 15), lamps (20; tail lamps and indicators 47, 48), a little on trim and plastic.
  var gloss = 0.0;
  if (!textured) {
    // 0xFFFF = default colour; 0xFF00 (0xFE00: glowing) + slot = that slot's colour: a material's
    // (slot = material id) or a paint's (31 + paint).
    let slot = select(31u, min(i.tex & 0xffu, 63u), i.tex != 0xffffu);
    base = frame.palette[slot].rgb;
    if ((i.tex & 0xff00u) == 0xff00u && i.tex != 0xffffu) {
      if (slot >= 32u && slot <= 45u) {
        gloss = 0.6;
      } else if (slot == 9u || slot == 15u) {
        gloss = 0.9;
      } else if (slot == 20u || slot == 47u || slot == 48u) {
        gloss = 0.7;
      } else if (slot == 46u || slot == 17u) {
        gloss = 0.2;
      }
    }
    // Faint per-voxel variation keeps the voxel scale readable on flat colours (fainter on paint).
    let cell = floor((i.local - normalize(i.localNormal) * 0.01) / i.cell);
    base *= select(0.88 + 0.12 * hash3(cell), 0.96 + 0.04 * hash3(cell), gloss > 0.0);
  }

  let toEye = frame.eye.xyz - i.world;
  let dist = length(toEye);
  // Doom sector light: brightness ~ level^2, diminishing with distance in dark sectors.
  let sector = i.light;
  let level = sector * sector;
  let diminish = clamp(1.3 - dist * 0.025 * (1.1 - sector), 0.3, 1.0);
  let hemi = mix(0.6, 1.0, n.z * 0.5 + 0.5);
  let contrast = 1.0 - 0.12 * abs(n.x); // Doom's "fake contrast" on x-facing walls
  let sun = max(dot(n, frame.sun.xyz), 0.0) * smoothstep(0.85, 1.0, sector);
  let ao = mix(0.3, 1.0, i.ao * i.ao);
  let shade = (0.75 * hemi * contrast + 0.6 * sun) * ao;
  var color = base * level * diminish * shade;

  // Gloss: the sky mirrored (Fresnel), and the sun's highlight.
  if (gloss > 0.0 && u32(frame.forward.w + 0.5) == 0u) {
    let v = normalize(toEye);
    let r = reflect(-v, n);
    let fres = 0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    let skyc = mix(frame.fog.rgb, frame.zenith.rgb, clamp(r.z * 1.5, 0.0, 1.0)) * select(0.3, 1.0, r.z > 0.0);
    let rs = max(dot(r, frame.sun.xyz), 0.0);
    let spec = pow(rs, 160.0) * 4.0 + pow(rs, 12.0) * 0.08;
    color = mix(color, skyc * level * ao, gloss * fres) + vec3f(spec) * gloss * level * ao * smoothstep(0.85, 1.0, sector);
  }

  // Muzzle flash / explosion light.
  let toFlash = frame.flash.xyz - i.world;
  let fd = length(toFlash);
  let facing = max(dot(n, toFlash / max(fd, 1e-3)), 0.0);
  color += base * vec3f(1.0, 0.72, 0.4) * frame.flash.w * facing / (1.0 + 0.2 * fd * fd);

  let view = u32(frame.forward.w + 0.5);
  if (view == 1u) {
    color = heat(i.debugValue / 255.0) * (0.25 + 0.75 * shade);
  } else if (view == 2u) {
    // fragments: each rubble fragment in its own colour, the rest greyed out
    let grey = dot(color, vec3f(0.3, 0.59, 0.11));
    if (i.debugId == 0u) {
      color = vec3f(grey * 0.6);
    } else {
      color = fragmentColor(i.debugId) * (0.35 + 0.65 * shade);
    }
  }

  // Glowing voxels (0xFE00 + material: burning wood, red-hot metal): embers under the flames,
  // flickering per voxel.
  if ((i.tex & 0xff00u) == 0xfe00u && view == 0u) {
    let cell = floor((i.local - normalize(i.localNormal) * 0.01) / i.cell);
    let r = hash3(cell);
    let flick = 0.6 + 0.4 * sin(frame.eye.w * (5.0 + 6.0 * r) + r * 40.0);
    color = color * 0.35 + vec3f(1.7, 0.42, 0.07) * flick * (0.55 + 0.45 * hash3(cell + vec3f(7.0)));
  }

  let fog = 1.0 - exp(-dist * frame.fog.w);
  color = mix(color, frame.fog.rgb, fog);

  // Detached islands fade out with an ordered dither (order-independent, keeps depth).
  if (object.params.x < 0.999 && object.params.x < bayer4(vec2u(i.clip.xy))) {
    discard;
  }
  return vec4f(color, 1.0);
}
