#!/usr/bin/env node
/**
 * Browser test of the water (?engine=wasm): the yard's reservoir, breached by a rocket, floods;
 * the water hose. Screenshots of each step; fails on console / page / WebGPU errors or water
 * that does not show or move. Usage (dev server running):
 *   node scripts/water-wasm.mjs [baseUrl] [outDir]
 */
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { launch, beforeLoad, afterLoad } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'water-wasm-out');
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
  await page.goto(`${base}?engine=wasm&world=yard&seed=1`, { waitUntil: 'load' });
  await afterLoad(page);
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
  const shot = async (name) => page.screenshot({ path: `${outDir}/${name}.png` });
  const engine = () => page.evaluate(() => window.__structvox.state().engine);
  // the reservoir (x 29.5..37.5 m, y 18.75..26.75 m) from its south side, above
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.noclip(true);
    sv.teleport(33.5, 12, 4);
    sv.look(90, -22);
  });
  await sleep(2500);
  await shot('water-00-reservoir');
  const e0 = await engine();
  check(e0.waterActive === 0, `the reservoir rests (${e0.waterActive} moving)`);
  // a rocket at its south wall (y = 18.75 m)
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(33.5, 10, 0.6);
    sv.look(90, -3);
    sv.select('rocket');
    sv.fire();
  });
  await sleep(1500);
  const e1 = await engine();
  check(e1.waterActive > 100, `the breach floods (${e1.waterActive} moving)`);
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(33.5, 12, 4);
    sv.look(90, -22);
  });
  for (const s of [2, 6, 12]) {
    await sleep(s === 2 ? 1000 : 4000);
    const e = await engine();
    console.log(`t=${s}s moving ${e.waterActive} loads ${e.waterLoads} afloat ${e.floating} env ${e.envMs} ms tick ${e.tickMs.toFixed(2)} ms`);
    await shot(`water-${String(s).padStart(2, '0')}-flood`);
  }
  // the hose on the house
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(7, 13.5, 1.6);
    sv.look(-90, 5);
    sv.select('hose');
  });
  for (let k = 0; k < 30; k++) {
    await page.evaluate(() => window.__structvox.fire());
    await sleep(100);
  }
  await sleep(1000);
  await shot('water-hose');
  check(errors.length === 0, `no console errors${errors.length ? `: ${errors.slice(0, 3).join(' | ')}` : ''}`);
} catch (e) {
  check(false, String(e));
} finally {
  await browser.close();
}
process.exit(failures.length ? 1 : 0);
