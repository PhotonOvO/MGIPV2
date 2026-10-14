import { readdir, stat } from "node:fs/promises";
import { basename, isAbsolute, resolve } from "node:path";

import {
  createSlpProcessRunner,
  DEFAULT_SLP_ANALYSIS_TIMEOUT_MS,
  SlpProcessError,
  type SlpProcessRunner,
} from "./process.ts";
import type {
  SlpCompareQuery,
  SlpHotspotsQuery,
  SlpHotspotRow,
  SlpRelationQuery,
  SlpStacksQuery,
  SlpTimelineQuery,
  SlpTraceEntry,
  SlpWindowQuery,
} from "./types.ts";

export interface SlpClientOptions {
  /** Path to slp.py. Default: SLP_PY env. */
  scriptPath?: string;
  /** Python interpreter. Default: SLP_PYTHON env or "python3". */
  executable?: string;
  /** Directory scanned by listTraces when a query is not an explicit path. Default: SLP_CAPTURES env or cwd. */
  capturesDir?: string;
  cwd?: string;
  runner?: SlpProcessRunner;
  analysisTimeoutMs?: number;
}

const DEFAULT_SCRIPT_PATH = process.env.SLP_PY ?? "";
const DEFAULT_CAPTURES_DIR = process.env.SLP_CAPTURES ?? "";

