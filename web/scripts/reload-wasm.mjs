#!/usr/bin/env node
/**
 * Browser run of a world loaded again and again on the real engine (?engine=wasm): the tower's
 * two west rows of ground columns are blasted, the same world is loaded afresh and blasted
 * again, for as many rounds as asked. Every round must come down like the first - the engine's
 * pieces and the front end's agree, and every one of them is posed by the engine.
 *
 * The bug it stands against: the pose window (frameAck / POSE_LAG) used to carry across a load,
 * so a bake that sends no poses left the page's last ack behind for good, `debris` messages were
 * held from the second world on, and its pieces stayed at the pose they detached at - a ghost of
 * the intact structure, with no rubble moving under it.
 *
 * Screenshots per round; fails on console / page / WebGPU errors (a WASM abort included) or a
 * failed check.
 * Usage (dev server running): node scripts/reload-wasm.mjs [baseUrl] [outDir] [rounds]
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { launch, beforeLoad, afterLoad, worldQuery } from './browser.mjs';

const puppeteer = (await import('puppeteer-core')).default;
const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'reload-wasm-out');
const rounds = Math.max(2, Number(process.argv[4] ?? 3));
mkdirSync(outDir, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const h = 0.125;
const browser = await launch(puppeteer);
const errors = [];
const log = [];
let failed = false;

function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}: ${detail}`);
  if (!ok) failed = true;
}

async function waitReady(page, what) {
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
    if (ok) return;
    if (Date.now() - t0 > 120000) throw new Error(`${what} did not load`);
    await sleep(200);
  }
}

/** The islands the renderer holds, and how many of them the engine has posed. */
function posedIslands() {
  let held = 0;
  let posed = 0;
  for (const isl of window.__structvox.renderer.islands.list) {
    held++;
    if (isl.cur !== null) posed++;
  }
  return { held, posed };
}

try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl|Aborted/.test(t)) errors.push(t);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await beforeLoad(page);
  await page.goto(`${base}?engine=wasm&${worldQuery('world=tower&seed=1')}`, { waitUntil: 'load' });
  await afterLoad(page);
  await waitReady(page, 'the tower world');

  // the ground columns of the two west rows (the engine demo's "pillars" scenario)
  const shots = [];
  for (let ix = 0; ix <= 1; ++ix) for (let iy = 0; iy <= 3; ++iy) shots.push([h * (40 + ix * 26 + 1), h * (40 + iy * 26 + 1), 1.0]);

  for (let round = 0; round < rounds; ++round) {
    if (round > 0) {
      // the world afresh, by the path the "Load world" button takes
      await page.evaluate(() => window.__structvox.load('tower', 1));
      await sleep(500);
      await waitReady(page, `the tower world of round ${round}`);
    }
    // a camera south-west of the tower, looking at it
    await page.evaluate(() => {
      const api = window.__structvox;
      api.noclip(true);
      api.teleport(-26, -22, 12);
      api.look(40, -12);
    });
    await sleep(1500);
    for (const p of shots) {
      await page.evaluate((pos) => window.__structvox.engine.blast(pos, 0.9, 1e6), p);
      await sleep(250);
    }
    for (let t = 0; t < 10; ++t) await sleep(1000);

    const s = await page.evaluate(() => window.__structvox.state());
    const isl = await page.evaluate(posedIslands);
    await page.screenshot({ path: `${outDir}/round_${String(round).padStart(2, '0')}.png` });
    const pieces = s.engine?.pieces ?? 0;
    const broken = s.engine?.bondsBroken ?? 0;
    const bodies = s.pieceBodies;
    log.push({
      round,
      pieces,
      broken,
      bodies,
      held: isl.held,
      posed: isl.posed,
      triangles: s.render?.triangles ?? 0,
      structures: s.engine?.structures ?? 0,
      maxUtilization: s.engine?.maxUtilization ?? 0,
    });

    check(`round ${round} came down`, broken > 1000 && pieces > 200, `${broken} bonds broken, ${pieces} pieces`);
    // the front end may be a stats message (4 Hz) out of step with the engine, never 10% out
    const slack = Math.max(64, Math.round(pieces * 0.1));
    check(`round ${round} holds the engine's pieces`, Math.abs(bodies - pieces) <= slack, `${bodies} held, ${pieces} in the engine (${slack} slack)`);
    // a piece the engine does not pose is drawn where it detached and never moves again
    check(`round ${round} poses every piece`, isl.held - isl.posed <= slack, `${isl.posed} of ${isl.held} posed`);
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
console.log(failed ? 'RELOAD RUN FAILED' : 'RELOAD RUN PASSED');
process.exit(failed ? 1 : 0);
