import { Registry } from "../world/registry.js";
import { smoothstep, lerp } from "../core/math.js";

/**
 * Landforms: the terrain height is composed by a stack of registered
 * landforms, each a pure function of position that edits `ctx.h` (meters)
 * in `order`. A landform may also leave hints for land cover (canyon walls,
 * ravine floors). Adding a new kind of terrain (volcanoes, glacial valleys,
 * karst...) means registering one entry here.
 *
 *   init(seedNoise)  -> per-world state (noise instances), built once
 *   apply(ctx, st)   -> edit ctx.h; ctx carries x, y (voxels), fx, fy, fz
 *                       (field meters), u (urbanization), mountain (0..1),
 *                       cfg (config.terrain) and climate() (lazy {t, m})
 */
export const LANDFORMS = new Registry("landform");

/**
 * Distance (m) to the zero isoline of f at the current point (f over its
 * gradient, by differences). The gradient is floored at gmin: near saddles
 * of the noise it tends to zero and the estimate would jump by hundreds of
 * metres over a few metres (cliffs in the terrain).
 */
function isoDistance(fn, ctx, v, e = 15, gmin = 0) {
  const { fx, fy, fz, fw } = ctx;
  let gx;
  let gy;
  if (fw === undefined) {
    gx = (fn(fx + e, fy, fz) - v) / e;
    gy = (fn(fx, fy + e, fz) - v) / e;
  } else {
    // torus chart: step e metres along the surface (x turns (fx, fy), y turns (fz, fw))
    const R = ctx.torusR;
    gx = (fn(fx - (e * fy) / R, fy + (e * fx) / R, fz, fw) - v) / e;
    gy = (fn(fx, fy, fz - (e * fw) / R, fw + (e * fz) / R) - v) / e;
  }
  return Math.abs(v) / Math.max(Math.hypot(gx, gy), gmin, 1e-9);
}

/**
 * Island shore profile (island mode, world/island.js): a lowland rising
 * inland from the coast, beaches on gentle stretches, sea cliffs of 10-55 m
 * where the coast is rocky; offshore a shelf sloping gently from beaches and
 * steeply from cliffs, then the deep sea, with skerries (rock islets) on the
 * shelf. The height is exactly 0 (sea level) on the coast line.
 */
function islandBase(ctx, st) {
  const c = ctx.coast;
  const cfg = ctx.cfg;
  if (c > 1600) return cfg.lowlandBase * (1 - Math.exp(-c / 1400));
  const K = ctx.cliff();
  if (c >= 0) {
    const low = cfg.lowlandBase * (1 - Math.exp(-c / 1400));
    if (K <= 0) return low;
    const n = st.m.n2(ctx.fx / 500, ctx.fy / 500);
    const Hc = (10 + 45 * K * (0.55 + 0.45 * n)) * K;
    return low + Hc * smoothstep(0, 12 + 22 * (1 - K), c);
  }
  const d = -c;
  const shelf = ctx.island.cfg.shelf ?? 2200;
  const beachD = d * 0.014;
  const cliffD = 12 * smoothstep(0, 10, d) + d * 0.05;
  let depth = lerp(beachD, cliffD, K) + Math.max(0, d - shelf) * 0.06;
  depth = Math.min(depth, 180);
  return -depth + ctx.island.skerry(ctx.x * 0.125, ctx.y * 0.125, c);
}

LANDFORMS.register({
  id: "continent",
  order: 0,
  init: (noise) => ({ n: noise("continent"), m: noise("plains") }),
  apply(ctx, st) {
    if (ctx.island) {
      ctx.h = islandBase(ctx, st);
      return;
    }
    const c = ctx.cfg;
    const s = c.continentScale;
    const k = st.n.fbmP(ctx.fx / s, ctx.fy / s, ctx.fz / s, ctx.fw / s, 3);
    const p = st.m.fbmP(ctx.fx / 9000, ctx.fy / 9000, ctx.fz / 9000, ctx.fw / 9000, 2);
    ctx.h = c.lowlandBase + c.continentAmplitude * k + 25 * p;
  },
});

