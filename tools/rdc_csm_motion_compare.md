# RenderDoc CSM motion comparator

`rdc_csm_motion_compare.py` discovers CSM, Light, TAA, and final-color evidence from RenderDoc captures without hard-coded resource IDs. It supports a strict publication gate for the fixed CSM move-then-stop smoke and a legacy two-capture diagnostic mode.

## Invocation

A completed smoke manifest automatically enables the release gate:

```powershell
python tools/rdc_csm_motion_compare.py C:\path\to\manifest.json -o C:\path\to\comparison.json
```

Equivalent explicit invocation:

```powershell
python tools/rdc_csm_motion_compare.py --manifest C:\path\to\manifest.json --release-gate --total-timeout 7200 --json
```

`--timeout` limits each `rdc open`/`rdc script` operation. `--total-timeout` is the outer wall-clock deadline for the complete comparison and defaults to 7200 seconds; a 90-second per-session cleanup reserve is held back from replay work.

`--case NAME` is diagnostic-only and is forbidden with `--release-gate`. A publication run must replay and extract both required cases and all six captures; there is no post-validation subset path. `--no-release-gate` is diagnostic only.

The publication sequence is fixed at `--warmup-frames 8 --motion-frames 24 --hold-frames 8`. Zero-based target frames are `last-moving=31`, `first-still=32`, and `settled=39`. A final manifest using any other sequence is an `error`.

Legacy two-capture diagnostic mode remains available:

```powershell
python tools/rdc_csm_motion_compare.py before.rdc after.rdc --json
```

Legacy mode does not provide release guarantees and rejects `--release-gate`.

Run the built-in contract tests before publishing script changes:

```powershell
python tools/rdc_csm_motion_compare.py --self-test
python tools/run_csm_shadow_motion_smoke.py --self-test
python -m unittest tools.test_renderdoc_capture_protocol_contract
python -m unittest tools.test_rdc_shadow_edge_metrics
```

## Exact release manifest contract

Missing, ambiguous, or inconsistent evidence is `error`, never pass. A release manifest must:

- use schema `mgif-csm-shadow-motion-smoke-v1` and have `status: "passed"`;
- bind `manifest_path` and `output_directory` to the evaluated manifest;
- declare boundary order `last-moving`, `first-still`, `settled` and the exact `8/24/8` frame sequence;
- select exactly one uniform release profile and contain its exact two cases, with no subset or mixed render modes:
  - TAA-on: `csm-translate-stop__taa-on__no-ddgi` and `csm-rotate-stop__taa-on__no-ddgi`;
  - no-post: `csm-translate-stop__no-post__no-ddgi` and `csm-rotate-stop__no-post__no-ddgi`;
- have successful doctor, pose, executable, capture, replay, and cleanup evidence;
- contain a passed formal work-directory disk preflight with an explicit byte estimate and safety margin;
- bind the harness, comparator, and independent ROI module by absolute path, literal version, byte size, and SHA-256 before and after all cases;
- bind every case to the same toolchain bundle SHA-256;
- bind six distinct canonical RDC paths and six distinct SHA-256 values across the two cases.

Each case must contain all three ordered boundary records and pass the pose contract. With the default tolerance `1e-5`:

- `last-moving.current == first-still.current`;
- `last-moving.current == first-still.previous`;
- `first-still.current == settled.current`;
- `first-still.current == settled.previous`;
- `last-moving.current != last-moving.previous`.

## Executable evidence and target binding

Release captures are bound to the executable content actually launched, not merely to a build-tree path.

For a formal 8/24/8 release, the source must be the rebuilt `out/build/x64-debug/Demo.exe`; `Demo.csm_shadow_reactive_test.exe` is rejected. The harness records that source evidence and creates a run-local immutable launch image whose filename contains its SHA-256. Dependencies and working directory remain the original build directory. The launch image is opened with a held Windows handle that permits reads but denies write/delete sharing, then that exact copy is passed to suspended `CreateProcess`.

Release validation requires:

- source and launch-image resolved path, byte size, and SHA-256;
- source and launch-image before/after evidence to remain identical;
- the launch image's SHA-derived filename to match its content;
- the deny-write/delete lock to remain held through all cases and close cleanly afterward;
- each target to be bound immediately to a duplicate of the exact launcher-owned process handle, with a positive PID, exact creation FILETIME, immutable image path, and no PID lookup;
- `TargetControl.GetPID()` to equal that held process handle's PID;
- each case's target binding and executable evidence to match the top-level evidence;
- a final current-file rehash to equal the recorded size and SHA-256.

Any unavailable identity, hash drift, process-image mismatch, or missing field fails closed. This evidence intentionally does not include a git commit or whole-worktree hash.

## Toolchain identity and disk preflight

The capture manifest binds five roles as one toolchain bundle: `run_csm_shadow_motion_smoke.py`, `rdc_csm_motion_compare.py`, `rdc_shadow_edge_metrics.py`, the full installed `rdc-cli` distribution, and the loaded RenderDoc Python module. Each role records an absolute path, version, byte size, and SHA-256. The `rdc-cli` role hashes every regular distribution file except bytecode/cache exclusions and records a canonical file-list SHA; legitimate zero-byte regular files are included with size `0` and the SHA-256 of empty content. Missing, non-regular, unreadable, or changing files fail closed. The harness hashes the bundle before capture and after every case; the comparator validates both manifest snapshots, rehashes its current runtime package/module, and writes the result under `comparison.release_evidence`.

Before launching the immutable Demo image, the harness estimates accepted RDC storage, one quarantined candidate per boundary, replay scratch, the launch image, and a safety margin of at least 4 GiB or 25 percent. Before opening any RDC, the comparator separately estimates decoded extraction and session storage plus the same minimum safety margin. An unavailable disk query or free space below `estimated_bytes + safety_margin_bytes` is an error; neither tool starts RenderDoc work after a failed preflight.

The preflight is backed by hard runtime budgets, not only an estimate. The harness permits at most two candidates per boundary (one accepted plus one quarantined) and budgets each candidate at 1 GiB; it rechecks candidate count, copied-byte budget, and current free space before every `CopyCapture`. The comparator rejects an RDC larger than 1 GiB, caps actual decoded readback at 512 MiB per capture, budgets 1.5 GiB of extraction output per capture, and rechecks output-byte and free-space limits before every NPY write. A cap violation fails before the write.

## Capture identity and target-frame binding

Release mode uses only each boundary's declared `capture_path`; alternatives are not silently selected. For each RDC it verifies path containment, size, fresh SHA-256, timestamps, matching non-negative integer capture IDs, and replay-path consistency. RenderDoc `captureId=0` is valid.

The file is rehashed before and after replay. Each case requires three distinct paths, capture IDs, and hashes, and the full manifest requires six globally unique canonical paths and hashes.

Each RDC must contain exactly one marker:

```text
CSM_AUTOMATION_FRAME mode=<mode> boundary=<last-moving|first-still|settled> frame=<N>
```

It must be nested under `GPUDrivenCSMShadow`, and its mode, boundary, and frame must match the case and boundary metadata. External `arm-last-moving` and `arm-settled` markers only arm next-frame capture; they are never accepted as target-frame proof.

## Real fullscreen draw evidence

The manifest selects one uniform publication profile. Both profiles bind Light and final color to their actual last draw, never to a marker EID:

- Light: exact inner `GPUDrivenLightPass`, one `Draw(3,1)`, fragment entry `fragmentHdrMain`, and color output `GPUDrivenSceneColorHDR`;
- final: exact inner `GPUDrivenFinalColor`, one `Draw(3,1)`, fragment entry `fragmentFinalColorMain`, and color output `OutputTexture`.

For every required fullscreen stage, the snapshot EID must equal `last_draw_eid`, the fragment shader and graphics pipeline must be valid, the output resource ID must equal the selected resource, and RenderDoc usage must prove that output was written inside the marker. The raw RenderDoc action fields must report exactly three vertices and `numInstances == 1`; raw zero is a hard error and is never normalized to one.

The TAA-on profile additionally forbids `taa.passthrough` and requires the inner `GPUDrivenTAAResolve` marker. The outer `GPUDrivenTAAResolvePass` event and marker-EID fallback are not draw evidence. The inner marker must contain exactly one actual `Draw(3,1)` whose snapshot EID equals the pass `last_draw_eid`, with:

- a graphics pipeline and fragment entry point `fragmentTAAResolveMain`;
- a color output named `GPUDrivenSceneColorHistory0` or `GPUDrivenSceneColorHistory1` in RGBA16F format;
- descriptor array element 4 bound to `GPUDrivenSceneColorHDR`;
- descriptor array element 7 bound to `GPUDrivenVelocity`;
- descriptor array element 8 bound to the opposite history resource;
- distinct HistoryRead and HistoryWrite resources;
- final pass descriptor array element 9 selecting that HistoryWrite;
- a final output named `OutputTexture`.

The harness applies the same inner-marker, raw draw, shader, output, and descriptor checks while quarantining candidate RDCs. In a formal TAA-on release, `historyValid` must be readable and exactly `1` at all three target frames 31, 32, and 39. History0 and History1 normalize to the logical resource `GPUDrivenSceneColorHistory` while retaining `physical_index`; the expected HistoryWrite evidence is `1 -> 0 -> 1`, consistent with frame-delta parity.

The no-post profile requires TAA to be absent: no inner `GPUDrivenTAAResolve`, no resolve draw, no TAA pipeline snapshot/export, and no history or velocity release inputs. Its final draw must consume the selected `GPUDrivenSceneColorHDR` directly. Any TAA evidence in a no-post release is an error.
## Release extraction inputs

Every profile exports directly decoded NumPy arrays for `SceneDepth`, decoded world-space normal from `SceneColor1`, base color from `SceneColor0`, `GPUDrivenSceneColorHDR`, and final `OutputTexture`. TAA-on additionally exports HistoryRead and HistoryWrite. HistoryRead is evidence-only; no-post exports neither history resource and does not require velocity as an ROI input.

The selected CSM texture must be a single-sample 2D array with `depth==1` and exactly four layers. Release extraction must export exactly slices `0, 1, 2, 3`, and must retain one non-empty projection-matrix evidence record for each cascade. A one-layer texture, a missing slice, duplicate/out-of-order slices, or fewer than four projection evidences is an error.

The extractor also exports unjittered `clip_from_world`, `world_from_clip`, `view_from_world`, and `world_from_view`, the viewport, Vulkan `zero_to_one` depth range, and MGIF framebuffer convention `top_to_negative_one`. Matrix evidence must be finite, unambiguous, and internally inverse-consistent.

Every `GetTextureData` readback is accepted only for a single-sample 2D texture (`samples==1`, `depth==1`), mip 0 and sample 0. The extractor proves the source format's bytes per pixel and requires the raw payload length to equal exactly `width * height * bytes_per_pixel`; it never truncates a buffer or guesses row pitch. Unsupported or non-tight layouts fail closed before decoding.

Every `.npy` is saved as `float32`, reopened immediately with pickle disabled, and checked against its declared shape, dtype, canonical channel order, source resource ID/format, and screen extent. The host reopens the files again and repeats those checks. All profile-required screen inputs must have identical dimensions and match the camera viewport. TAA-on marks HistoryRead explicitly as `evidence-only`; ROI publication uses HistoryWrite, not HistoryRead, as the temporal output stage.

Release mode forbids PNG fallback. Missing resources, ambiguous descriptors, failed decode, NPY metadata drift, unavailable camera evidence, inability to prove a tight readback, or exceeding the readback/output budgets is `error`.
## Shadow-edge ROI publication gate

The comparator imports the independent `rdc_shadow_edge_metrics.py` module and does not duplicate its ROI algorithm. For each same-pose pair it constructs two `ShadowFrame` instances from the exported depth, decoded normals, base color, profile outputs, and unjittered cameras, then calls `evaluate_shadow_edge_metrics(...)` with the canonical release configuration.

Required publication mappings are profile-specific:

- TAA-on: Light -> `scene_color_hdr`, HistoryWrite -> `history_write`, final -> `final`;
- no-post: Light -> `scene_color_hdr`, final -> `final`.

The independent API currently requires a `history_write` field in `ShadowFrame`; no-post supplies `scene_color_hdr` only as an adapter for that field and records this fact. The adapter is not treated as no-post HistoryWrite publication evidence.

