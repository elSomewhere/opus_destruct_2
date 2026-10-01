// Stage "config": every preset and size variant in every season, resolved and merged
// (presetConfig -> makeConfig), and its viewer mood, as JSON (JSON.stringify's key order and
// numbers); makeConfig is idempotent on its own output.
import { REF } from "../lib/rec.mjs";

const { makeConfig, DEFAULT_CONFIG } = await import(REF + "config/defaults.js");
const { PRESETS, presetConfig, presetViewer } = await import(REF + "config/presets.js");
const SEASONS = ["", "spring", "summer", "autumn", "winter"];

export default function* config() {
  yield JSON.stringify(DEFAULT_CONFIG);
  yield JSON.stringify(makeConfig({}));
  for (const p of PRESETS.all()) {
    const sizes = p.sizes ? ["", ...p.sizes.map((s) => s.id)] : [""];
    for (const size of sizes)
      for (const season of SEASONS) {
        const o = presetConfig(p.id, { size: size || null, seed: season === "autumn" ? 42 : null, season: season || null });
        const c = makeConfig(o);
        yield `${p.id} ${size} ${season} ${JSON.stringify(o)}`;
        yield JSON.stringify(c);
        yield JSON.stringify(makeConfig(c));
        yield JSON.stringify(presetViewer(p.id, season || null));
      }
  }
}
