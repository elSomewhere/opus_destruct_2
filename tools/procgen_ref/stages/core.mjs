// Stage "core": hashing, the seeded stream, noise, placements, oriented rects, 2D geometry and
// rects (reference core/*.js), on sample inputs from a fixed stream.
import { REF, line, samples } from "../lib/rec.mjs";

const { mix32, hash32, hashFloat, hashString, deriveSeed, Rng } = await import(REF + "core/hash.js");
const { SimplexNoise } = await import(REF + "core/noise.js");
const P = await import(REF + "core/placement.js");
const O = await import(REF + "core/obb.js");
const G = await import(REF + "core/geom2d.js");
const R = await import(REF + "core/rect.js");

export default function* core() {
  const r = samples(1);
  const big = () => Math.floor((r() - 0.5) * 2 ** 34);
  // hashes
  for (let i = 0; i < 2000; i += 1) {
    const a = big(), b = (r() - 0.5) * 1e5, c = Math.floor(r() * 1e6), d = r() * 3;
    yield line("h", mix32(a), hash32(a, b, c, d), hash32(a), hashFloat(a, b), hash32(c, d));
  }
  for (const s of ["", "a", "building", "C12_-3/b4/l2/B", "tree", "x".repeat(40)]) yield line("hs", s, hashString(s));
  for (let i = 0; i < 2000; i += 1) {
    const root = big(), n = (r() - 0.5) * 1e4, k = Math.floor(r() * 100);
    yield line("ds", deriveSeed(root, "building", n), deriveSeed(root, n, k, "x"), deriveSeed(root), deriveSeed(root, n * 1e6));
  }
  // the stream
  for (let i = 0; i < 300; i += 1) {
    const g = Rng.from(big(), "rng", i);
    const out = [];
    for (let k = 0; k < 8; k += 1) out.push(g.next());
    out.push(g.int(-5, 17), g.float(2, 9), g.chance(0.3) ? 1 : 0, g.sign(), g.pick([1, 2, 3, 4, 5]), g.gauss(1, 2));
    out.push(g.weighted([[1, 0.5], [2, 3], [3, 1]]), g.weighted([{ weight: 2, v: 7 }, { weight: 1, v: 8 }]).v);
    out.push(...g.shuffle([1, 2, 3, 4, 5, 6, 7]));
    const h = g.fork("child");
    out.push(h.next(), h.next());
    yield line("rng", ...out);
  }
  // noise
  for (const seed of [1337, 42, -7]) {
    const nz = new SimplexNoise(seed);
    for (let i = 0; i < 1500; i += 1) {
      const x = (r() - 0.5) * 2000, y = (r() - 0.5) * 2000, z = (r() - 0.5) * 50, w = (r() - 0.5) * 300;
      yield line("n", nz.n2(x, y), nz.n3(x, y, z), nz.n4(x, y, z, w), nz.fbm2(x, y), nz.fbm3(x / 7, y / 7, z), nz.fbm4(x, y, z, w, 5, 2.1, 0.45),
        nz.ridged2(x, y), nz.ridged3(x, y, z), nz.ridged4(x, y, z, w), nz.nP(x, y, z, undefined), nz.fbmP(x, y, z, w), nz.ridgedP(x, y, z, NaN));
    }
    // far away (ToInt32 of large cell indices)
    for (let i = 0; i < 200; i += 1) {
      const x = (r() - 0.5) * 1e10, y = (r() - 0.5) * 1e10;
      yield line("nf", nz.n2(x, y), nz.n3(x, y, x / 3), nz.n4(x, y, y / 5, x / 7));
    }
  }
  // placements
  for (const y of P.YAWS) yield line("yaw", y.c, y.s, y.r);
  for (const p of P.PITCHES) yield line("pitch", p.c, p.s, p.r, p.n);
  for (let i = 0; i < 400; i += 1) {
    const yaw = Math.floor(r() * 132), pitch = Math.floor(r() * 13) - 6, roll = Math.floor(r() * 132), yaw2 = r() < 0.5 ? 0 : Math.floor(r() * 132);
    const rot = P.rotationMatrix(yaw, pitch, roll, yaw2);
    const q = P.matrixQuat(rot.m, rot.d);
    const yp = P.yawProduct(yaw, Math.floor(r() * 132));
    yield line("rot", yaw, pitch, roll, yaw2, ...rot.m, rot.d, q.x, q.y, q.z, q.w, yp.c, yp.s, yp.r, P.yawIndex(yp), P.nearestYaw(r() - 0.5, r() - 0.5));
    const pl = new P.Placement({ origin: { x: big() % 4096, y: big() % 4096, z: Math.floor(r() * 300) }, yaw, yaw2, pitch: i % 3 ? 0 : pitch, roll: i % 5 ? 0 : roll, extent: { u0: -3, v0: 0, w0: 0, u1: 40, v1: 25, w1: 70 } });
    const out = [pl.yaw, pl.yaw2, pl.q, pl.px ?? "-", pl.py ?? "-"];
    for (let k = 0; k < 6; k += 1) {
      const x = big() % 4096, yy = big() % 4096, z = Math.floor(r() * 300);
      out.push(...pl.toLocal(x, yy, z), ...pl.toWorld(x % 50, yy % 50, z % 50), ...pl.toLocalXY(x, yy), ...pl.toWorldXY(x % 30, yy % 30), ...pl.dirToWorldXY(3, -2));
    }
    const bb = pl.worldAABB();
    out.push(bb.x0, bb.y0, bb.z0, bb.x1, bb.y1, bb.z1);
    yield line("pl", ...out);
    const c = P.Placement.cardinal(big() % 999, big() % 999, i % 4, { z: 5 });
    yield line("plc", c.px, c.py, c.q, ...c.toLocalXY(3.5, -2), ...c.toWorldXY(1.25, 7), ...c.toWorld(2, 3, 4));
    // oriented rects (yaw-only placements)
    const fl = new P.Placement({ origin: { x: big() % 999, y: big() % 999, z: 0 }, yaw });
    const lr = { x0: -5, y0: 2, x1: 20, y1: 31 };
    const bnd = O.obbBounds(fl, lr);
    yield line("obb", bnd.x0, bnd.y0, bnd.x1, bnd.y1, O.obbDistance(fl, lr, fl.origin.x + 40, fl.origin.y - 3), O.obbContains(fl, lr, fl.origin.x + 3, fl.origin.y + 9, 1) ? 1 : 0,
      ...O.localPointToWorld(fl, 2.5, 7), ...O.worldPointToLocal(fl, fl.origin.x + 11, fl.origin.y - 4));
    const poly = O.obbCorners(fl, { x0: 0, y0: 0, x1: 120, y1: 90 }).map(([x, y]) => [x + r() * 4, y + r() * 4]);
    const fit = O.fitLocalRect(new P.Placement({ origin: fl.origin, yaw: Math.floor(r() * 132) }), poly, { depth: [10, 60], minWidth: 8, maxWidth: 40, frontSlack: 3 });
    const clip = O.clipHalfPlane(poly, r() - 0.5, r() - 0.5, (r() - 0.5) * 100);
    yield line("fit", fit ? [fit.x0, fit.y0, fit.x1, fit.y1] : "-", ...clip.flat());
  }
  // geometry
  for (let i = 0; i < 500; i += 1) {
    const p = G.projectToSegment(big() % 500, big() % 500, big() % 500, big() % 500, big() % 500, big() % 500);
    yield line("seg", p.t, p.rawT, p.dist, p.side, p.along, p.len, p.cx, p.cy);
  }
  for (let i = 0; i < 40; i += 1) {
    const pts = [];
    for (let k = 0; k < 2 + (i % 5); k += 1) pts.push(i % 2 ? { x: big() % 900, y: big() % 900, z: r() * 30 } : { x: big() % 900, y: big() % 900 });
    const cr = G.catmullRom(pts, 6 + (i % 3), i % 4 === 3);
    const L = G.polylineLengths(cr);
    const at = G.polylineAt(cr, L, L[L.length - 1] * r());
    yield line("cr", ...cr.flatMap((q) => [q.x, q.y, q.z ?? "-"]), L[L.length - 1], at.x, at.y, at.tx, at.ty, at.z ?? "-");
    const b = G.pointsBounds(pts, 2.5);
    yield line("pb", b.x0, b.y0, b.x1, b.y1);
  }
  const grid = new G.SpatialGrid(64);
  for (let i = 0; i < 300; i += 1) {
    const x0 = big() % 2000, y0 = big() % 2000;
    grid.insert(i, { x0, y0, x1: x0 + Math.floor(r() * 150), y1: y0 + Math.floor(r() * 150) });
  }
  for (let i = 0; i < 200; i += 1) {
    const x0 = big() % 2000, y0 = big() % 2000;
    yield line("sg", ...grid.query({ x0, y0, x1: x0 + Math.floor(r() * 400), y1: y0 + Math.floor(r() * 400) }), "|", ...grid.queryPoint(x0, y0));
  }
  // rects
  for (let i = 0; i < 300; i += 1) {
    const a = R.rect(big() % 100, big() % 100, 0, 0);
    a.x1 = a.x0 + Math.floor(r() * 60);
    a.y1 = a.y0 + Math.floor(r() * 60);
    const b = R.rect(a.x0 + Math.floor((r() - 0.3) * 50), a.y0 + Math.floor((r() - 0.3) * 50), 0, 0);
    b.x1 = b.x0 + Math.floor(r() * 60);
    b.y1 = b.y0 + Math.floor(r() * 60);
    const it = R.rIntersect(a, b);
    const w = R.rSharedWall(a, { x0: a.x1 + 2, y0: a.y0 + 3, x1: a.x1 + 9, y1: a.y1 + 2 }, 1);
    yield line("r", R.rArea(a), it ? [it.x0, it.y0, it.x1, it.y1] : "-", R.rSubtract(a, b).flatMap((q) => [q.x0, q.y0, q.x1, q.y1]),
      w ? [w.orient, w.x0, w.x1, w.t0, w.t1, w.sideOfA] : "-", R.rKey(R.rEdgeStrip(a, "SENW"[i % 4], 2)), R.rKey(R.rOuterStrip(a, "NWSE"[i % 4], 3)));
  }
}
