/**
 * WebGPU device setup with explicit, user-presentable failure modes.
 */

export class WebGpuUnavailableError extends Error {
  override readonly name = 'WebGpuUnavailableError';
}

export interface GpuContext {
  device: GPUDevice;
  context: GPUCanvasContext;
  /** Canvas storage format (e.g. bgra8unorm). */
  canvasFormat: GPUTextureFormat;
  /** sRGB view of the canvas format; all pipelines render to it (linear shading). */
  renderFormat: GPUTextureFormat;
  /** Human-readable adapter description for the HUD. */
  description: string;
}

function srgbVariant(format: GPUTextureFormat): GPUTextureFormat {
  if (format === 'bgra8unorm') return 'bgra8unorm-srgb';
  if (format === 'rgba8unorm') return 'rgba8unorm-srgb';
  return format;
}

export async function initWebGpu(canvas: HTMLCanvasElement, onLost: (reason: string) => void): Promise<GpuContext> {
  if (!('gpu' in navigator) || !navigator.gpu) {
    throw new WebGpuUnavailableError(
      'WebGPU is not available in this browser. Use a current Chrome/Edge, Safari 26+, or Firefox 141+ ' +
        '(Windows / Apple Silicon), and make sure hardware acceleration is enabled.',
    );
  }
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) {
    throw new WebGpuUnavailableError('WebGPU is supported but no GPU adapter is available (blocklisted GPU or disabled acceleration).');
  }
  const device = await adapter.requestDevice({ label: 'structvox' });
  void device.lost.then((info) => {
    // 'destroyed' is our own teardown; anything else is a real loss.
    if (info.reason !== 'destroyed') onLost(info.message || String(info.reason));
  });
  device.addEventListener('uncapturederror', (ev) => {
    console.error('[webgpu] uncaptured error:', (ev as GPUUncapturedErrorEvent).error.message);
  });

  const context = canvas.getContext('webgpu');
  if (!context) throw new WebGpuUnavailableError('Could not create a WebGPU canvas context.');
  const canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  const renderFormat = srgbVariant(canvasFormat);
  context.configure({
    device,
    format: canvasFormat,
    viewFormats: renderFormat === canvasFormat ? [] : [renderFormat],
    alphaMode: 'opaque',
  });

  const info = adapter.info;
  const description = [info.vendor, info.architecture, info.description].filter((s) => s && s.length > 0).join(' ') || 'WebGPU adapter';
  return { device, context, canvasFormat, renderFormat, description };
}
