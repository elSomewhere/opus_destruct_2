// Stage "landmarks": the landmarks of an island's open country (world/landmarks.js) - on every
// island world (createWorld, its terrain made pure: lib/worlds.mjs pureTerrain): the lighthouse,
// the boathouses, the fish racks and the cairns planned (each landmark's kind, bounds, footprint and
// a digest of its boxes), blocks() round them and over the island, near() over sample rects, and the
// feature source (zRange, rasterize) round each landmark at LOD 0 to 4 (and an island whose config
// leaves its peak out: the cairns' default height). The open-ground test reads
// the cell plans (lots and urban spaces: a later stage of the port), so its answers are recorded in
// data/landmarks.json and the port replays them (Landmarks::free_source). Then free() itself on
// scripted roads (lib/roads.mjs scriptedRoads, served as a World's cell roads) and scripted cell
// plans (lots and spaces as rects: Landmarks::plan_occupied). tests/city/test_landmarks.cpp makes
// the same.
import { writeFileSync, mkdirSync } from "node:fs";
import { REF, line, samples } from "../lib/rec.mjs";
import { allWorlds, pureTerrain } from "../lib/worlds.mjs";
import { scriptedRoads, DATA } from "../lib/roads.mjs";
import { chunkDigest, chunksAt } from "../lib/shells.mjs";

const { createWorld } = await import(REF + "world/createWorld.js");
const { World } = await import(REF + "world/World.js");
const { Landmarks, landmarkSource } = await import(REF + "world/landmarks.js");
const { ChunkBuffer } = await import(REF + "voxel/chunk.js");

/** FNV-1a over a string (32 bits). */
function fnv(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i += 1) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

const bstr = (b) => [b.x0, b.y0, b.z0, b.x1, b.y1, b.z1].join(",");
const rstr = (q) => [q.x0, q.y0, q.x1, q.y1].join(",");

