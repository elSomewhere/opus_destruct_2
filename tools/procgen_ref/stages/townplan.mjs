// Stage "townplan": the town plans (city/townPlan.js) of towns of every world of lib/worlds.mjs
// (cityWorlds) - an island's harbour town with its wharf, a wrapping world's town a lap away, a
// village (no plan) - every landmark with the block it lands on (resolveTown), and landmarkUse of
// the blocks of the cells round each landmark and of a few more cells of the town, with their
// own property rects and with ones cut back (cellPlan asks with the rect cleared of the streets);
// LANDMARK_USE. Every base height the plans and the cells' networks can read is made first
// (lib/worlds.mjs warmBasesIn, warmIsland; docs/CITY.md §6): the church takes the highest of a
// few spots.
import { REF, line, samples } from "../lib/rec.mjs";
import { cityWorlds, warmBasesIn, warmIsland, withSeaTests } from "../lib/worlds.mjs";

const { World } = await import(REF + "world/World.js");
const { townPlan, landmarkUse, LANDMARK_USE } = await import(REF + "city/townPlan.js");

/** The places a world's plans are made for: the spawn town and two more, the spawn town a lap away, a village. */
export function placesOf(w) {
  const F = w.fields;
  const spawn = F.settlement(0, 0);
  const out = spawn ? [spawn] : [];
  out.push(...F.settlementsIn({ x0: -100000, y0: -100000, x1: 100000, y1: 100000 }).filter((s) => s !== spawn).slice(0, 2));
  if (F.wrap.on) {
    const s = F.settlement(F.nTown, 0);
    if (s) out.push(s);
  }
  const v = F.villagesIn({ x0: -60000, y0: -60000, x1: 60000, y1: 60000 })[0];
  if (v) out.push(v);
  return out;
}

export default function* townplan() {
  const r = samples(67);
  for (const kind of [...Object.keys(LANDMARK_USE), "nope"]) yield line("use", kind, LANDMARK_USE[kind]);
  for (const [key, overrides] of cityWorlds()) {
    const w = withSeaTests(new World(overrides));
    warmIsland(w);
    yield* townsOf(w, key, r, true);
  }
}

/**
 * The records of a world's town plans (placesOf), drawing from r; warm: make every base height
 * they can read first (a World of World.js; a pureTerrain world makes them as it samples).
 */
export function* townsOf(w, key, r, warm) {
  const A = w.arterials;
  {
    for (const s of placesOf(w)) {
      // (the plan's church, and every cell its landmarks and the blocks below reach)
      const m = s.radius * 1.3 + 3 * A.spacing;
      if (warm) warmBasesIn(w, { x0: s.x - m, y0: s.y - m, x1: s.x + m, y1: s.y + m });
      const plan = townPlan(w, s);
      yield line("town", key, s.id, s.x, s.y, s.radius, plan.anchors.length);
      if (!plan.anchors.length) continue;
      // (landmarkUse of a block where a landmark is resolves the town: every landmark's block)
      const a0 = plan.anchors[0];
      landmarkUse(w, { id: "?", prop: { x0: a0.x, y0: a0.y, x1: a0.x, y1: a0.y } });
      for (const a of plan.anchors) yield line("a", a.kind, a.x, a.y, a.pri, a.block);
      // the uses of the blocks of the cells round the landmarks and of a few more in the town
      const cells = [];
      const seen = new Set();
      const at = (x, y) => {
        const c = A.cellAt(Math.round(x), Math.round(y));
        const k = `${c.i},${c.j}`;
        if (!seen.has(k)) {
          seen.add(k);
          cells.push(c);
        }
      };
      for (const a of plan.anchors) at(a.x, a.y);
      for (let k = 0; k < 3; k += 1) {
        const ang = r() * 2 * Math.PI;
        const d = r() * 1.1 * s.radius;
        at(s.x + Math.cos(ang) * d, s.y + Math.sin(ang) * d);
      }
      for (const { i, j } of cells) {
        const net = w.cellNet(i, j);
        const uses = net.blocks.map((b) => {
          const p = b.prop;
          const cut = { x0: p.x0 + 12, y0: p.y0 + 4, x1: p.x1 - 6, y1: p.y1 - 10 };
          return `${landmarkUse(w, b) ?? "-"}/${landmarkUse(w, { id: b.id, prop: cut }) ?? "-"}`;
        });
        yield line("uses", net.id, i, j, uses.join(","));
      }
    }
  }
}
