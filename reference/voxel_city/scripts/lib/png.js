import { deflateSync } from "node:zlib";
import { writeFileSync } from "node:fs";

/** Minimal RGB canvas + PNG writer for headless debug renders. */
export class Canvas {
  constructor(w, h, bg = [20, 22, 26]) {
    this.w = w;
    this.h = h;
    this.px = new Uint8Array(w * h * 3);
    for (let i = 0; i < w * h; i += 1) {
      this.px[i * 3] = bg[0];
      this.px[i * 3 + 1] = bg[1];
      this.px[i * 3 + 2] = bg[2];
    }
  }

  set(x, y, c) {
    x |= 0;
    y |= 0;
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    const i = (y * this.w + x) * 3;
    this.px[i] = c[0];
    this.px[i + 1] = c[1];
    this.px[i + 2] = c[2];
  }

  blend(x, y, c, a) {
    x |= 0;
    y |= 0;
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    const i = (y * this.w + x) * 3;
    this.px[i] = this.px[i] * (1 - a) + c[0] * a;
    this.px[i + 1] = this.px[i + 1] * (1 - a) + c[1] * a;
    this.px[i + 2] = this.px[i + 2] * (1 - a) + c[2] * a;
  }

  fillRect(x0, y0, x1, y1, c) {
    for (let y = Math.max(0, y0 | 0); y <= Math.min(this.h - 1, y1 | 0); y += 1) {
      for (let x = Math.max(0, x0 | 0); x <= Math.min(this.w - 1, x1 | 0); x += 1) this.set(x, y, c);
    }
  }

  strokeRect(x0, y0, x1, y1, c) {
    for (let x = x0; x <= x1; x += 1) {
      this.set(x, y0, c);
      this.set(x, y1, c);
    }
    for (let y = y0; y <= y1; y += 1) {
      this.set(x0, y, c);
      this.set(x1, y, c);
    }
  }

  line(x0, y0, x1, y1, c) {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1);
    for (let i = 0; i <= n; i += 1) this.set(x0 + ((x1 - x0) * i) / n, y0 + ((y1 - y0) * i) / n, c);
  }

  save(path) {
    const { w, h, px } = this;
    const raw = Buffer.alloc((w * 3 + 1) * h);
    for (let y = 0; y < h; y += 1) {
      raw[y * (w * 3 + 1)] = 0;
      Buffer.from(px.buffer, y * w * 3, w * 3).copy(raw, y * (w * 3 + 1) + 1);
    }
    const chunks = [];
    const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0);
    ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    chunks.push(pngChunk("IHDR", ihdr));
    chunks.push(pngChunk("IDAT", deflateSync(raw, { level: 6 })));
    chunks.push(pngChunk("IEND", Buffer.alloc(0)));
    writeFileSync(path, Buffer.concat([sig, ...chunks]));
  }
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i += 1) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const td = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td), 0);
  return Buffer.concat([len, td, crc]);
}

export function hexToRgb(hex) {
  return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)];
}
