# Vulkan Validation Smoke Runbook

Phase 7 manual verification step. Follow in order. Record evidence before advancing to Plan 03.

---

## 1. Prerequisites

Confirm the Debug build exists:

```
out/build/x64-Debug/Demo.exe
```

Build with `build_debug_with_vsdevcmd.cmd` if missing. The Debug build automatically
enables the Vulkan validation layer via `#ifdef _DEBUG` in `render/RenderDevice.cpp`
(lines 952-956). No `VK_INSTANCE_LAYERS` environment variable or `vkconfig.exe` needed.

---

## 2. Run the Smoke

1. Open a terminal at the repository root (`G:/MGIF`).
2. Run: `out\build\x64-Debug\Demo.exe`
3. Watch the console output for Vulkan validation messages. Look for lines containing `VUID-`,
   `ERROR`, `WARNING`, or `Validation Error`.
4. Interact with the Demo for at least one full frame covering all render passes.
   Let GPU culling, depth pyramid, lighting, shadows, post-process, and ImGui all execute
   (typically happens within the first 2-3 seconds of the demo running).
5. Capture the console output to a file or take a screenshot of the window showing the rendered
   frame.

---

## 3. Pass Criteria

Per D-05 / D-06:

- Demo opens a window and renders a frame (画面出来).
- Console shows NO new `VUID-*`, layout transition error, or sync validation message that
  was not present before the Phase 2-6 migration.
- Pixel-level match is NOT required. Floating-point or scheduling differences are not failures.
- A clean run typically shows a few informational lines from Vulkan loader and then silence
  from the validation layer (no ERROR/WARNING lines).

---

## 4. Pass Chain — 22 addPass Calls in Execution Order

Source: `render/GPUDrivenRenderer.cpp` lines 487-510.

| Order | Pass Class | Notes |
|------:|------------|-------|
| 1 | `GPUDrivenDepthPrepass` | |
| 2 | `GPUDrivenDepthPyramidPass` | HiZ pyramid build |
| 3 | `GPUDrivenCullingPass` | GPU occlusion culling |
| 4 | `GPUDrivenVisibilitySortPass` | (conditional — only when enabled) |
| 5 | `GPUDrivenLightCullingPass` | |
| 6 | `GPUDrivenClusteredLightCullingPass` | |
| 7 | `GPUDrivenCSMShadowPass` | Cascaded shadow map |
| 8 | `GPUDrivenShadowAtlasPass` | Shadow atlas |
| 9 | `GPUDrivenGBufferPass` | Deferred G-buffer |
| 10 | `GPUDrivenAOPass` | Ambient occlusion |
| 11 | `GPUDrivenSSRPass` | Screen-space reflections |
| 12 | `GPUDrivenLightPass` | Deferred lighting |
| 13 | `GPUDrivenSkyboxPass` | |
| 14 | `GPUDrivenForwardPass` | Transparent/forward objects |
| 15 | `GPUDrivenVelocityPass` | Motion vectors |
| 16 | `GPUDrivenTAAResolvePass` | Temporal anti-aliasing |
| 17 | `GPUDrivenBloomPrefilterPass` | |
| 18 | `GPUDrivenBloomDownsamplePass` | |
| 19 | `GPUDrivenFinalColorPass` | Tone-map + final composite |
| 20 | `GPUDrivenPresentPass` | Swapchain present |
| 21 | `GPUDrivenImguiPass` | Debug overlay |

One full interactive frame exercises all 21 passes (22 including the conditional
`GPUDrivenVisibilitySortPass` when enabled). Ensure GPU culling and at least one
post-process effect (bloom or TAA) have run before recording evidence.

---

## 5. Store Evidence

Save screenshot and/or console log to:

```
.planning/phases/07-final-boundary-acceptance/
```

Use the filename format:

```
smoke_YYYYMMDD.png    (window screenshot)
smoke_YYYYMMDD.txt    (console output captured via > or copy-paste)
```

Example: `smoke_20260610.png`, `smoke_20260610.txt`.

---

## 6. After Passing

Plan 03 checkpoint requires the result of this RUNBOOK.

Once the smoke passes, type `approved` in the executor prompt to advance Plan 03.
If errors are found, record the VUID strings in the evidence file and report them
as blockers before proceeding.
