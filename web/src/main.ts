/**
 * structvox front end bootstrap.
 *
 * URL parameters:
 *   ?engine=mock|wasm         engine worker (default wasm when it is built, else mock)
 *   ?world=drive|rooms|city|tower|...   procedural world loaded at start (default: the drive
 *                             city with the WASM engine, rooms with the mock)
 *   ?preset=ID                a preset (docs/PRESETS.md: city/angledInfiniteCity, legacy/drive, ...):
 *                             loaded instead of ?world=
 *   ?seed=N                   world seed (default 1; a preset's own with ?preset=)
 *   ?debug=none|utilization|fragments   initial debug view ('bubbles', from v1 links, = fragments)
 *   ?people=N                 pedestrians about the player in the drive city (0: none)
 *   ?bodies=deep|shallow|hybrid   their bodies (default hybrid)
 */
import './styles.css';
import { EngineClient } from './engine/client.ts';
import type { PedestrianSettings, ProceduralKind } from './engine/protocol.ts';
import { DEFAULT_PARAMS, DEFAULT_PEDESTRIANS, DebugView, PEDESTRIAN_BODY_NAMES, PedestrianBodies, PROCEDURAL_KINDS } from './engine/protocol.ts';
import { createEngineWorker, engineKindFromUrl, EngineUnavailableError } from './engine/select.ts';
import { Game, type StructvoxDebugApi } from './game/game.ts';
import { WebGpuUnavailableError } from './render/gpu.ts';
import { Renderer } from './render/renderer.ts';
import { Overlay } from './ui/overlay.ts';

declare global {
  interface Window {
    /** Debug handle for the console and automated browser checks. */
    __structvox?: StructvoxDebugApi & { engine: EngineClient; renderer: Renderer };
  }
}

const VOXEL_SIZE = 0.125;

function parseWorld(v: string | null, engine: string): ProceduralKind {
  return PROCEDURAL_KINDS.find((k) => k === v) ?? (engine === 'wasm' ? 'drive' : 'rooms');
}

function parseDebug(v: string | null): DebugView {
  if (v === 'utilization') return DebugView.Utilization;
  if (v === 'fragments' || v === 'fragment' || v === 'bubbles' || v === 'bubble') return DebugView.Fragments;
  return DebugView.None;
}

/** Pedestrians from ?people= and ?bodies= (null: neither given - the engine's defaults). */
function parsePedestrians(url: URL): PedestrianSettings | null {
  const count = url.searchParams.get('people');
  const bodies = url.searchParams.get('bodies');
  if (count === null && bodies === null) return null;
  const p = { ...DEFAULT_PEDESTRIANS };
  if (count !== null) {
    const n = Math.floor(Number(count));
    if (Number.isFinite(n)) {
      p.count = Math.max(0, Math.min(256, n));
      p.enabled = p.count > 0;
    }
  }
  const b = Object.values(PedestrianBodies).find((v) => PEDESTRIAN_BODY_NAMES[v] === bodies);
  if (b !== undefined) p.bodies = b;
  return p;
}

function message(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

async function main(): Promise<void> {
  const canvas = document.querySelector<HTMLCanvasElement>('#view');
  const uiRoot = document.querySelector<HTMLElement>('#ui');
  if (!canvas || !uiRoot) throw new Error('index.html is missing #view or #ui');
  const overlay = new Overlay(uiRoot);
  const url = new URL(window.location.href);
  const kind = engineKindFromUrl(url);
  const preset = url.searchParams.get('preset');
  const seedParam = url.searchParams.get('seed');
  const seed = seedParam === null ? (preset ? 0 : 1) : Math.max(0, Math.floor(Number(seedParam) || 0));
  const world = { kind: parseWorld(url.searchParams.get('world'), kind), seed, ...(preset ? { preset } : {}) };
  const params = { ...DEFAULT_PARAMS, debugView: parseDebug(url.searchParams.get('debug')) };
  const pedestrians = parsePedestrians(url);

  overlay.setLoading('Initializing WebGPU', 0.02);
  let game: Game | null = null;
  let renderer: Renderer;
  try {
    renderer = await Renderer.create(canvas, (reason) => {
      game?.stop();
      overlay.fatal('The GPU device was lost', `${reason}. Reload the page to continue.`);
    });
  } catch (err) {
    const known = err instanceof WebGpuUnavailableError;
    overlay.fatal(known ? 'WebGPU is not available' : 'WebGPU initialization failed', message(err));
    return;
  }

  overlay.setLoading(`Starting the ${kind} engine`, 0.04);
  let worker: Worker;
  try {
    worker = await createEngineWorker(kind);
  } catch (err) {
    const notBuilt = err instanceof EngineUnavailableError && kind === 'wasm';
    overlay.fatal(notBuilt ? 'The WASM engine is not built yet' : 'Could not start the engine', message(err), [
      { href: '?engine=mock', label: 'Run with the mock engine' },
    ]);
    return;
  }
  if (kind === 'wasm' && !window.crossOriginIsolated) {
    overlay.toast('Page is not cross-origin isolated: SharedArrayBuffer (WASM threads) is unavailable. Serve with COOP/COEP headers.', 'error', 12000);
  }

  const engine = new EngineClient(worker, kind);
  game = new Game({ canvas, uiRoot, overlay, renderer, engine, world, params, voxelSize: VOXEL_SIZE, ...(pedestrians ? { pedestrians } : {}) });
  game.start();
  window.__structvox = { ...game.debugApi(), engine, renderer };
}

main().catch((err: unknown) => {
  console.error(err);
  document.body.append(Object.assign(document.createElement('pre'), { className: 'boot-error', textContent: `structvox failed to start: ${message(err)}` }));
});
