// Per-frame uniforms shared by every pipeline (group 0, binding 0). Mirrors FRAME_FLOATS
// in renderer.ts; all colours are linear.
struct Frame {
  viewProj: mat4x4f,
  eye: vec4f,      // xyz camera position, w = time (s)
  right: vec4f,    // xyz camera right, w = tan(fovY/2) * aspect
  up: vec4f,       // xyz camera up, w = tan(fovY/2)
  forward: vec4f,  // xyz camera forward, w = debug view (0 none, 1 utilization, 2 fragments)
  fog: vec4f,      // rgb horizon / fog colour, w = fog density (1/m)
  zenith: vec4f,   // rgb zenith colour, w = voxel size (m)
  sun: vec4f,      // xyz unit direction towards the sun, w = texture count
  flash: vec4f,    // xyz flash light position, w = intensity
  // colours: materials by id (0..20), [31] the untextured default, paints at 31 + paint
  // (renderer.ts PALETTE)
  palette: array<vec4f, 64>,
};

@group(0) @binding(0) var<uniform> frame: Frame;