export default function* landmarks() {
  const r = samples(83);
  const inputs = {};
  // (and an island whose config leaves out the peak)
  const worlds = [...allWorlds(), ["islandNoPeak", JSON.parse('{"seed":17,"world":{"mode":"island","island":{"radius":1800,"peak":null,"population":3000}}}')]];
  for (const [key, o] of worlds) {
    const w = createWorld(o);
    if (!w.landmarks) continue;
    pureTerrain(w);
    // (the open-ground test's answers, recorded for the port)
    const asked = [];
    const free = w.landmarks.free.bind(w.landmarks);
    w.landmarks.free = (x, y) => {
      const v = free(x, y);
      asked.push([x, y, v ? 1 : 0]);
      return v;
    };
    const items = w.landmarks.all();
    inputs[key] = asked;
    yield line("world", key, items.length, asked.length);
    for (const it of items) {
      const boxes = it.boxes.map((q) => [q.x0, q.y0, q.x1, q.y1, q.z0, q.z1, q.m].join(",")).join(";");
      yield line("lm", it.kind, bstr(it.bb), rstr(it.foot), it.boxes.length, fnv(boxes));
    }
    // blocks: round each landmark and over the island
    const isl = w.fields.island;
    const b = isl.bounds();
    let bits = "";
    for (const it of items)
      for (let k = 0; k < 24; k += 1) {
        const x = Math.floor(it.foot.x0 - 40 + r() * (it.foot.x1 - it.foot.x0 + 80));
        const y = Math.floor(it.foot.y0 - 40 + r() * (it.foot.y1 - it.foot.y0 + 80));
        const m = k % 3 === 0 ? 16 : k % 3 === 1 ? 0 : 6;
        bits += w.landmarks.blocks(x, y, m) ? "1" : "0";
      }
    for (let k = 0; k < 200; k += 1) {
      const x = Math.floor((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const y = Math.floor((b.y0 + r() * (b.y1 - b.y0)) * 8);
      bits += w.landmarks.blocks(x, y) ? "1" : "0";
    }
    yield line("blocks", bits);
    // near over sample rects
    for (let k = 0; k < 30; k += 1) {
      const x0 = Math.floor((b.x0 + r() * (b.x1 - b.x0)) * 8);
      const y0 = Math.floor((b.y0 + r() * (b.y1 - b.y0)) * 8);
      const q = { x0, y0, x1: x0 + Math.floor(r() * 20000), y1: y0 + Math.floor(r() * 20000) };
      yield line("near", rstr(q), w.landmarks.near(q).map((it) => items.indexOf(it)).join(",") || "-");
    }
    // the feature source round each landmark
    for (const it of items) {
      const q = it.bb;
      const pts = [
        [(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, (q.z0 + q.z1) / 2],
        [(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, q.z1],
        [(q.x0 + q.x1) / 2, (q.y0 + q.y1) / 2, q.z1 + 300],
        [q.x0 - 3000, q.y0 - 3000, q.z0],
      ];
      for (const [lod, cx, cy, cz] of chunksAt([0, 1, 2, 3, 4], pts)) {
        const ch = new ChunkBuffer(lod, cx, cy, cz);
        const zr = landmarkSource.zRange(w, ch.worldBox);
        landmarkSource.rasterize(w, ch);
        yield line("c", lod, cx, cy, cz, zr ? zr.join(",") : "-", ...chunkDigest(ch));
      }
    }
  }
  mkdirSync(DATA, { recursive: true });
  writeFileSync(new URL("landmarks.json", DATA), JSON.stringify(inputs) + "\n");
  // ---- the source on worlds without landmarks
  {
    const w = createWorld({ seed: 3 });
    const ch = new ChunkBuffer(0, 0, 0, 0);
    yield line("none", w.landmarks === null, landmarkSource.zRange(w, ch.worldBox) ?? "-");
    landmarkSource.rasterize(w, ch);
    yield line("nonec", ...chunkDigest(ch));
  }
  // ---- free() on scripted roads and plans
  const w = new World({ seed: 41 });
  const roads = new Map();
  const lots = new Map();
  w.cellNet = (i, j) => ({ roads: roads.get(`${i},${j}`) ?? [] });
  w.cellPlan = (i, j) => {
    const q = lots.get(`${i},${j}`) ?? [];
    const at = (kind) => (x, y) => q.find((p) => p.kind === kind && x >= p.x0 && x <= p.x1 && y >= p.y0 && y <= p.y1) ?? null;
    return { lotAt: at("lot"), spaceAt: at("space") };
  };
  const lm = new Landmarks(w);
  yield line("noisland", lm.all().length);
  for (let k = 0; k < 40; k += 1) {
    const i = 3 * (k % 8) - 12;
    const j = 3 * Math.floor(k / 8) - 6;
    const rc = w.arterials.cellRect(i, j);
    const ox = rc.x0 + 1000;
    const oy = rc.y0 + 1000;
    roads.set(`${i},${j}`, scriptedRoads(r, k, ox, oy));
    const q = [];
    const n = Math.floor(r() * 8);
    for (let s = 0; s < n; s += 1) {
      const x0 = ox + Math.floor(r() * 1200);
      const y0 = oy + Math.floor(r() * 1200);
      const kind = r() < 0.5 ? "lot" : "space";
      q.push({ kind, x0, y0, x1: x0 + Math.floor(r() * 300), y1: y0 + Math.floor(r() * 300) });
    }
    lots.set(`${i},${j}`, q);
    yield line("plan", i, j, q.map((p) => `${p.kind}${rstr(p)}`).join(";") || "-");
    let bits = "";
    for (let s = 0; s < 400; s += 1) {
      const x = Math.floor(ox - 100 + r() * 1400);
      const y = Math.floor(oy - 100 + r() * 1400);
      bits += lm.free(x, y) ? "1" : "0";
    }
    yield line("free", bits);
  }
}
