// Stage "chunk": the chunk writer (voxel/chunk.js) at LODs 0 to 8 - its geometry (world box,
// representatives, index ranges, core box) and a random sequence of writes (set, setIfAir,
// fillBox in every mode, isolating on and off, points off the grid, non-integer and non-finite
// coordinates), read back through sampleWorld, touches and digests of its data and iso arrays.
import { REF, line, samples } from "../lib/rec.mjs";

const { ChunkBuffer, chunkCoreBox, P } = await import(REF + "voxel/chunk.js");

/** FNV-1a over a typed array's values (the same in tests/city/test_chunk.cpp). */
function digest(a) {
  let h = 2166136261;
  for (let i = 0; i < a.length; i += 1) h = Math.imul(h ^ a[i], 16777619);
  return h >>> 0;
}

const range = ([lo, hi]) => (lo <= hi ? `${lo},${hi}` : "e");

export default function* chunk() {
  const r = samples(7);
  for (let lod = 0; lod <= 8; lod += 1)
    for (let c = 0; c < 6; c += 1) {
      const cx = Math.floor((r() - 0.5) * 8);
      const cy = Math.floor((r() - 0.5) * 8);
      const cz = Math.floor((r() - 0.3) * 4);
      const ch = new ChunkBuffer(lod, cx, cy, cz);
      if (r() < 0.5) ch.trackIsolated();
      const wb = ch.worldBox;
      const core = chunkCoreBox(lod, cx, cy, cz);
      yield line("c", lod, cx, cy, cz, ch.s, ch.half, ch.bx, ch.by, ch.bz, wb.x0, wb.y0, wb.z0, wb.x1, wb.y1, wb.z1, core.x0, core.y0, core.z0, core.x1, core.y1, core.z1, ch.wx(0), ch.wy(5), ch.wz(33));
      const span = P * ch.s;
      // a coordinate near the buffer on an axis: a representative, any voxel, off the grid, not finite
      const coord = (b0, rep) => {
        const t = r();
        if (t < 0.4) return rep(Math.floor(r() * (P + 6)) - 3);
        if (t < 0.9) return b0 + Math.floor((r() * 1.4 - 0.2) * span);
        if (t < 0.96) return b0 + Math.floor(r() * span) + 0.5;
        if (t < 0.98) return NaN;
        return r() < 0.5 ? Infinity : -Infinity;
      };
      const px = () => coord(wb.x0, (i) => ch.wx(i));
      const py = () => coord(wb.y0, (j) => ch.wy(j));
      const pz = () => coord(wb.z0, (k) => ch.wz(k));
      for (let op = 0; op < 150; op += 1) {
        const kind = Math.floor(r() * 9);
        if (kind === 0 || kind === 1) {
          const x = px();
          const y = py();
          const z = pz();
          const m = Math.floor(r() * 400);
          if (kind === 0) ch.set(x, y, z, m);
          else ch.setIfAir(x, y, z, m);
        } else if (kind === 2 || kind === 3) {
          const x0 = px();
          const y0 = py();
          const z0 = pz();
          const x1 = x0 + Math.floor(r() * span * 0.6);
          const y1 = y0 + Math.floor(r() * span * 0.6);
          const z1 = z0 + Math.floor(r() * span * 0.6) - Math.floor(span * 0.05);
          const m = Math.floor(r() * 400);
          const mode = Math.floor(r() * 4);
          ch.fillBox(x0, y0, z0, x1, y1, z1, m, mode);
        } else if (kind === 4) {
          ch.isolating = r() < 0.5;
        } else if (kind === 5) {
          const x = px();
          const y = py();
          const z = pz();
          const d = Math.floor((r() - 0.2) * span);
          // (JS keeps an empty range's raw bounds, the port clamps them: only emptiness is compared)
          yield line("r", range(ch.rangeX(x, x + d)), range(ch.rangeY(y - 7, y + d)), range(ch.rangeZ(z, z + d + 3)));
        } else if (kind === 6) {
          const x = px();
          const y = py();
          const z = pz();
          // (a NaN coordinate samples undefined in JS, 0 in the port: not sampled)
          if (x === x && y === y && z === z) yield line("s", ch.sampleWorld(x, y, z), ch.sampleWorld(x + 0.25, y, z - 0.5));
        } else if (kind === 7) {
          const x0 = wb.x0 + Math.floor((r() * 2 - 0.6) * span);
          const y0 = wb.y0 + Math.floor((r() * 2 - 0.6) * span);
          const z0 = wb.z0 + Math.floor((r() * 2 - 0.6) * span);
          const d = Math.floor(r() * span * 0.3);
          yield line("t", ch.touches(x0, y0, z0, x0 + d, y0 + d, z0 + d), ch.touchesRect({ x0, y0, x1: x0 + d, y1: y0 + 2 * d }, z0 - d, z0));
        } else {
          yield line("g", ch.get(Math.floor(r() * P), Math.floor(r() * P), Math.floor(r() * P)));
        }
        if (op % 25 === 24) yield line("d", digest(ch.data), ch.iso ? digest(ch.iso) : "-");
      }
      yield line("n", ch.countNonAir(), ch.nonAir, digest(ch.data), ch.iso ? digest(ch.iso) : "-");
    }
}
