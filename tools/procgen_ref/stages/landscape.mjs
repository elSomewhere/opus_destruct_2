// Stage "landscape": the ground surfaces of open spaces and lots (city/landscape.js). spaceSurface
// over synthetic open spaces of every kind it knows and one it does not (a park then) - plazas,
// quays, car parks, sports grounds, courtyards (the projects' too), squares, gardens, cemeteries
// and allotments facing every side or none, garage yards, wasteland, tank farms and container
// yards (city/industry.js), riverside parks, parks (city/parks.js) - at columns over, round, near
// the middle of and along the edges of each, on whole and fractional voxels, with canonical texture
// positions of their own now and then: out.mat, out.dz, out.water. lotSurface over synthetic
// building envelopes of every archetype and civic building (and one the code does not know), on
// plain frames of every front and turned frames, with footprints of one or two rects, annexes
// (garages: a driveway), with and without entranceU, at columns round each; a lot without a
// building; cobble; ALLOT. The envelopes and spaces are drawn from simple generators the C++ test
// (tests/city/test_landscape.cpp) reproduces; each is recorded as an input line.
import { REF, f, line, samples } from "../lib/rec.mjs";
import { frect } from "../lib/city.mjs";

const LS = await import(REF + "city/landscape.js");
const { Frame, turnedFrame } = await import(REF + "buildings/frame.js");

export const KINDS = ["plaza", "quay", "parking", "sports", "courtyard", "square", "garden", "cemetery", "allotments", "garages", "wasteland", "tankFarm", "containerYard",
  "riverside", "park", "meadow"];
const DISTRICTS = ["residential", "microdistrict", "oldtown", "harbour"];
const FRONTS = ["N", "E", "S", "W", undefined];
const WSIDES = ["N", "E", "S", "W"];
export const ARCHS = ["house", "rowhouse", "walkup", "midrise", "panelSlab", "panelTower", "office", "tower", "warehouse", "barn", "factory", "garage", "school", "townhouse",
  "wharfhouse", "cabin", "church"];
export const CIVICS = ["supermarket", "departmentStore", "petrolStation", "marketHall", "concertHall", "houseOfCulture", "musicClub", "cinema", "hospital", "polyclinic",
  "policeStation", "fireStation", "museum", "artGallery", "library", "townHall", "hotel", "bathhouse"];
const SEEDS = [1337, 7, -12345, 99, 2024];

/** A canonical texture position's lap offset (a wrapping world's), drawn from r: none most of the time. */
const lap = (r) => (r() < 0.3 ? Math.floor((r() - 0.5) * 8) * 25600 : 0);

