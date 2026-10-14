/**
 * Output-bounding and shell-guard policy for the slp_* tools.
 *
 * Mirrors src/rdc/tool-policy.ts. slp.py already emits bounded, ranked JSON
 * (every aggregate respects --top and reports row_limits), so bounding here is
 * defense in depth for the cases where a raw container dump or a deep stack
 * list still exceeds the tool-output budget.
 */

export const MAX_SLP_TOOL_OUTPUT_BYTES = 40 * 1024;

/**
 * slp.py is stateless, so unlike RenderDoc there is no typed session to protect
 * and most subcommands are safe to run through bash. The one genuine hazard is
 * `dump-json`, which prints an entire decoded container: measured at 5.6 MB for
 * functioncache.dat and 16.7 MB for the .session member of a real trace, i.e.
 * one to two orders of magnitude over any model context. Guide the model to the
 * typed tools instead.
 */
export const CONTEXT_FLOOD_SLP_REASON =
  "Do not run 'slp.py dump-json' through bash: it prints a whole decoded container " +
  "(5-17 MB, millions of tokens). Use the typed slp_* tools (slp_hotspots, slp_stacks, " +
  "slp_source, slp_stack) which return bounded, ranked results.";

export interface BoundedToolResult {
  content: [{ type: "text"; text: string }];
  details: { bytes: number; truncated: boolean; originalBytes: number };
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function stringify(value: unknown): string {
  return JSON.stringify(value, null, 2) ?? "null";
}

/**
 * Recursively shrink a value so it survives JSON round-tripping within budget:
 * cap array length, object key count, string length, and nesting depth. Same
 * shape as compactRenderDocValue so the two subsystems bound output identically.
 */
export function compactSlpValue(
  value: unknown,
  options: { depth?: number; maxArray?: number; maxKeys?: number; maxString?: number } = {},
): unknown {
  const depth = options.depth ?? 0;
  const maxArray = options.maxArray ?? 64;
  const maxKeys = options.maxKeys ?? 48;
  const maxString = options.maxString ?? 2_048;
  if (depth >= 7) return "[depth limit]";
  if (typeof value === "string") {
    return value.length <= maxString
      ? value
      : `${value.slice(0, maxString)}...[truncated ${value.length - maxString} chars]`;
  }
  if (Array.isArray(value)) {
    const items = value.slice(0, maxArray).map((item) =>
      compactSlpValue(item, { ...options, depth: depth + 1 }),
    );
    if (value.length > maxArray) items.push(`[truncated ${value.length - maxArray} items]`);
    return items;
  }
  if (isRecord(value)) {
    const entries = Object.entries(value).slice(0, maxKeys);
    const result = Object.fromEntries(entries.map(([key, child]) => [
      key,
      compactSlpValue(child, { ...options, depth: depth + 1 }),
    ]));
    if (Object.keys(value).length > maxKeys) {
      result._truncatedKeys = Object.keys(value).length - maxKeys;
    }
    return result;
  }
  return value;
}

/** Render a value as bounded tool output, escalating compaction until it fits. */
export function boundedToolResult(value: unknown): BoundedToolResult {
  let text = stringify(compactSlpValue(value));
  const originalBytes = Buffer.byteLength(text, "utf8");
  let truncated = false;
  if (originalBytes > MAX_SLP_TOOL_OUTPUT_BYTES) {
    truncated = true;
    text = stringify(compactSlpValue(value, { maxArray: 16, maxKeys: 24, maxString: 512 }));
  }
  if (Buffer.byteLength(text, "utf8") > MAX_SLP_TOOL_OUTPUT_BYTES) {
    text = stringify({
      truncated: true,
      originalBytes,
      preview: text.slice(0, 12_000),
      instruction: "Narrow the query: lower top, add a thread/time filter, or use --module-filter.",
    });
  }
  return {
    content: [{ type: "text", text }],
    details: { bytes: Buffer.byteLength(text, "utf8"), truncated, originalBytes },
  };
}

/**
 * True when a bash command invokes `slp.py dump-json` (the unbounded container
 * dump). Only the exact flooding subcommand is guarded; every other slp.py
 * subcommand is bounded by its own --top/--limit and stays usable through bash.
 *
 * Deliberately matches a `slp.py dump-json` substring even inside quotes: a
 * false block on `echo "slp.py dump-json"` costs the model one rephrase, while a
 * false pass floods the context with megabytes. The asymmetry favors blocking.
 */
export function isContextFloodSlpCommand(command: string): boolean {
  return /\bslp\.py["']?\s+dump-json\b/i.test(command);
}
