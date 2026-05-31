# GPU Driven Renderer UE5 Gap Worklist

Date: 2026-05-29
Scope: only the `GPUDrivenRenderer` path. Legacy `Renderer` work is included only when it blocks GPU-driven ownership.

Mobile target: Android/mobile Vulkan must be a first-class shipping target. Every feature below must either have a mobile-feasible implementation, a lower-cost fallback, or be explicitly marked as desktop/high-end experimental. Do not build a desktop-only UE5 clone and assume it will scale down later.

## Mobile Landing Rules

- Prefer bandwidth-light passes over large MRT expansion.
- Prefer half/quarter-resolution screen-space effects with temporal reuse.
- Prefer clustered/tiled compute work with bounded list sizes.
- Prefer precomputed/offline asset processing over heavy runtime generation.
- Prefer graceful quality tiers over single expensive algorithms.
- Avoid hardware ray tracing as a shipping dependency.
- Avoid full Virtual Shadow Maps, full Nanite, and full Lumen as baseline mobile requirements; use mobile-scaled variants.
- Keep async compute optional because mobile queue support and scheduling wins vary by device.
- Keep render targets, history buffers, and transient allocations under an explicit per-quality memory budget.
- Add device-tier gates for low, mid, high, and flagship mobile GPUs.

## Current Baseline

The current GPU-driven path already has these major pieces:

- Dedicated `GPUDrivenRenderer` pass graph.
- Persistent scene registration through `GPUSceneRegistry`.
- Object-level GPU culling, indirect draw buffers, and MDI submission for depth, GBuffer, CSM, and transparent forward.
- Hi-Z depth pyramid support, currently still partly bridged through shared renderer resources.
- Packed GBuffer with deferred direct lighting.
- Directional light with cascaded shadow maps.
- Coarse point-light culling.
- Alpha mask and alpha blend material routing.
- Experimental meshlet buffer/converter path.
- GPU visibility sort and transparent visibility patch, with some CPU-side ordering/feedback still present.
- Present, ImGui, and debug pass integration.

Compared with UE5-style rendering, the renderer is currently closer to an object-level GPU-driven deferred renderer than a full UE5-class renderer. For mobile, the target should be UE5-inspired feature coverage rather than literal UE5 feature parity. The largest gaps are mobile-scaled meshlet/cluster geometry, mobile GI/reflection approximations, scalable shadow atlases, temporal reconstruction/upscaling, production post processing, sky/fog, decals, texture streaming, and material/special-surface systems.

## P0 - Make The GPU-Driven Frame Fully GPU-Owned

### Task 0.1 - Remove Remaining Shared-Renderer Ownership From The GPU-Driven Frame

- [ ] Move scene attachment creation, GBuffer handles, output target handles, and depth pyramid ownership behind GPU-driven resource objects.
- [ ] Stop treating `Renderer` as the authority for GPU-driven pass resources; keep it only as a temporary RHI/facade provider until equivalent GPU-driven systems exist.
- [ ] Add a `GPUDrivenFrameGraph` or equivalent resource declaration layer so every GPU-driven pass declares inputs, outputs, transient lifetimes, and async eligibility.
- [x] Add frame diagnostics that report whether each pass is GPU-driven-owned, bridged, or legacy-owned. Completed in Phase 1 Task 1.1/1.2 via per-pass diagnostics and resource ownership summary.
- [x] Document the current GPU-driven pass graph contract, ownership state, and mobile risk before migrating resources. Completed in `gpu_driven_mobile_execution_plan.md` Phase 1 Task 1.3.

Missing passes/effects covered:

- Full GPU-driven pass graph ownership.
- Explicit render-graph resource lifetime and barriers.
- Foundation required before UE5-scale features can be added safely.

### Task 0.2 - Same-Frame Visibility, Compaction, And Draw Emission

- [ ] Replace CPU-adopted visibility/sort feedback with GPU-only same-frame visible list compaction.
- [ ] Emit final indirect command streams on GPU for opaque, alpha mask, transparent, shadow, and debug views.
- [ ] Generate material/PSO batch ranges on GPU.
- [ ] Support GPU-generated draw count buffers for all draw classes.
- [x] Add GPU-driven validation counters/diagnostics for visibility ownership, same-frame patching, bootstrap, CPU-seeded ordering, and mobile transparent overflow. Completed in Phase 2 via `GPUDrivenVisibilityDiagnostics`.