LANDFORMS.register({
  id: "hills",
  order: 10,
  init: (noise) => ({ hill: noise("hill"), detail: noise("detail") }),
  apply(ctx, st) {
    const c = ctx.cfg;
    const hs = c.hillScale;
    const ds = c.detailScale;
    // island: the coast stays at sea level; hills grow inland (sooner above cliffs)
    const fade = ctx.island ? (ctx.coast <= 0 ? 0 : smoothstep(0, 520 - 400 * ctx.cliff(), ctx.coast)) : 1;
    if (fade <= 0) {
      ctx.lowland = ctx.h;
      return;
    }
    const hills = c.hillAmplitude * st.hill.fbmP(ctx.fx / hs, ctx.fy / hs, ctx.fz / hs, ctx.fw / hs, 4) * (1 - 0.5 * ctx.mountain);
    const detail = c.detailAmplitude * st.detail.fbmP(ctx.fx / ds, ctx.fy / ds, ctx.fz / ds, ctx.fw / ds, 3);
    // (hills never sink an island's inland below sea level)
    ctx.h += (hills + detail) * fade;
    if (ctx.island && ctx.coast > 0) ctx.h = Math.max(ctx.h, Math.min(2.5, 0.3 + ctx.coast * 0.02));
    ctx.lowland = ctx.h;
  },
});

/**
 * Mountain ranges: a broad uplift under the belt plus ridged multifractal
 * peaks of up to ~6 km. Detail concentrates on ridges; valleys stay smooth.
 */
LANDFORMS.register({
  id: "mountains",
  order: 20,
  init: (noise) => ({ r: noise("mountain"), w: noise("mountainWarp") }),
  apply(ctx, st) {
    const M = ctx.mountain;
    if (M <= 0) return;
    const c = ctx.cfg;
    const s = c.mountainScale;
    const base = c.mountainBase ?? 400;
    const wx = 0.25 * s * st.w.fbmP(ctx.fx / s, ctx.fy / s, ctx.fz / s, ctx.fw / s, 2);
    const R = st.r.ridgedP((ctx.fx + wx) / s, (ctx.fy - wx) / s, ctx.fz / s, ctx.fw / s, 6, 2.05, c.mountainGain);
    // broad massif under the ridges; foothills where the belt fades
    const Mh = M * Math.sqrt(M);
    const uplift = base * M + c.mountainUplift * Mh;
    const peaks = c.mountainHeight * Mh * Math.pow(R, 1.2);
    // arêtes and couloirs: sharper mid-scale relief that grows with height
    const ds = c.mountainDetailScale;
    const dr = st.r.ridgedP(ctx.fx / ds + 13.1, ctx.fy / ds - 7.7, ctx.fz / ds, ctx.fw / ds, 3, 2.1, 0.45);
    const detail = c.mountainDetail * Mh * (0.35 + R) * (dr - 0.45);
    ctx.h += uplift + peaks + detail;
    ctx.ridge = R;
  },
});

/**
 * Dry plateaus: arid country stands high (Colorado-plateau style), which
 * gives canyons and mesas room to cut; towns sit in shallow basins (the
 * plateau fades over their foothill buffer like the mountains do).
 */
LANDFORMS.register({
  id: "plateau",
  order: 25,
  init: (noise) => ({ n: noise("plateau") }),
  apply(ctx, st) {
    const D = ctx.desert();
    if (D < 0.25) return;
    const k = smoothstep(0.25, 0.7, D) * (1 - ctx.mountain) * (1 - ctx.prox);
    if (k <= 0) return;
    const vary = 0.75 + 0.25 * st.n.fbmP(ctx.fx / 14000, ctx.fy / 14000, ctx.fz / 14000, ctx.fw / 14000, 2);
    ctx.h += ctx.cfg.plateauHeight * k * vary;
  },
});

/**
 * Glacial valleys: U-shaped trunk valleys along the isolines of a warped
 * noise, cut into the massifs (flat floors, steep walls), which break the
 * ranges into ridges and give them passes and long valleys.
 */
/**
 * U profile (0..1) of the glacial valleys at a field point (pc: {fx, fy,
 * fz, fw, torusR}) for mountainness M: 1 on a valley floor, 0 outside.
 */
