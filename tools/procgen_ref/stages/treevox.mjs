// Stage "treevox": trees rasterized into chunks (nature/trees.js rasterizeTree) at LODs 0, 2, 5
// and 8. Twenty sampled trees of every kind (lib/trees.mjs sampleTree; and of a kind the tables do
// not know), anywhere, near a corner of the LOD 8 chunk grid (so chunk edges of every LOD cut
// them), off the voxel grid, or beyond 2^31 (coordinates wrapping in the hashes); a look from
// every season under four climates at any local temperature, or none and a snow cover. Each is
// rasterized into the chunks its foot, its crown and the corners of its bounds lie in (every
// chunk it touches for the first three of a kind at LODs 0 and 2, always at LODs 5 and 8), empty
// or partly filled first, tracking isolated voxels or not, isolating or not; then groves of
// eight trees of any kinds into shared chunks, and wild logs along the axes. A chunk is recorded
// by its non-air count and the digests of its data and iso arrays (as stages/chunk.mjs digests
// them).
import { REF, line, samples } from "../lib/rec.mjs";
import { KINDS, sampleTree, treeFields, SEASONS } from "../lib/trees.mjs";

const { ChunkBuffer } = await import(REF + "voxel/chunk.js");
const { rasterizeTree, treeBounds } = await import(REF + "nature/trees.js");

/** FNV-1a over a typed array's values (stages/chunk.mjs digest). */
function digest(a) {
  let h = 2166136261;
  for (let i = 0; i < a.length; i += 1) h = Math.imul(h ^ a[i], 16777619);
  return h >>> 0;
}

const LODS = [0, 2, 5, 8];
const SNOWS = [0, 0.25, 0.45, 1];

/** A look's fields: leaves, mix, bare, snow, holes, accent ("-": no look). */
const lookFields = (L) => (L ? [L.leaves, L.mix, L.bare, L.snow, L.holes, L.accent] : ["-"]);

/** A tree's look from r: none (and a snow cover), or a season's at a local temperature. */
function sampleLook(r, t) {
  if (r() < 0.2) return SNOWS[Math.floor(r() * 4)];
  const S = SEASONS[Math.floor(r() * SEASONS.length)];
  // (half of them where the seasons turn: bare or not, snow or not)
  const tl = r() < 0.5 ? r() * 1.1 - 0.15 : 0.2 + r() * 0.5;
  const L = S.treeLook(t.kind, t.seed, tl);
  if (L) t.look = L;
  return 0;
}

/** The chunk coordinates (at chunk span S) of points, without repeats, in order. */
function chunksOf(S, pts) {
  const out = [];
  const seen = new Set();
  for (const [x, y, z] of pts) {
    const c = [Math.floor(x / S), Math.floor(y / S), Math.floor(z / S)];
    const key = c.join(",");
    if (seen.has(key)) continue;
    seen.add(key);
    out.push(c);
  }
  return out;
}

/** Every chunk (at chunk span S) a box touches, the padded buffers' one-voxel apron included. */
function chunksTouching(lod, bb) {
  const S = 32 << lod;
  const s = 1 << lod;
  const out = [];
  for (let cz = Math.floor((bb.z0 - s) / S); cz <= Math.floor((bb.z1 + s) / S); cz += 1)
    for (let cy = Math.floor((bb.y0 - s) / S); cy <= Math.floor((bb.y1 + s) / S); cy += 1)
      for (let cx = Math.floor((bb.x0 - s) / S); cx <= Math.floor((bb.x1 + s) / S); cx += 1) out.push([cx, cy, cz]);
  return out;
}

/** A chunk made ready from r: tracking isolated voxels or not, isolating or not, partly filled first or not. */
function prepare(r, lod, c, t) {
  const ch = new ChunkBuffer(lod, c[0], c[1], c[2]);
  const q = r();
  if (q < 0.3) ch.trackIsolated();
  if (q < 0.15 || q > 0.9) ch.isolating = true;
  if (r() < 0.3) {
    const w = Math.floor(r() * 24) * ch.s;
    const m = 1 + Math.floor(r() * 399);
    ch.fillBox(t.x - w, t.y - 3 * ch.s, t.z + t.h * 0.3, t.x + w, t.y + w, t.z + t.h * 0.6, m, 0);
  }
  return ch;
}

