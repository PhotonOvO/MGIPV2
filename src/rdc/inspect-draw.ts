import { createHash } from "node:crypto";

import type { RdcClient } from "./client.ts";
import type {
  RdcPassSummary,
  RdcPassTree,
  RdcShaderDataQuery,
  RdcShaderStage,
  RdcShaderSummary,
} from "./types.ts";

type JsonRecord = Record<string, unknown>;
type StageSlot = "vertex" | "pixel" | "compute";

export interface ShaderInspection {
  stage: RdcShaderStage;
  shader?: number;
  entry?: string;
  resources?: { readOnly?: number; readWrite?: number; constantBuffers?: number };
  reflection?: unknown;
  source?: {
    hasDebugInfo: boolean;
    files: string[];
    sha256: string;
    relevantLines: string[];
    truncated: boolean;
  };
}

export interface DrawInspection {
  eid: number;
  draw: {
    event?: number;
    type?: string;
    marker?: string;
    triangles?: number;
    instances?: number;
  };
  pass?: {
    name?: string;
    beginEid?: number;
    endEid?: number;
    colorTargets?: unknown;
    depthTarget?: unknown;
  };
  pipeline: {
    api?: string;
    graphicsPipeline?: number;
    computePipeline?: number;
    topology?: unknown;
    renderTargets?: unknown;
    depthTarget?: unknown;
    viewport?: unknown;
    scissor?: unknown;
    raster?: unknown;
    depth?: unknown;
    blend?: unknown;
  };
  bindings: {
    reflected: unknown;
    descriptors: unknown;
  };
  shaders: Partial<Record<StageSlot, ShaderInspection>>;
  constants: Partial<Record<StageSlot, unknown>>;
  warnings: string[];
}

export interface RdcInspectionClient {
  getDraw(eid: number, signal?: AbortSignal): Promise<unknown>;
  getPipeline(eid: number, signal?: AbortSignal): Promise<unknown>;
  getPipelineSection(eid: number, section: "viewport" | "scissor" | "blend" | "rasterizer" | "depth-stencil", signal?: AbortSignal): Promise<unknown>;
  getBindings(eid: number, query?: {}, signal?: AbortSignal): Promise<unknown>;
  getDescriptors(eid: number, signal?: AbortSignal): Promise<unknown>;
  getPasses(signal?: AbortSignal): Promise<RdcPassTree>;
  getPass(identifier: string | number, signal?: AbortSignal): Promise<unknown>;
  getShaders(eid: number, signal?: AbortSignal): Promise<RdcShaderSummary[]>;
  getShader(eid: number, stage: RdcShaderStage, query?: RdcShaderDataQuery, signal?: AbortSignal): Promise<unknown>;
  getShaderConstants(eid: number, stage: RdcShaderStage, signal?: AbortSignal): Promise<unknown>;
  getShaderSource(eid: number, stage: RdcShaderStage, signal?: AbortSignal): Promise<unknown>;
}

const STAGE_SLOTS: Partial<Record<RdcShaderStage, StageSlot>> = {
  vs: "vertex",
  ps: "pixel",
  cs: "compute",
};

