#!/usr/bin/env node
/**
 * Browser look at the environment in the streamed city (?engine=wasm): a pond near the spawn,
 * and a timber-floored building set on fire with the flamethrower. Screenshots; fails on
 * console / page / WebGPU errors. Usage (dev server running):
 *   node scripts/city-env-wasm.mjs [baseUrl] [outDir]
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { launch, beforeLoad, afterLoad } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'city-env-out');
mkdirSync(outDir, { recursive: true });
const failures = [];
const check = (cond, what) => {
  console.log(cond ? `ok   ${what}` : `FAIL ${what}`);
  if (!cond) failures.push(what);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await launch(puppeteer);
const errors = [];
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await beforeLoad(page);
  await page.goto(`${base}?engine=wasm&world=city&seed=1`, { waitUntil: 'load' });
  await afterLoad(page);
  const t0 = Date.now();
  for (;;) {
    const ok = await page.evaluate(() => {
      const s = window.__structvox?.state();
      return !!s && s.ready && s.engine !== null && s.engine.residentChunks > 20 && s.render !== null && s.render.chunksDrawn > 0;
    });
    if (ok) break;
    if (Date.now() - t0 > 60000) throw new Error('city did not load');
    await sleep(200);
  }
  check(true, 'city loaded');
  const shot = async (name) => page.screenshot({ path: `${outDir}/${name}.png` });
  // the pond of the lot north-east of the spawn (about 486, 510 m), from its edge
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.noclip(true);
    sv.teleport(480, 496, 4);
    sv.look(60, -20);
  });
  await sleep(4000);
  await shot('city-pond');
  // a building: the flamethrower at its ground floor, from the street
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(477, 492, 0.2);
    sv.look(90, 12);
    sv.select('flamer');
  });
  await sleep(1500);
  for (let k = 0; k < 40; k++) {
    await page.evaluate((k) => {
      const sv = window.__structvox;
      sv.look(60 + (k % 8) * 8, 10 + (k >> 3) * 6);
      sv.fire();
    }, k);
    await sleep(100);
  }
  await sleep(15000);
  const e = await page.evaluate(() => window.__structvox.state().engine);
  console.log(`fire: ${e.fireBurning} burning, ${e.fireHot} hot, smoke ${e.smokeCells} cells, water ${e.waterActive} moving, env ${e.envMs} ms, tick ${e.tickMs.toFixed(2)} ms`);
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(470, 488, 3);
    sv.look(70, 15);
  });
  await sleep(1500);
  await shot('city-fire');
  check(errors.length === 0, `no console errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, String(e));
} finally {
  await browser.close();
}
process.exit(failures.length ? 1 : 0);