The aggregate result and every profile-required stage must be `pass`. `fail`, `error`, `inconclusive`, a missing required stage, or inability to construct a valid receiver/edge ROI hard-fails publication. The complete independent metrics dictionary is stored in each pair's `release_evidence.shadow_edge_metrics` and aggregated under the case release evidence.

This local edge gate supplements the full-frame changed-fraction and maximum-absolute gates; it is specifically intended to catch small soft-penumbra changes that occupy too little of the frame for global fractions to be reliable.
## Cleanup and daemon ownership

For rdc-cli 0.6.1, the run-level legacy default-session diagnostic classifies inactive status as return code `1`, empty stdout, and stderr `error: no active session`; return code `0` means active. Named replay-session cleanup does not invoke the CLI close/status path.

The capture manifest's run-level cleanup uses schema `rdc-session-cleanup-v2`. It requires available, error-free before/after daemon and session snapshots, `process_access_denied_count == 0` on both sides, every named direct-shutdown result to prove held-handle daemon absence and exact state-file absence, and no owned daemon/session residue. External additions are diagnostic only and are neither killed nor publication-gated.

Each comparator extraction records `mgif-rdc-comparator-replay-session-cleanup-v4`. The named state-file PID must be acquired as a stable held process identity with exact Windows creation FILETIME, executable image, session path, and capture metadata. Missing or mismatched state/session/capture paths cannot establish ownership. Cleanup success requires direct state-token shutdown or same-handle recovery, the same owned daemon to be absent, the exact state file to be absent, the outer deadline not to have expired, and the held handle to close successfully.
Daemon publication proof is handle-derived and fail-closed. The named-session JSON is opened once and held while the reader takes raw `GetFileTime` snapshots before and after the read; those FILETIME values must be identical. From that same native handle, `GetFinalPathNameByHandleW(VOLUME_NAME_GUID | FILE_NAME_NORMALIZED)` must return a normalized volume-GUID path, `GetVolumeInformationByHandleW` must identify NTFS or ReFS, and `GetDriveTypeW` on the derived GUID root must report a fixed drive. The before/after native handle value, GUID final path, volume root, serial, filesystem metadata, and raw FILETIME must remain identical. UNC/remote/removable/FAT/unknown volumes, API failure, unresolved identity, or any drift fails closed. The original path and `path.resolve()` are never volume/timestamp trust inputs; path-based checks may only reject a replacement and can never establish ownership.

Creation ordering uses the same Windows 100 ns FILETIME domain and requires strict `creation_filetime_ticks < state_publication_filetime_ticks`. Equality is ambiguous and is rejected, as is every positive post-publication delta; there is no tolerance. Comparator-owned daemons must additionally be created no earlier than the invocation's integer `time_ns` open boundary. Image and `rdc.daemon_server` command-line metadata must be bound to the same held process identity. The fully hashed installed rdc-cli 0.6.1 package is AST-pinned to `start_daemon` before `wait_for_ping` before `create_session`, followed by `create_session -> save_session -> secure_write_text -> path.write_text`; this proves the daemon was created and answered its ping before the authoritative state publication. If any exact handle, creation, image, command, filesystem, or package-ordering evidence is unavailable, ownership is abandoned rather than approximated.

The Demo target is separately launch-owned. It is created suspended; the launcher-owned process handle is duplicated immediately, and PID, exact creation FILETIME, and image are read from that duplicate before RenderDoc injection and resume. `TargetControl.GetPID()` must equal the duplicate's PID. Missing identity, PID mismatch, same-image PID reuse, or an unclosed live target fails closed and grants zero termination authority to any replacement process.

Named replay shutdown imports `rdc.daemon_client.send_request` and `rdc.protocol.shutdown_request`, loads the exact session token from the unchanged state file, and sends the graceful request directly. If that request fails or times out while the owned daemon remains, cleanup terminates and waits only through the already-held stable process handle. Demo cleanup likewise uses its duplicated launch-owned handle. There is no subprocess close path, PID-only fallback, or PID-only tree cleanup. If the original process has exited and its numeric PID has been reused, the replacement is untouched and the original is considered ended.
## Numeric gates

Same-current-pose pairs are `last-moving_to_first-still` and `first-still_to_settled`. CSM layers are compared at the same array layer and texel. Integer registration is diagnostic and must report zero release shift. Light/final, and TAA when present, use same-pixel comparisons.