function valleyProfile(st, pc, M, s) {
  const fn = (fx, fy, fz, fw) => {
    const w = 0.3 * s * st.w.fbmP(fx / s, fy / s, fz / s, fw / s, 2);
    return st.n.fbmP((fx + w) / s, (fy - w) / s, fz / s, fw / s, 2);
  };
  const v = fn(pc.fx, pc.fy, pc.fz, pc.fw);
  // cheap reject far outside any valley (the distance test below decides)
  if (Math.abs(v) > 0.8) return 0;
  const d = isoDistance(fn, pc, v, 40, 0.7 / s);
  const W = 700 + 1100 * M;
  if (d > W) return 0;
  const t = d / W;
  // U profile: a flat floor over the inner quarter, walls rising outwards;
  // faded out before the cheap reject above so the terrain stays continuous
  return (t < 0.25 ? 1 : Math.pow(1 - (t - 0.25) / 0.75, 1.3)) * (1 - smoothstep(0.7, 0.8, Math.abs(v)));
}

LANDFORMS.register({
  id: "glacialValleys",
  order: 22,
  init: (noise) => ({ n: noise("valley"), w: noise("valleyWarp") }),
  apply(ctx, st) {
    const M = ctx.mountain;
    if (M < 0.12) return;
    const u = valleyProfile(st, ctx, M, ctx.cfg.valleyScale);
    if (u <= 0) return;
    // (faded in over the mountainness threshold: no wall where the cut starts)
    const depth = ctx.cfg.valleyDepth * Math.pow(M, 1.2) * smoothstep(0.12, 0.3, M);
    // never below the foothills
    const cut = Math.min(depth * u, Math.max(0, ctx.h - ctx.lowland - 150));
    ctx.h -= cut;
    if (cut > 50) ctx.valley = u;
  },
});

/**
 * Gullies and couloirs: grooves running down the fall line of mountain
 * flanks, between sharp ribs, so rock faces read as eroded, not smooth.
 * Built from kernels on an 80 m lattice (Gabor style): each kernel carries
 * stripes across its own downslope direction (from a cheap copy of the
 * range's relief at the kernel, cached), weighted by how steep it is there
 * and blended with compact windows (no seams). Two scales: couloirs ~140 m
 * apart and gullies ~45 m apart. Kernel phases come from a smooth noise, so
 * grooves connect from kernel to kernel down the slope. Crests and flats
 * (where the direction turns over) get none. The lattice divides a
 * wrapping world's period, so the pattern repeats with it.
 */
