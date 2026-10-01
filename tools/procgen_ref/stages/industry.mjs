// Stage "industry": heavy-industry layouts (city/industry.js) of sample spaces - tank farms,
// container yards and spaces of other kinds - seen through their props (dressIndustry: every
// tank, rack, column, flare, stack and crane with its options; a dry world, then a wet one with
// a quay) and their ground surface (industrySurface at sample columns); harbour quays (portQuay)
// against scripted worlds (a lake's half-plane with a ragged edge), quaySide, and dressPort.
// tests/city/test_industry.cpp scripts the same worlds.
import { REF, f, line, samples } from "../lib/rec.mjs";

const I = await import(REF + "city/industry.js");
const { hash32 } = await import(REF + "core/hash.js");

const PROP_OPTS = ["h", "reach", "pole", "r", "seed", "n", "hw", "len", "span", "plinth"];
const fextra = (o) => PROP_OPTS.map((k) => f(o?.[k])).join(",");
const KINDS = ["tankFarm", "containerYard", "parking", "tankFarm", "containerYard"];

// a lake's half-plane nx x + ny y > c with a ragged edge
function waterWorld(r) {
  const nx = r() - 0.5;
  const ny = r() - 0.5;
  const c = (r() - 0.5) * 3000;
  const level = Math.floor(r() * 40);
  const none = r() < 0.15;
  const seed = Math.floor(r() * 1e6);
  return {
    desc: [nx, ny, c, level, none, seed],
    shoreNear(x, y, d) {
      if (none) return null;
      return { level, dist: nx * x + ny * y - c - d, nx: -nx, ny: -ny, lake: null };
    },
    openWaterAt(x, y) {
      return nx * x + ny * y - c + ((hash32(seed, x, y) & 63) - 32) > 0;
    },
    isWet(x, y, m) {
      return nx * x + ny * y - c + m * 8 > 0;
    },
  };
}

export default function* industry() {
  const r = samples(31);
  for (let i = 0; i < 120; i += 1) {
    const kind = KINDS[i % KINDS.length];
    const x0 = Math.floor((r() - 0.5) * 20000);
    const y0 = Math.floor((r() - 0.5) * 20000);
    const w = 40 + Math.floor(r() * 1500);
    const h = 40 + Math.floor(r() * 1500);
    const z = Math.floor(r() * 200);
    const space = { id: `C${i % 9}_${-(i % 5)}/b${i}/o`, kind, rect: { x0, y0, x1: x0 + w, y1: y0 + h } };
    yield line("space", i, space.id, kind, x0, y0, x0 + w, y0 + h, z);
    // the layout through the props of a dry world without a quay
    const props = [];
    const addProp = (k, x, y, zz, ax, ay, bx, by, extra) => props.push(line(k, x, y, zz, ax, ay, bx, by, fextra(extra)));
    I.dressIndustry({}, space, z, addProp);
    yield line("dry", props.length);
    yield* props;
    // the ground surface
    const surf = [];
    for (let k = 0; k < 160; k += 1) {
      const x = x0 - 6 + Math.floor(r() * (w + 12));
      const y = y0 - 6 + Math.floor(r() * (h + 12));
      const out = { mat: -1 };
      const handled = I.industrySurface(space, x, y, out);
      surf.push(`${handled ? 1 : 0}${out.mat}`);
    }
    yield line("surf", surf.join(" "));
    // a harbour: the quay of a scripted world, then the props of a wet world
    const world = waterWorld(r);
    const q = I.portQuay(world, space.rect);
    yield line("quay", ...world.desc, q ? [q.axis, q.sign, q.c, q.level] : "-");
    if (q) {
      const sides = [];
      for (let k = 0; k < 12; k += 1) sides.push(I.quaySide(q, x0 + r() * w, y0 + r() * h));
      yield line("qs", ...sides);
    }
    const wet = { id: space.id, kind, rect: space.rect, quay: q };
    const props2 = [];
    const add2 = (k, x, y, zz, ax, ay, bx, by, extra) => props2.push(line(k, x, y, zz, ax, ay, bx, by, fextra(extra)));
    I.dressIndustry(world, wet, z, add2);
    I.dressPort(world, wet, z, add2);
    yield line("wet", props2.length);
    yield* props2;
  }
}
