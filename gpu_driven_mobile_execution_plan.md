# GPU Driven Mobile Execution Plan

Date: 2026-05-29
Status: Phase 6 clustered lighting and IBL baseline implementation completed; awaiting Phase 6 review before Phase 7
Source: `worklist.md`
Scope: only `GPUDrivenRenderer`; legacy `Renderer` changes are allowed only when they unblock GPU-driven ownership.

## Execution Rules

- Do not execute build or compile commands during implementation. Notify the user to trigger them manually.
- Execute one approved task at a time without asking again inside that task.
- Update this document and `worklist.md` immediately after each completed subtask.
- Stop and ask for review/approval after each phase is complete.
- Keep Android/mobile Vulkan as a first-class target.
- Prefer mobile-feasible implementations first; keep full Nanite/Lumen/VSM/RT-style work as optional high-end tiers.
- Avoid broad refactors unless they directly unblock the current task.

## Shader Language Convention

> All shaders in this plan use **Slang** (`.slang`). No GLSL source or extensions (`.vert`, `.frag`, `.comp`, `.rgen`, `.rchit`) are used.
>
> - Resource bindings: use `[[vk::binding(X, Y)]]` (set `Y`, binding `X`).
> - Entry points: mark with `[shader("vertex")]`, `[shader("fragment")]`, `[shader("compute")]`, `[shader("raygeneration")]`, or `[shader("closesthit")]`.
> - Multiple related entry points (e.g., vertex + fragment, or ray generation + closest hit) should live in the same `.slang` file, matching the existing project convention.
> - Push constants: use `[[vk::push_constant]] ConstantBuffer<T>`.
> - Structured buffers, textures, and samplers follow the same `[[vk::binding(...)]]` syntax.

## Review Gates

- [x] User approves Phase 1 scope before implementation starts.
- [x] User reviews Phase 1 results before Phase 2 starts.
- [x] User reviews Phase 2 results before Phase 3 starts.
- [x] User reviews Phase 3 results before Phase 4 starts.
- [x] User reviews Phase 4 results before Phase 5 starts.
- [ ] User reviews Phase 5 results before Phase 6 starts.
- [ ] User reviews Phase 6 results before Phase 7 starts. Pending user confirmation after implementation summary.
- [ ] User reviews Phase 7 results before Phase 8 starts.

## Phase 1 - GPU-Driven Ownership And Diagnostics (Tooling Phase)

Goal: add diagnostics scaffolding so later phases can verify behavior without a debugger.

> **Note:** This phase produces zero rendering behavior changes. It is a tooling prerequisite, not a rendering implementation.

### Task 1.1 - Add Per-Pass Ownership Diagnostics

Status: Completed

Implementation:

- [x] Add `GPUDrivenOwnershipState` enum (`gpuOwned`, `bridged`, `legacy`, `disabled`) to `render/GPUDrivenRenderer.h`.
- [x] Add `GPUDrivenPassDiagnostics` struct and per-pass array to `GPUDrivenRuntimeStats` in `render/GPUDrivenRenderer.h`.
- [x] In `GPUDrivenRenderer::render()`, populate diagnostics for each pass after execution:
  - `GPUDrivenDepthPrepass` → `gpuOwned`
  - `GPUDrivenDepthPyramid` → `gpuOwned` when `HiZDepthPyramid::valid()`
  - `GPUDrivenCulling` → `gpuOwned`
  - `GPUDrivenVisibilitySortPass` → `gpuOwned` (sort execution), hybrid ordering source
  - `GPUDrivenLightCulling` → `bridged`
  - `GPUDrivenCSMShadow` → `gpuOwned` submission, `bridged` resources
  - `GPUDrivenGBuffer` → `gpuOwned` MDI, `bridged` attachments
  - `GPUDrivenLightPass` → `bridged`
  - `GPUDrivenForwardPass` → `gpuOwned` MDI, hybrid ordering
  - `GPUDrivenDebug` / `Present` / `Imgui` → explicit classification
- [x] In `app/MinimalLatestApp.h`, render the per-pass table in the existing ImGui GPU-driven diagnostics window.
- [x] Update `worklist.md` marking diagnostics complete.

Acceptance:

- The UI shows pass ownership without needing a debugger.
- No render behavior changes.
- Build and compile commands are left to the developer during this plan phase.

Likely files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/RendererFacade.h`
- `render/RendererFacade.cpp`
- `app/MinimalLatestApp.h`
- `worklist.md`
- `gpu_driven_mobile_execution_plan.md`

### Task 1.2 - Make GPU-Driven Resource Ownership Gaps Explicit

Status: Completed

Implementation:

- [x] Add `GPUDrivenResourceSummary` struct to `render/GPUDrivenRenderer.h` with counters for:
  - scene attachments owned/bridged
  - depth pyramid owned/bridged
  - visibility owned/hybrid
  - lighting resources owned/bridged
  - shadow resources owned/bridged
  - material descriptors owned/bridged
- [x] In `GPUDrivenRenderer::updateRuntimeStats()`, populate counters from active handles and pass availability flags.
- [x] In `app/MinimalLatestApp.h`, display the summary in the diagnostics panel as a bullet list.
- [x] Update `worklist.md` Task 0.1 status.

Acceptance:

- The renderer reports which shared `Renderer` systems block full GPU-driven ownership.
- No ownership migration is performed yet.

Likely files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`
- `worklist.md`
- `gpu_driven_mobile_execution_plan.md`

### Task 1.3 - Document Current Pass Graph Contract

Status: Completed

Implementation:

- [x] Add the pass-order table below to `worklist.md` under a new "Pass Graph" section.
- [x] For each pass, document inputs, outputs, mobile cost risk, and current ownership state.
- [x] Add a "Missing Passes" subsection listing future passes as separate task references.

Acceptance:

- Later phases reference a stable pass contract.
- No code behavior changes.

Likely files:

- `worklist.md`
- `gpu_driven_mobile_execution_plan.md`

Current pass graph contract:

| Order | Pass | Main inputs | Main outputs | Ownership | Mobile risk |
|---:|---|---|---|---|---|
| 1 | `GPUDrivenDepthPrepass` | persistent draw data, camera, previous/bootstrap indirect stream | scene depth | GPU-owned submission, bridged attachments | depth bandwidth, alpha-mask cost |
| 2 | `GPUDrivenDepthPyramid` | scene depth | Hi-Z mip chain, GPU culling descriptor binding | GPU-owned when valid/bound | full-res pyramid bandwidth |
| 3 | `GPUDrivenCulling` | persistent cull objects, camera, Hi-Z | indirect command buffers, draw counts, cull stats/results | GPU-owned after bootstrap | over-culling artifacts, indirect count limits |
| 4 | `GPUDrivenVisibilitySortPass` | sort key/value buffers | sorted key/value buffers | GPU-owned sort, CPU-seeded transparent distance keys | sort cost on many transparent draws |
| 5 | `GPUDrivenLightCulling` | scene depth/Hi-Z, point/spot lights | coarse light bounds | bridged shared light resources | current coarse culling does not scale like clustered lighting |
| 6 | `GPUDrivenCSMShadow` | packed shadow meshes, shadow culling buffers | CSM depth array | GPU-owned submission, bridged CSM resources | shadow bandwidth and atlas/cascade memory |
| 7 | `GPUDrivenGBuffer` | scene depth, persistent/sorted indirect stream, materials | packed GBuffer MRT | GPU-owned MDI submission, bridged attachments/material descriptors | MRT bandwidth and material texture pressure |
| 8 | `GPUDrivenLightPass` | GBuffer, scene depth, CSM, light buffers | scene output color | bridged lighting descriptors/output resources | full-res deferred lighting cost |
| 9 | `GPUDrivenForwardPass` | scene output, depth, transparent indirect stream | blended scene output | GPU-owned MDI visibility, CPU-seeded ordering | transparency overdraw |
| 10 | `GPUDrivenDebug` | GPU cull buffers/results | debug overlay | disabled | none until enabled |
| 11 | `GPUDrivenPresent` | scene output | swapchain/present target | GPU-owned present copy/blit | final copy bandwidth |
| 12 | `GPUDrivenImgui` | UI draw data, swapchain target | UI overlay | bridged app UI backend | UI overdraw only |

Missing future passes remain separate tasks:

- HDR post/final color pass.
- Exposure pass.
- Bloom downsample/upsample passes.
- Velocity pass.
- TAA/upscale resolve pass.
- Clustered light culling pass.
- IBL generation/binding passes.
- Mobile shadow atlas passes.
- AO/SSR/transparency production passes.

Phase 1 completion criteria:

