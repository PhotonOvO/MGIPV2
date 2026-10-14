/**
 * CPU-side profiler types for Superluminal .slp traces.
 *
 * These mirror the GPU-side src/rdc/types.ts so the two profilers present a
 * consistent typed surface to the agent. slp.py is stateless (every subcommand
 * re-reads the trace), so unlike RdcSessionState there is no open/close
 * lifecycle: each tool call carries its own trace path.
 */

/** Names of the typed tools this subsystem registers. */
export const SLP_TOOL_NAMES = [
  "slp_traces",
  "slp_info",
  "slp_hotspots",
  "slp_callers",
  "slp_callees",
  "slp_timeline",
  "slp_stacks",
  "slp_compare",
  "slp_stack",
  "slp_source",
] as const;

export type SlpToolName = (typeof SLP_TOOL_NAMES)[number];

/** How hotspots ranks its rows. */
export type SlpHotspotMetric = "self" | "total" | "module" | "thread";

/** Shared filters accepted by every events.dat-based analysis command. */
export interface SlpWindowQuery {
  /** Internal ThreadID, OS thread id, or thread name (e.g. "Main"). All threads when omitted. */
  thread?: string;
  /** Window start in ms. */
  start?: number;
  /** Window end in ms. */
  end?: number;
  /** Drop samples whose stack contains a VkLayer_* validation frame. */
  excludeValidation?: boolean;
}

export interface SlpHotspotsQuery extends SlpWindowQuery {
  by?: SlpHotspotMetric;
  /** Rows to return (1-100). */
  top?: number;
  /** Hide rows below this percent. */
  minPct?: number;
  /** Only functions in modules whose name contains this substring. */
  moduleFilter?: string;
}

export interface SlpRelationQuery extends SlpWindowQuery {
  /** Function name pattern: case-insensitive substring or glob. */
  function: string;
  top?: number;
}

export interface SlpTimelineQuery extends SlpWindowQuery {
  /** Fixed bucket size in ms. Mutually exclusive with frameFunc. */
  bucket?: number;
  /** Function whose False->True stack transitions mark frame boundaries. */
  frameFunc?: string;
  top?: number;
  topFuncs?: number;
}

export interface SlpStacksQuery extends SlpWindowQuery {
  top?: number;
  minSamples?: number;
  /** Emit Brendan Gregg folded stacks for flamegraph.pl instead of a tree. */
  folded?: boolean;
}

export interface SlpCompareQuery {
  thread?: string;
  excludeValidation?: boolean;
  minAbsMs?: number;
  top?: number;
}

/** One ranked hotspot row as emitted by `slp.py hotspots --json`. */
export interface SlpHotspotRow {
  function: string | null;
  module: string | null;
  self_ms: number;
  self_pct: number;
  total_ms: number;
  total_pct: number;
  self_samples: number;
  /** True when no sample attributed to this row came from an unresolved module. */
  resolved: boolean;
  thread?: number;
  thread_name?: string;
}

/** Frame-time distribution produced by `slp.py timeline --frame-func`. */
export interface SlpFrameStats {
  frame_count: number;
  implied_fps: number;
  cpu_ms: { p50: number; p95: number; p99: number; max: number; mean: number };
  wall_ms: { p50: number; p95: number; p99: number; max: number; mean: number };
  pre_frame_samples: number;
}

/** One trace listed by `slp_traces`. */
export interface SlpTraceEntry {
  path: string;
  name: string;
  sizeBytes: number;
  modifiedMs: number;
  kind: "slp" | "folder";
}
