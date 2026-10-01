// Stage "streets": the local street patterns (city/streets.js: grid, subdivide, organic and the
// angled world's organic cuts on polygons, none) and the diagonal boulevards (city/diagonals.js)
// on their own - every pattern on sub-cells of every size and side, every registered district and
// variants of them (other patterns; merges, pedestrian streets, lanes and cobbles everywhere),
// with and without a district per part (emit.local), straight and tilted; each road and block
// emitted and the stream's state after; PATTERN_FAMILY; the diagonal families, line offsets,
// diagonalsOn of every city world and more configs, and the pieces crossing cell rects.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { cityWorlds } from "../lib/worlds.mjs";
import { frect, fside, fsides, fpoly } from "../lib/city.mjs";

const S = await import(REF + "city/streets.js");
const D = await import(REF + "city/diagonals.js");
const { Rng, hashFloat, hashString } = await import(REF + "core/hash.js");
const { DISTRICTS } = await import(REF + "world/registry.js");
await import(REF + "city/districts.js");
const { makeConfig } = await import(REF + "config/defaults.js");

const CLS = ["local", "collector", "arterial", "village", "rural"];
const PATTERNS = ["grid", "subdivide", "organic"];
const DEFAULT = Symbol("default paving");

/** A random side: no road, or a road of a class (with an id, or an arterial edge's null id). */
function side(r) {
  const k = Math.floor(r() * 4);
  const hr = 8 + Math.floor(r() * 60);
  const id = `s${Math.floor(r() * 1000)}`;
  return k === 0 ? { cls: null, hr: 0, id: null } : { cls: CLS[k], hr, id: k === 3 ? null : id };
}

/** The districts the patterns are run with: every registered one, then variants. */
export function districtVariants() {
  const base = DISTRICTS.all();
  const out = [];
  for (const d of base) {
    out.push(d);
    if (d.streets.pattern === "none") continue;
    for (const p of PATTERNS) if (p !== d.streets.pattern) out.push({ ...d, id: `${d.id}~${p}`, streets: { ...d.streets, pattern: p } });
  }
  for (const id of ["downtown", "mixed", "residential", "oldtown", "industrial"]) {
    const d = DISTRICTS.get(id);
    out.push({ ...d, id: `${id}~busy`, streets: { ...d.streets, mergeChance: 0.6, pedestrianChance: 0.4, laneChance: 0.7, laneClass: "lane", paving: "cobble" } });
  }
  return out;
}