- [x] Per-pass ownership is visible in diagnostics.
- [x] Current ownership gaps are explicit.
- [x] Pass graph contract is documented.
- [x] User is notified to run build/compile manually.

## Phase 2 - Same-Frame Visibility And Indirect Stream Cleanup

Goal: reduce CPU feedback in visibility, compaction, and indirect draw stream generation.

Mobile rationale: mobile needs fewer CPU/GPU sync points and predictable indirect command counts.

### Task 2.1 - Document Visibility Data Flow (Prerequisite)

Status: Completed

> **Note:** This task is a prerequisite documentation step, not a code implementation.

Implementation:

- [x] Trace current object lists from `GPUSceneRegistry` to GPU culling to indirect draws.
- [x] Identify every CPU-readback or CPU-adopted visibility path.
- [x] Document which paths are shipping, fallback, or experimental.
- [x] Update plan status and `worklist.md`.

Acceptance:

- A concrete migration list exists before shader changes.

Audit result:

- Scene object source: `GPUSceneRegistry` builds persistent cull objects and draw-uniform data.
- Visibility generation: `GPUDrivenCullingPass` writes current-frame indirect commands, draw counts, cull stats, and cull results.
- Sort input source: opaque/alpha material keys and transparent distance keys are generated on CPU, uploaded to sort buffers, then sorted on GPU.
- Opaque/alpha consumption: `GPUDrivenGBufferPass` patches a persistent indirect stream from current-frame GPU culling results, then consumes it with `drawIndexedIndirectCount`.
- Transparent consumption: `GPUDrivenForwardPass` patches the transparent slice from current-frame GPU culling results, then consumes it with `drawIndexedIndirectCount`.
- Depth prepass consumption: `GPUDrivenDepthPrepass` still uses previous-frame GPU culling or previous sorted bootstrap indirect data. This is an intentional temporal/bootstrap path, not same-frame ownership.
- CPU-readback: this phase did not find a required CPU readback for shipping visibility consumption; the hybrid pieces are CPU-seeded sort keys/order and previous-frame bootstrap.
- Experimental path: meshlet visibility remains experimental and outside Phase 2 changes.

### Task 2.2 - Make Opaque/Alpha Indirect Stream Fully Same-Frame

Status: Completed

Implementation:

- [x] Ensure `GPUDrivenGBufferPass` consumes same-frame GPU culling output for opaque and alpha mask.
- [x] Remove or isolate previous-frame/cached CPU bootstrap paths where safe.
- [x] Keep a first-frame bootstrap fallback with explicit diagnostic state.
- [x] Add counters for same-frame consumed draws vs fallback draws.

Acceptance:

- Opaque and alpha-mask draw counts are generated and consumed by GPU work in the same frame after bootstrap.
- Fallback path is visible in diagnostics.

Implementation note:

- `GPUDrivenGBufferPass` already uses same-frame `prepareAndDispatchVisibilityPatch` for opaque/alpha.
- Added `GPUDrivenVisibilityDiagnostics` and `recordGBufferVisibilityPatch` to expose same-frame opaque/alpha capacity.
- `GPUDrivenDepthPrepass` previous-frame/bootstrap use is now explicitly reported through `recordDepthPrepassVisibilitySource` rather than hidden.

### Task 2.3 - Make Transparent Visibility GPU-Authoritative

Status: Completed

Implementation:

- [x] Keep CPU-generated distance ordering only as an ordering seed.
- [x] Ensure actual transparent visibility and instance count are patched from current GPU culling results.
- [x] Add diagnostics for transparent sorted capacity, visible count, and overflow.
- [x] Add mobile-safe max transparent draw count.

Acceptance:

- Transparent draw execution is driven by GPU visibility.
- CPU ordering seed is clearly classified as hybrid, not visibility ownership.

Implementation note:

- `GPUDrivenForwardPass` now records whether the transparent visibility patch dispatched.
- Diagnostics report transparent capacity, same-frame transparent capacity, CPU ordering seed, and mobile transparent overflow against a 2048 draw default.

### Task 2.4 - Add GPU Material/PSO Binning Plan Stub

Status: Completed

Implementation:

- [x] Document current material sort key generation.
- [x] Add placeholder structures or comments for future GPU material binning only if they reduce future risk.
- [x] Do not implement a broad new batching system in this phase.

Acceptance:

- Material binning is scoped for a later phase without blocking current mobile progress.

Implementation note:

- Current material sort keys are CPU-seeded from mesh material indices before GPU bitonic sort.
- Added `materialSortKeysCpuSeeded` visibility diagnostics instead of introducing a broad batching rewrite.

Phase 2 completion criteria:

- [x] CPU feedback points are documented.
- [x] Same-frame opaque/alpha visibility is authoritative or explicitly blocked.
- [x] Transparent visibility is GPU-authoritative with CPU ordering seed documented.
- [x] Diagnostics distinguish bootstrap, hybrid, and GPU-owned visibility.
- [x] User is notified to run build/compile manually.

## Phase 3 - GPU-Owned Hi-Z Contract

Goal: make Hi-Z ownership and lifetime explicit and mobile-safe.

Mobile rationale: Hi-Z is central to GPU culling, but must not add excess bandwidth or fragile barriers.

### Task 3.1 - Separate Hi-Z Resource Contract From Shared Renderer State

Status: Completed

Implementation:

- [x] Ensure `HiZDepthPyramid` owns image, views, descriptors, generation count, source depth handle, and mobile mip policy.
- [x] Add a clear resize/update contract for GPU-driven scene extent.
- [x] Report invalid/empty Hi-Z state explicitly.
- [x] Update status docs.

Acceptance:

- Hi-Z is no longer described as an opaque shared renderer mirror in diagnostics.

Implementation note:

- `HiZDepthPyramid` now exposes source extent, pyramid extent, active/full mip counts, generation, valid state, last culling binding, mobile policy, and estimated memory.
- `GPUDrivenHiZDiagnostics` reports this contract through runtime stats and the ImGui GPU-driven diagnostics panel.

### Task 3.2 - Add Mobile Hi-Z Mip Policy

Status: Completed

Implementation:

- [x] Add configurable max mip count or minimum mip size.
- [x] Add half-resolution option if current full-resolution pyramid is too expensive for mobile.
- [x] Add diagnostics for mip count and memory estimate.

Acceptance:

- Hi-Z has a mobile-oriented quality/performance policy.

Implementation note:

- Current mobile policy keeps the existing half-source pyramid (`downsampleDivisor = 2`), clamps active mip count to `LDepthPyramidMaxMips`, and exposes `minMipSize`.
- The policy is currently fixed/defaulted, but it is now represented as data and can be promoted to a runtime quality setting in a later phase.

### Task 3.3 - Conservative Occlusion Controls

Status: Completed

Implementation:

- [x] Add controls for depth epsilon, near-plane skip, large-object skip, and fast-camera fallback.
- [x] Expose current conservative mode in diagnostics.
- [x] Keep defaults conservative for mobile.

Acceptance:

- Occlusion culling failure modes are visible and tunable.

Implementation note:

- Diagnostics now expose effective frustum/occlusion toggles, meshlet occlusion toggles, depth epsilon, conservative radius scale/bias, near reject epsilon, large-object footprint skip, camera delta, and fast-camera fallback state.
- A conservative fast-camera fallback is active: when camera delta exceeds the mobile threshold, Hi-Z occlusion is disabled for that frame while frustum culling remains available.
- Turning these values into editable runtime sliders is deferred until the UI quality-tier phase.

Phase 3 completion criteria:

- [x] Hi-Z resource ownership is explicit.
- [x] Mobile mip policy is implemented.
- [x] Conservative occlusion controls are visible.
- [x] User is notified to run build/compile manually.

## Phase 4 - Mobile HDR, Exposure, Bloom, And Color Pipeline

Goal: implement the first visible rendering upgrade that is broadly useful on mobile.

Mobile rationale: HDR plus tone mapping/exposure/bloom gives high visual return before expensive GI or geometry work.

### Task 4.1 - Implement HDR Scene Color Target

Status: Completed

Implementation:

- [x] In `render/SceneResources.h`, add `hdrSceneColor` Image handle and `hdrSceneColorFormat` (`R16G16B16A16_SFLOAT`).
- [x] In `render/SceneResources.cpp`, implement `createHdrSceneColor(uint32_t w, uint32_t h)` creating a color attachment with `COLOR_ATTACHMENT_OPTIMAL` initial layout and sampled usage for post-read.
- [x] In `GPUDrivenRenderer::createRenderTargets()`, allocate `hdrSceneColor` at scene resolution; keep existing `B8G8R8A8_UNORM` target as `ldrOutput` / present fallback.
- [x] In `GPUDrivenLightPass`, change `fragmentMain` output to write `outSceneColor` in linear HDR to the `hdrSceneColor` target.
- [x] In `GPUDrivenForwardPass`, bind `hdrSceneColor` as the blend target for transparent MDI so alpha compositing accumulates in HDR space.
- [x] In `GPUDrivenRenderer::destroyRenderTargets()`, add `hdrSceneColor` destruction.
- [x] In `app/MinimalLatestApp.h`, add diagnostics row showing `hdrSceneColor` extent/format/active state.