const GC = 80; // kernel spacing (m)
const GR = 120; // kernel window radius (m)
LANDFORMS.register({
  id: "gullies",
  order: 23,
  init: (noise) => ({ r: noise("mountain"), w: noise("mountainWarp"), ph: noise("gullyPhase"), valley: { n: noise("valley"), w: noise("valleyWarp") }, cache: new Map() }),
  apply(ctx, st) {
    const M = ctx.mountain;
    if (M < 0.1 || !ctx.toField) return;
    const c = ctx.cfg;
    const s = c.mountainScale;
    // the range's ridged relief (3 octaves, metres) less its glacial valleys, at a field point
    // (a fixed mountainness: a kernel is a pure function of its position, whoever asks first)
    const Mv = 0.7;
    const vDepth = c.valleyDepth * Math.pow(Mv, 1.2) * smoothstep(0.12, 0.3, Mv);
    const relief = (f) => {
      const wx = 0.25 * s * st.w.fbmP(f[0] / s, f[1] / s, f[2] / s, f[3] / s, 2);
      const r = c.mountainHeight * Math.pow(st.r.ridgedP((f[0] + wx) / s, (f[1] - wx) / s, f[2] / s, f[3] / s, 3, 2.05, c.mountainGain), 1.2);
      const pc = { fx: f[0], fy: f[1], fz: f[2], fw: f[3], torusR: ctx.torusR };
      return r - vDepth * valleyProfile(st.valley, pc, Mv, c.valleyScale);
    };
    const kernel = (i, j) => {
      const key = i * 1000003 + j;
      let k = st.cache.get(key);
      if (k) return k;
      const xm = (i + 0.5) * GC;
      const ym = (j + 0.5) * GC;
      const e = 30;
      const gx = (relief(ctx.toField(xm + e, ym)) - relief(ctx.toField(xm - e, ym))) / (2 * e);
      const gy = (relief(ctx.toField(xm, ym + e)) - relief(ctx.toField(xm, ym - e))) / (2 * e);
      const g = Math.hypot(gx, gy);
      const f = ctx.toField(xm, ym);
      // (phases vary slowly, so grooves carry on from kernel to kernel)
      const p1 = Math.PI * 2 * st.ph.fbmP(f[0] / 700, f[1] / 700, f[2] / 700, f[3] / 700, 2);
      const p2 = Math.PI * 2 * st.ph.fbmP(f[0] / 260 + 9.7, f[1] / 260, f[2] / 260, f[3] / 260, 2);
      k = { x: xm, y: ym, nx: g > 1e-6 ? -gy / g : 0, ny: g > 1e-6 ? gx / g : 0, a: smoothstep(0.15, 0.55, g), p1, p2 };
      if (st.cache.size > 60000) st.cache.clear();
      st.cache.set(key, k);
      return k;
    };
    const xm = ctx.x * 0.125;
    const ym = ctx.y * 0.125;
    const i0 = Math.floor((xm - GR) / GC - 0.5);
    const j0 = Math.floor((ym - GR) / GC - 0.5);
    const i1 = Math.floor((xm + GR) / GC - 0.5) + 1;
    const j1 = Math.floor((ym + GR) / GC - 0.5) + 1;
    let sw = 0;
    let sa = 0;
    let s1 = 0;
    let s2 = 0;
    for (let j = j0; j <= j1; j += 1)
      for (let i = i0; i <= i1; i += 1) {
        const k = kernel(i, j);
        const dx = xm - k.x;
        const dy = ym - k.y;
        const d2 = (dx * dx + dy * dy) / (GR * GR);
        if (d2 >= 1) continue;
        const w = (1 - d2) * (1 - d2);
        const q = dx * k.nx + dy * k.ny;
        sw += w;
        sa += w * k.a;
        s1 += w * k.a * Math.cos((Math.PI * 2 * q) / 140 + k.p1);
        s2 += w * k.a * Math.cos((Math.PI * 2 * q) / 45 + k.p2);
      }
    if (sw <= 0 || sa <= 1e-4) return;
    const a = sa / sw;
    // grooves (V-shaped) between sharp ribs; the mean level stays put
    const c1 = s1 / sa;
    const c2 = s2 / sa;
    const groove = (v) => 0.3 - Math.pow((1 - v) / 2, 1.6);
    // (forested mid-slopes are furrowed too, the high rock faces most)
    const A = (c.gullyDepth ?? 50) * smoothstep(0.1, 0.45, M) * (0.45 + 0.55 * M) * a;
    ctx.h += A * (groove(c1) + 0.35 * groove(c2));
  },
});

/** Desert mesas: the land steps up in flat benches with steep risers. */
LANDFORMS.register({
  id: "mesas",
  order: 30,
  init: () => ({}),
  apply(ctx) {
    if (ctx.h < 25 || ctx.u > 0.3) return;
    const D = ctx.desert();
    if (D < 0.25) return;
    const step = ctx.cfg.mesaStep;
    const f = ctx.h / step;
    const k = Math.floor(f);
    const stepped = (k + smoothstep(0.62, 0.96, f - k)) * step;
    // faded near the gates above so the benches do not start with a wall
    const fade = smoothstep(25, 45, ctx.h) * (1 - smoothstep(0.2, 0.3, ctx.u));
    ctx.h = lerp(ctx.h, stepped, smoothstep(0.25, 0.6, D) * (1 - ctx.mountain) * fade);
  },
});

/** Wind-aligned dunes in sandy deserts. */
LANDFORMS.register({
  id: "dunes",
  order: 31,
  init: (noise) => ({ n: noise("dune") }),
  apply(ctx, st) {
    if (ctx.u > 0.3) return;
    const D = ctx.desert();
    if (D <= 0) return;
    const warp = 40 * st.n.nP(ctx.fx / 400, ctx.fy / 400, ctx.fz / 400, ctx.fw / 400);
    const ridge = ctx.fw === undefined ? st.n.ridged2((ctx.fx + warp) / 140, (ctx.fy - warp) / 55 + 17.3, 2) : st.n.ridged4((ctx.fx + warp) / 140, ctx.fy / 140, (ctx.fz - warp) / 55 + 17.3, ctx.fw / 55, 2, 2.1, 0.5);
    ctx.h += D * D * 9 * ridge * ridge * (1 - ctx.mountain) * (1 - smoothstep(0.2, 0.3, ctx.u));
  },
});

