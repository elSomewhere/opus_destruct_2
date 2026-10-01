export const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
export const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
export const lerp = (a, b, t) => a + (b - a) * t;
export const invLerp = (a, b, v) => (a === b ? 0 : (v - a) / (b - a));
export const remap = (v, a0, a1, b0, b1) => lerp(b0, b1, invLerp(a0, a1, v));
export const fract = (v) => v - Math.floor(v);
export const mod = (a, n) => ((a % n) + n) % n;
export const sq = (v) => v * v;

export function smoothstep(e0, e1, v) {
  const t = clamp01((v - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

/** Polynomial smooth minimum; k is the blend radius. */
export function smin(a, b, k) {
  if (k <= 0) return Math.min(a, b);
  const h = clamp01(0.5 + (0.5 * (b - a)) / k);
  return lerp(b, a, h) - k * h * (1 - h);
}

/** Circular smooth minimum (exact fillet of radius k for perpendicular SDFs). */
export function sminCircular(a, b, k) {
  if (k <= 0) return Math.min(a, b);
  const ua = Math.max(k - a, 0);
  const ub = Math.max(k - b, 0);
  return Math.max(k, Math.min(a, b)) - Math.hypot(ua, ub);
}

export function floorDiv(a, b) {
  return Math.floor(a / b);
}

export function snap(v, step) {
  return Math.round(v / step) * step;
}

export function snapDown(v, step) {
  return Math.floor(v / step) * step;
}
