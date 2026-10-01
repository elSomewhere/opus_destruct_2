// Stage "trees": nature/trees.js's tables and models - TREE_KINDS and EVERGREEN; for 300 sampled
// trees of every kind (and of a kind the tables do not know; lib/trees.mjs sampleTree): its
// fields, its model's part count, bounds and palette, a digest of its parts (FNV-1a over their
// lines as the treemodels stage prints them), and treeBounds.
import { REF, line, samples } from "../lib/rec.mjs";
import { KINDS, sampleTree, treeFields, partText, fnv } from "../lib/trees.mjs";

const { TREE_KINDS, EVERGREEN, treeModel, treeBounds } = await import(REF + "nature/trees.js");

export default function* trees() {
  for (const [kind, spec] of Object.entries(TREE_KINDS)) yield line("k", kind, spec.h[0], spec.h[1], spec.r[0], spec.r[1], EVERGREEN.has(kind));
  const r = samples(29);
  for (const kind of KINDS)
    for (let i = 0; i < 300; i += 1) {
      const x = Math.floor((r() - 0.5) * 100000);
      const y = Math.floor((r() - 0.5) * 100000);
      const z = Math.floor(r() * 2400) - 400;
      // (off the voxel grid now and then)
      const off = r() < 0.15;
      const ox = off ? r() : 0;
      const oy = off ? r() : 0;
      const t = sampleTree(r, kind, x + ox, y - oy, off ? z + 0.5 : z);
      const m = treeModel(t);
      const bb = treeBounds(t);
      let h = 2166136261;
      for (const p of m.parts) h = fnv(partText(p) + "\n", h);
      const mb = m.bb ? [m.bb.x0, m.bb.y0, m.bb.z0, m.bb.x1, m.bb.y1, m.bb.z1] : ["-"];
      yield line("t", ...treeFields(t), m.parts.length, ...mb, m.pal, bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1, h >>> 0);
    }
}