Acceptance:

- `GPUDrivenLightPass` and `GPUDrivenForwardPass` write to `R16G16B16A16_SFLOAT` instead of `B8G8R8A8_UNORM`.
- The LDR present target is untouched and remains valid for non-HDR fallback.
- Diagnostics report HDR target memory.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/passes/GPUDrivenLightPass.cpp`
- `render/passes/GPUDrivenForwardPass.cpp`
- `shaders/shader.light.slang`
- `app/MinimalLatestApp.h`

### Task 4.2 - Implement Fixed And Adaptive Exposure

Status: Completed

Implementation:

- [x] In `shaders/shader_io.h`, add `PostProcessPushConstants` with fixed exposure, adaptive exposure, bloom, grading, and lens controls.
- [x] In `render/passes/GPUDrivenFinalColorPass.cpp`, populate the post-process push constants from `DebugPassOptions`.
- [x] In `render/passes/GPUDrivenFinalColorPass.cpp`, bind the push constant range to the final color pipeline layout.
- [x] In `app/MinimalLatestApp.h`, add ImGui controls for fixed exposure, adaptive exposure, target luminance, and min/max auto exposure.
- [x] Pass the slider value into `GPUDrivenFinalColorPass::render()` each frame.
- [x] In `GPUDrivenPostProcessDiagnostics`, report `exposureValue`, `exposureMode`, and whether the final color path is active.

Acceptance:

- Adjusting the ImGui exposure slider visibly brightens/darkens the output.
- Adaptive exposure mode is visible in diagnostics and changes final color exposure when enabled.

Likely files:

- `shaders/shader.light.slang`
- `render/passes/GPUDrivenFinalColorPass.cpp`
- `app/MinimalLatestApp.h`
- `render/GPUDrivenRenderer.cpp`

Implementation update:

- Post-process push constants now carry fixed exposure, adaptive exposure, bloom, color grading, and lens controls.
- Adaptive exposure uses a 4x4 HDR scene-color luminance sample grid in `fragmentFinalColorMain`.
- ImGui exposes adaptive exposure, target luminance, and min/max exposure controls.
- Diagnostics report exposure value, adaptive exposure state, target luminance, and exposure range.

### Task 4.3 - Implement Mobile Bloom Chain

Status: Completed

Implementation:

- [x] In `render/passes/GPUDrivenBloomPrefilterPass.h/.cpp`, implement `GPUDrivenBloomPrefilterPass`:
  - Create `bloomPrefilter` target at half resolution (`sceneW/2`, `sceneH/2`, `R16G16B16A16_SFLOAT`).
  - Run a full-screen pass that reads `hdrSceneColor`, applies threshold, writes prefilter output.
- [x] In `render/passes/GPUDrivenBloomDownsamplePass.h/.cpp`, implement `GPUDrivenBloomDownsamplePass`:
  - Create `bloomDownsample1` at quarter resolution.
  - Run a full-screen weighted downsample from half resolution to quarter resolution.
- [x] In `GPUDrivenRenderer::render()`, insert bloom passes after forward pass and before final color:
  1. `GPUDrivenBloomPrefilterPass` (half res)
  2. `GPUDrivenBloomDownsamplePass` (quarter res)
  3. final color samples half/quarter bloom and upsamples during composite.
- [x] In `GPUDrivenFinalColorPass`, composite half/quarter bloom additively using intensity push constant.
- [x] In `app/MinimalLatestApp.h`, add `Bloom` toggle (default on), `Bloom Intensity` slider, `Bloom Threshold` slider.
- [x] In `GPUDrivenPostProcessDiagnostics`, report bloom active state, target count, and estimated memory.

Acceptance:

- Bloom is visible in the output and can be toggled.
- All bloom intermediates are half resolution or smaller.
- Memory diagnostics show bloom cost.

Likely files:

- `render/passes/GPUDrivenBloomPrefilterPass.h`
- `render/passes/GPUDrivenBloomPrefilterPass.cpp`
- `render/passes/GPUDrivenBloomDownsamplePass.h`
- `render/passes/GPUDrivenBloomDownsamplePass.cpp`
- `render/passes/GPUDrivenFinalColorPass.cpp`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`

### Task 4.4 - Implement Final Color Pass (Tone Mapping + Display Transform)

Status: Completed

Implementation:

- [x] In `render/passes/GPUDrivenFinalColorPass.h/.cpp`, implement `GPUDrivenFinalColorPass`:
  - Input: `hdrSceneColor`, optional `bloomTexture` (half res), exposure push constant.
  - Output: `ldrOutput` (`B8G8R8A8_UNORM`) or directly to swapchain.
  - Pipeline: full-screen triangle with `fragmentFinalColorMain`.
- [x] In `shaders/shader.light.slang`, implement final color fragment entry:
  - Sample HDR scene color.
  - Apply fixed or adaptive exposure.
  - Additive composite bloom (if enabled).
  - Apply ACES tone mapping (`toneMapACES` from shared header).
  - Apply mobile color grading controls and lens/vignette effects.
  - Output sRGB LDR.
- [x] In `GPUDrivenRenderer::render()`, place `GPUDrivenFinalColorPass` as the last render pass before present.
- [x] In `shaders/shader.light.slang`, keep legacy `fragmentMain` for non-GPU-driven compatibility, and route GPU-driven lighting through `fragmentHdrMain`.
- [x] In `GPUDrivenFinalColorPass`, add mobile color grading controls: saturation, contrast, gamma, and vignette.
- [x] In `GPUDrivenFinalColorPass`, add lightweight lens dirt/glare contribution from bloom.
- [x] In `app/MinimalLatestApp.h`, add diagnostics row for final color: tone mapping active, grading active, lens active, output format.

Acceptance:

- GPU-driven path ends with a dedicated final color pass.
- GPU-driven lighting shader no longer performs tone mapping.
- Output is LDR with ACES tone mapping applied.
- Mobile color grading and lens controls are active and tunable.

Likely files:

- `render/passes/GPUDrivenFinalColorPass.h`
- `render/passes/GPUDrivenFinalColorPass.cpp`
- `shaders/shader_io.h`
- `shaders/shader.light.slang`
- `render/GPUDrivenRenderer.cpp`
- `render/GPUDrivenRenderer.h`
- `app/MinimalLatestApp.h`

Phase 4 completion criteria:

- [x] HDR scene color target (`R16G16B16A16_SFLOAT`) is allocated and written by light/forward passes.
- [x] Fixed and adaptive exposure are active and adjustable from ImGui.
- [x] Bloom prefilter, downsample, and upsample passes run at half/quarter resolution and composite into final color.
- [x] Final color pass owns tone mapping and SDR output; GPU-driven lighting shader no longer tone-maps.
- [x] Mobile color grading and lens controls are active in final color.
- [x] User is notified to run build/compile manually.

## Phase 5 - Motion Vectors, TAA, And Mobile Temporal Upscaling

Goal: add temporal stability and a core mobile performance lever.

Mobile rationale: temporal upscaling is more practical on mobile than adding heavy full-resolution effects.

### Task 5.1 - Implement Previous Transform Tracking

Status: Completed

Implementation:

- [x] In `shaders/shader_io.h`, add previous camera matrices to `CameraUniforms`.
- [x] In `GPUDrivenRenderer::render()`, build a temporal camera copy before pass submission and seed previous matrices from current on the first frame.
- [x] In `shaders/shader_io.h`, add `prevModelMatrix` to `DrawUniforms` so object previous transforms are available in GPU draw-data buffers.
- [x] In `GPUDrivenRenderer::uploadPersistentDrawData()`, upload current and previous model matrices, then schedule one reset upload so static objects return to zero velocity.
- [x] On first frame, camera and object previous data resolve to current data so velocity is zero.
- [x] In `GPUDrivenRenderer::render()`, update temporal camera/history state around pass graph submission.

Acceptance:

- Camera and object previous transforms are available in GPU buffers without CPU readback.
- First frame produces zero velocity.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`

### Task 5.2 - Implement Velocity Buffer Pass

Status: Completed

Implementation:

- [x] In `render/SceneResources.h`, add `velocityBuffer` Image handle (format `R16G16_SFLOAT`, same resolution as scene depth).
- [x] In `render/SceneResources.cpp`, create and clear the velocity buffer with color-attachment and sampled usage.
- [x] In `render/passes/GPUDrivenVelocityPass.h/.cpp`, create `GPUDrivenVelocityPass`:
  - Input: current/previous camera matrices and scene depth.
  - Output: `velocityBuffer`.
  - Mobile implementation uses fullscreen depth reprojection for camera velocity; object previous matrices are present in draw buffers for a later rasterized object-velocity upgrade.
- [x] Use a dedicated pass after HDR forward rather than expanding the GBuffer MRT set.
- [x] In final color, add a velocity magnitude debug visualization.
- [x] In `app/MinimalLatestApp.h`, add grouped `Show Velocity` debug toggle.

Acceptance:

- `velocityBuffer` contains per-pixel motion vectors in NDC or screen-space units.
- Debug view shows motion as grayscale/color.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/passes/GPUDrivenVelocityPass.h`
- `render/passes/GPUDrivenVelocityPass.cpp`
- `shaders/velocity.slang` (vertex + fragment entry points)
- `render/passes/GPUDrivenGBufferPass.cpp`
- `app/MinimalLatestApp.h`

### Task 5.3 - Implement TAA Resolve Pass

Status: Completed

Implementation:

- [x] In `render/SceneResources.h`, add double-buffered `historySceneColor` Images (`R16G16B16A16_SFLOAT`).
- [x] In `shaders/shader.light.slang`, implement fullscreen TAA resolve:
  - Jitter current-frame projection by Halton(2,3) sequence offset scaled by 1/renderSize.
  - Sample current HDR scene color and history texture reprojected using velocity.
  - Neighborhood clamp in YCoCg to reject disocclusion.
  - Blend current and clamped history with `blendWeight` (0.9 default).
  - Output resolved color to the current history target.
- [x] In `render/passes/GPUDrivenTAAResolvePass.h/.cpp`, implement `GPUDrivenTAAResolvePass`:
  - Render a fullscreen resolve at current render resolution.
  - Bind `hdrSceneColor`, `historySceneColor`, `velocityBuffer`, and temporal push constants.
- [x] In `GPUDrivenRenderer::render()`, place TAA resolve after forward pass and before bloom:
  1. Reproject history into the current history target.
  2. Sample the current history target as the temporal scene-color source for bloom/final color.
- [x] In `app/MinimalLatestApp.h`, add grouped `TAA` toggle, `TAA Jitter Scale`, and `TAA Blend Weight` sliders.
- [x] Add a push-constant placeholder field in `PostProcessPushConstants::params5`; reactive mask texture writing remains a future transparent/emissive task.

Acceptance:

- TAA toggle visibly changes image stability under camera motion.
- History buffer is correctly reprojected; ghosting is minimal.
- Jitter changes each frame and resets on camera cut.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/passes/GPUDrivenTAAResolvePass.h`
- `render/passes/GPUDrivenTAAResolvePass.cpp`
- `shaders/taa_resolve.slang`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`

### Task 5.4 - Implement Mobile Temporal Upscaling

Status: Explicitly blocked for internal render-size decoupling; UI/diagnostics landed

Implementation:

- [x] In `render/Renderer.h`, add `renderScale` float control (default 1.0).
- [ ] In `GPUDrivenRenderer::createRenderTargets()`, size `hdrSceneColor`, `velocityBuffer`, and depth targets by `renderScale * displaySize`. Blocked by the current single-size `SceneResources` model where GBuffer, depth, HDR, output, viewport, and present source all share one extent.
- [x] In `GPUDrivenFinalColorPass`, keep the output path compatible with temporal or spatial source selection:
  - If `renderScale < 1.0` and TAA is enabled, use TAA history at display resolution as the upscale source (TAA already outputs display size).
  - If TAA is disabled, fall back to `bilinear upscale` in `final_color.slang` or a dedicated `spatial_upscale.slang` pass that reads the lower-res scene color and writes display-res output.
- [ ] In `GPUDrivenTAAResolvePass`, ensure output resolution is display resolution when upscaling is active. Blocked until display/internal extents are decoupled.
- [x] In `app/MinimalLatestApp.h`, add grouped `Render Scale` slider (0.5-1.0) and `Upscaling Mode` dropdown (TAA / Spatial / Off).
- [x] In `GPUDrivenPostProcessDiagnostics`, report internal resolution, display resolution, upscale mode, and the active render-scale block.

Acceptance:

- Changing render scale below 1.0 increases performance and the image is reconstructed to display resolution.
- When TAA is off, spatial fallback still reconstructs to display resolution without crashing.
- Diagnostics show internal vs display resolution.

