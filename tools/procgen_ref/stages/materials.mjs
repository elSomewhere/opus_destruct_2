// Stage "materials": the palette (voxel/materials.js) record by record.
import { REF, line } from "../lib/rec.mjs";

const { MATERIALS, MAT, IS_TRANSPARENT, IS_SOLID, IS_CLIMB } = await import(REF + "voxel/materials.js");

export default function* materials() {
  for (const m of MATERIALS) yield line(m.id, m.name, m.rgb, m.noise, m.transparent, m.opacity, m.emissive, m.solid, m.climb, m.glow, MAT[m.name], IS_TRANSPARENT[m.id], IS_SOLID[m.id], IS_CLIMB[m.id]);
}