/**
 * Canyons: along the zero isolines of a warped noise in dry country, cut
 * down to ~260 m. The walls climb in bands of hard and soft rock: a bench,
 * a concave apron of scree, a near-vertical cliff, then the next bench; the
 * number and width of the bands change slowly along the canyon, the walls
 * wander in buttresses and alcoves and side gullies bite into them. The
 * floor is flat with a stream (or a dry wash in true desert).
 */
LANDFORMS.register({
  id: "canyons",
  order: 40,
  init: (noise) => ({ n: noise("canyon"), w: noise("canyonWarp"), b: noise("canyonBands"), g: noise("canyonGully") }),
  apply(ctx, st) {
    if (ctx.u > 0.2 || ctx.h < 8) return;
    const D = ctx.desert();
    if (D < 0.2) return;
    const s = ctx.cfg.canyonScale;
    const fn = (fx, fy, fz, fw) => {
      const w = 0.3 * s * st.w.fbmP(fx / s, fy / s, fz / s, fw / s, 2);
      return st.n.fbmP((fx + w) / s, (fy - w) / s, fz / s, fw / s, 3);
    };
    const v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (Math.abs(v) > 0.07) return;
    const d0 = isoDistance(fn, ctx, v, 15, 0.7 / s);
    let d = d0;
    // (faded out before the |v| reject so no wall appears where it cuts in)
    const mask = smoothstep(0.2, 0.55, D) * (1 - ctx.mountain) * (1 - smoothstep(0.05, 0.07, Math.abs(v))) * (1 - smoothstep(0.1, 0.2, ctx.u));
    const { fx, fy, fz, fw } = ctx;
    // buttresses and alcoves; side gullies (a ridged noise) bite deep into the walls
    const wander = st.b.fbmP(fx / 70, fy / 70, fz / 70, fw / 70, 2);
    const gully = 1 - Math.abs(st.g.fbmP(fx / 260, fy / 260, fz / 260, fw / 260, 2));
    d = Math.max(0, d + 16 * mask * wander - 55 * mask * Math.pow(gully, 6));
    const floor = 14 + 20 * mask;
    const rim = floor + 60 + 170 * mask;
    if (d > rim) return;
    const t = Math.max(0, (d - floor) / (rim - floor));
    // the bands: their number and proportions change slowly along the canyon
    const vb = st.b.fbmP(fx / 900 + 7.3, fy / 900, fz / 900, fw / 900, 2);
    const steps = 4 + 2.5 * (vb + 1);
    const q = t * steps;
    const u = q - Math.floor(q);
    const bench = 0.3 + 0.2 * st.b.nP(fx / 400 - 3.1, fy / 400, fz / 400, fw / 400);
    const cliff = Math.min(0.92, bench + 0.28);
    // bench (flat), scree apron (concave, a quarter of the rise), cliff (the rest, steep)
    const y = u < bench ? 0 : u < cliff ? 0.25 * Math.pow((u - bench) / (cliff - bench), 1.6) : 0.25 + 0.75 * smoothstep(0, 1, (u - cliff) / (1 - cliff));
    const stepped = Math.min(1, (Math.floor(q) + y) / steps);
    // never below sea level: a canyon cuts at most 90% of the land's height
    const depth = Math.max(0, Math.min(ctx.cfg.canyonDepth * mask, 0.9 * (ctx.h - 8)));
    const cut = depth * (1 - stepped);
    ctx.h -= cut;
    if (cut > 2) ctx.canyon = 1 - stepped;
    ctx.channel = Math.max(ctx.channel, 1 - smoothstep(floor * 0.5, floor + 10, d));
    // a stream winds along the flat floor (a dry sandy wash in true desert), on the canyon's own line
    const sh = Math.min(floor * 0.35, 3 + 3 * mask);
    if (d0 < sh + 2 && d < floor && depth > 40) ctx.stream = { d: d0, half: sh, extra: 0, wet: D < 0.72 };
  },
});

