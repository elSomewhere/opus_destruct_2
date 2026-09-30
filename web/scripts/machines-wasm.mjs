#!/usr/bin/env node
/**
 * Browser run of the machines world on the real engine (?engine=wasm): machines are pieces on
 * driven joints (docs/MOTION.md). The ropes and rods of the joints are drawn, the pieces' voxels
 * reach the client's collision, and the player rides the machines: dropped onto the lift's car,
 * they go up and down with it; onto the turntable, they go round with it (the client's collision
 * against the moving pieces, and their velocity). Screenshots; fails on console / page / WebGPU
 * errors (a WASM abort included) or a failed check.
 * Usage (dev server running): node scripts/machines-wasm.mjs [baseUrl] [outDir]
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { launch, beforeLoad, afterLoad } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'machines-wasm-out');
mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await launch(puppeteer);
const errors = [];
let failed = false;
const check = (name, ok, detail) => {
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
  await page.goto(`${base}?engine=wasm&world=machines&seed=1`, { waitUntil: 'load' });
  await afterLoad(page);
  const t0 = Date.now();
  for (;;) {
    let ok = false;
    try {
      ok = await page.evaluate(() => {
        const s = window.__structvox?.state();
        return !!s && s.ready && s.engine !== null && s.render !== null && s.pieceBodies > 0;
      });
    } catch (e) {
      if (!/Execution context was destroyed|Cannot find context/.test(String(e))) throw e;
    }
    if (ok) break;
    if (Date.now() - t0 > 120000) throw new Error(`machines world did not load (${errors.slice(0, 3).join(' | ') || 'no errors'})`);
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
  await view('crane', [4, 20, 4], 50, 8);
  await view('turntable', [24, -2, 5], 60, -15);
  await view('frames', [34, 20, 4], 90, -5);
  const r = await page.evaluate(() => {
    const s = window.__structvox.state();
    return { islands: s.render.islands, ropes: window.__structvox.renderer.ropes.count, pieces: s.pieceBodies };
  });
  check('the pieces are drawn', r.islands >= 10, `${r.islands} pieces held (the machines' parts, the hanging ones, the crates, the wall's rubble)`);
  check('the ropes and rods are drawn', r.ropes >= 6, `${r.ropes} (the crane's rope, the pendulum's rod, the chain's four)`);
  check("the pieces' voxels reach the client's collision", r.pieces >= 10, `${r.pieces} pieces (the machines' parts, the hanging ones, the crates)`);
  // the player dropped onto the lift's car (at (17.3, 6.45)), riding it for 12 s: up and down
  await page.evaluate(() => {
    const api = window.__structvox;
    api.noclip(false);
    api.teleport(17.3, 6.45, 6.0);
    api.look(180, -10);
  });
  const zs = [];
  let grounded = 0;
  for (let k = 0; k < 60; ++k) {
    await sleep(200);
    const s = await page.evaluate(() => window.__structvox.state());
    zs.push(s.player[2]);
    grounded += s.onGround ? 1 : 0;
    if (k === 30) await page.screenshot({ path: `${outDir}/riding.png` });
  }
  const lo = Math.min(...zs.slice(10)), hi = Math.max(...zs.slice(10));
  const x = await page.evaluate(() => window.__structvox.state().player);
  check('the player rides the lift', hi - lo > 3.0 && Math.abs(x[0] - 17.3) < 1.3 && Math.abs(x[1] - 6.45) < 1.3,
    `feet from ${lo.toFixed(2)} to ${hi.toFixed(2)} m, on the ground ${grounded} of 60 samples, at (${x[0].toFixed(2)}, ${x[1].toFixed(2)})`);
  // onto the turntable (its axis at (29.94, 7.94), 0.4 rad/s), 1.4 m out between two crates: round with it
  const c = [29.94, 7.94];
  await page.evaluate((p) => {
    const api = window.__structvox;
    api.teleport(p[0], p[1], 1.5);
  }, [c[0] + 1.4 * Math.cos(1.05), c[1] + 1.4 * Math.sin(1.05)]);
  await sleep(1000);
  const angle = async () => {
    const p = await page.evaluate(() => window.__structvox.state().player);
    return [Math.atan2(p[1] - c[1], p[0] - c[0]), Math.hypot(p[0] - c[0], p[1] - c[1]), p[2]];
  };
  const a0 = await angle();
  let turned = 0;
  let prev = a0[0];
  for (let k = 0; k < 20; ++k) {
    await sleep(200);
    const a = await angle();
    let d = a[0] - prev;
    if (d > Math.PI) d -= 2 * Math.PI;
    if (d < -Math.PI) d += 2 * Math.PI;
    turned += d;
    prev = a[0];
    if (k === 10) await page.screenshot({ path: `${outDir}/turntable-ride.png` });
  }
  const a1 = await angle();
  check('the player rides the turntable', turned > 1.0 && a1[1] < 3.0 && a1[2] < 1.5,
    `turned ${turned.toFixed(2)} rad in 4 s (0.4 rad/s), ${a1[1].toFixed(2)} m from its axis, feet at ${a1[2].toFixed(2)} m`);
  check('no console/page/WebGPU errors', errors.length === 0, errors.length ? errors.slice(0, 5).join(' | ') : 'none');
} finally {
  await browser.close();
}
console.log(failed ? 'MACHINES RUN FAILED' : 'MACHINES RUN PASSED');
process.exit(failed ? 1 : 0);
