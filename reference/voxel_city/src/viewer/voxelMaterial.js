import * as THREE from "three";

/**
 * Voxel materials built on three's Lambert pipeline (so they get the sun's
 * shadow map, fog and clipping planes) and patched with onBeforeCompile for:
 *   - per-voxel value noise (from world position, so greedy-merged faces
 *     still read as individual voxels)
 *   - baked vertex ambient occlusion
 *   - emissive voxels (lamps, screens, neon) and an indoor factor that dims
 *     sky light when the camera is under a roof or underground
 *   - per-vertex opacity for the transparent pass (glass, water)
 *   - a debug tint (uTint: rgb, strength), one material per tint colour
 *     (tiles by LOD, parts), the program shared
 */

const shared = {
  uIndoor: { value: 0 },
  uNight: { value: 0 },
  uLook: { value: new THREE.Vector4(0, 0, 0, 0) },
};

function patch(material, transparent, tint = null) {
  const uTint = { value: new THREE.Vector4(0, 0, 0, 0) };
  if (tint !== null) {
    const c = new THREE.Color(tint);
    uTint.value.set(c.r, c.g, c.b, 0.55);
  }
  material.userData.uTint = uTint;
  material.onBeforeCompile = (shader) => {
    shader.uniforms.uIndoor = shared.uIndoor;
    shader.uniforms.uNight = shared.uNight;
    shader.uniforms.uTint = uTint;
    shader.uniforms.uLook = shared.uLook;
    shader.vertexShader = shader.vertexShader
      .replace(
        "#include <common>",
        `#include <common>
attribute vec4 aux;
varying vec4 vAux;
varying vec3 vVoxWorld;
varying vec3 vVoxNormal;
varying float vVoxSize;`,
      )
      .replace(
        "#include <begin_vertex>",
        `#include <begin_vertex>
vAux = aux;
vVoxWorld = (modelMatrix * vec4(position, 1.0)).xyz;
vVoxNormal = normal;
vVoxSize = length(modelMatrix[0].xyz);`,
      );
    shader.fragmentShader = shader.fragmentShader
      .replace(
        "#include <common>",
        `#include <common>
varying vec4 vAux;
varying vec3 vVoxWorld;
varying vec3 vVoxNormal;
varying float vVoxSize;
uniform float uIndoor;
uniform float uNight;
uniform vec4 uTint;
uniform vec4 uLook;
float voxHash(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}`,
      )
      .replace(
        "#include <color_fragment>",
        `#include <color_fragment>
{
  // palette colors are sRGB; lighting happens in linear space
  diffuseColor.rgb = pow(max(diffuseColor.rgb, vec3(0.0)), vec3(2.2));
  vec3 cell = floor((vVoxWorld - normalize(vVoxNormal) * (0.5 * vVoxSize)) / vVoxSize + 0.0005);
  float h = voxHash(cell) - 0.5;
  diffuseColor.rgb *= 1.0 + h * vAux.z * 0.9;
  float up = clamp(normalize(vVoxNormal).z, 0.0, 1.0);
  float grime = (1.0 - up * 0.55) * (0.35 + 0.65 * voxHash(floor(vVoxWorld * 0.45)));
  diffuseColor.rgb *= 1.0 - uLook.y * grime * 0.48;
  diffuseColor.rgb *= 1.0 - uLook.z * (0.12 + up * 0.1);
  float luma = dot(diffuseColor.rgb, vec3(0.2126, 0.7152, 0.0722));
  diffuseColor.rgb = mix(diffuseColor.rgb, vec3(luma), uLook.x);
  diffuseColor.rgb = mix(diffuseColor.rgb, pow(uTint.rgb, vec3(2.2)), uTint.a);
  ${transparent ? "diffuseColor.a *= vAux.w;" : ""}
}`,
      )
      .replace(
        "#include <lights_fragment_end>",
        `#include <lights_fragment_end>
{
  float ao = vAux.x;
  float d = length(vVoxWorld - cameraPosition);
  float inside = uIndoor * (1.0 - smoothstep(14.0, 40.0, d));
  reflectedLight.indirectDiffuse *= ao * (1.0 - 0.45 * inside);
  reflectedLight.indirectDiffuse += diffuseColor.rgb * inside * 0.28 * ao;
  reflectedLight.directDiffuse *= 0.55 + 0.45 * ao;
}`,
      )
      .replace(
        "#include <emissivemap_fragment>",
        `#include <emissivemap_fragment>
{
  // aux.y: emissive level, or the reserved code 3 for window glass that
  // lights up (per window cell) at night
  float code = vAux.y * 255.0;
  float glass = 1.0 - step(0.5, abs(code - 3.0));
  totalEmissiveRadiance += diffuseColor.rgb * vAux.y * (1.0 - glass) * (1.1 + uNight * 2.0);
  vec3 win = floor((vVoxWorld - normalize(vVoxNormal) * 0.06) / vec3(1.25, 1.25, 3.0));
  float r = voxHash(win + 17.0);
  float lit = step(r, 0.27);
  vec3 tone = mix(vec3(1.0, 0.66, 0.34), vec3(0.85, 0.9, 1.0), step(r, 0.05));
  diffuseColor.rgb *= 1.0 - 0.6 * uNight * glass;
  totalEmissiveRadiance += glass * lit * uNight * tone * (0.55 + 0.4 * r / 0.27);
}`,
      )
      .replace(
        "#include <dithering_fragment>",
        `#include <dithering_fragment>
gl_FragColor.rgb += (voxHash(gl_FragCoord.xyz) - 0.5) * uLook.w / 255.0;`,
      );
  };
  material.customProgramCacheKey = () => (transparent ? "voxel-t" : "voxel-o");
}