/** Ravines: narrow V-shaped gorges in wet, wooded hill country, with a creek at the bottom. */
LANDFORMS.register({
  id: "ravines",
  order: 41,
  init: (noise) => ({ n: noise("ravine"), w: noise("ravineWarp") }),
  apply(ctx, st) {
    if (ctx.u > 0.12 || ctx.mountain > 0.5) return;
    const { m } = ctx.climate();
    const mask = smoothstep(0.52, 0.72, m) * (1 - ctx.desert()) * (1 - 2 * ctx.mountain);
    if (mask < 0.1) return;
    const s = ctx.cfg.ravineScale;
    const fn = (fx, fy, fz, fw) => {
      const w = 0.35 * s * st.w.fbmP(fx / s, fy / s, fz / s, fw / s, 2);
      return st.n.fbmP((fx + w) / s, (fy - w) / s, fz / s, fw / s, 2);
    };
    const v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (Math.abs(v) > 0.05) return;
    const d = isoDistance(fn, ctx, v, 6, 0.7 / s);
    const half = 9 + 20 * mask;
    if (d > half) return;
    const k = (1 - d / half) * (1 - smoothstep(0.038, 0.05, Math.abs(v)));
    // (on an island ravines fade out before the shore: no gorge below sea level)
    const shore = ctx.island ? smoothstep(80, 420, ctx.coast) : 1;
    const depth = ctx.cfg.ravineDepth * mask * smoothstep(0.1, 0.2, mask) * (1 - smoothstep(0.06, 0.12, ctx.u)) * shore;
    // (never down to the sea: a low island's ravine stays a dry gorge above it)
    ctx.h -= Math.min(depth * Math.pow(k, 1.25), Math.max(0, ctx.h - 1.5));
    ctx.ravine = k;
    ctx.channel = Math.max(ctx.channel, 1 - smoothstep(3, 9, d));
    // a creek at the bottom of the V
    if (d < 4.5) ctx.stream = { d, half: 2.2 + mask, extra: depth * (1 - Math.pow(k, 1.25)), wet: true };
  },
});

/**
 * How rugged the open country is (0..1): glaciated shield country (cool and
 * moist: the north, Karelia, the Nordic islands) is knobbly with knolls,
 * hollows, boulders and outcrops; mild lowlands are gentler; a slow noise
 * varies it region by region, and it grows into the foothills.
 */
export function ruggedness(ctx, st) {
  if (ctx._rugged >= 0) return ctx._rugged;
  const { t, m } = ctx.climate();
  const glacial = smoothstep(0.56, 0.38, t) * smoothstep(0.35, 0.6, m);
  const n = st.g.fbmP(ctx.fx / 6000, ctx.fy / 6000, ctx.fz / 6000, ctx.fw / 6000, 2);
  const r = 0.22 + 0.5 * glacial + 0.35 * n + (ctx.cfg.rugged ?? 0) + 0.45 * smoothstep(0.02, 0.25, ctx.mountain);
  ctx._rugged = Math.max(0, Math.min(1, r)) * (1 - 0.8 * ctx.desert());
  return ctx._rugged;
}

/**
 * Relief of open country at walking scale, so natural ground is not smooth:
 * knolls and hollows (~170 m), hummocks (~22 m) and small bumps (~4 m:
 * tree-throw mounds and pits, stones under the moss), all growing with the
 * country's ruggedness. It fades out towards towns, on beaches and along
 * the channels of streams, ravines and canyons (their beds stay even).
 * Hummocks and bumps count as roughness (land cover reads slopes without
 * them, so a bump never turns a meadow to rock).
 */
