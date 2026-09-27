/**
 * Minimal WAD directory reader for the mock engine: validates a WAD and locates a map so
 * `loadWad` can give a precise answer. Voxelizing maps is the real engine's job
 * (core/src/doom); the mock does not attempt it.
 */

export interface WadLump {
  name: string;
  offset: number;
  size: number;
}

export type WadInspection =
  | { ok: true; kind: 'IWAD' | 'PWAD'; lumps: number; map: string; textureLumps: number }
  | { ok: false; error: string };

const MAP_LUMPS = ['THINGS', 'LINEDEFS', 'SIDEDEFS', 'VERTEXES', 'SECTORS'];
const TEXTURE_LUMPS = new Set(['PLAYPAL', 'PNAMES', 'TEXTURE1', 'TEXTURE2', 'F_START', 'FF_START']);

export function readWadDirectory(buffer: ArrayBuffer): { kind: 'IWAD' | 'PWAD'; lumps: WadLump[] } | string {
  if (buffer.byteLength < 12) return 'file too small to be a WAD';
  const dv = new DataView(buffer);
  const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
  if (magic !== 'IWAD' && magic !== 'PWAD') return `not a WAD file (magic "${magic}")`;
  const count = dv.getInt32(4, true);
  const dir = dv.getInt32(8, true);
  if (count < 0 || dir < 12 || dir + count * 16 > buffer.byteLength) return 'corrupt WAD directory';
  const lumps: WadLump[] = [];
  for (let i = 0; i < count; i++) {
    const o = dir + i * 16;
    let name = '';
    for (let k = 0; k < 8; k++) {
      const c = dv.getUint8(o + 8 + k);
      if (c === 0) break;
      name += String.fromCharCode(c);
    }
    lumps.push({ name: name.toUpperCase(), offset: dv.getInt32(o, true), size: dv.getInt32(o + 4, true) });
  }
  return { kind: magic, lumps };
}

export function inspectWad(buffer: ArrayBuffer, map: string): WadInspection {
  const dir = readWadDirectory(buffer);
  if (typeof dir === 'string') return { ok: false, error: dir };
  const want = map.trim().toUpperCase();
  const at = dir.lumps.findIndex((l) => l.name === want);
  if (at < 0) {
    const maps = dir.lumps.filter((_lump, i) => dir.lumps[i + 1]?.name === 'THINGS').map((l) => l.name);
    return { ok: false, error: `map ${want} not found (maps: ${maps.slice(0, 12).join(', ') || 'none'})` };
  }
  const following = new Set(dir.lumps.slice(at + 1, at + 12).map((l) => l.name));
  const udmf = following.has('TEXTMAP');
  if (!udmf && !MAP_LUMPS.every((n) => following.has(n))) return { ok: false, error: `map ${want} is incomplete` };
  const textureLumps = dir.lumps.filter((l) => TEXTURE_LUMPS.has(l.name)).length;
  return { ok: true, kind: dir.kind, lumps: dir.lumps.length, map: want, textureLumps };
}
