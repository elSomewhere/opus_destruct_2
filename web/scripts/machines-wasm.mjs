#!/usr/bin/env node
/**
 * Browser run of the machines world on the real engine (?engine=wasm): kinematic bodies and
 * joints (docs/MOTION.md). Their grids are drawn with their frames as they move, the ropes and
 * rods of the joints are drawn, and the player rides the lift: dropped onto its deck, they go up
 * and down with it (the client's collision against the moving grid, and its velocity). Screenshots;
 * fails on console / page / WebGPU errors (a WASM abort included) or a failed check.
 * Usage (dev server running): node scripts/machines-wasm.mjs [baseUrl] [outDir]
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'machines-wasm-out');
mkdirSync(outDir, { recursive: true });
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: process.env.SMOKE_HEADFUL ? false : true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', '--window-size=1280,760'],
  defaultViewport: { width: 1280, height: 720, deviceScaleFactor: 1 },
});
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
  await page.goto(`${base}?engine=wasm&world=machines&seed=1`, { waitUntil: 'load' });
  const t0 = Date.now();
  for (;;) {
    let ok = false;
    try {
      ok = await page.evaluate(() => {
        const s = window.__structvox?.state();
        return !!s && s.ready && s.engine !== null && s.render !== null && s.render.gridChunksDrawn > 0;
      });
    } catch (e) {
      if (!/Execution context was destroyed|Cannot find context/.test(String(e))) throw e;
    }
    if (ok) break;
    if (Date.now() - t0 > 120000) throw new Error('machines world did not load');
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
    const s = window.__structvox.state().render;
    return { grids: s.gridChunks, drawn: s.gridChunksDrawn, ropes: window.__structvox.renderer.ropes.count };
  });
  check('the bodies\' grids are drawn', r.grids > 0, `${r.grids} grid chunks held`);
  check('the ropes and rods are drawn', r.ropes >= 6, `${r.ropes} (the crane's rope, the pendulum's rod, the chain's four)`);
  // the player dropped onto the lift's deck (at (17.35, 6.5)), riding it for 12 s: up and down
  await page.evaluate(() => {
    const api = window.__structvox;
    api.noclip(false);
    api.teleport(17.35, 6.5, 6.0);
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
  check('the player rides the lift', hi - lo > 3.0 && Math.abs(x[0] - 17.35) < 1.3 && Math.abs(x[1] - 6.5) < 1.3,
    `feet from ${lo.toFixed(2)} to ${hi.toFixed(2)} m, on the ground ${grounded} of 60 samples, at (${x[0].toFixed(2)}, ${x[1].toFixed(2)})`);
  check('no console/page/WebGPU errors', errors.length === 0, errors.length ? errors.slice(0, 5).join(' | ') : 'none');
} finally {
  await browser.close();
}
console.log(failed ? 'MACHINES RUN FAILED' : 'MACHINES RUN PASSED');
process.exit(failed ? 1 : 0);
