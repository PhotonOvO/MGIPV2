import { spawn } from "node:child_process";

/**
 * Process runner for slp.py, mirroring src/rdc/process.ts.
 *
 * Two differences from the RenderDoc runner matter:
 *
 * 1. The executable is a Python interpreter with slp.py as a fixed prefix arg,
 *    both configurable through environment (SLP_PYTHON, SLP_PY).
 * 2. slp.py distinguishes exit codes: 0 ok, 1 analysis failed, 2 bad arguments,
 *    3 environment missing (no zstd backend). Code 3 becomes a dedicated error
 *    kind so a tool can tell the model "fix the environment" instead of
 *    "retry", which is the wrong action for a missing dependency.
 */

export const DEFAULT_SLP_TIMEOUT_MS = 120_000;
/** Aggregate commands scan every event; give them more headroom than rdc replay. */
export const DEFAULT_SLP_ANALYSIS_TIMEOUT_MS = 300_000;
const DEFAULT_MAX_OUTPUT_BYTES = 64 * 1024 * 1024;

/** slp.py exit codes (kept in sync with the EXIT_* constants in slp.py). */
export const SLP_EXIT_OK = 0;
export const SLP_EXIT_ANALYSIS_FAILED = 1;
export const SLP_EXIT_BAD_ARGS = 2;
export const SLP_EXIT_ENV_MISSING = 3;

export interface SlpProcessResult<T> {
  data: T;
  stderr: string;
  exitCode: number;
}

export type SlpProcessErrorKind =
  | "spawn"
  | "exit"
  | "bad-args"
  | "env-missing"
  | "aborted"
  | "timeout"
  | "output-limit"
  | "invalid-json";

export class SlpProcessError extends Error {
  readonly kind: SlpProcessErrorKind;
  readonly args: readonly string[];
  readonly exitCode: number | null;
  readonly stdout: string;
  readonly stderr: string;

  constructor(options: {
    message: string;
    kind: SlpProcessErrorKind;
    args: readonly string[];
    exitCode: number | null;
    stdout?: string;
    stderr?: string;
    cause?: unknown;
  }) {
    super(options.message, { cause: options.cause });
    this.name = "SlpProcessError";
    this.kind = options.kind;
    this.args = options.args;
    this.exitCode = options.exitCode;
    this.stdout = options.stdout ?? "";
    this.stderr = options.stderr ?? "";
  }
}

export interface SlpRunOptions {
  signal?: AbortSignal;
  timeoutMs?: number;
}

export interface SlpProcessRunner {
  runJson<T>(args: readonly string[], options?: SlpRunOptions): Promise<SlpProcessResult<T>>;
  runText(args: readonly string[], options?: SlpRunOptions): Promise<SlpProcessResult<string>>;
}

export interface SlpProcessRunnerOptions {
  /** Python interpreter. Default: SLP_PYTHON env or "python3". */
  executable?: string;
  /** Path to slp.py. Default: SLP_PY env. Required. */
  scriptPath: string;
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  defaultTimeoutMs?: number;
  maxOutputBytes?: number;
}

interface RawProcessResult {
  stdout: string;
  stderr: string;
  exitCode: number;
}

/** Map a non-zero slp.py exit code to a structured error kind. */
function errorKindForExit(exitCode: number): SlpProcessErrorKind {
  switch (exitCode) {
    case SLP_EXIT_ENV_MISSING:
      return "env-missing";
    case SLP_EXIT_BAD_ARGS:
      return "bad-args";
    default:
      return "exit";
  }
}