Missing passes/effects covered:

- Visibility compaction pass.
- Batch compaction pass.
- GPU material binning pass.
- Same-frame indirect command generation pass.

### Task 0.3 - Production Hi-Z Ownership

- [x] Make `HiZDepthPyramid` resource ownership explicit in the GPU-driven renderer diagnostics. Completed in Phase 3 via `GPUDrivenHiZDiagnostics`.
- [x] Generate current-frame pyramids with a clear ownership contract and explicit source/pyramid/mip/generation state. Completed in Phase 3.
- [ ] Add previous-frame pyramid history and depth reprojection support for temporal occlusion stability.
- [x] Add conservative occlusion test mode diagnostics and fast-camera Hi-Z fallback for mobile defaults. Completed in Phase 3.

Missing passes/effects covered:

- GPU-owned Hi-Z generation pass.
- Temporal occlusion/reprojection support.
- Conservative occlusion culling mode.

## P2 - Mobile-Scaled Meshlet/Cluster Geometry

Full Nanite-style virtualized geometry should not be the mobile baseline. The mobile landing target is offline-built meshlets/clusters, GPU culling, and optional cluster LOD, with object-level MDI kept as the fallback.

### Task 1.1 - Production Meshlet/Cluster Asset Pipeline

- [ ] Replace prototype meshlet conversion with a production builder such as `meshoptimizer` or an equivalent offline cluster builder.
- [ ] Build cluster bounds, cone culling data, LOD hierarchy, material ranges, and index remap data.
- [ ] Serialize cluster data into scene cache assets.
- [ ] Add streaming-ready GPU buffer layout for cluster pages.

Missing UE5-like effect:

- Nanite-inspired dense geometry support, scaled for mobile memory and bandwidth.

Missing passes:

- Cluster build/import pipeline.
- Cluster page upload pass.

### Task 1.2 - GPU Cluster Culling And LOD Selection

- [ ] Implement frustum, backface cone, Hi-Z occlusion, and screen-error LOD selection for clusters.
- [ ] Add hierarchical cluster traversal.
- [ ] Emit visible cluster lists and indirect draw/mesh-shader commands on GPU.
- [ ] Add debug overlays for cluster LOD, rejected clusters, and overdraw.
- [ ] Add mobile tier limits for max visible clusters, max cluster pages, and fallback to object MDI.

Missing passes:

- Cluster hierarchy traversal pass.
- Cluster culling pass.
- Cluster LOD selection pass.
- Visible cluster compaction pass.

### Task 1.3 - Mesh Shader Or Compute Raster Path

- [ ] Choose shipping path: compute-driven indirect/index expansion first; Vulkan mesh shaders only as optional high-end path.
- [ ] Add feature fallback matrix for desktop and Android.
- [ ] Route GBuffer/depth/shadow submissions through cluster output instead of object-level MDI when enabled.

Missing passes:

- Mesh shader draw path or compute expansion pass.
- Cluster depth prepass.
- Cluster GBuffer pass.
- Cluster shadow pass.

## P2 - Mobile-Scaled Global Illumination And Reflections

Full Lumen-style dynamic GI is not a mobile baseline. The mobile landing target is SSGI/GTAO, reflection probes, baked/probe GI, and limited dynamic updates.

### Task 2.1 - Scene Representation For GI

- [ ] Build a GPU scene representation suitable for GI: surface cache cards, signed distance fields, radiance probes, or a simpler first version.
- [ ] Track dynamic object updates and invalidation regions.
- [ ] Add debug views for GI scene coverage.

Missing UE5-like effects:

- Mobile-scaled dynamic diffuse global illumination.
- Mobile-scaled indirect specular/reflection support.

Missing passes:

- Surface cache update pass.
- Probe/SDF/card update pass.
- GI scene debug pass.

### Task 2.2 - Screen-Space GI First Step