Likely files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/passes/GPUDrivenFinalColorPass.cpp`
- `render/passes/GPUDrivenTAAResolvePass.cpp`
- `shaders/final_color.slang`
- `shaders/spatial_upscale.slang`
- `app/MinimalLatestApp.h`

Phase 5 completion criteria:

- [x] Motion vectors exist.
- [x] TAA resolve exists.
- [x] Temporal upscaling exists or is explicitly blocked.
- [x] User is notified to run build/compile manually.

## Phase 6 - Clustered Lighting And IBL Baseline

Goal: improve direct and environment lighting without moving to expensive GI first.

Mobile rationale: bounded clustered lighting plus IBL is a practical mobile feature set.

Phase 6 target:

- Replace the current coarse screen-tile local-light bounds with a bounded clustered/froxel path that can scale to many point and spot lights on mobile.
- Keep the existing coarse light-culling path as a runtime fallback until clustered diagnostics prove stable.
- Treat the current BC6H/HDR equirect environment upload, skybox pass, and equirect diffuse baseline as the first IBL slice, not the final split-sum IBL implementation.
- Finish Phase 6 only when cluster buffers, cluster culling, light-pass consumption, IBL fallback, and diagnostics are all visible in the GPU-driven diagnostics panel.

Pass graph insertion:

| Order | Pass | Main inputs | Main outputs | Ownership target | Fallback |
|---:|---|---|---|---|---|
| 5a | `GPUDrivenClusteredLightCulling` | scene depth or Hi-Z, camera, local light buffers | cluster light counts, cluster light indices, occupancy/overflow stats | GPU-owned compute | existing `GPUDrivenLightCulling` coarse bounds |
| 8 | `GPUDrivenLightPass` | GBuffer, depth, CSM, clustered light lists, IBL textures | HDR scene color | GPUDriven-owned light/IBL descriptors and light pipeline | coarse light loop + flat ambient |

Mobile budget defaults:

- Cluster grid: `16 x 9 x 24` froxels at 1080p-class output.
- Max lights per cluster: `32`.
- Target cluster-list memory: about `432 KiB` for indices plus `14 KiB` for counts at the default grid.
- Dispatch granularity: one thread or one small workgroup per cluster; avoid per-pixel light-list construction.
- Low-tier policy: clustered lighting can be disabled from UI/quality tier and must automatically fall back to coarse light culling.

### Task 6.1 - Implement Clustered Light Data Layout And Buffers

Status: Completed - GPUDriven-owned light/cluster buffers, fixed-capacity lists, overflow encoding, and occupancy readback are implemented

Implementation:

- [x] In `shaders/shader_io.h`, add shared constants:
  - `LClusterGridSizeX = 16`
  - `LClusterGridSizeY = 9`
  - `LClusterGridSizeZ = 24`
  - `LMaxLightsPerCluster = 32`
  - `LClusterOverflowBit = 0x80000000u`
  - `LClusterCountMask = 0x7fffffffu`
- [x] Add `ClusteredLightUniforms` in `shaders/shader_io.h`:
  - `vec4 screenSizeAndClusterInfo` (`xy = render extent`, `z = cluster count xy`, `w = cluster count z`)
  - `vec4 clipInfo` (`x = near`, `y = far`, `z = linear/depth slicing mode`, `w = max lights per cluster`)
  - `mat4 viewMatrix`
  - `mat4 projectionMatrix`
  - `mat4 invProjectionMatrix`
  - `mat4 invViewMatrix`
  - `vec4 debugInfo` (`x = enable clustered`, `y = show heatmap`, `z = force overflow fallback`, `w = reserved`)
- [x] In the GPU-driven resource owner, add a `LightClusterBuffer` or equivalent resource struct:
  - `clusterLightCounts` SSBO: `uint32_t[clusterCount]`
  - `clusterLightIndices` SSBO: `uint32_t[clusterCount * LMaxLightsPerCluster]`
  - optional `clusterStats` SSBO: max occupancy, overflow clusters, tested lights, appended lights
- [x] Allocate buffers with storage-buffer usage and clear counts/stats at the start of the clustered culling pass.
- [x] Expose pass/resource handles for the pass graph:
  - `kPassClusterLightCountsHandle`
  - `kPassClusterLightIndicesHandle`
  - `kPassClusterLightStatsHandle`
  - `kPassClusteredLightUniformHandle`
- [x] Create and destroy clustered buffers alongside GPU-driven renderer lifecycle, not as hidden legacy `Renderer` state.
- [x] Recreate clustered buffers on scene extent changes only when grid dimensions or max list capacity change. Current Phase 6 grid dimensions and list capacity are fixed mobile constants, so scene extent changes only update uniforms/descriptors and do not require buffer reallocation.
- [x] Add overflow behavior: if lights per cluster exceed `LMaxLightsPerCluster`, set `LClusterOverflowBit` in the cluster count and keep the count clamped to `LMaxLightsPerCluster`.
- [x] In `GPUDrivenRuntimeStats`, add `GPUDrivenClusteredLightingDiagnostics` with:
  - enabled/fallback state
  - grid dimensions and cluster count
  - max lights per cluster
  - allocated byte count
  - max occupancy
  - overflow cluster count
  - total appended light references
  - point/spot lights tested
- [x] In `app/MinimalLatestApp.h`, display cluster grid dimensions, buffer memory, max occupancy, appended references, overflow count, and active fallback.

Acceptance:

- Cluster buffers are allocated with explicit dimensions and memory size.
- The layout is shared between C++ and Slang through `shader_io.h`.
- Overflow is represented without writing past the fixed mobile list capacity.
- Diagnostics show cluster occupancy and whether the renderer is using clustered or fallback lighting.

Likely files:

- `shaders/shader_io.h`
- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `app/MinimalLatestApp.h`

### Task 6.2 - Implement Clustered Light Culling Compute Pass

Status: Completed - GPUDriven-owned clustered pass, dispatch, barriers, stats, and view-space froxel AABB culling are implemented for point and spot lights; area lights and logarithmic/depth-derived froxels are deferred beyond the Phase 6 mobile baseline

Implementation:

- [x] In `render/passes/GPUDrivenClusteredLightCullingPass.h/.cpp`, implement `GPUDrivenClusteredLightCullingPass`:
  - Dispatch compute shader `clustered_light_cull.slang`.
  - Dependencies:
    - scene depth or depth pyramid read
    - point/spot light buffers read
    - clustered uniforms read
    - cluster count/index/stat buffers write
  - Work group shape: one cluster per invocation for the first implementation; a later optimization may use one workgroup per screen tile and loop z slices.
- [x] In `shaders/clustered_light_cull.slang`, implement:
  - [x] Clear `clusterLightCounts` and stats before append using command-buffer reset.
  - [x] Reconstruct each froxel from screen tile bounds and linear depth-slice bounds using inverse projection.
  - [x] Use the same linear view-depth z slicing in culling and fragment consumption. Logarithmic/depth-derived froxels are explicitly deferred to a later quality pass after the baseline is stable.
  - [x] Use conservative view-space sphere-vs-cluster-AABB tests for point lights.
  - [x] Conservatively sphere-test spot lights against the cluster bounds and encode spot references with `LClusterLightTypeSpotBit`.
  - [x] Append encoded light references into `clusterLightIndices[cluster * LMaxLightsPerCluster + localIndex]`.
  - [x] Encode light type in the high bits consistently with existing light buffers.
  - [x] If append count exceeds `LMaxLightsPerCluster`, set `LClusterOverflowBit` and increment overflow diagnostics.
- [x] Add memory barriers from compute writes to fragment reads before `GPUDrivenLightPass`.
- [x] In `GPUDrivenRenderer::render()` / pass executor setup, place `GPUDrivenClusteredLightCullingPass` after depth/Hi-Z generation and before `GPUDrivenLightPass`.
- [x] Keep fallback:
  - UI toggle disables clustered pass and runs existing `GPUDrivenLightCulling`.
  - Any invalid cluster resource, descriptor failure, or overflow fallback policy uses the coarse/full light path.
  - The fallback path must not require changing shader bindings at runtime.

Acceptance:

- Clustered culling dispatches after depth is available and before lighting.
- Point and spot lights are culled per cluster.
- Buffer barriers make the cluster list visible to the fragment shader.
- Overflow never causes out-of-bounds writes.
- Coarse fallback remains functional when toggle is off.

Likely files:

- `render/passes/GPUDrivenClusteredLightCullingPass.h`
- `render/passes/GPUDrivenClusteredLightCullingPass.cpp`
- `shaders/clustered_light_cull.slang`
- `shaders/shader_io.h`
- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `render/Pass.h`

### Task 6.3 - Consume Clustered Light Lists In The HDR Light Pass

Status: Completed - HDR light pass consumes GPUDriven-owned cluster counts/indices for point and spot lights, falls back on overflow/disabled clustered mode, and exposes heatmap/overflow visualization

Implementation:

- [x] In `shaders/shader.light.slang`, add a clustered branch inside the existing HDR entry point.
- [x] Bind clustered-light resources to the existing lighting descriptor layout:
  - clustered uniforms
  - cluster counts
  - cluster indices
  - optional cluster stats/debug buffer
- [x] In the shader, compute cluster ID from:
  - pixel position or UV for `x/y`
  - linear view depth for `z`
  - the same z-slice formula used by `clustered_light_cull.slang`
- [x] Read `clusterLightCounts[clusterId]`:
  - `count = packedCount & LClusterCountMask`
  - `overflow = (packedCount & LClusterOverflowBit) != 0`
- [x] For non-overflow clusters, loop only over `clusterLightIndices`.
- [x] For overflow clusters, fall back to the existing full/coarse local-light evaluation for that pixel.
- [x] Keep directional light and CSM shadowing unchanged in this phase.
- [x] Preserve the current HDR output target (`hdrSceneColor`) and existing final-color/tone-map pipeline.
- [x] Add clustered debug visualization:
  - heatmap mode for per-cluster count
  - overflow highlight mode
  - fallback/coarse mode indicator
- [x] In `GPUDrivenLightPass.cpp`, use the clustered-capable GPUDriven-owned light pipeline.
- [x] Keep the current coarse light path available through shader/runtime fallback.

Acceptance:

- Lighting shader only iterates lights in the current cluster on the clustered path.
- Directional light, cascaded shadowing, emissive, and HDR output remain unchanged.
- Overflow clusters and disabled clustered mode render with the existing fallback instead of black pixels or missing local lights.
- Cluster heatmap can be shown from the diagnostics UI.

Likely files:

- `shaders/shader.light.slang`
- `shaders/shader_io.h`
- `render/passes/GPUDrivenLightPass.cpp`
- `render/passes/GPUDrivenLightPass.h`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`

### Task 6.4 - Implement IBL Resource Loading And Generation

Status: Completed baseline - GPUDriven-owned mobile BC6H/HDR KTX2 equirect upload, skybox validation, fallback, and split-sum readiness diagnostics are implemented; cube convolution and BRDF LUT generation are explicitly deferred

Implementation:

- [x] Add mobile baseline IBL environment upload:
  - Load `resources/environment/lilienstein_4k.ktx2` as a BC6H/HDR equirect KTX2 texture.
  - Upload its mip chain as a sampled 2D Vulkan image.
  - Bind it into the existing light-pass texture array.
- [x] During renderer init, attempt IBL load; on failure, fall back to simple sky ambient and log diagnostic.
- [x] In `GPUDrivenRuntimeStats`, report IBL state (loaded/fallback/empty).
- [x] Add `GPUDrivenSkyboxPass` as a visible validation path for loaded HDRI.
- [x] Add final IBL image handles in the GPU-driven resource owner as deferred readiness state for Phase 6 diagnostics:
  - `irradianceMap` (64x64x6 cubemap, `R16G16B16A16_SFLOAT`)
  - `prefilteredEnvMap` (256x256x6 mip chain, `R16G16B16A16_SFLOAT`)
  - `brdfLut` (256x256 2D, `R16G16_SFLOAT`)
- [x] Choose the shipping generation path before coding:
  - Preferred mobile path: offline-generate irradiance cube, GGX-prefiltered environment cube, and BRDF/DFG LUT into KTX2 assets, then upload at runtime.
  - Development fallback: runtime compute generation from EXR/HDR/equirect assets for desktop or tools builds only.
- [x] If runtime generation is kept, implement `loadIBLFromHDR(path)`: explicitly not kept for the Phase 6 mobile runtime baseline; offline-generated KTX2 split-sum assets are the chosen shipping path.
  - Load equirectangular HDR using `stb_image`, EXR loader, or existing image loader.
  - Dispatch `equirect_to_cube.slang` to project to cubemap.
  - Dispatch `compute_irradiance.slang` to convolve irradiance.
  - Dispatch `compute_prefilter.slang` per mip to generate GGX-prefiltered specular.
  - Generate `brdfLut` offline or dispatch `compute_brdf_lut.slang` once.