export function createSlpProcessRunner(options: SlpProcessRunnerOptions): SlpProcessRunner {
  const executable = options.executable ?? process.env.SLP_PYTHON ?? "python3";
  const scriptPath = options.scriptPath;
  // Every invocation is: <python> <slp.py> <args...>
  const prefixArgs = [scriptPath];
  const defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_SLP_TIMEOUT_MS;
  const maxOutputBytes = options.maxOutputBytes ?? DEFAULT_MAX_OUTPUT_BYTES;

  if (!scriptPath || scriptPath.trim().length === 0) {
    throw new Error("slp.py path is required (set SLP_PY or pass scriptPath)");
  }

  async function runRaw(args: readonly string[], runOptions: SlpRunOptions = {}): Promise<RawProcessResult> {
    const finalArgs = [...prefixArgs, ...args];
    const timeoutMs = runOptions.timeoutMs ?? defaultTimeoutMs;
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
      throw new RangeError("slp timeout must be a positive finite number");
    }
    if (runOptions.signal?.aborted) {
      throw new SlpProcessError({
        message: "slp process was aborted before it started",
        kind: "aborted",
        args: finalArgs,
        exitCode: null,
      });
    }

    return new Promise<RawProcessResult>((resolve, reject) => {
      const child = spawn(executable, finalArgs, {
        cwd: options.cwd,
        env: options.env,
        shell: false,
        windowsHide: true,
        stdio: ["ignore", "pipe", "pipe"],
      });

      const stdoutChunks: Buffer[] = [];
      const stderrChunks: Buffer[] = [];
      let stdoutBytes = 0;
      let stderrBytes = 0;
      let terminalKind: "aborted" | "timeout" | "output-limit" | undefined;
      let settled = false;

      const output = () => ({
        stdout: Buffer.concat(stdoutChunks).toString("utf8"),
        stderr: Buffer.concat(stderrChunks).toString("utf8"),
      });
      const stop = (kind: typeof terminalKind) => {
        if (terminalKind === undefined) {
          terminalKind = kind;
          child.kill();
        }
      };
      const onAbort = () => stop("aborted");
      runOptions.signal?.addEventListener("abort", onAbort, { once: true });
      const timeout = setTimeout(() => stop("timeout"), timeoutMs);

      child.stdout.on("data", (chunk: Buffer) => {
        stdoutBytes += chunk.byteLength;
        if (stdoutBytes + stderrBytes > maxOutputBytes) {
          stop("output-limit");
          return;
        }
        stdoutChunks.push(chunk);
      });
      child.stderr.on("data", (chunk: Buffer) => {
        stderrBytes += chunk.byteLength;
        if (stdoutBytes + stderrBytes > maxOutputBytes) {
          stop("output-limit");
          return;
        }
        stderrChunks.push(chunk);
      });

      const cleanup = () => {
        clearTimeout(timeout);
        runOptions.signal?.removeEventListener("abort", onAbort);
      };

      child.once("error", (cause) => {
        if (settled) return;
        settled = true;
        cleanup();
        const captured = output();
        reject(new SlpProcessError({
          message: `Unable to start ${executable} ${scriptPath}: ${cause.message}`,
          kind: "spawn",
          args: finalArgs,
          exitCode: null,
          ...captured,
          cause,
        }));
      });

      child.once("close", (code) => {
        if (settled) return;
        settled = true;
        cleanup();
        const captured = output();
        if (terminalKind !== undefined) {
          const message = terminalKind === "timeout"
            ? `slp process timed out after ${timeoutMs} ms`
            : terminalKind === "aborted"
              ? "slp process was aborted"
              : `slp process exceeded ${maxOutputBytes} output bytes`;
          reject(new SlpProcessError({
            message,
            kind: terminalKind,
            args: finalArgs,
            exitCode: code,
            ...captured,
          }));
          return;
        }
        const exitCode = code ?? -1;
        if (exitCode !== SLP_EXIT_OK) {
          const kind = errorKindForExit(exitCode);
          const diagnostic = captured.stderr.trim() || captured.stdout.trim() || "no diagnostic output";
          reject(new SlpProcessError({
            message: `slp exited with code ${exitCode}: ${diagnostic}`,
            kind,
            args: finalArgs,
            exitCode,
            ...captured,
          }));
          return;
        }
        resolve({ ...captured, exitCode });
      });
    });
  }

  return {
    async runJson<T>(args: readonly string[], runOptions: SlpRunOptions = {}) {
      const result = await runRaw(args, runOptions);
      try {
        return {
          data: JSON.parse(result.stdout) as T,
          stderr: result.stderr,
          exitCode: result.exitCode,
        };
      } catch (cause) {
        throw new SlpProcessError({
          message: `slp returned invalid JSON: ${cause instanceof Error ? cause.message : String(cause)}`,
          kind: "invalid-json",
          args: [...prefixArgs, ...args],
          exitCode: result.exitCode,
          stdout: result.stdout,
          stderr: result.stderr,
          cause,
        });
      }
    },
    async runText(args: readonly string[], runOptions: SlpRunOptions = {}) {
      const result = await runRaw(args, runOptions);
      return {
        data: result.stdout.trim(),
        stderr: result.stderr,
        exitCode: result.exitCode,
      };
    },
  };
}
