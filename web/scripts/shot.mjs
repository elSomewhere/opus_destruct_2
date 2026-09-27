#!/usr/bin/env node
/**
 * Scripted screenshots for development: loads a page, waits until the world is drawn, runs an
 * optional script (steps separated by ';;', each evaluated in the page, with waits `wait:MS`)
 * and saves screenshots (`shot:NAME`). Prints console errors.
 * Usage: node scripts/shot.mjs URL OUTDIR "step;;wait:500;;shot:a;;..."
 * SHOT_READY=EXPR waits for another condition (e.g. the lab: "window.__lab?.actors.length > 0").
 */
import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const puppeteer = (await import('puppeteer-core')).default;
const url = process.argv[2] ?? 'http://localhost:5190/';
const outDir = resolve(process.argv[3] ?? 'shots');
const script = process.argv[4] ?? 'shot:main';
const size = (process.env.SHOT_SIZE ?? '1280x720').split('x').map(Number);
// SHOT_CLIP=x,y,w,h crops every screenshot
const clipEnv = process.env.SHOT_CLIP?.split(',').map(Number);
const clip = clipEnv ? { x: clipEnv[0], y: clipEnv[1], width: clipEnv[2], height: clipEnv[3] } : undefined;
mkdirSync(outDir, { recursive: true });
const chrome = process.env.CHROME_PATH ?? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: true,
  args: ['--enable-unsafe-webgpu', '--ignore-gpu-blocklist', '--no-first-run', `--window-size=${size[0]},${size[1] + 40}`],
  defaultViewport: { width: size[0], height: size[1], deviceScaleFactor: 1 },
});
const errors = [];
try {
  const page = await browser.newPage();
  page.on('console', (m) => {
    const t = m.text();
    if (m.type() === 'error' || /\[webgpu\]|\[wgsl/.test(t)) errors.push(t);
    if (process.env.SHOT_LOG) console.log(`[${m.type()}] ${t}`);
  });
  page.on('pageerror', (e) => errors.push(`pageerror ${e.message}`));
  await page.goto(url, { waitUntil: 'load' });
  const t0 = Date.now();
  for (;;) {
    const ok = await page
      .evaluate(
        process.env.SHOT_READY ??
          (() => {
            const s = window.__structvox?.state();
            return !!s && s.ready && s.render !== null && s.render.chunksDrawn > 0;
          }),
      )
      .catch(() => false);
    if (ok) break;
    if (Date.now() - t0 > 90000) throw new Error('timed out waiting for the world');
    await sleep(200);
  }
  await sleep(500);
  for (const raw of script.split(';;')) {
    const step = raw.trim();
    if (!step) continue;
    if (step.startsWith('wait:')) await sleep(Number(step.slice(5)));
    else if (step.startsWith('shot:')) {
      await page.screenshot({ path: `${outDir}/${step.slice(5)}.png`, clip });
      console.log(`shot ${outDir}/${step.slice(5)}.png`);
    } else if (step.startsWith('seq:')) {
      // seq:NAME:COUNT:COLS:JS -> COUNT screenshots, JS evaluated before each, tiled COLS wide
      const [, name, count, cols, ...js] = step.split(':');
      const n = Number(count);
      for (let i = 0; i < n; i++) {
        await page.evaluate(js.join(':'));
        await sleep(60);
        await page.screenshot({ path: `${outDir}/${name}_${String(i).padStart(2, '0')}.png`, clip });
      }
      const rows = Math.ceil(n / Number(cols));
      execFileSync('ffmpeg', ['-y', '-loglevel', 'error', '-i', `${outDir}/${name}_%02d.png`, '-filter_complex', `tile=${cols}x${rows}`, `${outDir}/${name}.png`]);
      console.log(`sheet ${outDir}/${name}.png`);
    } else {
      const v = await page.evaluate(step);
      if (v !== undefined) console.log('=>', typeof v === 'string' ? v : JSON.stringify(v));
    }
  }
} catch (e) {
  errors.push(String(e));
} finally {
  await browser.close();
}
if (errors.length) {
  console.log(`errors (${errors.length}):`);
  for (const e of errors.slice(0, 20)) console.log('  ', e);
  process.exitCode = 1;
}