- [ ] Add screen-space diffuse GI using GBuffer depth, normals, roughness, and previous color.
- [ ] Add spatial denoise and temporal accumulation.
- [ ] Add fallback controls for quality/performance tiers.
- [ ] Run at half or quarter resolution on mobile and reuse TAA/upscale history.

Missing passes:

- SSGI trace pass.
- SSGI spatial denoise pass.
- SSGI temporal accumulate pass.

### Task 2.3 - Probe Or Surface-Cache GI

- [ ] Add world-space GI cache beyond screen space.
- [ ] Support multiple bounces or cached irradiance propagation.
- [ ] Composite diffuse GI into the deferred light pass.
- [ ] Prefer static probes or low-frequency dynamic probes for mobile; keep surface-cache updates optional.

Missing passes:

- Probe update pass.
- Probe trace/injection pass.
- Irradiance filtering pass.
- GI composite pass.

### Task 2.4 - Reflections

- [x] Add SSR as the first reflection tier. Phase 7 baseline adds GPUDriven-owned half-resolution SSR trace resources and light-pass integration, with IBL as the fallback.
- [ ] Add roughness-aware reflection denoising.
- [ ] Add reflection captures or probe fallback for off-screen data.
- [ ] Optional later: hardware ray tracing reflection tier.
- [ ] Use probe fallback as the mobile default and SSR only for selected quality tiers.

Missing passes:

- SSR trace pass.
- Reflection resolve pass.
- Reflection temporal/spatial denoise pass.
- Reflection capture/probe update pass.

## P1 - Mobile-Scalable Shadowing

Full Virtual Shadow Maps are too expensive as a mobile baseline. The mobile landing target is a tiled shadow atlas with GPU-driven culling, stable caching, and optional virtual-page concepts for flagship devices.

### Task 3.1 - Tiled Shadow Atlas With Optional Virtual Pages

- [x] Implement a tiled shadow atlas as the mobile baseline. Phase 7 adds a GPUDriven-owned shadow atlas resource/pass, fixed 512x512 directional cascade tile rendering, UI toggle, diagnostics, and explicit CSM lighting fallback; local-light atlas tiles and atlas sampling remain deferred.
- [ ] Keep full Virtual Shadow Maps as a desktop/flagship experimental tier.
- [ ] Add page allocation, page cache, invalidation, and per-light page tables.
- [ ] Render shadow pages using GPU-driven visible sets.
- [ ] Add page residency debug view.

Missing UE5-like effect:

- Virtual Shadow Maps or a mobile-feasible tiled shadow atlas equivalent.

Missing passes:

- Shadow page marking pass.
- Shadow page allocation/compaction pass.
- Shadow page render pass.
- Shadow page cache invalidation pass.

### Task 3.2 - Contact Shadows And Soft Shadows

- [ ] Add screen-space contact shadows for directional, point, and spot lights.
- [ ] Add receiver-plane depth bias or equivalent bias stabilization.
- [ ] Add PCSS/EVSM/MSM or another scalable soft-shadow path.
- [ ] Add cascade/page transition filtering.
- [ ] Use low-sample screen-space contact shadows and fixed-radius PCF as mobile defaults.

Missing passes:

- Contact shadow trace pass.
- Shadow filter pass.
- Shadow resolve pass.

### Task 3.3 - Local Light Shadows

- [ ] Add point-light shadow cubemap or atlas support.
- [ ] Add spot-light shadow maps.
- [ ] Integrate local shadowed lights with the tiled/clustered light path.

Missing passes:

- Spot shadow culling pass.
- Point shadow face culling pass.
- Local shadow render pass.
- Local shadow resolve path in lighting.

## P0 - Tiled/Clustered Lighting Upgrade

### Task 4.1 - Replace Coarse Point-Light Culling With Clustered Lighting

- [x] Build view-space clusters for the GPU-driven path. Completed in Phase 6 with a `16 x 9 x 24` GPUDriven-owned cluster grid and inverse-projection cluster AABBs.
- [ ] Cull point, spot, and area lights into cluster lists. Point-light culling is completed in Phase 6; spot/area culling remains.
- [x] Support many-light scenes without per-pixel scanning over all lights in a tile. Completed in Phase 6 for point lights through bounded cluster index lists.
- [x] Add cluster heatmap and list occupancy debug views. Completed in Phase 6 with max occupancy, appended references, and overflow diagnostics.
- [x] Bound per-cluster light list size for mobile and add overflow fallback. Completed in Phase 6 with `LMaxLightsPerCluster` and coarse fallback for overflow clusters.

