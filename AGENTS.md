# Project Agent Instructions

在回答之前，先停顿一下。想想用户真正想问的是什么，这件事有哪几种不同角度。考虑清楚后再回答。

For RenderDoc investigations:

- Use the project-level `renderdoc-debug` skill and the typed `rdc_*` tools.
- Do not invoke `rdc` or `renderdoccmd` through the shell.
- Collect RenderDoc evidence before proposing a root cause.
- Cite evidence IDs in every hypothesis and root-cause conclusion.
- Make the smallest source patch that tests the active hypothesis.
- A fix is confirmed only after rebuild, recapture, and RenderDoc verification.

For Superluminal CPU profiling investigations:

- Use the project-level `superluminal-perf` skill and the typed `slp_*` tools.
- Do not run `slp.py dump-json` through the shell; it prints whole decoded containers (5-17 MB). Use the bounded `slp_*` tools instead.
- Call `slp_info` on every new trace first; if the main executable's symbols did not resolve, stop and ask for a re-capture with PDBs rather than analyzing `?`-marked guess names.
- Never base a conclusion on a frame name ending in `?` (unresolved module, export-table guess).
- Aggregate with the tools, never reason over raw per-event output. Distinguish CPU-bound from GPU/wait-bound using per-frame cpu_ms vs wall_ms before proposing any CPU optimization.
- Never compare absolute ms across traces of different duration; use `slp_compare` (normalized by each trace's own total).
- Report how much Vulkan validation-layer / profiler overhead was excluded; Debug builds carry large measurement overhead that Release will not.
- Cite the tool, trace, thread, window, and number behind every hotspot claim and hypothesis.
- A fix is confirmed only after rebuild, re-profile, and a normalized `slp_compare` against the baseline.

Environment for the `slp_*` tools (set before running):

- `SLP_PY` — path to `slp.py` (e.g. `/mnt/f/superluminal-cli/slp.py`).
- `SLP_CAPTURES` — directory of `.slp` traces and GUI session folders (e.g. `/mnt/f/Captures`).
- `SLP_PYTHON` — optional interpreter override; default `python3`. A zstd backend is required for event-based analysis: either the `zstandard` pip module or the system `libzstd` (loaded via ctypes automatically).
