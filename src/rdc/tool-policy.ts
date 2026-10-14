import { compactRenderDocValue } from "./inspect-draw.ts";

export const MAX_RDC_TOOL_OUTPUT_BYTES = 40 * 1024;
export const DIRECT_RENDERDOC_REASON = "Use typed rdc_* tools instead of invoking RenderDoc directly.";

export interface BoundedToolResult {
  content: [{ type: "text"; text: string }];
  details: { bytes: number; truncated: boolean; originalBytes: number };
}

function stringify(value: unknown): string {
  return JSON.stringify(value, null, 2) ?? "null";
}

export function boundedToolResult(value: unknown): BoundedToolResult {
  let text = stringify(compactRenderDocValue(value));
  const originalBytes = Buffer.byteLength(text, "utf8");
  let truncated = false;
  if (originalBytes > MAX_RDC_TOOL_OUTPUT_BYTES) {
    truncated = true;
    text = stringify(compactRenderDocValue(value, {
      maxArray: 16,
      maxKeys: 24,
      maxString: 512,
    }));
  }
  if (Buffer.byteLength(text, "utf8") > MAX_RDC_TOOL_OUTPUT_BYTES) {
    text = stringify({
      truncated: true,
      originalBytes,
      preview: text.slice(0, 12_000),
      instruction: "Narrow the query with limit/filter/pass or inspect a specific EID.",
    });
  }
  return {
    content: [{ type: "text", text }],
    details: {
      bytes: Buffer.byteLength(text, "utf8"),
      truncated,
      originalBytes,
    },
  };
}

function splitShellSegments(command: string): string[] {
  const segments: string[] = [];
  let current = "";
  let quote: "'" | "\"" | undefined;
  let escaped = false;
  for (let index = 0; index < command.length; index += 1) {
    const character = command[index]!;
    if (escaped) {
      current += character;
      escaped = false;
      continue;
    }
    if (character === "`" || (character === "\\" && quote === "\"")) {
      current += character;
      escaped = true;
      continue;
    }
    if (quote !== undefined) {
      current += character;
      if (character === quote) quote = undefined;
      continue;
    }
    if (character === "'" || character === "\"") {
      quote = character;
      current += character;
      continue;
    }
    const separates = character === "\n"
      || character === "\r"
      || character === ";"
      || character === "|"
      || (character === "&" && command[index + 1] === "&");
    if (separates) {
      if (current.trim().length > 0) segments.push(current.trim());
      current = "";
      if (command[index + 1] === character) index += 1;
      continue;
    }
    current += character;
  }
  if (current.trim().length > 0) segments.push(current.trim());
  return segments;
}

function takeToken(command: string): { token: string; rest: string } {
  const input = command.trimStart();
  if (input.length === 0) return { token: "", rest: "" };
  const first = input[0]!;
  if (first === "'" || first === "\"") {
    const end = input.indexOf(first, 1);
    if (end >= 0) return { token: input.slice(1, end), rest: input.slice(end + 1).trimStart() };
  }
  const whitespace = input.search(/\s/);
  if (whitespace < 0) return { token: input, rest: "" };
  return { token: input.slice(0, whitespace), rest: input.slice(whitespace).trimStart() };
}

function executableBase(token: string): string {
  const normalized = token.replace(/^['\"]|['\"]$/g, "").replace(/\\/g, "/");
  return normalized.slice(normalized.lastIndexOf("/") + 1).toLowerCase();
}
function isRenderDocExecutable(token: string): boolean {
  return /^(?:rdc|renderdoccmd)(?:\.(?:exe|cmd|bat|ps1))?$/.test(executableBase(token));
}
function unwrapPayload(value: string): string {
  const trimmed = value.trim();
  if ((trimmed.startsWith("\"") && trimmed.endsWith("\""))
      || (trimmed.startsWith("'") && trimmed.endsWith("'"))) {
    return trimmed.slice(1, -1);
  }
  return trimmed;
}

function segmentInvokesRenderDoc(segment: string, depth = 0): boolean {
  if (depth > 4) return false;
  let candidate = segment.trim();
  if (candidate.startsWith("&")) candidate = candidate.slice(1).trimStart();
  const { token, rest } = takeToken(candidate);
  const base = executableBase(token);
  if (isRenderDocExecutable(token)) return true;
  if (/^(?:python|python3|py)(?:\.exe)?$/.test(base)) {
    const module = /^-m\s+(\S+)/i.exec(rest)?.[1] ?? "";
    if (/^rdc(?:\.cli)?$/i.test(module)) return true;
  }
  if (/^(?:cmd|cmd\.exe)$/.test(base)) {
    const payload = /^\/[cdsk]+\s+([\s\S]+)$/i.exec(rest)?.[1];
    return payload !== undefined && isDirectRenderDocCommand(unwrapPayload(payload), depth + 1);
  }
  if (/^(?:powershell|powershell\.exe|pwsh|pwsh\.exe)$/.test(base)) {
    const payload = /(?:^|\s)-(?:command|c)\s+([\s\S]+)$/i.exec(rest)?.[1];
    return payload !== undefined && isDirectRenderDocCommand(unwrapPayload(payload), depth + 1);
  }
  if (/^(?:bash|sh|zsh)(?:\.exe)?$/.test(base)) {
    const payload = /^-(?:c|lc)\s+([\s\S]+)$/i.exec(rest)?.[1];
    return payload !== undefined && isDirectRenderDocCommand(unwrapPayload(payload), depth + 1);
  }
  if (/^(?:call|start-process|start|iex|invoke-expression)$/i.test(base)) {
    return isDirectRenderDocCommand(unwrapPayload(rest), depth + 1);
  }
  return false;
}

function nestedSubcommands(command: string): string[] {
  const nested: string[] = [];
  let singleQuoted = false;
  for (let index = 0; index < command.length - 1; index += 1) {
    const character = command[index]!;
    if (character === "'" && command[index - 1] !== "`" && command[index - 1] !== "\\") {
      singleQuoted = !singleQuoted;
      continue;
    }
    if (singleQuoted || character !== "$" || command[index + 1] !== "(") continue;
    let depth = 1;
    const start = index + 2;
    for (let cursor = start; cursor < command.length; cursor += 1) {
      if (command[cursor] === "(" && command[cursor - 1] !== "\\") depth += 1;
      if (command[cursor] === ")" && command[cursor - 1] !== "\\") depth -= 1;
      if (depth === 0) {
        nested.push(command.slice(start, cursor));
        index = cursor;
        break;
      }
    }
  }
  return nested;
}

export function isDirectRenderDocCommand(command: string, depth = 0): boolean {
  if (depth > 4) return false;
  if (splitShellSegments(command).some((segment) => segmentInvokesRenderDoc(segment, depth))) return true;
  return nestedSubcommands(command).some((nested) => isDirectRenderDocCommand(nested, depth + 1));
}
