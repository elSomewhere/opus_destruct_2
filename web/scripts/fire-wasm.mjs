#!/usr/bin/env node
/**
 * Browser test of the environment (?engine=wasm): the yard's timber house set on fire with the
 * flamethrower, screenshots as it burns, then the extinguisher. Fails on console / page /
 * WebGPU errors or a fire that does not take. Usage (dev server running):
 *   node scripts/fire-wasm.mjs [baseUrl] [outDir] [seconds]
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'fire-wasm-out');
const seconds = Number(process.argv[4] ?? 60);
mkdirSync(outDir, { recursive: true });
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const failures = [];
const check = (cond, what) => {
  console.log(cond ? `ok   ${what}` : `FAIL ${what}`);
  if (!cond) failures.push(what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: process.env.SMOKE_HEADFUL ? false : true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', '--window-size=1280,760'],
  defaultViewport: { width: 1280, height: 720, deviceScaleFactor: 1 },
});
const errors = [];
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await page.goto(`${base}?engine=wasm&world=yard&seed=1`, { waitUntil: 'load' });
  const t0 = Date.now();
  for (;;) {
    const ok = await page.evaluate(() => {
      const s = window.__structvox?.state();
      return !!s && s.ready && s.engine !== null && s.render !== null && s.render.chunksDrawn > 0;
    });
    if (ok) break;
    if (Date.now() - t0 > 60000) throw new Error('yard did not load');
    await sleep(200);
  }
  check(true, 'yard loaded');
  // in front of the timber house's north wall (y = 9 m), looking at it
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.noclip(true);
    sv.teleport(7, 13.5, 0.2);
    sv.look(-90, -5);
    sv.select('flamer');
  });
  await sleep(500);
  for (let k = 0; k < 30; k++) {
    await page.evaluate(() => window.__structvox.fire());
    await sleep(100);
  }
  const shot = async (name) => page.screenshot({ path: `${outDir}/${name}.png` });
  await shot('fire-00');
  let peak = 0;
  for (let s = 5; s <= seconds; s += 5) {
    await sleep(5000);
    const e = await page.evaluate(() => window.__structvox.state().engine);
    peak = Math.max(peak, e.fireBurning);
    console.log(`t=${s}s burning ${e.fireBurning} hot ${e.fireHot} env ${e.envMs} ms tick ${e.tickMs.toFixed(2)} ms pieces ${e.pieces} particles ${(await page.evaluate(() => window.__structvox.state().render.particles))}`);
    if (s % 15 === 0) await shot(`fire-${String(s).padStart(2, '0')}`);
  }
  check(peak > 50, `the house caught fire (peak ${peak} burning voxels)`);
  // from farther away and above: the smoke over the house
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(7, 26, 5);
    sv.look(-90, -12);
  });
  await sleep(1500);
  await shot('fire-overview');
  const smoke = await page.evaluate(() => window.__structvox.state().engine.smokeCells);
  check(smoke > 50, `smoke rises from the fire (${smoke} cells)`);
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(7, 13.5, 0.2);
    sv.look(-90, -5);
  });
  // put it out
  await page.evaluate(() => window.__structvox.select('extinguisher'));
  for (let k = 0; k < 40; k++) {
    await page.evaluate((k) => {
      const sv = window.__structvox;
      sv.look(-90 + ((k % 8) - 4) * 6, -5 + ((k >> 3) - 2) * 6);
      sv.fire();
    }, k);
    await sleep(100);
  }
  await sleep(1500);
  const after = await page.evaluate(() => window.__structvox.state().engine.fireBurning);
  console.log(`after the extinguisher: ${after} burning`);
  await shot('fire-out');
  check(errors.length === 0, `no console errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, String(e));
} finally {
  await browser.close();
}
process.exit(failures.length ? 1 : 0);