export default function* streets() {
  const r = samples(61);
  const base = DISTRICTS.all();
  const variants = districtVariants();
  for (const d of variants) yield line("district", d.id, d.streets.pattern, d.streets.mergeChance, d.streets.pedestrianChance, d.streets.laneChance, d.streets.laneClass, d.streets.paving);
  for (let it = 0; it < 900; it += 1) {
    const d = variants[Math.floor(r() * variants.length)];
    const W = 120 + Math.floor(r() * 6000);
    const H = 120 + Math.floor(r() * 6000);
    const snap = r() < 0.7 ? 8 : 1;
    const x0 = Math.round(((r() - 0.5) * 40000) / snap) * snap;
    const y0 = Math.round(((r() - 0.5) * 40000) / snap) * snap;
    const rect = { x0, y0, x1: x0 + W, y1: y0 + H };
    const sides = { N: side(r), E: side(r), S: side(r), W: side(r) };
    const seed = Math.floor(r() * 4294967296);
    const mode = Math.floor(r() * 6);
    const useLocal = r() < 0.35;
    const localSeed = Math.floor(r() * 1e6);
    const opts = [undefined, { angled: true, tilt: 0 }, { angled: true, tilt: 0.35 }, { angled: true, tilt: 1 }, { angled: true }, { angled: false, tilt: 1 }][mode];
    yield line("call", it, d.id, frect(rect), fsides(sides), seed, mode, useLocal, localSeed);
    let n = 0;
    const lines = [];
    const emit = {
      road: (cls, ax, ay, bx, by, pav = DEFAULT) => {
        const id = `t${n}`;
        n += 1;
        lines.push(line("r", id, cls, ax, ay, bx, by, pav === DEFAULT ? "dflt" : f(pav)));
        return { cls, hr: 8 + (hashString(cls) % 41), id };
      },
      block: (rr, ss, poly) => lines.push(line("b", frect(rr), fsides(ss), fpoly(poly))),
      local: useLocal
        ? (rr) => {
            const h = hashFloat(localSeed, rr.x0, rr.y0, rr.x1 * 3 + rr.y1);
            if (h < 0.2) return null;
            return base[Math.floor(h * 1000) % base.length];
          }
        : null,
    };
    const rng = new Rng(seed);
    S.STREET_PATTERNS[d.streets.pattern]({ rect, sides }, d, rng, emit, opts);
    yield* lines;
    yield line("end", n, rng.next());
  }
  for (const p of ["grid", "organic", "subdivide", "none", "nope"]) yield line("family", p, S.PATTERN_FAMILY[p]);

  // ---- diagonals
  for (const fam of D.DIAGONAL_FAMILIES) yield line("diag", fam.f, fam.yaw, fam.c, fam.s, fam.r);
  for (let k = 0; k < 300; k += 1) {
    const seed = Math.floor((r() - 0.5) * 4e9);
    const fam = D.DIAGONAL_FAMILIES[Math.floor(r() * 2)];
    const kk = Math.floor((r() - 0.5) * 4000);
    const spacing = 800 + Math.floor(r() * 30000);
    yield line("off", seed, fam.f, kk, spacing, D.diagonalOffset(seed, fam, kk, spacing));
  }
  for (const [key, overrides] of cityWorlds()) yield line("on", key, D.diagonalsOn(makeConfig(overrides)));
  for (const json of DIAGONAL_CONFIGS) yield line("on", json, D.diagonalsOn(makeConfig(JSON.parse(json))));
  for (let k = 0; k < 500; k += 1) {
    const seed = Math.floor((r() - 0.5) * 4e9);
    const spacing = r() < 0.2 ? undefined : 200 + Math.floor(r() * 3000);
    const x0 = Math.floor((r() - 0.5) * 400000);
    const y0 = Math.floor((r() - 0.5) * 400000);
    const w = r() < 0.1 ? 1 + Math.floor(r() * 4) : 200 + Math.floor(r() * 9000);
    const h = r() < 0.1 ? 1 + Math.floor(r() * 4) : 200 + Math.floor(r() * 9000);
    const config = { seed, world: { angles: { enabled: true, diagonalSpacing: spacing } } };
    const pieces = D.diagonalPieces(config, { x0, y0, x1: x0 + w, y1: y0 + h });
    yield line("pieces", k, seed, spacing, x0, y0, w, h, pieces.length);
    for (const p of pieces) yield line("pc", p.fam.f, p.k, p.D, p.a.x, p.a.y, p.b.x, p.b.y);
  }
  // tiny rects on a line: corners cut short, lines through a corner
  for (let k = 0; k < 300; k += 1) {
    const seed = Math.floor((r() - 0.5) * 4e9);
    const spacing = 200 + Math.floor(r() * 3000);
    const fam = D.DIAGONAL_FAMILIES[Math.floor(r() * 2)];
    const kk = Math.floor((r() - 0.5) * 40);
    const Dk = D.diagonalOffset(seed, fam, kk, spacing * 8);
    const x = Math.floor((r() - 0.5) * 100000);
    const y = Math.floor((Dk + fam.s * x) / fam.c);
    const x0 = x - Math.floor(r() * 3);
    const y0 = y - Math.floor(r() * 3);
    const w = 1 + Math.floor(r() * 3);
    const h = 1 + Math.floor(r() * 3);
    const config = { seed, world: { angles: { enabled: true, diagonalSpacing: spacing } } };
    const pieces = D.diagonalPieces(config, { x0, y0, x1: x0 + w, y1: y0 + h });
    yield line("tiny", k, seed, spacing, fam.f, kk, x0, y0, w, h, pieces.length);
    for (const p of pieces) yield line("pc", p.fam.f, p.k, p.D, p.a.x, p.a.y, p.b.x, p.b.y);
  }
}

/** Configs for diagonalsOn beyond the city worlds (JSON, parsed alike by both sides). */
export const DIAGONAL_CONFIGS = [
  '{"world":{"angles":{"enabled":true}}}',
  '{"world":{"angles":{"enabled":true,"features":{"roads":false}}}}',
  '{"world":{"angles":{"enabled":true,"features":{"roads":0}}}}',
  '{"world":{"angles":{"enabled":true},"chart":"torus","size":30000}}',
  '{"world":{"angles":{"enabled":true},"chart":"cube","planet":{"radius":120000,"face":1}}}',
  '{"world":{"angles":{"enabled":1,"features":{"roads":true}},"chart":"flat"}}',
  '{"world":{"angles":{"enabled":false,"features":{"roads":true}}}}',
];
