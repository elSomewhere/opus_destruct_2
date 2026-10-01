import { rasterizeTree } from "../nature/trees.js";

/**
 * Feature source: street dressing (props + urban trees) from each cell's
 * stage-3 plan.
 */
export const dressingSource = {
  id: "dressing",
  // before buildings: buildings overwrite any crown that reaches into rooms
  order: 8,
  maxLod: 4,

  zRange(world, rect) {
    let lo = Infinity;
    let hi = -Infinity;
    for (const { i, j } of world.cellsOverlapping(rect)) {
      const d = world.dressing(i, j);
      for (const p of d.propGrid.query(rect)) {
        if (p.bb.z0 < lo) lo = p.bb.z0;
        if (p.bb.z1 > hi) hi = p.bb.z1;
      }
      for (const t of d.treeGrid.query(rect)) {
        if (t.bb.z0 < lo) lo = t.bb.z0;
        if (t.bb.z1 > hi) hi = t.bb.z1;
      }
    }
    return lo === Infinity ? null : [lo, hi];
  },

  rasterize(world, chunk) {
    const box = chunk.worldBox;
    const small = chunk.lod >= 3;
    for (const { i, j } of world.cellsOverlapping(box)) {
      const d = world.dressing(i, j);
      for (const p of d.propGrid.query(box)) {
        if (small && !p.big && p.kind !== "busShelter" && p.kind !== "hedge") continue;
        if (p.bb.z1 < box.z0 || p.bb.z0 > box.z1) continue;
        // (a prop is isolated voxels to a physics host, ANGLED_WORLD_PLAN.md §4.6)
        chunk.isolating = true;
        for (const q of p.boxes) chunk.fillBox(q.x0, q.y0, q.z0, q.x1, q.y1, q.z1, q.m, q.m === 0 ? 0 : 1);
        chunk.isolating = false;
      }
      // (each tree carries its seasonal look, set by the dressing plan)
      for (const t of d.treeGrid.query(box)) rasterizeTree(chunk, t);
    }
  },
};
