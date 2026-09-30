#!/usr/bin/env node
/**
 * Browser run of the angles world on the real engine (?engine=wasm): structures in oriented grids
 * (docs/GRIDS.md) are drawn - their chunks' meshes placed by the grids' frames - and the client's
 * collision knows them (a box moving into the 45 degree wall stops at it; the player lands on the
 * ramp), then they come down under the engine demo's blasts. Screenshots before and after; fails
 * on console / page / WebGPU errors (a WASM abort included) or a failed check.
 * Usage (dev server running): node scripts/angles-wasm.mjs [baseUrl] [outDir] [seconds]
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { launch, beforeLoad, afterLoad } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'angles-wasm-out');
const seconds = Number(process.argv[4] ?? 12);
mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const h = 0.125;
const deg = Math.PI / 180;
// a grid's point (voxel units in its lattice) in the world: origin (voxels) + R_z(yaw) p
const turnedZ = (origin, yawDeg, p) => {
  const c = Math.cos(yawDeg * deg), s = Math.sin(yawDeg * deg);
  return [h * (origin[0] + c * p[0] - s * p[1]), h * (origin[1] + s * p[0] + c * p[1]), h * (origin[2] + p[2])];
};
const browser = await launch(puppeteer);
const errors = [];
const checks = [];
let failed = false;
const check = (name, ok, detail) => {
  checks.push({ name, ok, detail });
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${detail}`);
  if (!ok) failed = true;
};
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl|Aborted/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await beforeLoad(page);
  await page.goto(`${base}?engine=wasm&world=angles&seed=1`, { waitUntil: 'load' });
  await afterLoad(page);
  const t0 = Date.now();
  for (;;) {
    let ok = false;
    try {
      ok = await page.evaluate(() => {
        const s = window.__structvox?.state();
        return !!s && s.ready && s.engine !== null && s.render !== null && s.render.chunksDrawn > 0;
      });
    } catch (e) {
      // the dev server reloads the page when the WASM build output changes: keep waiting
      if (!/Execution context was destroyed|Cannot find context/.test(String(e))) throw e;
    }
    if (ok) break;
    if (Date.now() - t0 > 120000) throw new Error('angles world did not load');
    await sleep(200);
  }
  await sleep(1500);
  const view = async (name, pos, yaw, pitch) => {
    await page.evaluate(
      (p, y, q) => {
        const api = window.__structvox;
        api.noclip(true);
        api.teleport(p[0], p[1], p[2]);
        api.look(y, q);
      },
      pos,
      yaw,
      pitch,
    );
    await sleep(700);
    await page.screenshot({ path: `${outDir}/${name}.png` });
  };
  await view('overview', [-6, -10, 18], 41, -22);
  await view('portal', [16, -7, 5], 60, -8);
  await view('bridge', [20, 6, 14], 45, -25);
  const drawn = await page.evaluate(() => window.__structvox.state().render.chunksDrawn);
  check('chunks drawn', drawn > 0, `${drawn}`);

  // client-side collision: a player box 1 m off the 45 degree wall (grid origin (290, 110),
  // 3 voxels thick about its centre line), moving 2 m towards it, stops short of it
  {
    const c = [h * 290, h * 110];
    const n = [-Math.SQRT1_2, Math.SQRT1_2]; // the wall's normal
    const p = [c[0] + n[0], c[1] + n[1], 0.05];
    const r = await page.evaluate(
      (p0, mv) => window.__structvox.collideLocal([p0[0] - 0.3, p0[1] - 0.3, p0[2]], [p0[0] + 0.3, p0[1] + 0.3, p0[2] + 1.7], mv),
      p,
      [-2 * n[0], -2 * n[1], 0],
    );
    const moved = r ? Math.hypot(r.move[0], r.move[1]) : NaN;
    // (its corner touches when the centre is 0.1875 + 0.3 sqrt(2) = 0.612 m from the centre line,
    // 0.388 m closer; the sweep goes along x first, which closes on the wall at sqrt(1/2) of its
    // move: 0.549 m along x, then nothing along y. The turned cubes exactly.)
    check('box stops at the 45 degree wall', r !== null && Math.abs(moved - 0.388 * Math.SQRT2) < 0.01, `moved ${moved.toFixed(3)} m of 2 (${(0.388 * Math.SQRT2).toFixed(3)} expected)`);
  }
  // the player lands on the ramp (grid origin (40, 60, -1), pitched 15 degrees, 3 voxels thick:
  // its top at grid x' = 36 is the grid's point (36, 0, 2.5))
  {
    const s15 = Math.sin(15 * deg), c15 = Math.cos(15 * deg);
    const x = h * (40 + 36 * c15 - 2.5 * s15);
    const top = h * (-1 + 36 * s15 + 2.5 * c15);
    await page.evaluate((px, py) => {
      const api = window.__structvox;
      api.noclip(false);
      api.teleport(px, py, 3.0);
    }, x, h * 60);
    await sleep(2000);
    const s = await page.evaluate(() => window.__structvox.state());
    const z = s.player[2];
    check('player stands on the ramp', s.onGround && Math.abs(z - top) < 0.3, `feet at ${z.toFixed(3)} m, ramp top ${top.toFixed(3)} m, on ground ${s.onGround}`);
  }

  // the engine demo's shots: a bridge pier, the portal's left column, the ramp's block, the turned
  // tower's west columns, the 20 degree wall, the crates and the monolith's foot
  const shots = [
    [[h * 224, h * 168, 1.0], 1.2, 1e6],
    [[h * 224, h * 168, 3.5], 1.2, 1e6],
    [[h * 161.5, h * 41, h * 14], 0.7, 1e6],
    [[h * 106, h * 60, 1.0], 1.2, 1e6],
    ...[0, 1, 2].map((k) => [turnedZ([80, 200, 0], 30, [-25.5, -25.5 + 26 * k, 0.8 / h]), 0.9, 1e6]),
    [turnedZ([290, 50, 0], 20, [12, 0, 1.5 / h]), 0.8, 5e5],
    [[h * 170, h * 120, 1.2], 0.6, 2e5],
    [[h * 350, h * 140, 0.4], 0.5, 5e5],
  ];
  await page.evaluate(() => {
    const api = window.__structvox;
    api.noclip(true);
    api.teleport(-6, -10, 18);
    api.look(41, -22);
  });
  for (const [pos, r, e] of shots) {
    await page.evaluate((p, rr, ee) => window.__structvox.engine.blast(p, rr, ee), pos, r, e);
    await sleep(200);
  }
  const log = [];
  const tStart = Date.now();
  let shot = 0;
  while (Date.now() - tStart < seconds * 1000) {
    await sleep(1000);
    const s = await page.evaluate(() => window.__structvox.state());
    const line = { t: ((Date.now() - tStart) / 1000).toFixed(1), fps: Number(s.fps).toFixed(1), ...(s.engine ?? {}) };
    log.push(line);
    console.log(JSON.stringify(line).slice(0, 300));
    if (shot < 12) await page.screenshot({ path: `${outDir}/after_${String(shot++).padStart(2, '0')}.png` });
    if (errors.some((x) => /Aborted|RuntimeError/.test(x))) break;
  }
  writeFileSync(`${outDir}/log.json`, JSON.stringify({ checks, log }, null, 1));
  await view('portal_after', [16, -7, 5], 60, -8);
} catch (e) {
  failed = true;
  console.log(`FAIL ${e.message}`);
} finally {
  await browser.close();
}
if (errors.length) {
  console.log(`errors:\n  ${errors.slice(0, 10).join('\n  ')}`);
  failed = true;
}
console.log(failed ? 'ANGLES RUN FAILED' : 'ANGLES RUN PASSED');
process.exit(failed ? 1 : 0);