Missing passes:

- Depth bounds/froxel build pass.
- Clustered light culling pass. Phase 6 baseline exists for point lights using view-space sphere-vs-cluster-AABB tests; spot/area and depth-derived/log-z froxels remain.
- Light list compaction pass.

### Task 4.2 - Add Area Lights And UE-Style Light Features

- [ ] Add rectangle, disk, and tube area lights.
- [ ] Add IES profile support.
- [ ] Add physically based falloff controls and units.
- [ ] Add shadowing path for selected local lights.

Missing effects:

- Area lights.
- IES lights.
- Physically based many-light rendering.

## P1 - Image-Based Lighting, Sky, Atmosphere, And Volumetrics

### Task 5.1 - Finish IBL Integration

- [x] Load HDR environment maps. Completed in Phase 6 baseline with GPUDriven-owned BC6H KTX2 equirect upload from `resources/environment/lilienstein_4k.ktx2`.
- [ ] Generate irradiance cube, prefiltered specular cube, and BRDF/DFG LUT.
- [x] Bind IBL resources into the GPU-driven light pass. Completed in Phase 6 baseline through GPUDriven-owned light-pass descriptors.
- [x] Replace flat ambient term with sky light and reflection probes. Completed as a mobile baseline using GPUDriven-owned equirect diffuse/specular sampling with flat ambient fallback; cube convolution remains future work.
- [x] Add GPU-driven HDRI skybox pass for empty pixels as a visible validation path before the full sky/atmosphere phase.

Missing passes:

- Environment import/upload pass.
- Irradiance convolution pass.
- Specular prefilter pass.
- BRDF LUT generation pass.
- IBL composite in lighting. Phase 6 baseline exists through GPUDriven-owned equirect IBL; split-sum cube resources remain.

### Task 5.2 - Sky Atmosphere And Sun/Sky Lighting

- [ ] Add physically based sky atmosphere lookup textures.
- [ ] Render sky as a pass behind scene geometry.
- [ ] Feed sky luminance into exposure and IBL.
- [ ] Add sun disk and aerial perspective.
- [ ] Prefer LUT-based sky at low resolution; avoid per-pixel expensive atmosphere math on mobile.

Missing passes:

- Transmittance LUT pass.
- Multi-scattering LUT pass.
- Sky view LUT pass.
- Aerial perspective pass.
- Sky render pass.

### Task 5.3 - Volumetric Fog And Light Shafts

- [ ] Add froxel volume allocation.
- [ ] Inject lights, shadows, and participating media.
- [ ] Temporal reprojection and filtering for volumetric lighting.
- [ ] Composite volumetrics before transparent rendering.
- [ ] Mobile baseline should be height fog plus optional low-resolution froxel fog on high tiers.

Missing passes:

- Froxel grid build pass.
- Volume light injection pass.
- Volume scattering integration pass.
- Volumetric temporal reprojection pass.
- Volumetric composite pass.

### Task 5.4 - Clouds

- [ ] Add optional volumetric cloud layer.
- [ ] Add cloud shadow projection.
- [ ] Integrate clouds with sky, exposure, and aerial perspective.
- [ ] Treat volumetric clouds as high-end optional; use skybox/2D impostor fallback on mobile.

Missing passes:

- Cloud shape/weather texture update pass.
- Cloud raymarch pass.
- Cloud shadow pass.
- Cloud composite pass.

## P1 - Material And Surface Feature Parity

### Task 6.1 - Expand GBuffer And Material Model

- [ ] Add clear coat, anisotropy, sheen, subsurface, transmission, and thin translucency fields.
- [ ] Add material feature flags for shader permutation or bindless material evaluation.
- [ ] Add material quality tiers to avoid unbounded shader cost.
- [ ] Add debug views for all packed material channels.
- [ ] Keep mobile GBuffer packing compact; route expensive lobes through material quality tiers.

