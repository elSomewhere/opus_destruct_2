/**
 * GPU side of the texture atlas: a mipmapped rgba8unorm-srgb 2D array texture plus a
 * storage buffer of per-texture records (see atlas-pack.ts for the layout rules).
 */
import { atlasInfoRecords, downsampleSrgb, mipLevelCount, packAtlas, type AtlasSource } from './atlas-pack.ts';

export class GpuAtlas {
  readonly texture: GPUTexture;
  readonly view: GPUTextureView;
  readonly info: GPUBuffer;
  /** Number of texture ids covered by `info` (ids >= count render untextured). */
  readonly count: number;
  readonly bytes: number;

  private constructor(texture: GPUTexture, info: GPUBuffer, count: number, bytes: number) {
    this.texture = texture;
    this.view = texture.createView({ dimension: '2d-array' });
    this.info = info;
    this.count = count;
    this.bytes = bytes;
  }

  static create(device: GPUDevice, sources: readonly AtlasSource[]): GpuAtlas {
    const maxPage = Math.min(2048, device.limits.maxTextureDimension2D);
    const atlas = packAtlas(sources, maxPage);
    if (atlas.layers > device.limits.maxTextureArrayLayers) {
      throw new Error(`texture atlas needs ${atlas.layers} layers (limit ${device.limits.maxTextureArrayLayers})`);
    }
    const levels = mipLevelCount(atlas.pageSize);
    const texture = device.createTexture({
      label: 'texture atlas',
      size: [atlas.pageSize, atlas.pageSize, atlas.layers],
      format: 'rgba8unorm-srgb',
      mipLevelCount: levels,
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    let bytes = 0;
    atlas.pages.forEach((page, layer) => {
      let data = page;
      let size = atlas.pageSize;
      for (let level = 0; level < levels; level++) {
        device.queue.writeTexture(
          { texture, mipLevel: level, origin: [0, 0, layer] },
          data,
          { bytesPerRow: size * 4, rowsPerImage: size },
          [size, size, 1],
        );
        bytes += data.byteLength;
        if (level + 1 < levels) {
          data = downsampleSrgb(data, size);
          size >>= 1;
        }
      }
    });
    const records = atlasInfoRecords(atlas);
    const info = device.createBuffer({
      label: 'texture atlas records',
      size: records.byteLength,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
    });
    device.queue.writeBuffer(info, 0, records);
    return new GpuAtlas(texture, info, atlas.entries.length, bytes + records.byteLength);
  }

  /** Placeholder before any `textures` message: everything renders untextured. */
  static empty(device: GPUDevice): GpuAtlas {
    const texture = device.createTexture({
      label: 'texture atlas (empty)',
      size: [1, 1, 1],
      format: 'rgba8unorm-srgb',
      usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
    });
    const info = device.createBuffer({ label: 'texture atlas records (empty)', size: 32, usage: GPUBufferUsage.STORAGE });
    return new GpuAtlas(texture, info, 0, 0);
  }

  destroy(): void {
    this.texture.destroy();
    this.info.destroy();
  }
}
