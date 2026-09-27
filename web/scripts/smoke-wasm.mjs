#!/usr/bin/env node
/**
 * Browser smoke test of the real engine (?engine=wasm): procedural rooms under pistol and
 * rocket fire, then a Freedoom map through the WAD loader. Fails on console / page / WebGPU
 * errors. Usage (dev server running): node scripts/smoke-wasm.mjs [baseUrl] [outDir]
 */
import { existsSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'smoke-wasm-out');
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
const logs = [];
async function waitFor(page, fn, arg, ms, what) {
  const t0 = Date.now();
  for (;;) {
    let v = null;
    try {
      v = await page.evaluate(fn, arg);
    } catch (e) {
      // the dev server reloads the page when the WASM build output changes: keep waiting
      if (!/Execution context was destroyed|Cannot find context/.test(String(e))) throw e;
    }
    if (v) return v;
    if (Date.now() - t0 > ms) {
      check(false, `${what} (timed out after ${ms} ms)`);
      return null;
    }
    await sleep(100);
  }
}
const settled = () => {
  const s = window.__structvox?.state();
  return !!s && s.ready && s.engine !== null && s.render !== null && s.render.chunksDrawn > 0;
};
const state = (page) => page.evaluate(() => window.__structvox.state());
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    logs.push(`${m.type()} ${t}`);
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await page.goto(`${base}?engine=wasm&world=rooms&seed=1`, { waitUntil: 'load' });
  await waitFor(page, settled, null, 60000, 'wasm rooms world ready and drawn');
  let s = await state(page);
  check(s.engine.voxels > 100000, `rooms: ${s.engine.voxels} voxels, ${s.render.chunksTotal} chunks drawn ${s.render.chunksDrawn}`);
  await page.screenshot({ path: `${outDir}/01-rooms.png` });
  // client-side collision (occupancy) agrees with the engine's collide
  const cmp = await page.evaluate(async () => {
    const sv = window.__structvox;
    let seed = 12345;
    const rnd = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);
    let same = 0, total = 0, maxErr = 0;
    for (let k = 0; k < 300; k++) {
      const x = 0.5 + rnd() * 18, y = 0.5 + rnd() * 10, z = -0.2 + rnd() * 3.5;
      const min = [x - 0.3, y - 0.3, z], max = [x + 0.3, y + 0.3, z + 1.75];
      const move = [(rnd() - 0.5) * 4, (rnd() - 0.5) * 4, (rnd() - 0.5) * 4];
      const local = sv.collideLocal(min, max, move);
      const remote = await sv.engine.collide(min, max, move);
      if (!local) return { total: -1 };
      total++;
      const err = Math.max(...local.move.map((v, i) => Math.abs(v - remote.move[i])));
      maxErr = Math.max(maxErr, err);
      if (err === 0 && local.onGround === remote.onGround) same++;
    }
    return { same, total, maxErr };
  });
  check(cmp.total > 0 && cmp.same === cmp.total, `client collision matches the engine on ${cmp.same}/${cmp.total} sweeps (max diff ${cmp.maxErr})`);
  const v0 = s.engine.voxels;
  await page.evaluate(async () => {
    const sv = window.__structvox;
    sv.select('pistol');
    for (let i = 0; i < 6; i++) {
      sv.look(-20 + i * 8, 2);
      sv.fire();
      await new Promise((r) => setTimeout(r, 200));
    }
  });
  await sleep(1500);
  s = await state(page);
  check(s.engine.voxels < v0, `pistol carved ${v0 - s.engine.voxels} voxels (${s.engine.structures} structures, ${s.engine.bondsBroken} bonds broken)`);
  await page.screenshot({ path: `${outDir}/02-pistol.png` });
  // Rigid piece: cut a 1.5 m square out of the first room's ceiling slab (before the rocket,
  // whose collapse may bring that ceiling down); the piece falls 3 m as a rigid body, lands on
  // the floor and stays there as rubble.
  const p0 = s.engine.pieces;
  await page.evaluate(() => {
    const sv = window.__structvox;
    const h = 0.125, cx = 27, cy = 23, z = 25.5, half = 6;
    sv.teleport(cx * h - 2.5, cy * h, 0.1);
    sv.look(0, 25);
    for (let k = -half; k <= half; k += 2)
      for (const [x, y] of [[cx + k, cy - half], [cx + k, cy + half], [cx - half, cy + k], [cx + half, cy + k]])
        sv.engine.carve([x * h, y * h, z * h], 0.25);
  });
  const pieces = await waitFor(page, (p) => {
    const s = window.__structvox.state();
    return s.render && s.render.islands > 0 && s.engine.pieces > p ? s.engine.pieces : 0;
  }, p0, 8000, 'ceiling piece became a rigid piece');
  await sleep(350);
  await page.screenshot({ path: `${outDir}/03-piece-falling.png` });
  await waitFor(page, () => window.__structvox.state().engine.awakePieces === 0, null, 8000, 'piece came to rest');
  await sleep(2000);
  await page.screenshot({ path: `${outDir}/03b-piece-resting.png` });
  s = await state(page);
  check(pieces > p0 && s.engine.pieces >= pieces && s.render.islands >= pieces, `pieces: ${s.engine.pieces} alive as rubble, ${s.engine.contacts} contacts, rigid ${s.engine.rigidMs} ms/tick`);
  const v1 = s.engine.voxels;
  await page.evaluate(async () => {
    const sv = window.__structvox;
    sv.select('rocket');
    await new Promise((r) => setTimeout(r, 400));
    sv.look(0, 8);
    sv.fire();
  });
  await waitFor(page, (v) => window.__structvox.state().engine.voxels < v, v1, 8000, 'rocket removed voxels');
  await sleep(600);
  await page.screenshot({ path: `${outDir}/04-rocket.png` });
  await waitFor(page, () => {
    const e = window.__structvox.state().engine;
    return e.structuresSolving === 0 && e.awakePieces === 0;
  }, null, 30000, 'structures and pieces settled');
  s = await state(page);
  console.log(`info rocket: ${s.engine.bondsBroken} bonds broken, ${s.engine.detachedPieces} pieces (${s.engine.pieces} alive, ${s.render.islands} islands), max util ${s.engine.maxUtilization}`);
  await page.screenshot({ path: `${outDir}/04b-after.png` });
  // Streamed 1 km^2 city: fly along a street and check bounded residency
  await page.evaluate(() => window.__structvox.load('city', 2));
  await waitFor(page, () => {
    const s = window.__structvox.state();
    return s.ready && s.engine && s.engine.residentChunks > 20 && s.render && s.render.chunksDrawn > 0;
  }, null, 60000, 'streamed city ready');
  s = await state(page);
  const r0 = s.engine.residentChunks;
  await page.screenshot({ path: `${outDir}/05-city.png` });
  const start = s.player;
  let maxResident = r0;
  for (let k = 1; k <= 20; k++) {
    await page.evaluate((x, y, z) => window.__structvox.teleport(x, y, z), start[0] + 12 * k, start[1], start[2] + 20);
    await sleep(250);
    const st = await state(page);
    maxResident = Math.max(maxResident, st.engine.residentChunks);
  }
  s = await state(page);
  check(s.engine.evictedChunks > 0, `city streaming: resident ${r0} -> max ${maxResident}, evicted ${s.engine.evictedChunks}, stream ${s.engine.streamMs} ms`);
  check(maxResident < 6 * r0 + 400, 'residency stays bounded while flying');
  await page.screenshot({ path: `${outDir}/06-city-flight.png` });
  // Freedoom MAP01 through the WAD loader
  const wad = resolve('../data/freedoom/freedoom2.wad');
  if (existsSync(wad)) {
    const input = await page.$('input[type=file]');
    await input.uploadFile(wad);
    const t0 = Date.now();
    await page.evaluate(() => {
      const btn = [...document.querySelectorAll('button')].find((b) => b.textContent === 'Load WAD');
      btn.click();
    });
    await sleep(1000);
    await waitFor(page, () => {
      const s = window.__structvox.state();
      return s.ready && s.engine && s.engine.voxels > 1e6 && s.render && s.render.chunksDrawn > 0;
    }, null, 300000, 'MAP01 loaded and drawn');
    s = await state(page);
    check(s.engine.voxels > 1e6, `MAP01: ${s.engine.voxels} voxels in ${((Date.now() - t0) / 1000).toFixed(1)} s, ${s.render.chunksTotal} chunks`);
    check((s.engine.movers ?? 0) > 0, `MAP01 sector movers: ${s.engine.movers}`);
    await sleep(1500);
    await page.screenshot({ path: `${outDir}/07-map01.png` });
    await page.evaluate(async () => {
      const sv = window.__structvox;
      sv.select('rocket');
      await new Promise((r) => setTimeout(r, 400));
      sv.fire();
    });
    await sleep(4000);
    await page.screenshot({ path: `${outDir}/08-map01-rocket.png` });
    s = await state(page);
    console.log(`info MAP01 after rocket: ${s.engine.structures} structures, ${s.engine.bondsBroken} bonds broken, ${s.engine.pieces} pieces, tick ${s.engine.tickMs} ms`);
    // a map the WAD does not have: an error toast, and the rooms world instead of an empty view
    await page.evaluate(() => {
      const map = [...document.querySelectorAll('input[type=text]')].find((i) => i.value === 'MAP01');
      map.value = 'MAP99';
      [...document.querySelectorAll('button')].find((b) => b.textContent === 'Load WAD').click();
    });
    const fallback = await waitFor(page, () => {
      const s = window.__structvox.state();
      const toast = [...document.querySelectorAll('.toast.error')].map((t) => t.textContent).find((t) => t.includes('MAP99'));
      return s.ready && s.engine && s.engine.voxels < 1e6 && s.render && s.render.chunksDrawn > 0 && toast ? toast : null;
    }, null, 60000, 'missing map falls back to the rooms world');
    s = await state(page);
    check(!!fallback, `missing map: "${fallback?.slice(0, 80)}…", then ${s.engine.voxels} voxels`);
  }
  await page.close();
} catch (e) {
  failures.push(`exception ${e?.stack ?? e}`);
  console.error(e);
} finally {
  await browser.close();
}
check(errors.length === 0, `no console/page/WebGPU errors${errors.length ? ':\n  ' + errors.slice(0, 10).join('\n  ') : ''}`);
if (failures.length) console.log(logs.filter((l) => l.includes('wasm') || l.includes('engine')).slice(-20).join('\n'));
console.log(failures.length ? `SMOKE FAILED (${failures.length})` : 'SMOKE PASSED');
process.exit(failures.length ? 1 : 0);
