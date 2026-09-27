#!/usr/bin/env node
/**
 * Browser run of the tower collapse on the real engine (?engine=wasm): blasts the two west rows
 * of ground columns (the engine demo's "pillars" scenario), then records screenshots and engine
 * stats while it comes down. Fails on console / page / WebGPU errors (a WASM abort included).
 * Usage (dev server running): node scripts/tower-wasm.mjs [baseUrl] [outDir] [seconds]
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'tower-wasm-out');
const seconds = Number(process.argv[4] ?? 20);
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
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl|Aborted/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await page.goto(`${base}?engine=wasm&world=tower&seed=1`, { waitUntil: 'load' });
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
    if (Date.now() - t0 > 120000) throw new Error('tower world did not load');
    await sleep(200);
  }
  // a camera south-west of the tower, looking at it
  await page.evaluate(() => {
    const api = window.__structvox;
    api.noclip(true);
    api.teleport(-26, -22, 12);
    api.look(40, -12);
  });
  await sleep(1000);
  const h = 0.125;
  const col = (ix, iy) => [h * (40 + ix * 26 + 1), h * (40 + iy * 26 + 1), 1.0];
  const shots = [];
  for (let ix = 0; ix <= 1; ++ix) for (let iy = 0; iy <= 3; ++iy) shots.push(col(ix, iy));
  for (const p of shots) {
    await page.evaluate((pos) => window.__structvox.engine.blast(pos, 0.9, 1e6), p);
    await sleep(250);
  }
  const log = [];
  const tStart = Date.now();
  let shot = 0;
  while (Date.now() - tStart < seconds * 1000) {
    await sleep(1000);
    const s = await page.evaluate(() => window.__structvox.state());
    const e = s.engine ?? {};
    const line = {
      t: ((Date.now() - tStart) / 1000).toFixed(1),
      fps: Number(s.fps).toFixed(1),
      ...e,
    };
    log.push(line);
    console.log(JSON.stringify(line).slice(0, 400));
    if (shot < 24) await page.screenshot({ path: `${outDir}/shot_${String(shot++).padStart(2, '0')}.png` });
    if (errors.some((x) => /Aborted|RuntimeError/.test(x))) break;
  }
  writeFileSync(`${outDir}/log.json`, JSON.stringify(log, null, 1));
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
console.log(failed ? 'TOWER RUN FAILED' : 'TOWER RUN PASSED');
process.exit(failed ? 1 : 0);