const chunkLine = (lod, c, ch) => line("v", lod, c[0], c[1], c[2], ch.countNonAir(), digest(ch.data), ch.iso ? digest(ch.iso) : "-");

export default function* treevox() {
  const r = samples(37);
  for (const kind of KINDS)
    for (let i = 0; i < 20; i += 1) {
      let x = Math.floor((r() - 0.5) * 60000);
      let y = Math.floor((r() - 0.5) * 60000);
      const z = Math.floor(r() * 1200) - 200;
      if (r() < 0.4) {
        x = Math.round(x / 8192) * 8192 + Math.floor((r() - 0.5) * 60);
        y = Math.round(y / 8192) * 8192 + Math.floor((r() - 0.5) * 60);
      }
      if (i === 8) x += 2 ** 31;
      if (i === 9) y -= 2 ** 31 + 4096;
      const off = r() < 0.2;
      const ox = off ? r() : 0;
      const oy = off ? r() : 0;
      const oz = off && r() < 0.5 ? 0.5 : 0;
      const t = sampleTree(r, kind, x + ox, y + oy, z + oz);
      const snow = sampleLook(r, t);
      const bb = treeBounds(t);
      yield line("t", ...treeFields(t), snow, ...lookFields(t.look), bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1);
      for (const lod of LODS) {
        const S = 32 << lod;
        const whole = lod >= 5 || i < 3;
        const list = whole
          ? chunksTouching(lod, bb)
          : chunksOf(S, [
              [t.x, t.y, t.z],
              [t.x, t.y, t.z + t.h * 0.7],
              [bb.x0, bb.y1, t.z + t.h * 0.5],
              [bb.x1, bb.y0, bb.z1],
              [bb.x1 + S * 0.5, t.y, t.z],
            ]);
        for (const c of list) {
          const ch = prepare(r, lod, c, t);
          rasterizeTree(ch, t, snow);
          yield chunkLine(lod, c, ch);
        }
      }
    }
  // groves: eight trees of any kinds close together, into shared chunks
  for (let g = 0; g < 30; g += 1) {
    const gx = Math.floor((r() - 0.5) * 60000);
    const gy = Math.floor((r() - 0.5) * 60000);
    const gz = Math.floor(r() * 600);
    const trees = [];
    const snows = [];
    for (let k = 0; k < 8; k += 1) {
      const kind = KINDS[Math.floor(r() * KINDS.length)];
      const x = gx + Math.floor(r() * 160);
      const y = gy + Math.floor(r() * 160);
      const z = gz + Math.floor(r() * 24);
      const t = sampleTree(r, kind, x, y, z);
      snows.push(sampleLook(r, t));
      trees.push(t);
      yield line("gt", g, ...treeFields(t), snows[k], ...lookFields(t.look));
    }
    for (const lod of LODS) {
      const S = 32 << lod;
      const list = chunksOf(S, [
        [gx + 80, gy + 80, gz + 40],
        [gx + 40, gy + 120, gz + 12],
        [gx + 150, gy + 20, gz + 70],
      ]);
      for (const c of list) {
        const ch = prepare(r, lod, c, trees[0]);
        for (let k = 0; k < 8; k += 1) rasterizeTree(ch, trees[k], snows[k]);
        yield chunkLine(lod, c, ch);
      }
    }
  }
  // wild logs along the axes (limbs parallel to x and y), lying or windthrown
  for (const yaw of [0, 33, 66, 99])
    for (const plate of [false, true]) {
      const x = Math.floor((r() - 0.5) * 60000);
      const y = Math.floor((r() - 0.5) * 60000);
      const z = Math.floor(r() * 600);
      const h = 3 + Math.floor(r() * 5);
      const rr = 24 + Math.floor(r() * 24);
      const seed = Math.floor(r() * 4294967296);
      const t = { x, y, z, h, r: rr, kind: "log", seed, open: false, wild: true, reach: 27, yaw, plate };
      const bb = treeBounds(t);
      yield line("lt", ...treeFields(t), bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1);
      for (const lod of [0, 2])
        for (const c of chunksTouching(lod, bb)) {
          const ch = prepare(r, lod, c, t);
          rasterizeTree(ch, t);
          yield chunkLine(lod, c, ch);
        }
    }
}
