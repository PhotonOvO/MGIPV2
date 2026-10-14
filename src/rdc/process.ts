import { spawn } from "node:child_process";
import { statSync } from "node:fs";
import { delimiter, isAbsolute, join } from "node:path";

export const DEFAULT_RDC_TIMEOUT_MS = 60_000;
const DEFAULT_MAX_OUTPUT_BYTES = 64 * 1024 * 1024;

export interface RdcProcessResult<T> {
  data: T;
  stderr: string;
  exitCode: number;
}

export type RdcProcessErrorKind =
  | "spawn"
  | "exit"
  | "aborted"
  | "timeout"
  | "output-limit"
  | "invalid-json";

export class RdcProcessError extends Error {
  readonly kind: RdcProcessErrorKind;
  readonly executable: string;
  readonly args: readonly string[];
  readonly exitCode: number | null;
  readonly stdout: string;
  readonly stderr: string;

  constructor(options: {
    message: string;
    kind: RdcProcessErrorKind;
    executable: string;
    args: readonly string[];
    exitCode: number | null;
    stdout?: string;
    stderr?: string;
    cause?: unknown;
  }) {
    super(options.message, { cause: options.cause });
    this.name = "RdcProcessError";
    this.kind = options.kind;
    this.executable = options.executable;
    this.args = options.args;
    this.exitCode = options.exitCode;
    this.stdout = options.stdout ?? "";
    this.stderr = options.stderr ?? "";
  }
}

export interface RdcRunOptions {
  signal?: AbortSignal;
  timeoutMs?: number;
}

export interface RdcProcessRunner {
  runJson<T>(args: readonly string[], options?: RdcRunOptions): Promise<RdcProcessResult<T>>;
  runText(args: readonly string[], options?: RdcRunOptions): Promise<RdcProcessResult<string>>;
}

export interface RdcProcessRunnerOptions {
  executable?: string;
  prefixArgs?: readonly string[];
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

// Resolve the typed rdc helper across host boundaries. Under WSL the harness
// runs on linux but the helper is a Windows binary published as "rdc.exe", and
// spawn("rdc") does not apply PATHEXT-style lookup, so it fails with ENOENT
// even though "rdc.exe" is on PATH. Try the bare name first (native linux/macOS
// and Windows), then the platform script suffixes, so the typed tools keep
// working in both environments.
function resolveRdcExecutable(
  executable: string,
  env: NodeJS.ProcessEnv | undefined,
): string {
  if (executable.includes("/") || executable.includes("\\") || isAbsolute(executable)) {
    // An explicit path is taken as authored; report the original ENOENT rather
    // than silently substituting a different binary.
    return executable;
  }
  const candidates = [executable, `${executable}.exe`, `${executable}.cmd`, `${executable}.bat`];
  const directories = (env?.PATH ?? "").split(delimiter).filter(Boolean);
  for (const candidate of candidates) {
    for (const directory of directories) {
      const resolved = join(directory, candidate);
      try {
        if (statSync(resolved).isFile()) return resolved;
      } catch {
        // Not present in this directory; keep searching.
      }
    }
  }
  return executable;
}

export function createRdcProcessRunner(
  options: RdcProcessRunnerOptions = {},
): RdcProcessRunner {
  const requestedExecutable = options.executable ?? "rdc";
  const executable =
    requestedExecutable === "rdc"
      ? resolveRdcExecutable(requestedExecutable, options.env ?? process.env)
      : requestedExecutable;
  const prefixArgs = [...(options.prefixArgs ?? [])];
  const defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_RDC_TIMEOUT_MS;
  const maxOutputBytes = options.maxOutputBytes ?? DEFAULT_MAX_OUTPUT_BYTES;

  async function runRaw(
    args: readonly string[],
    runOptions: RdcRunOptions = {},
  ): Promise<RawProcessResult> {
    const finalArgs = [...prefixArgs, ...args];
    const timeoutMs = runOptions.timeoutMs ?? defaultTimeoutMs;
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) {
      throw new RangeError("rdc timeout must be a positive finite number");
    }
    if (runOptions.signal?.aborted) {
      throw new RdcProcessError({
        message: "rdc process was aborted before it started",
        kind: "aborted",
        executable,
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
        reject(new RdcProcessError({
          message: `Unable to start ${executable}: ${cause.message}`,
          kind: "spawn",
          executable,
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
            ? `rdc process timed out after ${timeoutMs} ms`
            : terminalKind === "aborted"
              ? "rdc process was aborted"
              : `rdc process exceeded ${maxOutputBytes} output bytes`;
          reject(new RdcProcessError({
            message,
            kind: terminalKind,
            executable,
            args: finalArgs,
            exitCode: code,
            ...captured,
          }));
          return;
        }
        const exitCode = code ?? -1;
        if (exitCode !== 0) {
          const diagnostic = captured.stderr.trim() || captured.stdout.trim() || "no diagnostic output";
          reject(new RdcProcessError({
            message: `rdc exited with code ${exitCode}: ${diagnostic}`,
            kind: "exit",
            executable,
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
    async runJson<T>(args: readonly string[], runOptions: RdcRunOptions = {}) {
      const result = await runRaw(args, runOptions);
      try {
        return {
          data: JSON.parse(result.stdout) as T,
          stderr: result.stderr,
          exitCode: result.exitCode,
        };
      } catch (cause) {
        throw new RdcProcessError({
          message: `rdc returned invalid JSON: ${cause instanceof Error ? cause.message : String(cause)}`,
          kind: "invalid-json",
          executable,
          args: [...prefixArgs, ...args],
          exitCode: result.exitCode,
          stdout: result.stdout,
          stderr: result.stderr,
          cause,
        });
      }
    },
    async runText(args: readonly string[], runOptions: RdcRunOptions = {}) {
      const result = await runRaw(args, runOptions);
      return {
        data: result.stdout.trim(),
        stderr: result.stderr,
        exitCode: result.exitCode,
      };
    },
  };
}

const defaultRunner = createRdcProcessRunner();

export async function runRdc<T>(
  args: string[],
  signal?: AbortSignal,
): Promise<RdcProcessResult<T>> {
  return defaultRunner.runJson<T>(args, { signal });
}

export async function runRdcText(
  args: string[],
  signal?: AbortSignal,
): Promise<RdcProcessResult<string>> {
  return defaultRunner.runText(args, { signal });
}
