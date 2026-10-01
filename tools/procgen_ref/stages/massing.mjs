// Stage "massing": the coarse building voxelizer (buildings/massing.js) - snowAt over covers and
// columns; then envelopes of every archetype (planBuildingEnvelopeAs in a style, now and then
// planBuildingEnvelope in the district's pick) on scripted lots (plain and turned: canonical
// annexes), in districts as every flavor sees them, in worlds of every season and explicit snow
// cover (lib/shells.mjs), some with a chamfered corner or a snow cover of their own: their roof
// snow cover, and the chunks voxelizeMassing draws at LOD 0, 1, 2, 3 and 5 round their roof, a
// ground corner, the street facade and random points of their box, and those pitchedRoofOnly
// draws at LOD 0 and 1 round the roof and the annexes (digests of their voxels).
// tests/city/test_massing.cpp draws the same.
import { REF, line, samples } from "../lib/rec.mjs";
import { districtList, envLine } from "../lib/buildings.mjs";
import { SHELL_WORLDS, shellWorld, shellEnvelope, chunkDigest, chunksAt } from "../lib/shells.mjs";

const { ARCHETYPES, STYLES } = await import(REF + "world/registry.js");
const A = await import(REF + "buildings/archetypes.js");
const M = await import(REF + "buildings/massing.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");

const LODS = [0, 1, 2, 3, 5];
const COVERS = [0.05, 0.3, 0.5, 0.8, 0.99, 1, 1.3];
const SNOWS = [0, 0.45, 1];

export default function* massing() {
  const r = samples(61);
  // ---- snowAt over covers and columns
  for (let k = 0; k < 300; k += 1) {
    const seed = Math.floor((r() - 0.5) * 4294967296);
    const cover = COVERS[k % COVERS.length];
    const x0 = Math.floor((r() - 0.5) * 200000);
    const y0 = Math.floor((r() - 0.5) * 200000);
    let bits = "";
    for (let j = 0; j < 24; j += 1) for (let i = 0; i < 24; i += 1) bits += M.snowAt(seed, cover, x0 + i * 3, y0 + j * 5) ? "1" : "0";
    yield line("snow", seed, cover, x0, y0, bits);
  }
  // ---- envelopes of every archetype in every world
  const DS = districtList();
  const styles = STYLES.all().map((s) => s.id);
  const all = ARCHETYPES.all();
  // (every archetype eight times, then more churches: steeples, domes, tented spires)
  const kinds = [];
  for (let n = 0; n < 8; n += 1) kinds.push(...all);
  for (let n = 0; n < 24; n += 1) kinds.push(ARCHETYPES.get("church"));
  let k = 0;
  for (const a of kinds) {
    const wk = Math.floor(r() * SHELL_WORLDS.length);
    const w = shellWorld(wk);
    const style = r() < 0.15 ? null : styles[Math.floor(r() * styles.length)];
    const d = DS[Math.floor(r() * DS.length)];
    const { env } = shellEnvelope(r, a, style, d, w, (k += 1));
    const ch = r();
    const cl = r();
    const ck = r();
    const sn = r();
    const oh = r();
    if (env && env.roof.type === "flat" && ch < 0.3) {
      const m = 2 + Math.floor(ck * 2);
      env.chamfer = { side: cl < 0.5 ? "L" : "R", a: (ck < 0.5 ? 20 : 21) * m, b: (ck < 0.5 ? 21 : 20) * m };
    }
    if (env && sn < 0.06) env._snow = SNOWS[Math.floor(sn * 50)];
    // (a roof whose overhang leaves sides out: they take the default)
    if (env && env.roof.type !== "flat" && oh < 0.15) env.roof = { ...env.roof, overhang: oh < 0.075 ? { F: 1, B: undefined, L: undefined, R: 5 } : { F: undefined, B: 4, L: 2, R: undefined } };
    const cut = env?.chamfer ? `${env.chamfer.side}${env.chamfer.a}/${env.chamfer.b}` : "-";
    yield `${line("m", a.id, wk, style ?? "-", d.id, cut)} ${envLine(env)}`;
    if (!env) continue;
    yield line("cover", M.roofSnowCover(w, env));
    const b = env.bounds;
    const R = env.R;
    const zTop = A.floorZ(env, env.floors);
    const pts = [
      [(R.x0 + R.x1) / 2, (R.y0 + R.y1) / 2, zTop + 6],
      [(R.x0 + R.x1) / 2, (R.y0 + R.y1) / 2, (zTop + env.topZ) / 2],
      [R.x0, R.y0, env.baseZ + 4],
    ];
    for (const an of env.annexes) pts.push([(an.world.x0 + an.world.x1) / 2, (an.world.y0 + an.world.y1) / 2, env.baseZ + 4]);
    if (env.steeple) {
      // (the tower's belfry and its spire's finial)
      const t = A.envelopeFrame(env).rectToWorld(env.tiers[0].rects[env.steeple.rect]);
      const top = zTop + env.steeple.shaft + env.steeple.spire;
      pts.push([(t.x0 + t.x1) / 2, (t.y0 + t.y1) / 2, zTop + env.steeple.shaft - 10], [(t.x0 + t.x1) / 2, (t.y0 + t.y1) / 2, top + 6]);
    }
    // (chunks the building does not reach: beside it, above and below it)
    pts.push([b.x1 + 80, b.y1 + 80, zTop], [(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, env.topZ + 80], [(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2, env.bottomZ - 80]);
    const fx = R.x0 + r() * (R.x1 - R.x0);
    const fz = env.baseZ + r() * (zTop - env.baseZ);
    pts.push([fx, R.y0, fz]);
    for (let q = 0; q < 2; q += 1) {
      const x = b.x0 + r() * (b.x1 - b.x0);
      const y = b.y0 + r() * (b.y1 - b.y0);
      const z = env.bottomZ + r() * (env.topZ - env.bottomZ);
      pts.push([x, y, z]);
    }
    for (const [lod, cx, cy, cz] of chunksAt(LODS, pts)) {
      const c = new ChunkBuffer(lod, cx, cy, cz);
      M.voxelizeMassing(w, env, c);
      yield line("c", lod, cx, cy, cz, ...chunkDigest(c));
    }
    for (const [lod, cx, cy, cz] of chunksAt([0, 1], [...pts.slice(0, 3 + env.annexes.length), pts[pts.length - 6]])) {
      const c = new ChunkBuffer(lod, cx, cy, cz);
      M.pitchedRoofOnly(w, env, c);
      yield line("p", lod, cx, cy, cz, ...chunkDigest(c));
    }
  }
}
