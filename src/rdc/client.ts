import { rename, stat } from "node:fs/promises";
import { resolve } from "node:path";

import {
  RdcProcessError,
  createRdcProcessRunner,
  type RdcProcessRunner,
} from "./process.ts";
import type {
  RdcAssertPixelOptions,
  RdcBindingQuery,
  RdcCaptureExecutableOptions,
  RdcCaptureInfo,
  RdcCaptureResult,
  RdcConstantBufferQuery,
  RdcDiffOptions,
  RdcDraw,
  RdcDrawQuery,
  RdcEventQuery,
  RdcExportTextureOptions,
  RdcOpenResult,
  RdcPassTree,
  RdcPipelineSection,
  RdcPixelAssertionResult,
  RdcPixelQuery,
  RdcPickPixelQuery,
  RdcSessionState,
  RdcShaderDataQuery,
  RdcShaderStage,
  RdcShaderSummary,
  RdcTextureStatsQuery,
} from "./types.ts";

export interface RdcClientOptions {
  sessionName?: string;
  runner?: RdcProcessRunner;
  cwd?: string;
  replayTimeoutMs?: number;
  captureTimeoutMs?: number;
}

const DEFAULT_REPLAY_TIMEOUT_MS = 120_000;
const DEFAULT_CAPTURE_TIMEOUT_MS = 180_000;