Missing UE5-like effects:

- Clear coat.
- Anisotropy.
- Sheen.
- Subsurface.
- Transmission.

Missing passes:

- Material classification pass, if material bins move fully GPU-side.
- Extended deferred shading path.

### Task 6.2 - Decals

- [ ] Add deferred decal volume rendering.
- [ ] Support DBuffer-like material modifications before lighting.
- [ ] Add normal, roughness, base color, emissive, and opacity decal modes.
- [ ] Add GPU culling/binning for decal volumes.

Missing passes:

- Decal culling pass.
- DBuffer/deferred decal pass.
- Decal debug pass.

### Task 6.3 - Special Surfaces

- [ ] Add water shading path with planar/SSR reflection fallback.
- [ ] Add skin/subsurface profile path.
- [ ] Add hair/strand or card hair shading path.
- [ ] Add cloth/fuzz shading approximation.

Missing effects:

- Water.
- Skin.
- Hair.
- Cloth/fuzz.

## P1 - Transparency, Particles, And Composition

### Task 7.1 - Production Transparent Rendering

- [ ] Replace CPU-generated transparent ordering with GPU sorting or weighted blended OIT.
- [ ] Add per-pixel or per-tile transparency controls for high-overlap scenes.
- [ ] Support refractive transparent materials.
- [ ] Add separate translucency resolution and composite step.
- [ ] Use weighted blended OIT or sorted MDI as mobile baseline; per-pixel linked lists are not mobile baseline.

Missing passes:

- Transparent GPU sort pass.
- Weighted OIT accumulation pass or per-pixel linked-list pass.
- Refraction pass.
- Separate translucency composite pass.

### Task 7.2 - GPU Particles

- [ ] Add GPU particle simulation buffers.
- [ ] Add GPU particle culling and sorting.
- [ ] Render lit and unlit particles into transparent/separate translucency.
- [ ] Add collision/depth interaction.

Missing passes:

- Particle simulation pass.
- Particle culling pass.
- Particle sort pass.
- Particle render pass.

## P0 - Temporal AA, Upscaling, And Motion Data

### Task 8.1 - Motion Vectors

- [x] Store previous transforms for camera and objects. Completed in Phase 5 with previous camera matrices and `DrawUniforms::prevModelMatrix`.
- [x] Add velocity buffer output for opaque, alpha mask, and selected transparent objects. Completed in Phase 5 as a fullscreen camera-velocity buffer; rasterized object velocity remains a future quality upgrade.
- [ ] Add skinned/animated object velocity support when animation is added.

Missing passes:

- Rasterized object velocity pass or GBuffer velocity target for non-camera object motion.
- Skinned/animated previous-frame transform update pass, once animation exists.

### Task 8.2 - TAA / TSR-Style Reconstruction

- [x] Add jittered projection. Completed in Phase 5 with Halton jitter in the GPU-driven temporal camera copy.
- [x] Add TAA resolve with history validation. Completed in Phase 5 with double-buffered HDR history and velocity reprojection.
- [ ] Add reactive masks for transparent/emissive/high-motion pixels.
- [x] Add optional temporal upscaling mode. Phase 5 added controls/diagnostics; internal render-size decoupling is explicitly blocked by the current single-extent `SceneResources` model.
- [x] Target mobile temporal upscaling as a core performance feature, with FSR-style spatial fallback. Phase 5 records the mobile path contract and exposes the blocked state in diagnostics.

Missing UE5-like effects:

- Temporal AA.
- TSR-like upscaling.

Missing passes:

- Reactive mask pass.
- True internal-resolution upscale resolve pass after `SceneResources` display/internal extents are decoupled.

## P0 - Post Processing And Color Pipeline

### Task 9.1 - HDR Color Pipeline