LANDFORMS.register({
  id: "relief",
  order: 43,
  init: (noise) => ({ k: noise("reliefKnoll"), h: noise("reliefHummock"), b: noise("reliefBump"), g: noise("reliefRugged") }),
  apply(ctx, st) {
    if (ctx.u > 0.08) return;
    const cfg = ctx.cfg;
    let mask = (1 - smoothstep(0.02, 0.07, ctx.u)) * (1 - ctx.channel);
    if (ctx.island) mask *= ctx.coast <= 0 ? 0 : smoothstep(4, 60, ctx.coast);
    if (mask <= 0.01) return;
    const R = ruggedness(ctx, st);
    const { fx, fy, fz, fw } = ctx;
    const ks = cfg.reliefKnollScale ?? 170;
    const hs = cfg.reliefHummockScale ?? 22;
    const knoll = st.k.fbmP(fx / ks, fy / ks, fz / ks, fw / ks, 3);
    const hum = st.h.fbmP(fx / hs, fy / hs, fz / hs, fw / hs, 2);
    const bump = st.b.nP(fx / 4.2, fy / 4.2, fz / 4.2, fw / 4.2);
    // (high mountains carry their own relief)
    const low = 1 - 0.7 * smoothstep(0.1, 0.5, ctx.mountain);
    const kn = (cfg.reliefKnoll ?? 7) * (0.15 + 0.85 * R) * knoll * low;
    // rugged ground: sharper hummocks (rounded tops, narrow hollows)
    const hm = (cfg.reliefHummock ?? 1.3) * (0.3 + 0.7 * R) * (hum + 0.35 * R * (hum * hum - 0.3));
    const bp = (cfg.reliefBump ?? 0.35) * (0.4 + 0.6 * R) * bump;
    const rough = (hm + bp) * mask;
    ctx.h += kn * mask + rough;
    ctx.rough += rough;
  },
});

/**
 * Roughness: hummocks, knolls and crags on mountain ground (a couple of
 * metres at 2-6 m wavelengths), strongest on high ridges, so close-up
 * mountainsides are not smooth staircases. Part of the terrain (every
 * consumer sees the same ground); well below a voxel at coarse LODs.
 */
LANDFORMS.register({
  id: "roughness",
  order: 60,
  init: (noise) => ({ n: noise("rough") }),
  apply(ctx, st) {
    const M = ctx.mountain;
    if (M < 0.08 || ctx.u > 0.05) return;
    const a = st.n.nP(ctx.fx / 6, ctx.fy / 6, ctx.fz / 6, ctx.fw / 6);
    const b = st.n.nP(ctx.fx / 2.3 + 17.2, ctx.fy / 2.3, ctx.fz / 2.3, ctx.fw / 2.3);
    let r = (0.55 * a + 0.3 * b) * 0.9 * M;
    // crags: blocky knobs on the high ridges
    const c = st.n.nP(ctx.fx / 11 - 5.1, ctx.fy / 11, ctx.fz / 11, ctx.fw / 11);
    if (c > 0.25) r += (c - 0.25) * 4.5 * M * (0.4 + ctx.ridge);
    r *= smoothstep(0.08, 0.16, M) * (1 - smoothstep(0.02, 0.05, ctx.u));
    ctx.h += r;
    ctx.rough += r;
  },
});

/**
 * Granite outcrops (Scandinavian "berg i dagen"): glacially smoothed rock
 * knolls a few metres high breaking through the forest and the meadows of
 * cool, wet country, and bare rock slabs (svaberg) sloping into the sea on
 * low island shores. Leaves `ctx.outcrop` (0..1 bare rock) for land cover.
 */