function isRecord(value: unknown): value is JsonRecord {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function numberValue(record: JsonRecord, ...keys: string[]): number | undefined {
  for (const key of keys) {
    if (typeof record[key] === "number") return record[key];
  }
  return undefined;
}

function stringValue(record: JsonRecord, ...keys: string[]): string | undefined {
  for (const key of keys) {
    if (typeof record[key] === "string") return record[key];
  }
  return undefined;
}

export function compactRenderDocValue(
  value: unknown,
  options: { depth?: number; maxArray?: number; maxKeys?: number; maxString?: number } = {},
): unknown {
  const depth = options.depth ?? 0;
  const maxArray = options.maxArray ?? 64;
  const maxKeys = options.maxKeys ?? 48;
  const maxString = options.maxString ?? 2_048;
  if (depth >= 7) return "[depth limit]";
  if (typeof value === "string") {
    return value.length <= maxString ? value : `${value.slice(0, maxString)}...[truncated ${value.length - maxString} chars]`;
  }
  if (Array.isArray(value)) {
    const items = value.slice(0, maxArray).map((item) => compactRenderDocValue(item, {
      ...options,
      depth: depth + 1,
    }));
    if (value.length > maxArray) items.push(`[truncated ${value.length - maxArray} items]`);
    return items;
  }
  if (isRecord(value)) {
    const entries = Object.entries(value).slice(0, maxKeys);
    const result = Object.fromEntries(entries.map(([key, child]) => [
      key,
      compactRenderDocValue(child, { ...options, depth: depth + 1 }),
    ]));
    if (Object.keys(value).length > maxKeys) result._truncatedKeys = Object.keys(value).length - maxKeys;
    return result;
  }
  return value;
}

function findPass(tree: RdcPassTree, eid: number): RdcPassSummary | undefined {
  const passes = Array.isArray(tree.passes) ? tree.passes : [];
  return passes.find((pass) => {
    const begin = pass.begin_eid;
    const end = pass.end_eid;
    return typeof begin === "number" && typeof end === "number" && begin <= eid && eid <= end;
  });
}

function summarizeSource(value: unknown): ShaderInspection["source"] | undefined {
  if (!isRecord(value)) return undefined;
  const files = Array.isArray(value.files) ? value.files.filter(isRecord) : [];
  const namedSources = files.map((file) => ({
    filename: stringValue(file, "filename") ?? "<embedded>",
    source: stringValue(file, "source") ?? "",
  }));
  const fallback = stringValue(value, "source") ?? "";
  const combined = namedSources.length > 0
    ? namedSources.map((file) => `// ${file.filename}\n${file.source}`).join("\n")
    : fallback;
  const allLines = combined.split(/\r?\n/);
  const relevant = allLines.filter((line) =>
    /\b(cbuffer|constant|texture|sampler|sample|factor|color|return|output)\b/i.test(line),
  );
  const selected = (relevant.length > 0 ? relevant : allLines.filter((line) => line.trim().length > 0))
    .slice(0, 32)
    .map((line) => line.length <= 300 ? line : `${line.slice(0, 300)}...[truncated]`);
  return {
    hasDebugInfo: value.has_debug_info === true,
    files: namedSources.map((file) => file.filename).slice(0, 16),
    sha256: createHash("sha256").update(combined).digest("hex"),
    relevantLines: selected,
    truncated: relevant.length > 32 || allLines.length > 256,
  };
}

function summarizeConstants(value: unknown): unknown {
  if (!isRecord(value) || !Array.isArray(value.constants)) {
    return compactRenderDocValue(value, { maxArray: 96 });
  }
  const variables: unknown[] = [];
  for (const bufferValue of value.constants) {
    if (!isRecord(bufferValue) || !Array.isArray(bufferValue.variables)) continue;
    for (const variableValue of bufferValue.variables) {
      if (!isRecord(variableValue)) continue;
      variables.push({
        buffer: stringValue(bufferValue, "name"),
        bindPoint: numberValue(bufferValue, "bind_point", "bindPoint"),
        name: stringValue(variableValue, "name"),
        type: stringValue(variableValue, "type"),
        rows: numberValue(variableValue, "rows"),
        columns: numberValue(variableValue, "columns"),
        value: compactRenderDocValue(variableValue.value, {
          maxArray: 32,
          maxKeys: 16,
          maxString: 512,
        }),
      });
    }
  }
  return variables;
}

async function captureOptional<T>(
  label: string,
  warnings: string[],
  operation: () => Promise<T>,
): Promise<T | undefined> {
  try {
    return await operation();
  } catch (error) {
    warnings.push(`${label}: ${error instanceof Error ? error.message : String(error)}`);
    return undefined;
  }
}

export async function inspectDraw(
  client: RdcInspectionClient | RdcClient,
  eid: number,
  signal?: AbortSignal,
): Promise<DrawInspection> {
  if (!Number.isSafeInteger(eid) || eid < 0) throw new RangeError("eid must be a non-negative safe integer");
  const warnings: string[] = [];
  const [rawDraw, rawPipeline, rawBindings, rawDescriptors, passTree, shaderRows] = await Promise.all([
    client.getDraw(eid, signal),
    client.getPipeline(eid, signal),
    client.getBindings(eid, {}, signal),
    captureOptional("descriptors", warnings, () => client.getDescriptors(eid, signal)),
    captureOptional("passes", warnings, () => client.getPasses(signal)),
    client.getShaders(eid, signal),
  ]);

  const passSummary = passTree === undefined ? undefined : findPass(passTree, eid);
  const stageRows = shaderRows.filter((row): row is RdcShaderSummary & { stage: RdcShaderStage } =>
    typeof row.stage === "string" && row.stage in STAGE_SLOTS,
  );
  const [viewport, scissor, blend, raster, depth, passDetail, ...stageDetails] = await Promise.all([
    captureOptional("pipeline.viewport", warnings, () => client.getPipelineSection(eid, "viewport", signal)),
    captureOptional("pipeline.scissor", warnings, () => client.getPipelineSection(eid, "scissor", signal)),
    captureOptional("pipeline.blend", warnings, () => client.getPipelineSection(eid, "blend", signal)),
    captureOptional("pipeline.rasterizer", warnings, () => client.getPipelineSection(eid, "rasterizer", signal)),
    captureOptional("pipeline.depth-stencil", warnings, () => client.getPipelineSection(eid, "depth-stencil", signal)),
    passSummary?.name === undefined
      ? Promise.resolve(undefined)
      : captureOptional("pass detail", warnings, () => client.getPass(passSummary.name as string, signal)),
    ...stageRows.flatMap((row) => [
      captureOptional(`shader.${row.stage}.reflection`, warnings, () => client.getShader(eid, row.stage, undefined, signal)),
      captureOptional(`shader.${row.stage}.constants`, warnings, () => client.getShaderConstants(eid, row.stage, signal)),
      captureOptional(`shader.${row.stage}.source`, warnings, () => client.getShaderSource(eid, row.stage, signal)),
    ]),
  ]);

  const draw = isRecord(rawDraw) ? rawDraw : {};
  const pipeline = isRecord(rawPipeline) ? rawPipeline : {};
  const pass = isRecord(passDetail) ? passDetail : {};
  const shaders: DrawInspection["shaders"] = {};
  const constants: DrawInspection["constants"] = {};
  stageRows.forEach((row, index) => {
    const slot = STAGE_SLOTS[row.stage];
    if (slot === undefined) return;
    const reflection = stageDetails[index * 3];
    const stageConstants = stageDetails[index * 3 + 1];
    const source = stageDetails[index * 3 + 2];
    shaders[slot] = {
      stage: row.stage,
      shader: row.shader,
      entry: row.entry,
      resources: {
        readOnly: row.ro,
        readWrite: row.rw,
        constantBuffers: row.cbuffers,
      },
      reflection: compactRenderDocValue(reflection),
      source: summarizeSource(source),
    };
    constants[slot] = summarizeConstants(stageConstants);
  });

  const colorTargets = pass.color_targets;
  const depthTarget = pass.depth_target;
  return {
    eid,
    draw: {
      event: numberValue(draw, "Event", "eid"),
      type: stringValue(draw, "Type", "type"),
      marker: stringValue(draw, "Marker", "marker"),
      triangles: numberValue(draw, "Triangles", "triangles"),
      instances: numberValue(draw, "Instances", "instances"),
    },
    pass: passSummary === undefined ? undefined : {
      name: passSummary.name,
      beginEid: passSummary.begin_eid,
      endEid: passSummary.end_eid,
      colorTargets: compactRenderDocValue(colorTargets),
      depthTarget: compactRenderDocValue(depthTarget),
    },
    pipeline: {
      api: stringValue(pipeline, "api", "API"),
      graphicsPipeline: numberValue(pipeline, "graphics_pipeline"),
      computePipeline: numberValue(pipeline, "compute_pipeline"),
      topology: compactRenderDocValue(pipeline.topology),
      renderTargets: compactRenderDocValue(colorTargets),
      depthTarget: compactRenderDocValue(depthTarget),
      viewport: compactRenderDocValue(viewport),
      scissor: compactRenderDocValue(scissor),
      raster: compactRenderDocValue(raster),
      depth: compactRenderDocValue(depth),
      blend: compactRenderDocValue(blend),
    },
    bindings: {
      reflected: compactRenderDocValue(rawBindings),
      descriptors: compactRenderDocValue(rawDescriptors),
    },
    shaders,
    constants,
    warnings,
  };
}