- [x] Add GPU-driven post-process diagnostics for current output format, recommended mobile HDR format, tone-map location, and output/HDR memory budget. Completed in Phase 4 as the migration contract.
- [x] Add a GPU-driven `R16G16B16A16_SFLOAT` HDR scene color target. Completed in Phase 4.
- [x] Keep GPU-driven lighting and transparent forward output in HDR until the final post chain. Completed in Phase 4.
- [x] Add a GPU-driven final color pass that owns exposure, bloom composite, ACES tone mapping, and SDR output. Completed in Phase 4.
- [x] Report final color pass, color grading, lens controls, and display transform activation state in GPU-driven diagnostics. Completed in Phase 4.
- [ ] Add scene color history resources.
- [x] Add mobile color grading support through final-pass saturation, contrast, gamma, and vignette controls. Completed in Phase 4.
- [x] Add SDR display transform through the GPU-driven final color pass. Completed in Phase 4.
- [x] Use compact HDR formats and avoid unnecessary full-resolution intermediate copies on mobile. Completed in Phase 4 with FP16 scene color and half/quarter bloom.

Remaining passes:

- Scene color history pass.

### Task 9.2 - Exposure

- [x] Add fixed-exposure diagnostics and ImGui control, and explicitly report whether adaptive exposure is active. Completed in Phase 4.
- [x] Add mobile adaptive exposure using a 4x4 HDR luminance sample grid in final color. Completed in Phase 4.
- [x] Add exposure debug overlay values in post-process diagnostics. Completed in Phase 4.
- [ ] Add full luminance histogram for higher-quality exposure.

Remaining passes:

- Optional luminance histogram pass.

### Task 9.3 - Bloom And Lens Effects

- [x] Add mobile bloom memory budget diagnostics for a half/quarter-resolution FP16 chain. Completed in Phase 4.
- [x] Add threshold bloom extraction at half resolution. Completed in Phase 4.
- [x] Add half/quarter downsample and final composite bloom. Completed in Phase 4.
- [x] Add lightweight lens dirt/glare contribution from bloom in final color. Completed in Phase 4.
- [x] Keep bloom mip-chain half/quarter resolution and make lens effects optional. Completed in Phase 4.

Remaining passes:

- Optional authored lens dirt texture / flare sprite pass.

### Task 9.4 - Depth Of Field, Motion Blur, And Film Effects

- [ ] Add circle-of-confusion generation.
- [ ] Add near/far DOF gather/scatter.
- [ ] Add camera and object motion blur using velocity buffer.
- [ ] Add vignette, grain, chromatic aberration, and sharpen as optional final passes.

Missing passes:

- DOF CoC pass.
- DOF gather/scatter pass.
- Motion blur tile-max pass.
- Motion blur resolve pass.
- Final post composite pass.

## P2 - Ambient Occlusion And Screen-Space Detail

### Task 10.1 - GTAO / SSAO

- [ ] Add normal/depth-based ambient occlusion.
- [ ] Add bilateral denoise.
- [x] Integrate AO with direct and indirect lighting. Phase 7 AO multiplies indirect ambient/IBL only.
- [x] Make half-resolution GTAO/SSAO a mobile baseline candidate before SSGI. Phase 7 adds half-resolution GPUDriven GTAO and denoise passes.

Missing passes:

- SSAO/GTAO pass.
- AO denoise pass.
- AO composite path in lighting.

### Task 10.2 - Screen-Space Shadows And Detail Normals

- [ ] Add small-scale screen-space shadow/contact occlusion.
- [ ] Add optional bent normal output for GI/IBL.

Missing passes:

- Screen-space contact occlusion pass.
- Bent-normal generation pass.

## P2 - Streaming And Virtualization

### Task 11.1 - Virtual Texturing

- [ ] Add virtual texture page table and physical cache.
- [ ] Add GPU feedback for visible texture pages.
- [ ] Add sparse/streaming upload path.
- [ ] Integrate material texture sampling with page tables.
- [ ] For mobile baseline, prefer conventional texture streaming first; virtual texturing is high-end optional.

Missing UE5-like effect:

- Runtime virtual textures / streaming virtual textures.

Missing passes:

- VT feedback pass.
- VT page resolve/update pass.
- VT debug pass.

### Task 11.2 - Geometry And Material Streaming

- [ ] Add cluster page streaming for the Nanite-class path.
- [ ] Add material/texture residency tracking.
- [ ] Add async upload synchronization with GPU-driven scene updates.

