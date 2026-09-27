/**
 * Engine worker selection: `?engine=mock|wasm` (default: the WASM engine when it is built, else
 * the mock).
 *
 * Worker entry points are discovered with a glob over src/worker/*-worker.ts, so a missing
 * wasm-worker.ts is not a build error: selecting it reports "not built yet" at runtime.
 * Each match is bundled as a module worker (Vite `?worker`).
 */

export type EngineKind = 'mock' | 'wasm';
export const ENGINE_KINDS: readonly EngineKind[] = ['mock', 'wasm'];
export const DEFAULT_ENGINE: EngineKind = 'wasm';

interface WorkerModule {
  default: new (options?: { name?: string }) => Worker;
}

const workerModules = import.meta.glob<WorkerModule>('../worker/*-worker.ts', { query: '?worker' });

export class EngineUnavailableError extends Error {
  override readonly name = 'EngineUnavailableError';
}

export function engineKindFromUrl(url: URL): EngineKind {
  const v = url.searchParams.get('engine');
  if (v === 'wasm' || v === 'mock') return v;
  return availableEngines().includes(DEFAULT_ENGINE) ? DEFAULT_ENGINE : 'mock';
}

export function availableEngines(): EngineKind[] {
  return ENGINE_KINDS.filter((k) => `../worker/${k}-worker.ts` in workerModules);
}

/** Creates the worker for `kind`, or throws EngineUnavailableError. */
export async function createEngineWorker(kind: EngineKind): Promise<Worker> {
  const load = workerModules[`../worker/${kind}-worker.ts`];
  if (!load) {
    throw new EngineUnavailableError(
      kind === 'wasm'
        ? 'The WASM engine is not built yet: web/src/worker/wasm-worker.ts does not exist. ' +
            'Build the Emscripten module and its worker (see web/README.md), or use ?engine=mock.'
        : `No worker entry point for engine "${kind}".`,
    );
  }
  const mod = await load();
  return new mod.default({ name: `structvox-${kind}` });
}
