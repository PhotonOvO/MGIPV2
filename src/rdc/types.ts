export type RdcShaderStage = "vs" | "hs" | "ds" | "gs" | "ps" | "cs";

export type RdcPipelineSection =
  | "topology"
  | "viewport"
  | "scissor"
  | "blend"
  | "stencil"
  | "rasterizer"
  | "depth-stencil"
  | "msaa"
  | "vbuffers"
  | "ibuffer"
  | "samplers"
  | "push-constants"
  | "vinputs";

export interface RdcSessionState {
  capturePath?: string;
  openedAt?: string;
}

export interface RdcCaptureInfo extends Record<string, unknown> {
  Capture?: string;
  API?: string;
  Events?: number;
  "Draw Calls"?: string;
  Clears?: number;
  Copies?: number;
}

export interface RdcDraw extends Record<string, unknown> {
  eid?: number;
  type?: string;
  triangles?: number;
  instances?: number;
  pass?: string;
  marker?: string;
}

export interface RdcPassSummary extends Record<string, unknown> {
  name?: string;
  begin_eid?: number;
  end_eid?: number;
  draws?: number;
  dispatches?: number;
  triangles?: number;
}

export interface RdcPassTree extends Record<string, unknown> {
  passes?: RdcPassSummary[];
}

export interface RdcShaderSummary extends Record<string, unknown> {
  eid?: number;
  stage?: RdcShaderStage;
  shader?: number;
  entry?: string;
  ro?: number;
  rw?: number;
  cbuffers?: number;
}

export interface RdcOpenResult {
  capturePath: string;
  openedAt: string;
  info: RdcCaptureInfo;
}

export interface RdcPixelAssertionResult {
  passed: boolean;
  exitCode: number;
  details: unknown;
  stderr: string;
}

export interface RdcCaptureResult {
  outputPath: string;
  details: unknown;
}

export interface RdcDrawQuery {
  limit?: number;
  pass?: string;
  sort?: string;
}

// `draws` only lists raster draws, so compute dispatches cannot be enumerated
// through it. `events` is the CLI surface that can, and supports type/glob/EID
// range filters.
export interface RdcEventQuery {
  limit?: number;
  type?: string;
  filter?: string;
  range?: string;
}

export interface RdcShaderDataQuery {
  reflect?: boolean;
  constants?: boolean;
  source?: boolean;
  disasmTarget?: string;
  all?: boolean;
}

export interface RdcTextureStatsQuery {
  eid?: number;
  mip?: number;
  slice?: number;
  histogram?: boolean;
}

// Reads the resolved value of one texel of the color target bound at one EID.
// Unlike getPixelHistory this returns the final value instead of the per-draw
// modification list, but it is still bound to raster color targets: rdc-cli
// rejects a dispatch-only EID with "no color targets at eid". Use exportTexture
// for compute outputs.
export interface RdcPickPixelQuery {
  eid?: number;
  target?: number;
}

export interface RdcExportTextureOptions {
  mip?: number;
  output?: string;
}

export interface RdcConstantBufferQuery {
  stage?: RdcShaderStage;
  set?: number;
  binding?: number;
  raw?: boolean;
  output?: string;
}

export interface RdcBindingQuery {
  set?: number;
  binding?: number;
}

export interface RdcPixelQuery {
  eid?: number;
  target?: number;
  sample?: number;
}

export interface RdcDiffOptions {
  draws?: boolean;
  resources?: boolean;
  passes?: boolean;
  stats?: boolean;
  framebuffer?: boolean;
  pipelineMarker?: string;
  eid?: number;
  target?: number;
  threshold?: number;
  timeoutSeconds?: number;
  diffOutput?: string;
}

export interface RdcAssertPixelOptions {
  eid: number;
  x: number;
  y: number;
  expected: readonly [number, number, number, number];
  tolerance?: number;
  target?: number;
}

export interface RdcCaptureExecutableOptions {
  executable: string;
  appArgs?: readonly string[];
  output: string;
  frame?: number;
  timeoutSeconds?: number;
  api?: string;
  waitForExit?: boolean;
  apiValidation?: boolean;
  refAllResources?: boolean;
}