- [x] Implement `createSimpleSkyAmbientFallback()`:
  - no-crash fallback when IBL files are missing
  - flat ambient color for low-tier mobile
  - valid fallback descriptors through the existing light-pass texture array
- [x] Add diagnostics fields for final IBL:
  - source mode (`flat`, `equirect`, `cube_precomputed`, `runtime_generated`)
  - irradiance valid
  - prefiltered valid
  - BRDF LUT valid
  - total estimated memory
  - generation/upload status

Implementation update:

- The first Phase 6 IBL slice uses the prepared BC6H/HDR KTX2 panorama directly as a mobile-feasible baseline instead of generating cube resources during startup.
- The GPU-driven path now owns its equirect environment image/view/status instead of reading the legacy `Renderer` IBL resource.
- The default runtime asset must be stored with `KTX_SS_NONE`; Zstd-supercompressed KTX2 remains an offline input format unless libktx/zstd is added to the runtime.
- `GPUDrivenSkyboxPass` now depth-tests against reverse-Z clear depth and writes the loaded HDRI into `hdrSceneColor` before transparent forward rendering.
- The equirect light-pass baseline now uses the panorama for diffuse sky light and background only; specular IBL is disabled because ordinary panorama mips are not GGX-prefiltered reflection data.
- Runtime diagnostics report path, format, extent, mip count, memory, enabled state, intensity, and fallback state.
- Full irradiance cube / prefiltered cube / BRDF LUT generation remains deferred; Phase 6 reports those resources as `Deferred` rather than pretending they are valid.

Acceptance:

- The mobile baseline still works with only `resources/environment/lilienstein_4k.ktx2`.
- Final IBL textures contain valid irradiance and prefiltered data, not placeholders.
- All IBL descriptors are valid even when asset loading fails.
- Fallback path produces flat ambient without crashing.
- Diagnostics report baseline equirect state and final split-sum IBL resource state separately.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/Renderer.h`
- `render/Renderer.cpp`
- `shaders/equirect_to_cube.slang`
- `shaders/compute_irradiance.slang`
- `shaders/compute_prefilter.slang`
- `shaders/compute_brdf_lut.slang`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`

### Task 6.5 - Integrate IBL Into GPU-Driven Light Pass

Status: Completed baseline - GPUDriven-owned equirect IBL is integrated in the light pass and skybox, IBL debug modes are exposed, and split-sum cube-map sampling remains explicitly deferred until precomputed resources exist

Implementation:

- [x] In `shaders/shader_io.h`, add light-pass IBL controls (`enabled`, `intensity`, max mip, valid texture).
- [x] Replace existing hardcoded ambient term with diffuse IBL contribution scaled by `iblIntensity` in the current HDR light pass.
- [x] Bind IBL through the existing combined texture array used by the lighting pipeline.
- [x] In `app/MinimalLatestApp.h`, add `IBL Intensity` slider and `Use IBL` toggle.
- [x] When IBL toggle is off, revert to simple ambient constant.
- [x] In the clustered HDR light path, after direct lighting, sample the current equirect fallback:
  - equirect panorama with normal `N` for indirect diffuse
  - equirect panorama with reflection vector `R` only in debug/specular placeholder mode
  - split-sum `irradianceMap`, `prefilteredEnvMap`, and `brdfLut` are deferred until precomputed resources exist
- [x] Use split-sum approximation: `prefiltered * (F * lut.x + lut.y)`. Deferred in runtime output because Phase 6 has no prefiltered cube/BRDF LUT resources; shader-side equirect specular fallback remains disabled.
- [x] Apply AO only to indirect diffuse, not direct light or direct specular.
- [x] Keep current equirect sampling as a labeled fallback when split-sum resources are unavailable.
- [x] Ensure skybox/background and material IBL use the same environment selection and intensity controls.
- [x] Add IBL debug modes:
  - diffuse only
  - specular only
  - fallback ambient only
  - environment background only

Acceptance:

- Baseline PBR has environment diffuse/specular contribution.
- Split-sum IBL is used when irradiance/prefilter/BRDF resources are valid.
- Equirect fallback remains available and explicitly reported.
- Toggle disables IBL and returns to flat ambient without artifacts.

Likely files:

- `shaders/shader.light.slang`
- `shaders/shader_io.h`
- `render/passes/GPUDrivenLightPass.cpp`
- `render/passes/GPUDrivenLightPass.h`
- `app/MinimalLatestApp.h`

### Task 6.6 - Phase 6 Diagnostics, Quality Controls, And Review Package

Status: Completed - UI controls and diagnostics cover clustered resources, occupancy/overflow, fallback, IBL baseline, IBL debug modes, and explicit split-sum deferred readiness

Implementation:

- [x] Add UI controls:
  - `Use Clustered Lighting`
  - `Cluster Heatmap`
  - `Cluster Overflow Highlight`
  - `Use IBL`
  - `IBL Intensity`
  - `IBL Debug Mode`
- [x] Add diagnostics table entries:
  - clustered pass ownership (`gpuOwned`, `bridged`, `disabled`, or `fallback`)
  - coarse fallback reason
  - cluster grid and memory
  - max occupancy / overflow count
  - IBL source mode and valid resources
  - skybox enabled/disabled
- [x] Add log messages for:
  - clustered resources created/destroyed
  - clustered fallback activation
  - IBL load success/failure
  - split-sum IBL resource readiness
- [x] Update `worklist.md` after each completed Phase 6 subtask:
  - P0 Task 4.1 clustered lighting
  - P1 Task 5.1 IBL integration
- [x] Update this plan's review gate when Phase 6 is complete.
- [x] Keep build/compile execution manual during this plan phase.

Acceptance:

- User can verify clustered vs fallback lighting from UI without a debugger.
- User can verify IBL baseline vs split-sum resource state from UI without a debugger.
- Phase 6 review package clearly lists completed work, deferred work, and manual build request.

Likely files:

- `render/GPUDrivenRenderer.h`
- `render/GPUDrivenRenderer.cpp`
- `app/MinimalLatestApp.h`
- `worklist.md`
- `gpu_driven_mobile_execution_plan.md`

Phase 6 completion criteria:

- [x] Clustered lighting path exists with fallback.
- [x] Clustered lighting diagnostics show grid, occupancy, overflow, memory, and fallback reason.
- [x] IBL baseline resource is valid or flat fallback is active with valid descriptors.
- [x] Split-sum IBL resources are either valid or explicitly deferred with equirect fallback status.
- [x] GPU-driven light pass uses clustered local lights when enabled.
- [x] GPU-driven light pass uses IBL and can revert to flat ambient.
- [x] Existing HDR, TAA/final color, CSM, skybox, and transparent forward paths still have valid inputs after Phase 6 changes.
- [x] User is notified to run build/compile manually.

## Phase 7 - Mobile Shadows, AO, Reflections, And Transparency

Goal: add the next set of visible effects while keeping mobile cost bounded.

### Task 7.1 - Implement Mobile Tiled Shadow Atlas

Status: Baseline Implemented - GPUDriven-owned shadow atlas image, pass node, fixed-size tile rendering for directional CSM cascades, UI toggle, diagnostics, and explicit CSM lighting fallback exist. Local light atlas tiles and atlas sampling are still deferred.

Implementation:

- [x] In GPUDriven-owned resources, add `shadowAtlas` Image (2048x2048 depth atlas).
- [x] In `render/passes/GPUDrivenShadowAtlasPass.h/.cpp`, implement `GPUDrivenShadowAtlasPass` baseline:
  - Allocate fixed 512x512 atlas tiles for directional cascades.
  - Render packed shadow casters into the GPUDriven-owned atlas using the existing CSM shadow pipeline and MDI draw data.
- [ ] Extend atlas allocation per local light/cascade using a simple CPU-side tile allocator (`ShadowAtlasAllocator`).
- [ ] For each visible local light from GPU culling results, render shadow tiles using MDI into the atlas.
- [ ] In `shaders/shadow_atlas.slang`, render depth from light POV; sample material alpha mask if needed.
- [ ] In `shaders/shader.light.slang`, modify shadow sampling:
  - Compute atlas UV from light space position + tile offset/scale.
  - Sample `shadowAtlas` instead of separate CSM array.
- [x] Keep `GPUDrivenCSMShadow` as fallback; add toggle `Use Shadow Atlas` in ImGui.
- [x] In `GPUDrivenRuntimeStats`, report atlas utilization, tile count, and fallback state.

