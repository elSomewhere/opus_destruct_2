#!/usr/bin/env node
/**
 * Browser smoke test (plan §E7): loads the app in headless Chrome with WebGPU, plays a
 * short scripted scenario through the window.__structvox debug handle and fails on any
 * console error, page error or WebGPU validation error.
 *
 * Usage (dev server running, e.g. `npm run dev -- --port 5190`):
 *   npm i --no-save puppeteer-core
 *   node scripts/smoke.mjs [baseUrl] [outDir]
 * Env: CHROME_PATH (browser binary), SMOKE_HEADFUL=1 (show the window).
 */
import { existsSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

let puppeteer;
try {
  puppeteer = (await import('puppeteer-core')).default;
} catch {
  console.error('puppeteer-core is not installed. Run: npm i --no-save puppeteer-core');
  process.exit(2);
}

const base = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'smoke-out');
mkdirSync(outDir, { recursive: true });

function chromePath() {
  if (process.env.CHROME_PATH) return process.env.CHROME_PATH;
  const candidates = {
    darwin: ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/Applications/Chromium.app/Contents/MacOS/Chromium'],
    linux: ['/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser'],
    win32: ['C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'],
  }[process.platform] ?? [];
  const found = candidates.find((p) => existsSync(p));
  if (!found) throw new Error('No Chrome found; set CHROME_PATH');
  return found;
}

