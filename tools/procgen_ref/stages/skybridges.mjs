// Stage "skybridges": skybridges (city/skybridges.js) - planSkybridges over scripted cells: two
// rows of buildings facing each other across a street (along x or y, facades 40 to 280 voxels
// apart), offices, towers (bridges at podium level; residential towers left out), walk-ups, now and
// then a turned one, downtown or not, futuristic or not, with a highway corridor across the street
// now and then; the bridges and every building's sky doors; then the feature source over each
// bridge's chunks at LOD 0 to 4 (zRange, rasterize: the bridges of the cell whose rect meets the
// chunk). tests/city/test_skybridges.cpp plans and draws the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { shellWorld, chunkDigest, chunksAt } from "../lib/shells.mjs";

const { STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const S = await import(REF + "city/skybridges.js");
const Fr = await import(REF + "buildings/frame.js");
const { Rng } = await import(REF + "core/hash.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");

const KINDS = ["office", "office", "office", "tower", "tower", "walkup"];
const FLAVORS = ["futuristic", "futuristic", "modern", ""];

const rstr = (q) => [q.x0, q.y0, q.x1, q.y1].join(",");
const doorStr = (d) => `${d.floor}/${d.span.x0 !== undefined ? `x${d.span.x0},${d.span.x1}` : `y${d.span.y0},${d.span.y1}`}/${d.bridge}`;

export default function* skybridges() {
  const r = samples(79);
  const DS = districtList();
  const downtown = DS.filter((d) => d.id === "downtown");
  const styles = STYLES.all().map((s) => s.id);
  for (let cell = 0; cell < 100; cell += 1) {
    const w = shellWorld(cell % 3);
    const alongX = r() < 0.5;
    const n = 2 + Math.floor(r() * 3);
    const ox = Math.floor((r() - 0.5) * 100000);
    const oy = Math.floor((r() - 0.5) * 100000);
    const gap = 50 + Math.floor(r() * 210);
    // (now and then one wide futuristic office facing a row of narrow ones, on either side: its
    // bridges and doors add up)
    const wide = r() < 0.4;
    const wideSide = r() < 0.5 ? 0 : 1;
    const buildings = [];
    for (const side of [0, 1]) {
      let pos = (alongX ? ox : oy) + Math.floor(r() * 80) - 40;
      const big = wide && side === wideSide;
      for (let q = 0; q < (big ? 1 : wide ? n + 2 : n); q += 1) {
        const kd = KINDS[Math.floor(r() * KINDS.length)];
        const kind = wide ? "office" : kd;
        const u = r();
        const U = big ? 1200 + Math.floor(u * 600) : wide ? 160 + Math.floor(u * 40) : 160 + Math.floor(u * 300);
        const V = 160 + Math.floor(r() * 300);
        const shift = Math.floor(r() * 60);
        const turned = r() < 0.08;
        const yaw = Math.floor(r() * 132);
        const dt = r() < 0.7;
        const d = wide || dt ? downtown[Math.floor(r() * downtown.length)] : DS[Math.floor(r() * DS.length)];
        const style = styles[Math.floor(r() * styles.length)];
        const fl = FLAVORS[Math.floor(r() * FLAVORS.length)];
        const flavor = wide ? "futuristic" : fl;
        const z = 400 + Math.floor(r() * 4);
        const lot = { id: `C0_${cell}/b${side}/l${q}`, district: d.id, corner: false, micro: false };
        if (alongX) {
          lot.front = side ? "N" : "S";
          const y0 = side ? oy + 1 + gap : oy - V + 1;
          lot.rect = { x0: pos, y0, x1: pos + U - 1, y1: y0 + V - 1 };
        } else {
          lot.front = side ? "W" : "E";
          const x0 = side ? ox + 1 + gap : ox - V + 1;
          lot.rect = { x0, y0: pos, x1: x0 + V - 1, y1: pos + U - 1 };
        }
        if (turned && !big) {
          lot.front = Fr.nominalFront(yaw);
          lot.turn = { yaw, origin: { x: lot.rect.x0, y: lot.rect.y0 }, ou: 0, ov: 0, U, V };
          lot.rect = { ...Fr.lotFrameOf(lot).R };
        }
        pos += U + shift;
        const extra = { u: r(), core: r(), groundZ: z, config: w.config };
        const env = A.planBuildingEnvelopeAs(lot, kind, style, d, new Rng(Math.floor(r() * 4294967296)), extra);
        yield `${line("b", kind, flavor || "-")} ${envLine(env)}`;
        if (!env) continue;
        if (flavor) env.flavor = flavor;
        buildings.push(env);
      }
    }
    // a highway corridor across the street now and then
    const hc = r() < 0.4;
    const ha = Math.floor(r() * 1500);
    const hs = 100 + Math.floor(r() * 600);
    const c = alongX ? { x0: ox + ha, y0: oy - 40, x1: ox + ha + hs, y1: oy + gap + 40 } : { x0: ox - 40, y0: oy + ha, x1: ox + gap + 40, y1: oy + ha + hs };
    const corridors = hc ? [{ hitsRect: (q) => !(q.x1 < c.x0 || q.x0 > c.x1 || q.y1 < c.y0 || q.y0 > c.y1) }] : [];
    const bridges = S.planSkybridges(w, { id: `C0_${cell}` }, buildings, corridors);
    yield line("cell", cell, alongX, gap, hc ? rstr(c) : "-", bridges.length);
    for (const br of bridges) yield line("bridge", br.id, br.a, br.b, br.alongX, rstr(br.rect), br.za, br.zb, br.bb.z0, br.bb.z1);
    for (const env of buildings) yield line("doors", env.id, env.skyDoors ? env.skyDoors.map(doorStr).join(";") : "-");
    // the feature source: the cell's bridges whose rect meets the chunk (and a cell without any)
    const world = { cellsOverlapping: () => [{ i: 0, j: 0 }, { i: 1, j: 0 }], cellPlan: (i) => (i === 0 ? { skybridges: bridges } : {}) };
    for (const br of bridges) {
      const q = br.rect;
      const pts = [
        [(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, (br.za + br.zb) / 2 + 10],
        [q.x0, q.y0, br.za],
        [q.x1, q.y1, br.zb + 24],
        [q.x1 + 300, q.y1, br.za],
        [(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, br.zb + 400],
      ];
      for (const [lod, cx, cy, cz] of chunksAt([0, 1, 2, 3, 4], pts)) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        const zr = S.skybridgeSource.zRange(world, ch.worldBox);
        S.skybridgeSource.rasterize(world, ch);
        yield line("c", lod, cx, cy, cz, zr ? zr.join(",") : "-", ...chunkDigest(ch));
      }
    }
  }
}