export default function* landscape() {
  const r = samples(41);
  yield line("allot", LS.ALLOT.w, LS.ALLOT.d, LS.ALLOT.path);
  // open spaces
  const out = { mat: 0, dz: 0, water: false };
  for (let k = 0; k < 352; k += 1) {
    const kind = KINDS[k % KINDS.length];
    const i = Math.floor((r() - 0.5) * 100);
    const j = Math.floor((r() - 0.5) * 100);
    const x0 = Math.floor((r() - 0.5) * 600000);
    const y0 = Math.floor((r() - 0.5) * 600000);
    const w = 8 + Math.floor(r() * 1200);
    const h = 8 + Math.floor(r() * 1200);
    const district = r() < 0.4 ? "projects" : DISTRICTS[Math.floor(r() * DISTRICTS.length)];
    const front = FRONTS[Math.floor(r() * FRONTS.length)];
    const space = { id: `C${i}_${j}/b${k}/o`, kind, district, rect: { x0, y0, x1: x0 + w, y1: y0 + h }, ...(front ? { front } : {}) };
    yield line("space", k, space.id, kind, district, front, frect(space.rect));
    const pts = [];
    for (let q = 0; q < 100; q += 1) pts.push([x0 - 8 + Math.floor(r() * (w + 17)), y0 - 8 + Math.floor(r() * (h + 17))]);
    for (let q = 0; q < 24; q += 1) pts.push([x0 + r() * w, y0 + r() * h]);
    for (let q = 0; q < 24; q += 1) pts.push([Math.round(x0 + w / 2 + (r() - 0.5) * 80), Math.round(y0 + h / 2 + (r() - 0.5) * 80)]);
    for (let q = 0; q < 24; q += 1) {
      const t = Math.floor(r() * 4);
      const a = Math.floor(r() * 24);
      const along = r();
      const x = t === 0 ? x0 + a : t === 1 ? x0 + w - a : x0 + Math.floor(along * w);
      const y = t === 2 ? y0 + a : t === 3 ? y0 + h - a : y0 + Math.floor(along * h);
      pts.push([x, y]);
    }
    const recs = [];
    for (const [x, y] of pts) {
      const lx = lap(r);
      const ly = lap(r);
      out.mat = 0;
      out.dz = 0;
      out.water = false;
      LS.spaceSurface(space, x, y, out, x + lx, y + ly);
      recs.push(`${f(x)},${f(y)},${f(lx)},${f(ly)}:${out.mat}/${f(out.dz)}/${f(out.water)}`);
    }
    for (let q = 0; q < recs.length; q += 16) yield line("ss", recs.slice(q, q + 16).join(" "));
  }
  // lots: synthetic envelopes
  for (let k = 0; k < 360; k += 1) {
    const civic = k % 3 === 2 ? CIVICS[Math.floor(r() * CIVICS.length)] : null;
    const arch = civic ?? ARCHS[Math.floor(r() * ARCHS.length)];
    const front = WSIDES[Math.floor(r() * 4)];
    const x0 = Math.floor((r() - 0.5) * 600000);
    const y0 = Math.floor((r() - 0.5) * 600000);
    const turned = r() < 0.25;
    let frame;
    let turn = null;
    if (turned) {
      turn = { yaw: Math.floor(r() * 132), origin: { x: x0, y: y0 }, ou: Math.floor((r() - 0.5) * 40), ov: Math.floor((r() - 0.5) * 40) };
      const U = 40 + Math.floor(r() * 400);
      const V = 40 + Math.floor(r() * 300);
      frame = turnedFrame(turn, U, V, front);
    } else {
      const R = { x0, y0, x1: x0 + 39 + Math.floor(r() * 400), y1: y0 + 39 + Math.floor(r() * 300) };
      frame = new Frame(R, front);
    }
    const { U, V } = frame;
    const ground = [{ x0: 0, y0: 0, x1: U - 1 - Math.floor(r() * U * 0.3), y1: V - 1 - Math.floor(r() * V * 0.3) }];
    if (r() < 0.4) ground.push({ x0: Math.floor(r() * U * 0.5), y0: Math.floor(r() * V * 0.5), x1: U - 1, y1: V - 1 });
    const annexes = [];
    const na = Math.floor(r() * 3);
    for (let q = 0; q < na; q += 1) {
      const kind = r() < 0.6 ? "garage" : "canopy";
      const ax = Math.floor((r() - 0.3) * U);
      const ay = Math.floor((r() - 0.2) * V);
      const rect = { x0: ax, y0: ay, x1: ax + 20 + Math.floor(r() * 40), y1: ay + 30 + Math.floor(r() * 40) };
      annexes.push(turned ? { kind, canon: rect, world: frame.rectToWorld(rect) } : { kind, world: frame.rectToWorld(rect) });
    }
    const entranceU = r() < 0.8 ? Math.floor(r() * U) : undefined;
    const seed = SEEDS[k % SEEDS.length];
    const env = { archetype: arch, ...(civic ? { civic } : {}), front, R: frame.R, U, V, ...(turn ? { turn } : {}), tiers: [{ f0: 0, f1: 0, rects: ground }], annexes,
      ...(entranceU !== undefined ? { entranceU } : {}) };
    yield line("env", k, arch, civic, front, frect(env.R), U, V, turn ? `${turn.yaw},${turn.origin.x},${turn.origin.y},${turn.ou},${turn.ov}` : "-", entranceU, seed,
      ground.map(frect).join(";"), annexes.map((a) => `${a.kind}/${a.canon ? frect(a.canon) : "-"}/${frect(a.world)}`).join(";"));
    // columns round the building, then cells of its own frame (in front, round and behind it; on a
    // car park's third row of stall lines; on the centre line of a schoolyard's court)
    const R = env.R;
    const cols = [];
    for (let q = 0; q < 120; q += 1) cols.push([R.x0 - 80 + Math.floor(r() * (R.x1 - R.x0 + 160)), R.y0 - 80 + Math.floor(r() * (R.y1 - R.y0 + 160))]);
    for (let q = 0; q < 60; q += 1) {
      const u = Math.floor(-60 + r() * (U + 120));
      const v = q < 48 ? Math.floor(-100 + r() * (V + 300)) : q < 54 ? -96 : V + 92;
      cols.push(frame.toWorld(u, v));
    }
    const recs = [];
    for (const [x, y] of cols) {
      const lx = lap(r);
      const ly = lap(r);
      recs.push(`${f(x)},${f(y)},${f(lx)},${f(ly)}:${LS.lotSurface(null, env, x, y, seed, x + lx, y + ly)}`);
    }
    for (let q = 0; q < recs.length; q += 20) yield line("ls", recs.slice(q, q + 20).join(" "));
  }
  // a lot without a building
  for (let k = 0; k < 40; k += 1) {
    const seed = SEEDS[k % SEEDS.length];
    const recs = [];
    for (let q = 0; q < 40; q += 1) {
      const x = Math.floor((r() - 0.5) * 600000);
      const y = Math.floor((r() - 0.5) * 600000);
      const lx = lap(r);
      recs.push(`${f(x)},${f(y)},${f(lx)}:${LS.lotSurface(null, null, x, y, seed, x + lx, y - lx)}`);
    }
    yield line("bare", seed, recs.join(" "));
  }
  // cobbles
  const recs = [];
  for (let q = 0; q < 400; q += 1) {
    const hx = (r() - 0.5) * 600000;
    const hy = (r() - 0.5) * 600000;
    const x = q % 2 ? hx : Math.floor(hx);
    const y = q % 2 ? hy : Math.floor(hy);
    recs.push(`${f(x)},${f(y)}:${LS.cobble(x, y)}`);
  }
  for (let q = 0; q < recs.length; q += 40) yield line("cobble", recs.slice(q, q + 40).join(" "));
}
