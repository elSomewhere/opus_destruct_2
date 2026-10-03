/** Tool overlays in the game's render pass. Positions and linear RGBA, seven floats per vertex. */
export class DebugRenderer {
  private buffer: GPUBuffer | null = null;
  private count = 0;
  private depthCount = 0;
  private pipeline: GPURenderPipeline;
  private depthPipeline: GPURenderPipeline;
  private device: GPUDevice;
  constructor(device: GPUDevice, layout: GPUBindGroupLayout, frame: string, format: GPUTextureFormat, samples: number, depthFormat: GPUTextureFormat) {
    this.device = device;
    const module = device.createShaderModule({label:'tool lines', code:frame + `
struct DebugOut { @builtin(position) p: vec4f, @location(0) color: vec4f };
@vertex fn vsDebug(@location(0) p: vec3f, @location(1) color: vec4f) -> DebugOut {
  var o: DebugOut; o.p = frame.viewProj * vec4f(p, 1); o.color = color; return o;
}
@fragment fn fsDebug(i: DebugOut) -> @location(0) vec4f { return i.color; }
`});
    const blend: GPUBlendState = {color:{srcFactor:'src-alpha',dstFactor:'one-minus-src-alpha'},alpha:{srcFactor:'one',dstFactor:'one-minus-src-alpha'}};
    const descriptor: GPURenderPipelineDescriptor = {label:'tool lines',layout:device.createPipelineLayout({bindGroupLayouts:[layout]}),
      vertex:{module,entryPoint:'vsDebug',buffers:[{arrayStride:28,attributes:[{shaderLocation:0,offset:0,format:'float32x3'},{shaderLocation:1,offset:12,format:'float32x4'}]}]},
      fragment:{module,entryPoint:'fsDebug',targets:[{format,blend}]},primitive:{topology:'line-list'},
      depthStencil:{format:depthFormat,depthWriteEnabled:false,depthCompare:'always'},multisample:{count:samples}};
    this.pipeline = device.createRenderPipeline(descriptor);
    this.depthPipeline = device.createRenderPipeline({...descriptor,label:'depth tested tool lines',
      depthStencil:{format:depthFormat,depthWriteEnabled:false,depthCompare:'greater-equal'}});
  }
  upload(lines?: Float32Array, depthLines?: Float32Array): void {
    this.depthCount = depthLines ? Math.floor(depthLines.length / 14) * 2 : 0;
    this.count = (lines ? Math.floor(lines.length / 14) * 2 : 0) + this.depthCount;
    if (!this.count) return;
    const bytes = this.count * 28;
    if (!this.buffer || this.buffer.size < bytes) {
      this.buffer?.destroy();
      this.buffer = this.device.createBuffer({label:'tool lines',size:Math.ceil(bytes / 256) * 256,usage:GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST});
    }
    if (depthLines && this.depthCount) this.device.queue.writeBuffer(this.buffer,0,depthLines as Float32Array<ArrayBuffer>,0,this.depthCount * 7);
    if (lines && this.count > this.depthCount) this.device.queue.writeBuffer(this.buffer,this.depthCount * 28,lines as Float32Array<ArrayBuffer>,0,(this.count - this.depthCount) * 7);
  }
  draw(pass: GPURenderPassEncoder): void {
    if (!this.count || !this.buffer) return;
    pass.setVertexBuffer(0,this.buffer);
    if (this.depthCount) { pass.setPipeline(this.depthPipeline);pass.draw(this.depthCount); }
    if (this.count > this.depthCount) { pass.setPipeline(this.pipeline);pass.draw(this.count - this.depthCount,1,this.depthCount); }
  }
  dispose(): void { this.buffer?.destroy();this.buffer=null; }
}
