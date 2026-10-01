export const LOOKS = {
  classic: { label: "Classic", toneMapping: "none", exposure: 1, desaturate: 0, grime: 0, wetness: 0, grain: 0, fog: 1, sun: 1, ambient: 1 },
  "bleak-overcast": { label: "Bleak overcast", toneMapping: "agx", exposure: 0.82, desaturate: 0.5, grime: 0.38, wetness: 0.12, grain: 0.06, fog: 1.8, sun: 0.45, ambient: 0.82 },
  "rain-at-dusk": { label: "Rain at dusk", toneMapping: "agx", exposure: 0.72, desaturate: 0.3, grime: 0.34, wetness: 0.85, grain: 0.08, fog: 2.2, sun: 0.38, ambient: 0.65, rain: 1 },
  "hard-noon": { label: "Hard noon", toneMapping: "aces", exposure: 1.12, desaturate: 0.05, grime: 0.12, wetness: 0, grain: 0.015, fog: 0.55, sun: 1.35, ambient: 0.9 },
  fog: { label: "Dense fog", toneMapping: "agx", exposure: 0.9, desaturate: 0.4, grime: 0.12, wetness: 0.2, grain: 0.03, fog: 5, sun: 0.28, ambient: 0.9 },
  "wet-night": { label: "Wet night", toneMapping: "agx", exposure: 0.62, desaturate: 0.2, grime: 0.3, wetness: 1, grain: 0.1, fog: 2.4, sun: 0.25, ambient: 0.48, rain: 0.65, time: 22.5 },
  "winter-grey": { label: "Winter grey", toneMapping: "agx", exposure: 0.9, desaturate: 0.62, grime: 0.18, wetness: 0.08, grain: 0.04, fog: 1.7, sun: 0.58, ambient: 0.95 },
};

export const lookById = (id) => LOOKS[id] ?? LOOKS.classic;

