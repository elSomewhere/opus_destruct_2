// Stage "ids": idOf (svx/ids.js) on fixed key lists like the exporters' and on random ones
// (strings of printable ASCII, integers, fractions, booleans: each hashed as String(key)).
import { REF, line, samples } from "../lib/rec.mjs";

const { idOf } = await import(REF + "svx/ids.js");

export default function* ids() {
  yield line("fixed", idOf(), idOf(""), idOf("hw", "e12", 1, 0, 3, 7), idOf("hwramp", "e3", 2, 1), idOf("C3_-2/r5", "lane", 0, 1, -1, 4), idOf("C0_0/r1", "park", 2, 1, 17),
    idOf("C0_0/r1", "walk", 1, true), idOf("C0_0/r1", "cross", 3, "a"), idOf(0.1 + 0.2, -0, 1e21, 1e-7, -5.5), idOf("x".repeat(300)));
  const r = samples(11);
  const word = () => {
    let s = "";
    const n = Math.floor(r() * 14);
    for (let i = 0; i < n; i += 1) s += String.fromCharCode(32 + Math.floor(r() * 95));
    return s;
  };
  for (let i = 0; i < 4000; i += 1) {
    const n = Math.floor(r() * 8);
    const keys = [];
    for (let k = 0; k < n; k += 1) {
      const t = r();
      if (t < 0.4) keys.push(word());
      else if (t < 0.7) keys.push(Math.floor((r() - 0.5) * 2e6));
      else if (t < 0.9) keys.push((r() - 0.5) * 1e3);
      else keys.push(r() < 0.5);
    }
    yield line("id", n, idOf(...keys));
  }
}