Acceptance:

- Directional cascade tiles render into the shared atlas baseline.
- Local light atlas tiles are still deferred.
- Lighting shader atlas sampling is still deferred; CSM remains the active lighting source.
- CSM fallback remains functional.

Likely files:

- `render/SceneResources.h`
- `render/SceneResources.cpp`
- `render/passes/GPUDrivenShadowAtlasPass.h`
- `render/passes/GPUDrivenShadowAtlasPass.cpp`
- `shaders/shadow_atlas.slang` (vertex + fragment entry points)
- `shaders/shader.light.slang`
- `app/MinimalLatestApp.h`

### Task 7.2 - Implement Contact Shadows Pass

Status: Skipped by user request for this Phase 7 pass

Implementation:

- [ ] In `render/passes/GPUDrivenContactShadowsPass.h/.cpp`, implement `GPUDrivenContactShadowsPass`:
  - Dispatch compute shader `contact_shadows.slang` at half resolution.
  - Input: scene depth, normal (from GBuffer), light direction.
  - Trace 8-16 steps in screen space toward light; early-out on depth hit within thickness threshold.
  - Output `contactShadows` texture (R8_UNORM).
- [ ] In `shaders/contact_shadows.slang`, implement:
  - Ray march in view space or screen space.
  - Use bilateral depth comparison with object thickness heuristic.
  - Output 1.0 = lit, 0.0 = occluded.
- [ ] In `shaders/shader.light.slang`, multiply directional light shadow term by `contactShadows` sample.
- [ ] In `app/MinimalLatestApp.h`, add `Contact Shadows` toggle, `Contact Shadow Distance` slider, `Contact Shadow Steps` slider.
- [ ] Disable contact shadows on low-tier mobile by default (detected by GPU name or manual quality tier).

Acceptance:

- Contact shadows appear within short distance of occluders.
- Toggle disables pass entirely without artifacts.
- Step count is bounded and visible in diagnostics.

Likely files:

- `render/passes/GPUDrivenContactShadowsPass.h`
- `render/passes/GPUDrivenContactShadowsPass.cpp`
- `shaders/contact_shadows.slang`
- `shaders/shader.light.slang`
- `app/MinimalLatestApp.h`

### Task 7.3 - Implement Half-Resolution GTAO Pass

Status: Completed baseline - GPUDriven-owned half-resolution AO resources, GTAO compute, denoise compute, UI controls, diagnostics, and light-pass indirect AO integration are implemented

Implementation:

- [x] In `render/passes/GPUDrivenAOPass.h/.cpp`, implement `GPUDrivenAOPass`:
  - Create `aoRaw` target at half resolution (`R16_SFLOAT` for storage-image compatibility).
  - Dispatch `gtao.slang` compute shader.
- [x] In `shaders/gtao.slang`, implement:
  - Reconstruct view-space position from depth.
  - Sample normal from GBuffer.
  - Use horizon-based ambient occlusion with 2-4 directions and 4-6 steps (mobile reduced).
  - Apply falloff and inner radius to avoid halo artifacts.
  - Write raw AO to `aoRaw`.
- [x] In `render/passes/GPUDrivenAOPass.cpp`, implement bilateral denoise:
  - Dispatch `ao_denoise.slang` (2 passes: horizontal + vertical).
  - Use edge-aware blur with depth and normal weights.
  - Output `aoDenoised`.
- [x] In `shaders/shader.light.slang`, sample `aoDenoised` and multiply indirect diffuse (IBL + ambient) by AO; do not apply AO to direct specular.
- [x] In `app/MinimalLatestApp.h`, add `AO` toggle, `AO Radius`, `AO Intensity` sliders.
- [x] In `GPUDrivenRuntimeStats`, report AO resolution and memory readiness.

Acceptance:

- AO is visible in corners and crevices.
- Denoised output has minimal noise under camera motion.
- AO only affects indirect lighting, not direct specular.

Likely files:

- `render/passes/GPUDrivenAOPass.h`
- `render/passes/GPUDrivenAOPass.cpp`
- `shaders/gtao.slang`
- `shaders/ao_denoise.slang`
- `shaders/shader.light.slang`
- `app/MinimalLatestApp.h`

### Task 7.4 - Implement SSR With Probe Fallback

Status: Completed baseline - GPUDriven-owned half-resolution SSR trace resources, UI controls, diagnostics, and light-pass reflection add are implemented; probe fallback remains IBL

Implementation:

- [x] In `render/passes/GPUDrivenSSRPass.h/.cpp`, implement `GPUDrivenSSRPass`:
  - Create `ssrRaw` target at half resolution (`R16G16B16A16_SFLOAT` for color + confidence).
  - Dispatch `ssr_trace.slang`.
- [x] In `shaders/ssr_trace.slang`, implement:
  - Reconstruct view-space position and reflection vector from GBuffer roughness/metalness.
  - Hi-Z ray march using `HiZDepthPyramid` for acceleration.
  - Trace up to 64 steps; early-out on miss.
  - Sample `hdrSceneColor` at hit point.
  - Output color and hit confidence (0 = miss, 1 = hit).
- [x] In `shaders/shader.light.slang`, in HDR light pass:
  - Sample `ssrRaw`.
  - If confidence > threshold, blend SSR into specular reflection.
  - If confidence < threshold, fall back to `prefilteredEnvMap` (IBL).
  - Scale by Fresnel and roughness.
- [x] In `app/MinimalLatestApp.h`, add `SSR` toggle, `SSR Max Steps`, `SSR Thickness` sliders.
- [x] Disable SSR on low-tier mobile by default; IBL remains the baseline reflection source.

Acceptance:

- Smooth surfaces show screen-space reflections where valid.
- Missed rays fall back to IBL without visible pop.
- Pass runs at half resolution.

Likely files:

- `render/passes/GPUDrivenSSRPass.h`
- `render/passes/GPUDrivenSSRPass.cpp`
- `shaders/ssr_trace.slang`
- `shaders/shader.light.slang`
- `render/HiZDepthPyramid.h`
- `app/MinimalLatestApp.h`

### Task 7.5 - Implement Production Transparency Path

Status: Skipped by user request for this Phase 7 pass

Implementation:

- [ ] In `render/passes/GPUDrivenForwardPass.cpp`, keep existing GPU-sorted MDI as the mobile baseline.
- [ ] Add weighted blended OIT as optional path:
  - Create `transparencyAccum` (`R16G16B16A16_SFLOAT`) and `transparencyRevealage` (`R8_UNORM`) targets.
  - In `shaders/forward_oit.slang`, write weighted color to accum and weight to revealage.
  - In `render/passes/GPUDrivenOITCompositePass.cpp`, composite over opaque scene color using `accum.rgb / accum.a` and `revealage`.
- [ ] In `app/MinimalLatestApp.h`, add `Transparency Mode` dropdown: `Sorted MDI`, `Weighted OIT`.
- [ ] Memory budget gate: if estimated VRAM usage with OIT targets exceeds budget (query `GPUDrivenRuntimeStats::estimatedMemoryMB`), force `Sorted MDI` and warn in UI.
- [ ] Explicitly do not implement per-pixel linked lists in the mobile baseline.

Acceptance:

- Sorted MDI works for all transparent draws.
- Weighted OIT is selectable when memory budget allows.
- Low-memory devices default to sorted MDI without user intervention.

Likely files:

- `render/passes/GPUDrivenForwardPass.cpp`
- `render/passes/GPUDrivenOITCompositePass.h`
- `render/passes/GPUDrivenOITCompositePass.cpp`
- `shaders/forward_oit.slang`
- `shaders/oit_composite.slang`
- `app/MinimalLatestApp.h`
- `render/GPUDrivenRenderer.cpp`

Phase 7 completion criteria:

- [x] Mobile shadow atlas direction is implemented or blocked with reason.
- [x] AO exists.
- [x] SSR/probe reflection tier exists.
- [x] Transparency path is skipped by user request for this pass.
- [x] User is notified to run build/compile manually.

## Phase 8 - Optional High-End And Long Tail

Goal: add advanced features only after the mobile baseline is stable.

### Task 8.1 - Implement Mobile-Scaled Meshlet/Cluster Path

Status: Pending

Implementation:

- [ ] In `render/GPUDrivenMeshletProcessPass.h/.cpp`, implement `GPUDrivenMeshletProcessPass`:
  - Read `MeshletClusterBuffer` and `MeshletCullData` from GPU scene.
  - Dispatch `meshlet_cull.slang` with cluster-frustum and cluster-occlusion tests.
  - Output `MeshletIndirectCommandBuffer` (indexed indirect).