function positiveInteger(value: number, name: string): string {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${name} must be a non-negative safe integer`);
  }
  return String(value);
}

function clampTop(value: number | undefined, fallback: number, max = 100): number {
  if (value === undefined) return fallback;
  return Math.max(1, Math.min(Math.trunc(value), max));
}

/** Shared flag builder for the window/thread filters every analysis command takes. */
function windowArgs(query: SlpWindowQuery): string[] {
  const args: string[] = [];
  if (query.thread !== undefined) args.push("--thread", String(query.thread));
  if (query.start !== undefined) args.push("--start", String(query.start));
  if (query.end !== undefined) args.push("--end", String(query.end));
  if (query.excludeValidation) args.push("--exclude-validation");
  // Always JSON + quiet: the client parses JSON and must not interleave progress.
  args.push("--json", "--quiet");
  return args;
}

/**
 * Typed client over slp.py.
 *
 * slp.py is stateless: each method shells out to one subcommand with --json and
 * parses the result. There is no open/close lifecycle (contrast RdcClient), so
 * every method takes an explicit trace path.
 *
 * Errors surface as SlpProcessError with a `kind` the tool layer maps to a
 * model-facing message: "env-missing" tells the model to fix the environment
 * rather than retry, "exit" carries slp.py's own diagnostic.
 */
export class SlpClient {
  private readonly runner: SlpProcessRunner;
  private readonly capturesDir: string;
  private readonly cwd: string;
  private readonly analysisTimeoutMs: number;

  constructor(options: SlpClientOptions = {}) {
    const scriptPath = options.scriptPath ?? DEFAULT_SCRIPT_PATH;
    if (!scriptPath) {
      throw new Error("slp.py path is not configured. Set SLP_PY or pass scriptPath.");
    }
    this.cwd = resolve(options.cwd ?? process.cwd());
    this.capturesDir = options.capturesDir ?? DEFAULT_CAPTURES_DIR;
    this.analysisTimeoutMs = options.analysisTimeoutMs ?? DEFAULT_SLP_ANALYSIS_TIMEOUT_MS;
    this.runner = options.runner ?? createSlpProcessRunner({
      scriptPath,
      executable: options.executable,
      cwd: this.cwd,
      defaultTimeoutMs: this.analysisTimeoutMs,
    });
  }

  /** Resolve a trace path: absolute/relative paths pass through; a bare name is looked up under capturesDir. */
  private resolveTracePath(trace: string): string {
    if (trace.trim().length === 0) throw new Error("trace path cannot be empty");
    return isAbsolute(trace) ? trace : resolve(this.cwd, trace);
  }

  /**
   * List available traces under capturesDir, newest first. Returns [] when no
   * captures directory is configured. A trace is a *.slp file or a directory
   * holding a *.session member.
   */
  async listTraces(signal?: AbortSignal): Promise<SlpTraceEntry[]> {
    if (!this.capturesDir) return [];
    let entries: string[];
    try {
      entries = await readdir(this.capturesDir);
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code === "ENOENT") return [];
      throw error;
    }
    const traces: SlpTraceEntry[] = [];
    for (const name of entries) {
      const full = resolve(this.capturesDir, name);
      const meta = await stat(full).catch(() => undefined);
      if (!meta) continue;
      if (meta.isFile() && name.toLowerCase().endsWith(".slp")) {
        traces.push({ path: full, name, sizeBytes: meta.size, modifiedMs: meta.mtimeMs, kind: "slp" });
      } else if (meta.isDirectory() && !name.toLowerCase().endsWith(".etl")) {
        // A GUI session folder contains <name>.session; check cheaply.
        const children = await readdir(full).catch(() => [] as string[]);
        if (children.some((child) => child.toLowerCase().endsWith(".session"))) {
          traces.push({ path: full, name, sizeBytes: 0, modifiedMs: meta.mtimeMs, kind: "folder" });
        }
      }
    }
    traces.sort((a, b) => b.modifiedMs - a.modifiedMs);
    void signal;
    return traces;
  }

  /** `stats` + `threads`, returned as raw text (both are small and already summary-shaped). */
  async info(trace: string, signal?: AbortSignal): Promise<{ stats: string; threads: string }> {
    const path = this.resolveTracePath(trace);
    const timeout = { signal, timeoutMs: this.analysisTimeoutMs };
    const [stats, threads] = await Promise.all([
      this.runner.runText(["stats", path], timeout),
      this.runner.runText(["threads", path], timeout),
    ]);
    return { stats: stats.data, threads: threads.data };
  }

  async hotspots(trace: string, query: SlpHotspotsQuery = {}, signal?: AbortSignal) {
    const path = this.resolveTracePath(trace);
    const args = ["hotspots", path, ...windowArgs(query), "--by", query.by ?? "self",
      "--top", String(clampTop(query.top, 20))];
    if (query.minPct !== undefined) args.push("--min-pct", String(query.minPct));
    if (query.moduleFilter) args.push("--module-filter", query.moduleFilter);
    const result = await this.runner.runJson<SlpHotspotsResult>(args, { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }

  async relations(
    trace: string,
    direction: "callers" | "callees",
    query: SlpRelationQuery,
    signal?: AbortSignal,
  ) {
    if (!query.function || query.function.trim().length === 0) {
      throw new Error("function pattern cannot be empty");
    }
    const path = this.resolveTracePath(trace);
    const args = [direction, path, query.function, ...windowArgs(query),
      "--top", String(clampTop(query.top, 15))];
    const result = await this.runner.runJson<SlpRelationResult>(args, { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }

  async timeline(trace: string, query: SlpTimelineQuery = {}, signal?: AbortSignal) {
    if (query.frameFunc !== undefined && query.bucket !== undefined) {
      throw new Error("frameFunc and bucket are mutually exclusive");
    }
    const path = this.resolveTracePath(trace);
    const args = ["timeline", path, ...windowArgs(query)];
    if (query.frameFunc !== undefined) args.push("--frame-func", query.frameFunc);
    else if (query.bucket !== undefined) args.push("--bucket", String(query.bucket));
    args.push("--top", String(clampTop(query.top, 20)), "--top-funcs", String(clampTop(query.topFuncs, 5, 10)));
    const result = await this.runner.runJson<SlpTimelineResult>(args, { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }

  async stacks(trace: string, query: SlpStacksQuery = {}, signal?: AbortSignal) {
    const path = this.resolveTracePath(trace);
    const args = ["stacks", path, ...windowArgs(query),
      "--top", String(clampTop(query.top, 20)), "--min-samples", positiveInteger(query.minSamples ?? 1, "minSamples")];
    const result = await this.runner.runJson<SlpStacksResult>(args, { signal, timeoutMs: this.analysisTimeoutMs });
    // Folded output is a text blob for flamegraph.pl, not for the model; strip it
    // from what the tool returns unless explicitly requested.
    if (!query.folded && result.data.folded) delete result.data.folded;
    return result.data;
  }

  async compare(baseline: string, candidate: string, query: SlpCompareQuery = {}, signal?: AbortSignal) {
    const basePath = this.resolveTracePath(baseline);
    const candPath = this.resolveTracePath(candidate);
    const args = ["compare", basePath, candPath];
    if (query.thread !== undefined) args.push("--thread", String(query.thread));
    if (query.excludeValidation) args.push("--exclude-validation");
    if (query.minAbsMs !== undefined) args.push("--min-abs-ms", String(query.minAbsMs));
    args.push("--top", String(clampTop(query.top, 15)), "--json", "--quiet");
    const result = await this.runner.runJson<SlpCompareResult>(args, { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }

  /** Symbolize specific callstack node ids (text output; ids are few and bounded). */
  async stack(trace: string, ids: readonly number[], signal?: AbortSignal): Promise<string> {
    if (ids.length === 0) throw new Error("at least one callstack id is required");
    if (ids.length > 50) throw new Error("at most 50 callstack ids per call");
    const path = this.resolveTracePath(trace);
    const result = await this.runner.runText(["callstack", path, ...ids.map((id) => positiveInteger(id, "id"))],
      { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }

  /** Map a function to source file + hot lines via functioncache (text output, already bounded by --limit). */
  async source(trace: string, func: string, limit = 5, hotLines = 5, signal?: AbortSignal): Promise<string> {
    if (!func || func.trim().length === 0) throw new Error("function pattern cannot be empty");
    const path = this.resolveTracePath(trace);
    const result = await this.runner.runText(
      ["source", path, func, "--limit", positiveInteger(limit, "limit"), "--hot-lines", positiveInteger(hotLines, "hotLines")],
      { signal, timeoutMs: this.analysisTimeoutMs });
    return result.data;
  }
}

// ---------------------------------------------------------------- result types
// Kept structural (not exhaustively typed) because slp.py's JSON carries
// analysis-specific aggregates; the tools pass these through to boundedToolResult.

export interface SlpHotspotsResult {
  trace: string;
  by: string;
  thread: number | null;
  thread_name?: string | null;
  window_ms: [number | null, number | null];
  sample_interval_ms: number;
  samples_used: number;
  samples_total: number;
  skipped: Record<string, number>;
  unresolved_frame_ratio: number;
  totals: { distinct: number; self_ms: number; self_pct: number; total_cpu_ms: number };
  row_limits?: Record<string, { shown: number; total: number; truncated: boolean }>;
  rows: SlpHotspotRow[];
}

export interface SlpRelationResult {
  trace: string;
  pattern: string;
  direction: string;
  matched_samples: number;
  matched_ms: number;
  self_ms: number;
  samples_used: number;
  targets: Array<{ function: string; samples: number; ms: number; pct: number }>;
  rows: Array<{ neighbor: string; samples: number; ms: number; pct: number }>;
  row_limits?: Record<string, { shown: number; total: number; truncated: boolean }>;
}

export interface SlpTimelineResult {
  trace: string;
  mode: "frame" | "bucket";
  frame_func?: string;
  bucket_ms?: number;
  thread: number | null;
  samples_used: number;
  stats: Record<string, unknown>;
  slowest_frames?: Array<Record<string, unknown>>;
  rows: Array<Record<string, unknown>>;
  row_limits?: Record<string, { shown: number; total: number; truncated: boolean }>;
}

export interface SlpStacksResult {
  trace: string;
  distinct_stacks: number;
  samples_used: number;
  coverage_top_pct: number;
  rows: Array<Record<string, unknown>>;
  folded?: string[];
}

export interface SlpCompareResult {
  baseline: { label: string; samples_used: number; duration_ms: number; total_cpu_ms: number };
  candidate: { label: string; samples_used: number; duration_ms: number; total_cpu_ms: number };
  thread: number | null;
  improved: Array<Record<string, unknown>>;
  regressed: Array<Record<string, unknown>>;
  new: Array<Record<string, unknown>>;
  gone: Array<Record<string, unknown>>;
  row_limits?: Record<string, { shown: number; total: number; truncated: boolean }>;
}

/** Re-export for the tool layer to classify errors. */
export { SlpProcessError };
export function isEnvMissingError(error: unknown): boolean {
  return error instanceof SlpProcessError && error.kind === "env-missing";
}

/** Basename helper reused by the tools for labels. */
export function traceLabel(path: string): string {
  return basename(path);
}