function positiveInteger(value: number, name: string): string {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${name} must be a non-negative safe integer`);
  }
  return String(value);
}

function positiveFinite(value: number, name: string): string {
  if (!Number.isFinite(value) || value < 0) {
    throw new RangeError(`${name} must be a non-negative finite number`);
  }
  return String(value);
}

function parseJsonOrText(stdout: string, stderr: string): unknown {
  const text = stdout.trim();
  if (text.length === 0) return { message: stderr.trim() };
  try {
    return JSON.parse(text) as unknown;
  } catch {
    return { message: text };
  }
}

function capturePathFromResult(value: unknown): string | undefined {
  if (typeof value !== "object" || value === null || !("path" in value)) return undefined;
  const path = (value as { path?: unknown }).path;
  return typeof path === "string" && path.trim().length > 0 ? path : undefined;
}

export class RdcClient {
  readonly sessionName: string;
  private readonly runner: RdcProcessRunner;
  private readonly cwd: string;
  private readonly replayTimeoutMs: number;
  private readonly captureTimeoutMs: number;
  private state: RdcSessionState = {};

  constructor(options: RdcClientOptions = {}) {
    this.sessionName = options.sessionName ?? "pi-renderdoc";
    if (!/^[A-Za-z0-9_.-]+$/.test(this.sessionName)) {
      throw new Error("rdc session name may contain only letters, digits, dot, underscore, and dash");
    }
    this.cwd = resolve(options.cwd ?? process.cwd());
    this.runner = options.runner ?? createRdcProcessRunner({ cwd: this.cwd });
    this.replayTimeoutMs = options.replayTimeoutMs ?? DEFAULT_REPLAY_TIMEOUT_MS;
    this.captureTimeoutMs = options.captureTimeoutMs ?? DEFAULT_CAPTURE_TIMEOUT_MS;
  }

  getSessionState(): Readonly<RdcSessionState> {
    return { ...this.state };
  }

  private sessionArgs(args: readonly string[]): string[] {
    return ["--session", this.sessionName, ...args];
  }

  private requireOpen(): void {
    if (this.state.capturePath === undefined) {
      throw new Error("No RenderDoc capture is open. Call openCapture() first.");
    }
  }

  private resolvePath(path: string): string {
    if (path.trim().length === 0) throw new Error("Capture path cannot be empty");
    return resolve(this.cwd, path.replace(/^@/, ""));
  }

  async openCapture(path: string, signal?: AbortSignal): Promise<RdcOpenResult> {
    if (this.state.capturePath !== undefined) {
      throw new Error(`A capture is already open: ${this.state.capturePath}`);
    }
    const capturePath = this.resolvePath(path);
    const metadata = await stat(capturePath);
    if (!metadata.isFile()) throw new Error(`Capture is not a file: ${capturePath}`);

    await this.runner.runText(this.sessionArgs(["open", capturePath]), {
      signal,
      timeoutMs: this.replayTimeoutMs,
    });
    const openedAt = new Date().toISOString();
    this.state = { capturePath, openedAt };
    try {
      const info = await this.getInfo(signal);
      return { capturePath, openedAt, info };
    } catch (error) {
      await this.closeCapture(undefined, true);
      throw error;
    }
  }

  async closeCapture(signal?: AbortSignal, ignoreMissing = false): Promise<void> {
    try {
      await this.runner.runText(this.sessionArgs(["close", "--shutdown"]), {
        signal,
        timeoutMs: this.replayTimeoutMs,
      });
    } catch (error) {
      if (!ignoreMissing || !(error instanceof RdcProcessError)) throw error;
    } finally {
      this.state = {};
    }
  }

  async getInfo(signal?: AbortSignal): Promise<RdcCaptureInfo> {
    this.requireOpen();
    const result = await this.runner.runJson<RdcCaptureInfo>(
      this.sessionArgs(["info", "--json"]),
      { signal, timeoutMs: this.replayTimeoutMs },
    );
    return result.data;
  }

  async getDraws(query: RdcDrawQuery = {}, signal?: AbortSignal): Promise<RdcDraw[]> {
    this.requireOpen();
    const args = ["draws"];
    if (query.pass !== undefined) args.push("--pass", query.pass);
    if (query.sort !== undefined) args.push("--sort", query.sort);
    if (query.limit !== undefined) args.push("--limit", positiveInteger(query.limit, "limit"));
    args.push("--json");
    const result = await this.runner.runJson<RdcDraw[]>(this.sessionArgs(args), {
      signal,
      timeoutMs: this.replayTimeoutMs,
    });
    return result.data;
  }

  async getDraw(eid: number, signal?: AbortSignal): Promise<unknown> {
    this.requireOpen();
    return this.query(["draw", positiveInteger(eid, "eid"), "--json"], signal);
  }

  async getPipeline(eid: number, signal?: AbortSignal): Promise<unknown> {
    this.requireOpen();
    return this.query(["pipeline", positiveInteger(eid, "eid"), "--json"], signal);
  }

  async getPipelineSection(
    eid: number,
    section: RdcPipelineSection,
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    return this.query(["pipeline", positiveInteger(eid, "eid"), section, "--json"], signal);
  }

  async getBindings(
    eid: number,
    query: RdcBindingQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["bindings", positiveInteger(eid, "eid")];
    if (query.set !== undefined) args.push("--set", positiveInteger(query.set, "set"));
    if (query.binding !== undefined) args.push("--binding", positiveInteger(query.binding, "binding"));
    args.push("--json");
    return this.query(args, signal);
  }

  async getDescriptors(eid: number, signal?: AbortSignal): Promise<unknown> {
    this.requireOpen();
    return this.query(["descriptors", positiveInteger(eid, "eid"), "--json"], signal);
  }

  async getPasses(signal?: AbortSignal): Promise<RdcPassTree> {
    this.requireOpen();
    return this.query(["passes", "--json"], signal) as Promise<RdcPassTree>;
  }

  async getPass(identifier: string | number, signal?: AbortSignal): Promise<unknown> {
    this.requireOpen();
    if (typeof identifier === "string" && identifier.trim().length === 0) {
      throw new Error("pass identifier cannot be empty");
    }
    const value = typeof identifier === "number"
      ? positiveInteger(identifier, "pass index")
      : identifier;
    return this.query(["pass", value, "--json"], signal);
  }

  async getShader(
    eid: number,
    stage: RdcShaderStage,
    query: RdcShaderDataQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["shader", positiveInteger(eid, "eid"), stage];
    if (query.constants === true) args.push("--constants");
    if (query.source === true) args.push("--source");
    if (query.disasmTarget !== undefined) {
      if (query.disasmTarget.trim().length === 0) {
        throw new Error("shader disassembly target cannot be empty");
      }
      args.push("--target", query.disasmTarget);
    }
    // Reflection stays on by default so existing callers keep their payload.
    if (query.reflect !== false) args.push("--reflect");
    args.push("--json");
    return this.query(args, signal);
  }

  async getShaders(eid: number, signal?: AbortSignal): Promise<RdcShaderSummary[]> {
    this.requireOpen();
    return this.query(
      ["shader", positiveInteger(eid, "eid"), "--all", "--json"],
      signal,
    ) as Promise<RdcShaderSummary[]>;
  }

  async getShaderConstants(
    eid: number,
    stage: RdcShaderStage,
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    // The VFS path /draws/<eid>/shader/<stage>/constants does not exist in this
    // CLI, so `cat` always failed and every inspect_draw reported the constants
    // as not found. `shader --constants` is the supported surface.
    return this.getShader(eid, stage, { reflect: false, constants: true }, signal);
  }

  async getShaderSource(
    eid: number,
    stage: RdcShaderStage,
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    // See getShaderConstants: use the supported `shader --source` surface
    // instead of a nonexistent VFS path.
    return this.getShader(eid, stage, { reflect: false, source: true }, signal);
  }

  async getEvents(query: RdcEventQuery = {}, signal?: AbortSignal): Promise<unknown> {
    this.requireOpen();
    const args = ["events"];
    if (query.type !== undefined) {
      if (query.type.trim().length === 0) throw new Error("event type cannot be empty");
      args.push("--type", query.type);
    }
    if (query.filter !== undefined) {
      if (query.filter.trim().length === 0) throw new Error("event filter cannot be empty");
      args.push("--filter", query.filter);
    }
    if (query.range !== undefined) {
      if (!/^\d+:\d+$/.test(query.range.trim())) {
        throw new Error("event range must use the N:M form");
      }
      args.push("--range", query.range.trim());
    }
    if (query.limit !== undefined) args.push("--limit", positiveInteger(query.limit, "limit"));
    args.push("--json");
    return this.query(args, signal);
  }

  async getPickPixel(
    x: number,
    y: number,
    query: RdcPickPixelQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["pick-pixel", positiveInteger(x, "x"), positiveInteger(y, "y")];
    if (query.eid !== undefined) args.push(positiveInteger(query.eid, "eid"));
    if (query.target !== undefined) {
      args.push("--target", positiveInteger(query.target, "target"));
    }
    args.push("--json");
    return this.query(args, signal);
  }

  // Exports one texture resource as PNG. This is the only way to obtain the
  // pixels of a compute output (for example the FlaxGI debug atlas), because
  // pick_pixel and pixel_history both require a raster color target at the
  // EID and therefore reject dispatch-only events.
  //
  // Registered as the top-level command `texture` (rdc-cli 0.6.1), not as an
  // `export` subgroup.
  async exportTexture(
    resourceId: number,
    options: RdcExportTextureOptions = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["texture", positiveInteger(resourceId, "resourceId")];
    if (options.mip !== undefined) args.push("--mip", positiveInteger(options.mip, "mip"));
    if (options.output !== undefined) {
      if (options.output.trim().length === 0) throw new Error("export output path cannot be empty");
      args.push("-o", this.resolvePath(options.output));
      // PNG bytes must not be parsed as JSON; run through the text path.
      const result = await this.runner.runText(this.sessionArgs(args), {
        signal,
        timeoutMs: this.replayTimeoutMs,
      });
      return { path: this.resolvePath(options.output), details: result.data };
    }
    args.push("--raw", "--json");
    return this.query(args, signal);
  }

  async getTextureStats(
    resourceId: number,
    query: RdcTextureStatsQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["tex-stats", positiveInteger(resourceId, "resourceId")];
    if (query.eid !== undefined) args.push(positiveInteger(query.eid, "eid"));
    if (query.mip !== undefined) args.push("--mip", positiveInteger(query.mip, "mip"));
    if (query.slice !== undefined) args.push("--slice", positiveInteger(query.slice, "slice"));
    if (query.histogram === true) args.push("--histogram");
    args.push("--json");
    return this.query(args, signal);
  }

  async getConstantBuffer(
    eid: number,
    query: RdcConstantBufferQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    const args = ["cbuffer", positiveInteger(eid, "eid")];
    if (query.stage !== undefined) args.push("--stage", query.stage);
    if (query.set !== undefined) args.push("--set", positiveInteger(query.set, "set"));
    if (query.binding !== undefined) {
      args.push("--binding", positiveInteger(query.binding, "binding"));
    }
    if (query.raw === true) args.push("--raw");
    if (query.output !== undefined) {
      if (query.output.trim().length === 0) throw new Error("cbuffer output path cannot be empty");
      args.push("-o", this.resolvePath(query.output));
    }
    args.push("--json");
    return this.query(args, signal);
  }

  async getPixelHistory(
    x: number,
    y: number,
    query: RdcPixelQuery = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    this.requireOpen();
    let eid = query.eid;
    if (eid === undefined) {
      const passTree = await this.getPasses(signal);
      eid = (passTree.passes ?? []).reduce<number | undefined>((latest, pass) => {
        const end = pass.end_eid;
        return typeof end === "number" && (latest === undefined || end > latest) ? end : latest;
      }, undefined);
      if (eid === undefined) {
        const draws = await this.getDraws({}, signal);
        eid = draws.reduce<number | undefined>((latest, draw) => {
          const event = draw.eid;
          return typeof event === "number" && (latest === undefined || event > latest) ? event : latest;
        }, undefined);
      }
      if (eid === undefined) {
        throw new Error("Cannot resolve a frame event for pixel history; the capture has no passes or draws");
      }
    }
    const args = [
      "pixel",
      positiveInteger(x, "x"),
      positiveInteger(y, "y"),
      positiveInteger(eid, "eid"),
    ];
    if (query.target !== undefined) args.push("--target", positiveInteger(query.target, "target"));
    if (query.sample !== undefined) args.push("--sample", positiveInteger(query.sample, "sample"));
    args.push("--json");
    return this.query(args, signal);
  }

  async diffCaptures(
    before: string,
    after: string,
    options: RdcDiffOptions = {},
    signal?: AbortSignal,
  ): Promise<unknown> {
    const beforePath = this.resolvePath(before);
    const afterPath = this.resolvePath(after);
    const modes: Array<{ name: string; args: string[] }> = [];
    if (options.draws) modes.push({ name: "draws", args: ["--draws"] });
    if (options.resources) modes.push({ name: "resources", args: ["--resources"] });
    if (options.passes) modes.push({ name: "passes", args: ["--passes"] });
    if (options.stats) modes.push({ name: "stats", args: ["--stats"] });
    if (options.framebuffer) modes.push({ name: "framebuffer", args: ["--framebuffer"] });
    if (options.pipelineMarker !== undefined) {
      modes.push({ name: "pipeline", args: ["--pipeline", options.pipelineMarker] });
    }
    if (modes.length === 0) modes.push({ name: "summary", args: [] });

    const results: Record<string, unknown> = {};
    for (const mode of modes) {
      const args = ["diff", beforePath, afterPath, ...mode.args];
      if (options.eid !== undefined) args.push("--eid", positiveInteger(options.eid, "eid"));
      if (mode.name === "framebuffer") {
        if (options.target !== undefined) args.push("--target", positiveInteger(options.target, "target"));
        if (options.threshold !== undefined) {
          args.push("--threshold", positiveFinite(options.threshold, "threshold"));
        }
        if (options.diffOutput !== undefined) {
          args.push("--diff-output", this.resolvePath(options.diffOutput));
        }
      }
      if (options.timeoutSeconds !== undefined) {
        args.push("--timeout", positiveFinite(options.timeoutSeconds, "timeoutSeconds"));
      }
      args.push("--json");
      try {
        const result = await this.runner.runJson<unknown>(this.sessionArgs(args), {
          signal,
          timeoutMs: this.captureTimeoutMs,
        });
        results[mode.name] = result.data;
      } catch (error) {
        if (!(error instanceof RdcProcessError) || error.kind !== "exit" || error.exitCode !== 1) {
          throw error;
        }
        results[mode.name] = parseJsonOrText(error.stdout, error.stderr);
      }
    }
    return { before: beforePath, after: afterPath, modes: results };
  }

  async assertPixel(
    options: RdcAssertPixelOptions,
    signal?: AbortSignal,
  ): Promise<RdcPixelAssertionResult> {
    this.requireOpen();
    if (options.expected.length !== 4 || options.expected.some((value) => !Number.isFinite(value))) {
      throw new Error("expected must contain four finite RGBA values");
    }
    const args = [
      "assert-pixel",
      positiveInteger(options.eid, "eid"),
      positiveInteger(options.x, "x"),
      positiveInteger(options.y, "y"),
      "--expect",
      options.expected.join(" "),
      "--tolerance",
      positiveFinite(options.tolerance ?? 0.05, "tolerance"),
      "--target",
      positiveInteger(options.target ?? 0, "target"),
      "--json",
    ];
    try {
      const result = await this.runner.runJson<unknown>(this.sessionArgs(args), {
        signal,
        timeoutMs: this.replayTimeoutMs,
      });
      return { passed: true, exitCode: result.exitCode, details: result.data, stderr: result.stderr };
    } catch (error) {
      if (!(error instanceof RdcProcessError) || error.kind !== "exit" || error.exitCode !== 1) {
        throw error;
      }
      return {
        passed: false,
        exitCode: error.exitCode,
        details: parseJsonOrText(error.stdout, error.stderr),
        stderr: error.stderr,
      };
    }
  }

  async captureExecutable(
    options: RdcCaptureExecutableOptions,
    signal?: AbortSignal,
  ): Promise<RdcCaptureResult> {
    const outputPath = this.resolvePath(options.output);
    const executable = this.resolvePath(options.executable);
    const executableMetadata = await stat(executable).catch(() => undefined);
    if (!executableMetadata?.isFile()) {
      throw new Error(`Capture executable is not a file: ${executable}`);
    }
    if (await stat(outputPath).catch(() => undefined)) {
      throw new Error(`Refusing to overwrite an existing capture: ${outputPath}`);
    }
    const timeoutSeconds = options.timeoutSeconds ?? Math.ceil(this.captureTimeoutMs / 1_000);
    const args = ["capture", "--output", outputPath];
    if (options.api !== undefined) args.push("--api", options.api);
    if (options.frame !== undefined) args.push("--frame", positiveInteger(options.frame, "frame"));
    args.push("--timeout", positiveFinite(timeoutSeconds, "timeoutSeconds"));
    // rdc-cli 0.6.1 forwards this flag to ExecuteAndInject, which waits for the
    // target to exit before target-control connects. Keep it explicit-only.
    if (options.waitForExit === true) args.push("--wait-for-exit");
    if (options.apiValidation) args.push("--api-validation");
    if (options.refAllResources) args.push("--ref-all-resources");
    args.push("--json", "--", executable, ...(options.appArgs ?? []));
    const result = await this.runner.runJson<unknown>(this.sessionArgs(args), {
      signal,
      timeoutMs: Math.max(this.captureTimeoutMs, (timeoutSeconds + 30) * 1_000),
    });

    const reportedPath = resolve(this.cwd, capturePathFromResult(result.data) ?? outputPath);
    const captured = await stat(reportedPath).catch(() => undefined);
    if (!captured?.isFile()) {
      throw new Error(`rdc reported success but did not produce a capture file: ${reportedPath}`);
    }
    if (reportedPath !== outputPath) {
      await rename(reportedPath, outputPath);
    }
    const details = typeof result.data === "object" && result.data !== null && !Array.isArray(result.data)
      ? { ...result.data, path: outputPath }
      : result.data;
    return { outputPath, details };
  }

  private async query(args: readonly string[], signal?: AbortSignal): Promise<unknown> {
    const result = await this.runner.runJson<unknown>(this.sessionArgs(args), {
      signal,
      timeoutMs: this.replayTimeoutMs,
    });
    return result.data;
  }
}