Missing passes:

- Cluster residency feedback pass.
- Streaming upload pass.
- Residency validation pass.

## P2 - Ray Tracing Optional Tier

Hardware ray tracing is not a mobile shipping dependency. This section is optional for desktop and flagship experiments only.

### Task 12.1 - Hardware Ray Tracing Foundation

- [ ] Add Vulkan ray tracing capability detection and fallback paths.
- [ ] Build BLAS/TLAS from GPU scene data.
- [ ] Add update/refit path for dynamic objects.

Missing passes:

- BLAS build/update pass.
- TLAS build/update pass.

### Task 12.2 - RT Shadows, Reflections, And GI

- [ ] Add RT shadow tier for selected lights.
- [ ] Add RT reflections tier.
- [ ] Add RTGI or path-traced reference mode for validation.
- [ ] Share denoisers with screen-space GI/reflection paths where possible.

Missing passes:

- RT shadow trace pass.
- RT reflection trace pass.
- RTGI trace pass.
- RT denoise passes.

## P2 - Animation, Skinning, And Deformation

### Task 13.1 - GPU Skinning And Morph Targets

- [ ] Add joint/morph buffers to GPU scene.
- [ ] Run skinning/morph update on GPU.
- [ ] Emit deformed bounds for culling.
- [ ] Support velocity for deformed objects.

Missing passes:

- GPU skinning pass.
- Morph target pass.
- Deformed bounds update pass.

### Task 13.2 - World Position Offset / Procedural Deformation

- [ ] Add material-driven vertex deformation support.
- [ ] Add conservative bounds expansion or GPU bounds update.
- [ ] Integrate deformed bounds with shadow and visibility culling.

Missing effects:

- UE-style material WPO.

Missing passes:

- Procedural bounds update pass.
- Deformation-aware depth/GBuffer/shadow variants.

## P3 - Tooling, Debug, And Quality Gates

### Task 14.1 - Render Debug Views

- [ ] Add pass/resource graph visualization.
- [ ] Add GBuffer channel viewer.
- [ ] Add shadow page/cascade viewer.
- [ ] Add Hi-Z mip viewer.
- [ ] Add light cluster viewer.
- [ ] Add GI/reflection history viewer.
- [ ] Add overdraw, quad utilization, and material complexity views.

Missing passes:

- Debug visualization passes for every major intermediate resource.

### Task 14.2 - GPU Profiling And Regression Tests

- [ ] Add GPU timestamp scopes per pass.
- [ ] Add automated frame captures for reference scenes.
- [ ] Add image-diff tests for stable scenes.
- [ ] Add stress scenes: many objects, many lights, alpha, emissive, fast camera, huge meshes.
- [ ] Add GPU memory budget reporting.

Missing systems:

- Render regression harness.
- GPU memory and pass timing dashboard.

### Task 14.3 - Platform Feature Matrix

- [ ] Define desktop high-end, desktop fallback, and Android fallback profiles.
- [ ] Mark every pass as required, optional, or fallback.
- [ ] Add runtime feature toggles and graceful degradation.

Missing systems:

- UE-style scalable quality tiers.
- Platform fallback matrix.

## Suggested Implementation Order

1. Finish GPU-owned visibility, compaction, and Hi-Z ownership.
2. Add mobile HDR scene color, exposure, bloom, tone mapping, and color grading.
3. Add motion vectors, TAA, and temporal upscaling.
4. Replace coarse light culling with bounded clustered lighting.
5. Finish IBL and sky light so baseline PBR no longer depends on flat ambient.
6. Add half-resolution GTAO/SSAO, then SSR/probe reflections.
7. Replace CSM-only shadowing with a mobile tiled shadow atlas and contact shadows.
8. Add production transparency with weighted OIT or GPU-sorted MDI.
9. Add mobile-scaled meshlet/cluster culling behind object-level MDI fallback.
10. Add mobile-scaled GI beyond screen space only after the baseline frame is stable.
11. Add height fog, optional low-resolution volumetrics, decals, special surfaces, and scalability tooling.
12. Keep full Nanite/Lumen/VSM/RT-style features as high-end optional tiers, not mobile baseline blockers.