LANDFORMS.register({
  id: "outcrops",
  order: 45,
  init: (noise) => ({ n: noise("outcrop"), d: noise("outcropDetail") }),
  apply(ctx, st) {
    if (ctx.u > 0.12) return;
    const { t, m } = ctx.climate();
    const cool = smoothstep(0.5, 0.4, t) * smoothstep(0.45, 0.6, m) * (1 - smoothstep(0.35, 0.6, ctx.mountain));
    // low island shores: rock slabs down to the water (not on beaches of sand)
    let shore = 0;
    if (ctx.island && ctx.coast > -25 && ctx.coast < 70) shore = smoothstep(-25, 0, ctx.coast) * (1 - smoothstep(30, 70, ctx.coast)) * smoothstep(0.1, 0.45, ctx.cliff() + 0.25);
    if (cool <= 0 && shore <= 0) return;
    const { fx, fy, fz, fw } = ctx;
    const n = st.n.fbmP(fx / 130, fy / 130, fz / 130, fw / 130, 2);
    // (a ragged outline: the rock edge frays into tongues and islets)
    const edge = 0.14 * st.d.fbmP(fx / 22 + 3.3, fy / 22, fz / 22, fw / 22, 2);
    const k = smoothstep(0.4, 0.72, n + 0.35 * shore + edge) * Math.max(cool, shore) * (1 - smoothstep(0.04, 0.12, ctx.u));
    if (k <= 0.01) return;
    // a dome (whaleback), bumpier on top, broken by joints into slabs and low ledges
    const bump = 0.6 * st.d.nP(fx / 9, fy / 9, fz / 9, fw / 9);
    const dome = 5.5 * Math.pow(k, 1.6) + bump * k;
    const f = dome / 1.1;
    const ledged = (Math.floor(f) + smoothstep(0.55, 0.9, f - Math.floor(f))) * 1.1;
    const joint = 1 - Math.abs(st.d.nP(fx / 16 - 11.9, fy / 16, fz / 16, fw / 16));
    const crack = smoothstep(0.9, 0.98, joint) * Math.min(0.9, dome * 0.35);
    const rise = (0.55 * dome + 0.45 * ledged - crack) * (shore > 0 ? 1 - 0.7 * shore : 1);
    ctx.h += ctx.island && ctx.coast < 0 ? Math.min(rise, 1.5) * smoothstep(-25, 0, ctx.coast) : rise;
    ctx.outcrop = k;
  },
});

/**
 * Creeks: small winding streams through the woods and meadows of wet
 * country, along the isolines of a warped noise (a few hundred metres
 * apart), cut a metre or two into the ground; they run out into the sea on
 * an island. The ground tile carves the bed and fills it with water
 * (`ctx.stream`).
 */
LANDFORMS.register({
  id: "creeks",
  order: 42,
  init: (noise) => ({ n: noise("creek"), w: noise("creekWarp"), m: noise("creekMask") }),
  apply(ctx, st) {
    if (ctx.u > 0.1 || ctx.mountain > 0.6 || ctx.stream) return;
    const { m } = ctx.climate();
    const wet = smoothstep(0.5, 0.64, m) * (1 - ctx.desert());
    if (wet <= 0.05) return;
    const s = ctx.cfg.creekScale ?? 900;
    // (one octave under a warp: long winding lines, no little closed loops)
    const fn = (fx, fy, fz, fw) => {
      const w = 0.4 * s * st.w.fbmP(fx / s, fy / s, fz / s, fw / s, 2);
      return st.n.fbmP((fx + w) / s, (fy - w) / s, fz / s, fw / s, 1);
    };
    const v = fn(ctx.fx, ctx.fy, ctx.fz, ctx.fw);
    if (Math.abs(v) > 0.035) return;
    // creeks come and go (springs, soaks) over kilometres
    const on = smoothstep(-0.15, 0.3, st.m.fbmP(ctx.fx / 2400, ctx.fy / 2400, ctx.fz / 2400, ctx.fw / 2400, 2));
    if (on <= 0) return;
    const d = isoDistance(fn, ctx, v, 5, 0.7 / s);
    const half = (0.7 + 1.0 * on) * wet;
    // a shallow valley with gentle banks, the channel in its floor
    const bank = half + 4 + 5 * on;
    if (d > bank) return;
    const shoreK = ctx.island ? smoothstep(0, 60, ctx.coast) : 1;
    const fade = (1 - smoothstep(0.024, 0.035, Math.abs(v))) * (1 - smoothstep(0.05, 0.1, ctx.u)) * (1 - smoothstep(0.4, 0.6, ctx.mountain));
    const depth = (0.5 + 0.9 * on) * wet * fade * shoreK;
    if (depth <= 0.05) return;
    const q = 1 - smoothstep(half * 0.3, bank, d);
    const k = q * q * (3 - 2 * q);
    ctx.h -= Math.min(depth * k, Math.max(0, ctx.h - 1.2));
    ctx.channel = Math.max(ctx.channel, 1 - smoothstep(half, bank + 2, d));
    if (d < half + 1 && ctx.h > 1.4 && on > 0.2) ctx.stream = { d, half, extra: 0, wet: true };
  },
});