const failures = [];
const notes = [];
function check(cond, what) {
  if (cond) notes.push(`ok   ${what}`);
  else failures.push(`FAIL ${what}`);
  console.log(cond ? `ok   ${what}` : `FAIL ${what}`);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const browser = await puppeteer.launch({
  executablePath: chromePath(),
  headless: process.env.SMOKE_HEADFUL ? false : true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', '--no-default-browser-check', '--window-size=1280,760'],
  defaultViewport: { width: 1280, height: 720, deviceScaleFactor: 1 },
});

const consoleErrors = [];
function watch(page, label) {
  page.on('console', (msg) => {
    const text = msg.text();
    if (msg.type() === 'error' || /\[webgpu\]|\[wgsl/.test(text)) consoleErrors.push(`${label}: ${msg.type()} ${text}`);
  });
  page.on('pageerror', (err) => consoleErrors.push(`${label}: pageerror ${err.message}`));
  page.on('response', (res) => {
    if (res.status() >= 400) consoleErrors.push(`${label}: HTTP ${res.status()} ${res.url()}`);
  });
}

async function waitFor(page, fn, arg, timeoutMs, what) {
  const t0 = Date.now();
  for (;;) {
    const v = await page.evaluate(fn, arg);
    if (v) return v;
    if (Date.now() - t0 > timeoutMs) {
      check(false, `${what} (timed out after ${timeoutMs} ms)`);
      return null;
    }
    await sleep(100);
  }
}

const state = (page) => page.evaluate(() => window.__structvox.state());
const statsSeq = (page) => page.evaluate(() => window.__structvox?.state().statsSeq ?? 0);
/** Ready, drawing, and a stats message newer than `seq` reports an empty mesh queue. */
const worldSettled = (seq) => {
  const s = window.__structvox?.state();
  return !!s && s.ready && s.statsSeq > seq && s.engine !== null && s.engine.meshQueue === 0 && s.render !== null && s.render.chunksDrawn > 0;
};

try {
  // --- 1. Mock engine, rooms world ------------------------------------------------------
  const page = await browser.newPage();
  watch(page, 'mock');
  await page.goto(`${base}?engine=mock&world=rooms&seed=1`, { waitUntil: 'load' });
  check(await page.evaluate(() => self.crossOriginIsolated), 'page is cross-origin isolated (COOP/COEP)');
  await waitFor(page, worldSettled, 0, 30000, 'rooms world ready and fully meshed');
  let s = await state(page);
  check(s.render.chunksTotal > 100 && s.engine.voxels > 1e6, `rooms: ${s.engine.voxels} voxels, ${s.render.chunksTotal} chunks, ${s.render.chunksDrawn} drawn`);
  const fps = await page.evaluate(
    () =>
      new Promise((res) => {
        let n = 0;
        const t0 = performance.now();
        const f = () => {
          n++;
          if (performance.now() - t0 >= 2000) res((n * 1000) / (performance.now() - t0));
          else requestAnimationFrame(f);
        };
        requestAnimationFrame(f);
      }),
  );
  notes.push(`rAF rate over 2 s: ${fps.toFixed(1)} fps (headless, 1280x720, 4x MSAA)`);
  console.log(`info rAF ${fps.toFixed(1)} fps`);
  await page.screenshot({ path: `${outDir}/01-spawn.png` });

  // --- 2. Hitscan: pistol + shotgun carve the far wall ----------------------------------
  const v0 = s.engine.voxels;
  await page.evaluate(async () => {
    const sv = window.__structvox;
    sv.select('pistol');
    for (let i = 0; i < 5; i++) {
      sv.look(84 + i * 3, 5);
      sv.fire();
      await new Promise((r) => setTimeout(r, 180));
    }
    sv.select('shotgun');
    await new Promise((r) => setTimeout(r, 300));
    sv.look(96, 3);
    sv.fire();
    sv.look(90, 3);
  });
  await sleep(700);
  s = await state(page);
  check(s.engine.voxels < v0, `hitscan carved ${v0 - s.engine.voxels} voxels`);
  await page.screenshot({ path: `${outDir}/02-hitscan.png` });

  // --- 3. Rockets drop the bridge (room 2,0): blast + detached island + fade -------------
  const v1 = s.engine.voxels;
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.teleport(41.5, 2.5, 0.01);
    sv.select('rocket');
  });
  await sleep(500);
  const aim = (dx, dy) => (Math.atan2(dy, dx) * 180) / Math.PI;
  await page.evaluate((yaw) => {
    window.__structvox.look(yaw, -3);
    window.__structvox.fire();
  }, aim(-2.1, 6.0));
  await waitFor(page, (v) => window.__structvox.state().engine.voxels < v, v1, 4000, 'first rocket blast removed voxels');
  await sleep(800);
  await page.evaluate((yaw) => {
    window.__structvox.look(yaw, -3);
    window.__structvox.fire();
    window.__structvox.look(90, 14);
  }, aim(2.1, 6.0));
  const islands = await waitFor(page, () => window.__structvox.state().render?.islands ?? 0, null, 4000, 'bridge detached (island rendered)');
  if (islands) {
    await sleep(350);
    await page.screenshot({ path: `${outDir}/03-bridge-falling.png` });
  }
  s = await state(page);
  check(s.engine.islands >= 1 && s.engine.detachedVoxels > 1000, `detached islands: ${s.engine.islands}, ${s.engine.detachedVoxels} voxels`);
  await sleep(2500);
  s = await state(page);
  check(s.render.islands === 0, 'islands faded out and were released');
  await page.screenshot({ path: `${outDir}/04-after-collapse.png` });

  // --- 4. Debug views re-mesh with debug bytes ---------------------------------------------
  let seq = await statsSeq(page);
  await page.evaluate(() => window.__structvox.setDebugView(1));
  await waitFor(page, worldSettled, seq, 20000, 'utilization view re-meshed');
  await page.screenshot({ path: `${outDir}/05-utilization.png` });
  await page.evaluate(() => {
    const sv = window.__structvox;
    sv.setDebugView(2);
    sv.look(160, -5);
    sv.fire();
  });
  await sleep(900);
  await page.screenshot({ path: `${outDir}/06-bubbles.png` });
  s = await state(page);
  check(s.engine.activeBubbles >= 1, `bubble debug: ${s.engine.activeBubbles} active bubble(s)`);
  await page.evaluate(() => window.__structvox.setDebugView(0));

  // --- 5. Other procedural worlds load ------------------------------------------------------
  for (const [kind, seed] of [
    ['city', 2],
    ['tower', 3],
  ]) {
    seq = await statsSeq(page);
    await page.evaluate((k, sd) => window.__structvox.load(k, sd), kind, seed);
    await waitFor(page, worldSettled, seq, 40000, `${kind} world ready and meshed`);
    s = await state(page);
    check(s.render.chunksDrawn > 0, `${kind}: ${s.engine.voxels} voxels, ${s.render.chunksTotal} chunks`);
    await page.screenshot({ path: `${outDir}/07-${kind}.png` });
  }

  // --- 6. WAD loader path (mock answers with an error event and a fallback world) -----------
  const wad = resolve('../data/freedoom/freedoom2.wad');
  if (existsSync(wad)) {
    seq = await statsSeq(page);
    const input = await page.$('input[type=file]');
    await input.uploadFile(wad);
    await page.evaluate(() => {
      const btn = [...document.querySelectorAll('button')].find((b) => b.textContent === 'Load WAD');
      btn.click();
    });
    const toast = await waitFor(page, () => document.querySelector('.toast.error')?.textContent ?? '', null, 20000, 'loadWad produced an error toast');
    check(!!toast && toast.includes('MAP01'), `mock loadWad answer: "${(toast || '').slice(0, 90)}..."`);
    await waitFor(page, worldSettled, seq, 30000, 'fallback world after loadWad');
  } else {
    notes.push('skip WAD loader (data/freedoom/freedoom2.wad missing)');
  }
  await page.close();

  // --- 7. ?engine=wasm: the real engine starts (scripts/smoke-wasm.mjs tests it), or reports ---
  //         "not built yet" when its worker is missing
  const wasm = await browser.newPage();
  watch(wasm, 'wasm');
  await wasm.goto(`${base}?engine=wasm`, { waitUntil: 'load' });
  const outcome = await waitFor(
    wasm,
    () => document.querySelector('.fatal h1')?.textContent || (window.__structvox?.state().ready ? 'ready' : ''),
    null,
    60000,
    'wasm engine page starts or shows a fatal screen',
  );
  check(!!outcome && /ready|WASM engine is not built yet/.test(outcome), `?engine=wasm -> "${outcome}"`);
  await wasm.screenshot({ path: `${outDir}/08-wasm.png` });
  await wasm.close();
} catch (err) {
  failures.push(`FAIL exception: ${err?.stack ?? err}`);
  console.error(err);
} finally {
  await browser.close();
}

// The wasm page legitimately logs nothing; any error anywhere is a failure.
check(consoleErrors.length === 0, `no console/page/WebGPU errors${consoleErrors.length ? `:\n  ${consoleErrors.join('\n  ')}` : ''}`);
console.log(`\nscreenshots: ${outDir}`);
console.log(failures.length === 0 ? 'SMOKE PASSED' : `SMOKE FAILED (${failures.length})`);
process.exit(failures.length === 0 ? 0 : 1);