function makeMaterial(transparent, tint = null) {
  const m = transparent
    ? new THREE.MeshLambertMaterial({ vertexColors: true, transparent: true, depthWrite: false, side: THREE.DoubleSide })
    : new THREE.MeshLambertMaterial({ vertexColors: true });
  patch(m, transparent, tint);
  return m;
}

/**
 * The voxel materials: `opaque` and `transparent`, `tinted(transparent,
 * colour)` (debug views, made once per colour), `all()` every one made (to
 * set the clipping plane, wireframe, a recompile for fog or shadows).
 */
export function makeVoxelMaterials() {
  const opaque = makeMaterial(false);
  const transparent = makeMaterial(true);
  const tints = new Map();
  const uniforms = { uIndoor: shared.uIndoor, uNight: shared.uNight, uLook: shared.uLook };
  const all = () => [opaque, transparent, ...tints.values()];
  const tinted = (isTransparent, colour) => {
    const key = `${isTransparent ? "t" : "o"}${colour}`;
    let m = tints.get(key);
    if (!m) {
      m = makeMaterial(isTransparent, colour);
      // (as the base materials are set: clipping, wireframe)
      const base = isTransparent ? transparent : opaque;
      m.clippingPlanes = base.clippingPlanes;
      m.wireframe = base.wireframe;
      tints.set(key, m);
    }
    return m;
  };
  return { opaque, transparent, uniforms, tinted, all };
}

/** Build a BufferGeometry from a mesher payload. */
export function geometryFromMesh(m) {
  const g = new THREE.BufferGeometry();
  g.setAttribute("position", new THREE.BufferAttribute(m.position, 3, false));
  g.setAttribute("normal", new THREE.BufferAttribute(m.normal, 3, true));
  g.setAttribute("color", new THREE.BufferAttribute(m.color, 3, true));
  g.setAttribute("aux", new THREE.BufferAttribute(m.aux, 4, true));
  g.setIndex(new THREE.BufferAttribute(m.index, 1));
  g.boundingSphere = new THREE.Sphere(new THREE.Vector3(16, 16, 16), 16 * Math.sqrt(3) + 1);
  g.boundingBox = new THREE.Box3(new THREE.Vector3(0, 0, 0), new THREE.Vector3(32, 32, 32));
  return g;
}