- [ ] In `render/passes/GPUDrivenGBufferPass.cpp`, add meshlet indirect draw path:
  - If meshlet data is present, call `drawIndexedIndirect` from `MeshletIndirectCommandBuffer`.
  - Otherwise, fall back to object-level MDI.
- [ ] In `render/SceneResources.h`, add `MeshletClusterBuffer` and `MeshletCullData` handles.
- [ ] In `GPUDrivenRenderer::updateObjectData()`, build meshlet cull data from loaded meshlets (offline asset path).
- [ ] Add mobile tier limit: clamp max meshlets per object to 4096 on low-tier, 16384 on high-tier; log diagnostic when clamping occurs.

Acceptance:

- Meshlet culling runs on GPU and produces valid indirect commands.
- Objects without meshlets render correctly via MDI fallback.
- Mobile tier limit prevents excessive memory use.

Likely files:

- `render/passes/GPUDrivenMeshletProcessPass.h`
- `render/passes/GPUDrivenMeshletProcessPass.cpp`
- `shaders/meshlet_cull.slang`
- `render/passes/GPUDrivenGBufferPass.cpp`
- `render/SceneResources.h`
- `render/GPUDrivenRenderer.cpp`

### Task 8.2 - Implement Mobile-Scaled GI Beyond Screen Space

Status: Pending

Implementation:

- [ ] In `render/SceneResources.h`, add `irradianceVolume` texture (3D or 2D atlas, low resolution e.g., 32x32x32 per probe).
- [ ] In `render/passes/GPUDrivenProbeUpdatePass.h/.cpp`, implement `GPUDrivenProbeUpdatePass`:
  - Dispatch `probe_update.slang` once per N frames (staggered).
  - Ray trace or rasterize a low-resolution cubemap per probe from probe position.
  - Filter and write into `irradianceVolume`.
- [ ] In `shaders/shader.light.slang`, sample `irradianceVolume` trilinearly using world position for indirect diffuse.
- [ ] Add optional SSGI pass `GPUDrivenSSGIPass`:
  - Dispatch `ssgi.slang` at half resolution.
  - Ray march in screen space for indirect diffuse.
  - Fallback to probe data when screen-space ray misses.
- [ ] Add quality toggles: `Use Probes`, `Use SSGI`, `Probe Update Rate`.
- [ ] Low-tier mobile uses probes only (no SSGI); high-tier enables SSGI.

Acceptance:

- Indirect diffuse is visible from probes and/or SSGI.
- Fallback from SSGI to probes is seamless.
- Probe updates do not run every frame on mobile.

Likely files:

- `render/SceneResources.h`
- `render/passes/GPUDrivenProbeUpdatePass.h`
- `render/passes/GPUDrivenProbeUpdatePass.cpp`
- `shaders/probe_update.slang`
- `shaders/shader.light.slang`
- `render/passes/GPUDrivenSSGIPass.h`
- `render/passes/GPUDrivenSSGIPass.cpp`
- `shaders/ssgi.slang`
- `app/MinimalLatestApp.h`

### Task 8.3 - Implement Fog, Sky, Decals, And Special Surfaces

Status: Pending

Implementation:

- [ ] In `shaders/height_fog.slang`, implement height fog:
  - Exponential fog with height falloff using camera and world position.
  - Sample `fogDensityTexture` (optional 2D scroll for animated fog).
  - Composite in `GPUDrivenFinalColorPass` before tone mapping.
- [ ] In `render/passes/GPUDrivenSkyPass.h/.cpp`, implement `GPUDrivenSkyPass`:
  - Draw sky dome or full-screen pass with procedural sky or cubemap.
  - If HDRI sky is loaded, sample it; otherwise use procedural gradient.
- [ ] In `render/passes/GPUDrivenDecalPass.h/.cpp`, implement `GPUDrivenDecalPass`:
  - Allocate decal buffer (transform + material index).
  - Dispatch `decal_apply.slang` that projects decals onto GBuffer (normal + albedo + roughness).
  - Limit to 64 active decals on mobile.
- [ ] In `app/MinimalLatestApp.h`, add toggles: `Height Fog`, `Volumetric Fog (Low-Res)`, `Decals`.
- [ ] Water/skin/hair/cloth: defer to material shader variants (BRDF model changes), not new passes.

Acceptance:

- Fog is visible and affects scene depth.
- Sky renders correctly behind geometry.
- Decals modify GBuffer without full mesh overdraw.
- Mobile limits (64 decals, no volumetric fog on low-tier) are enforced.

Likely files:

- `shaders/height_fog.slang`
- `render/passes/GPUDrivenSkyPass.h`
- `render/passes/GPUDrivenSkyPass.cpp`
- `shaders/sky.slang` (vertex + fragment entry points)
- `render/passes/GPUDrivenDecalPass.h`
- `render/passes/GPUDrivenDecalPass.cpp`
- `shaders/decal_apply.slang`
- `render/passes/GPUDrivenFinalColorPass.cpp`
- `app/MinimalLatestApp.h`

### Task 8.4 - Implement Conventional Texture Streaming And VT Experiments

Status: Pending

Implementation:

- [ ] In `render/TextureStreamer.h/.cpp`, implement `TextureStreamer`:
  - Track per-texture resident mip level based on screen-space footprint.
  - Load higher mips asynchronously from `assets/` (background thread + upload queue).
  - Evict lowest-priority mips when VRAM budget is exceeded.
- [ ] In `render/GPUDrivenRenderer.cpp`, integrate `TextureStreamer::update()` into frame start.
- [ ] In `GPUDrivenRuntimeStats`, report streaming load queue length, resident texture count, and estimated streamed memory.
- [ ] Add optional virtual texture feedback pass (desktop/high-end only):
  - Render pass outputs tile IDs to `vtFeedbackBuffer`.
  - CPU reads back reduced-resolution feedback (once every 4 frames).
  - Load tile pages into `vtPageCache` sparse image or atlas.
- [ ] Mobile baseline uses conventional streaming only; VT is gated by tier.

Acceptance:

- Textures stream higher resolution as objects approach camera.
- VRAM stays under budget via eviction.
- VT feedback does not run on mobile baseline.

Likely files:

- `render/TextureStreamer.h`
- `render/TextureStreamer.cpp`
- `render/GPUDrivenRenderer.cpp`
- `render/passes/GPUDrivenVTFeedbackPass.h`
- `render/passes/GPUDrivenVTFeedbackPass.cpp`
- `shaders/vt_feedback.slang`
- `app/MinimalLatestApp.h`

### Task 8.5 - Implement Ray Tracing Experiments (Desktop Only)

Status: Pending

Implementation:

- [ ] In `render/RayTracingContext.h/.cpp`, create `RayTracingContext`:
  - Build BLAS from static mesh geometry.
  - Build TLAS from object transforms per frame.
- [ ] In `render/passes/GPUDrivenRTReflectionPass.h/.cpp`, implement reflection ray tracing:
  - Dispatch `rt_reflection.slang` generating rays from GBuffer.
  - Use `rt_reflection.slang` to sample materials and evaluate BRDF.
  - Denoise with bilateral filter or reuse TAA history.
- [ ] In `render/passes/GPUDrivenRTAOPass.h/.cpp`, implement ray-traced AO:
  - Dispatch `rt_ao.slang` with short rays.
  - Combine with GTAO as high-end tier.
- [ ] Gated by device tier: only enable if `rayTracingPipeline` feature is present and device is not mobile.
- [ ] Keep screen-space/probe fallback: if RT is disabled, automatically fall back to SSR + IBL.

Acceptance:

- Ray-traced reflections appear on desktop when enabled.
- Mobile devices never attempt to build BLAS/TLAS.
- Fallback to SSR/IBL is automatic and seamless.

Likely files:

- `render/RayTracingContext.h`
- `render/RayTracingContext.cpp`
- `render/passes/GPUDrivenRTReflectionPass.h`
- `render/passes/GPUDrivenRTReflectionPass.cpp`
- `shaders/rt_reflection.slang` (raygeneration + closesthit entry points)
- `render/passes/GPUDrivenRTAOPass.h`
- `render/passes/GPUDrivenRTAOPass.cpp`
- `shaders/rt_ao.slang`
- `app/MinimalLatestApp.h`

Phase 8 completion criteria:

- [ ] Optional features are gated by device tier enum (`low`, `medium`, `high`, `desktop`).
- [ ] Mobile baseline remains intact when all optional features are disabled.
- [ ] User is notified to run build/compile manually.

## Proposed Approval Scope

Recommended first approval: Phase 1 only.

Reason:

- It is low risk.
- It does not change rendering behavior.
- It gives a reliable dashboard for all later ownership migration.
- It lets us update `worklist.md` status honestly as implementation progresses.
