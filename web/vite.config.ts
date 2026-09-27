import { resolve } from 'node:path';
import { defineConfig } from 'vite';

/**
 * Cross-origin isolation (plan §B9): SharedArrayBuffer, and therefore WASM threads, only
 * exist when the page is cross-origin isolated. Dev and preview both send the headers so
 * the WASM worker behaves the same in either mode. Production hosting must send them too.
 */
const crossOriginIsolation = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin',
} as const;

export default defineConfig({
  server: {
    headers: crossOriginIsolation,
    fs: {
      // The WASM worker may import the Emscripten output straight from the repo's
      // build/ directory during development (see README, "WASM worker integration").
      allow: ['..'],
    },
  },
  preview: {
    headers: crossOriginIsolation,
  },
  worker: {
    // Module workers: the mock is plain ESM and an Emscripten -sEXPORT_ES6 module can be
    // imported from a module worker.
    format: 'es',
  },
  build: {
    target: 'es2022',
    sourcemap: true,
    rollupOptions: {
      // the game, and the svx_anim lab (characters without the physics engine)
      input: { main: resolve(import.meta.dirname, 'index.html'), lab: resolve(import.meta.dirname, 'lab.html') },
    },
  },
});
