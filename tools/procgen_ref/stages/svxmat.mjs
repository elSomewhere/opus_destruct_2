// Stage "svxmat": the export's material classes (svx/materials.js) - the classes, every city
// material's classification (class as structure and as ground, looks, flora), the looks of every
// class in LOOKS's order, the flora list, vox() bytes, and svxMaterials() as JSON.
import { REF, line } from "../lib/rec.mjs";

const { MATERIALS } = await import(REF + "voxel/materials.js");
const S = await import(REF + "svx/materials.js");

export default function* svxmat() {
  yield line("base", S.CITY_BASE, S.CLASSES.length);
  for (const c of S.CLASSES) yield line("class", c.name, c.id, !!c.own, S.CLASS[c.name].id);
  for (const m of MATERIALS) {
    const e = S.CLASSIFY[m.id];
    yield line("m", m.id, m.name, !!e.air, !!e.liquid, !!e.flora, e.s, e.g, e.sLook, e.gLook, e.floraIdx);
  }
  for (const [id, list] of S.LOOKS) yield line("looks", id, list);
  yield line("flora", S.FLORA);
  for (const c of S.CLASSES) yield line("vox", c.id, S.vox(c.id, false), S.vox(c.id, true));
  yield JSON.stringify(S.svxMaterials());
}
