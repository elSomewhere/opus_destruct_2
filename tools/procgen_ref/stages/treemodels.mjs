// Stage "treemodels": the models of nature/trees.js part by part - 24 sampled trees of every kind
// (and of a kind the tables do not know; lib/trees.mjs sampleTree): wild or not (leaning, lopsided,
// twin leaders, kept within tight reaches: less lean, then smaller crowns), open-grown or not,
// logs lying at given or drawn yaws and tilts or windthrown on their root plates; every field of
// every part (foliage first, limbs after), the model's bounds and palette.
import { REF, line, samples } from "../lib/rec.mjs";
import { KINDS, sampleTree, treeFields, partFields } from "../lib/trees.mjs";

const { treeModel, treeBounds } = await import(REF + "nature/trees.js");

export default function* treemodels() {
  const r = samples(31);
  for (const kind of KINDS)
    for (let i = 0; i < 24; i += 1) {
      const x = Math.floor((r() - 0.5) * 100000);
      const y = Math.floor((r() - 0.5) * 100000);
      const z = Math.floor(r() * 2400) - 400;
      const off = r() < 0.15;
      const ox = off ? r() : 0;
      const oy = off ? r() : 0;
      const t = sampleTree(r, kind, x + ox, y - oy, off ? z + 0.5 : z);
      const m = treeModel(t);
      const bb = treeBounds(t);
      const mb = m.bb ? [m.bb.x0, m.bb.y0, m.bb.z0, m.bb.x1, m.bb.y1, m.bb.z1] : ["-"];
      yield line("t", ...treeFields(t), m.parts.length, ...mb, m.pal, bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1);
      for (const p of m.parts) yield line(...partFields(p));
    }
}