Release thresholds are immutable. Every threshold-bearing CLI option must equal the canonical value below; an override such as `--depth-epsilon 0.2` is an error rather than a weaker publication run. This applies even to TAA-specific fields in a no-post profile.

- registration/search: `max_shift=128`, `max_csm_registration_shift=0`;
- pose/projection: `pose_tolerance=1e-5`, `max_csm_matrix_delta=1e-5`, `max_csm_center_motion_texels=1e-3`, `power2_tolerance=0.05`, `grid_phase_tolerance=0.10`, `cascade_blend_fraction=0.10`;
- change epsilons: `depth_epsilon=1e-6`, `color_epsilon=1e-3`;
- CSM limits: changed fraction `0.001`, MAE `1e-5`, active changed fraction `0.001`, foreground mismatch fraction `0.0001`, max absolute delta `1e-4`, minimum active pixels `64`, minimum active fraction `1e-5`;
- screen limits: Light/TAA/final changed fraction `0.01` and max absolute delta `0.01` for each stage.

The independent ROI configuration is likewise fixed and validated at runtime: `min_valid_reprojection_ratio=0.95`, `max_edge_chamfer_p95_px=0.25`, `max_outside_1px_mismatch=0.005`, `max_normalized_residual_p99=0.05`, `world_up=(0,1,0)`, `max_receiver_tilt_degrees=25`, `max_reprojection_normal_degrees=12`, world reprojection absolute/relative/pixel tolerances `1e-3/2e-4/0.25`, base-color reprojection tolerance `0.08`, linear-depth edge absolute/relative thresholds `0.02/0.02`, normal edge `18` degrees, base-color edge `0.08`, geometry edge margin `2` pixels, minimum receiver/ROI fractions `0.01/0.005`, minimum 1080p receiver/ROI/edge pixels `512/256/16`, minimum splat weight `0.25`, minimum log-luma edge gradient `0.005`, adaptive edge quantile/fraction `0.99/0.25`, log-luma epsilon `1e-4`, residual scale floor `0.25`, chamfer radius `8`, projection chunk rows `128`, and quantile histogram bins `4096`.

Any NaN/Inf, unavailable coverage, missing matrix, semantic fingerprint mismatch, ambiguous selection, or non-canonical threshold/configuration is `error`, even if ordinary finite-pixel metrics could still be computed.
## Report and exit codes

Top-level `comparison.release_evidence` records the executable-evidence validator result, manifest/runtime toolchain validation, disk preflight and runtime budgets, total-deadline evidence, cleanup validation, declared global uniqueness, exact six-capture rehash validation, release profile, and full capture binding. Each case release evidence records executable/toolchain binding, target-frame markers, four-cascade resource semantics, profile-specific draw/history-or-absence evidence, capture binding, and per-pair shadow-edge metrics.

Exit codes:

- `0`: pass;
- `1`: completed comparison with one or more numeric threshold failures;
- `2`: malformed/missing/ambiguous evidence, extraction failure, invalid ROI, timeout, or cleanup error;
- `130`: interrupted.

Use `--json` for stdout JSON or `--output` for an atomically written report.

## Headless sessions and temporary data

Each extraction uses a unique named `rdc --session` and follows `open -> script -> direct token shutdown -> held-handle wait/recovery -> exact state-file cleanup` in a `finally` path. The state PID is bound to a held stable process handle before replay is trusted. Host cleanup is `closed=true` only when direct shutdown evidence passes without subprocess/PID/port/tree fallback, the owned daemon is absent on that handle, the allocated state file is absent, cleanup errors are empty, and the handle closes. `passed=true` additionally requires that the outer deadline was not exceeded. Replay work cannot consume the reserved cleanup budget; if the outer deadline is nevertheless reached, bounded handle-scoped recovery still runs, but the comparison remains an error even after clean shutdown. Owned daemon residue, residual state, missing exact identity, or handle-close failure can never be reported as success.

`--timeout` and `--total-timeout` must be positive finite values, and the total timeout must exceed the cleanup reserve. By default export data is temporary; `--work-dir` retains it and `--keep-work-dir` copies an automatically created directory into the current directory.