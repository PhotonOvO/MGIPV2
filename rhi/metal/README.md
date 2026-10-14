# Metal 4 RHI Backend

This directory contains the Apple-platform Metal 4 implementation of the Demo
RHI. It targets the Metal 4 core API introduced with macOS 26 and iOS 26 and is
compiled with Xcode 26 or newer.

Authoritative references:

- Apple, Understanding the Metal 4 core API:
  <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- Slang, Metal-specific functionalities:
  <https://shader-slang.org/slang/user-guide/metal-target-specific>
- metal-cpp: <https://github.com/bkaradzic/metal-cpp>
- Pinned metal-cpp commit: `9a8bc57c0318b84863bec2faf6605594877c5f5d`
- Pinned archive SHA-256:
  `46736574f7afd977d7d35a13aa781edb595a43650d437b50f5779ec750879530`

## Requirements

- Xcode 26 or newer with a macOS 26 or newer SDK
- The optional Xcode Metal Toolchain component for production shader builds
- A Metal 4 runtime for execution and smoke testing
- C++20

The CMake configuration rejects SDKs older than macOS 26. The deployment host
may be older for compile-only validation, but Metal 4 executables must only be
run on a host where `queryMetal4Availability()` reports the SDK contract,
runtime protocols, and a device as available.

## Core API Contract

The CPU command path is implemented in `.cpp` files and uses these Metal 4
command types exclusively through metal-cpp:

- `MTL4::CommandQueue`
- `MTL4::CommandAllocator`
- `MTL4::CommandBuffer`
- `MTL4::RenderCommandEncoder`
- `MTL4::ComputeCommandEncoder`

`tests/metal4_source_contract.cmake` scans the backend and fails if a legacy
command family, Objective-C runtime call, runtime MSL source path, `.mm` file,
hand-authored `.metal` file, or legacy drawable submission call is introduced.

Apple's Metal 4 command-buffer lifecycle is represented directly:

1. The device creates reusable command buffers.
2. `beginCommandBuffer(allocator)` attaches allocator storage.
3. Encoders record render, compute, copy, blit, and acceleration work.
4. `endCommandBuffer()` makes the buffer executable.
5. a compatible Metal 4 queue commits the executable buffer.
6. after GPU completion, allocator reset makes its command buffers reusable.

Metal 4 command buffers do not retain referenced resources. The backend retains
native objects while recorded work is executable or pending and retires RHI
objects only after every recorded submission token has completed.

## Vulkan Mapping

| RHI / Vulkan behavior | Metal 4 implementation |
| --- | --- |
| device and physical capabilities | `MTL::Device` capability queries |
| graphics, compute, transfer queues | three `MTL4::CommandQueue` wrappers |
| command pool | `MTL4::CommandAllocator` |
| reusable command buffer | `MTL4::CommandBuffer` begin/end lifecycle |
| graphics commands | `MTL4::RenderCommandEncoder` |
| compute and transfer commands | `MTL4::ComputeCommandEncoder` |
| descriptor sets | logical RHI tables composed into `MTL4::ArgumentTable` snapshots |
| timeline synchronization | `MTL::SharedEvent` values and RHI submission tokens |
| pipeline barriers | Metal 4 stage and hazard barriers |
| explicit residency | `MTL::ResidencySet` |
| timestamp query pool | `MTL4::CounterHeap` |
| image views | `MTL::TextureViewPool` resource IDs |
| SPIR-V shader module | embedded native metallib selected from the same Slang source |

Metal 4 treats resources as untracked. `resourceBarrier()` maintains RHI
subresource state and lowers producer stages, consumer stages, and write hazards
to Metal 4 barriers before the next encoder consumes the resource.

## Argument Tables

Pipeline reflection is lowered into one composed Metal 4 binding layout with
separate buffer, texture, and sampler indices. Logical RHI table slots never
leak native binding indices into renderer code.

Each dispatch or draw uses an allocator-owned `MTL4::ArgumentTable` snapshot.
The snapshot records the logical tables and their versions. Metal 4 consumes
the table contents when a draw or dispatch is encoded, so changing a snapshot
after `endCommandBuffer()` cannot update already encoded work. The backend also
records a lightweight RHI replay stream. Immediately before queue submission,
it rebuilds the native `MTL4::CommandBuffer` when any source table version has
changed, then collects resource validation, residency, and pending-use tracking
from the final versions. Command buffers whose tables did not change keep their
original native encoding. This matches the RHI contract that a table may be
updated while work is executable but not yet submitted.

Updates are rejected while a table is referenced by incomplete GPU work.
Texture-view bindings validate both the view and its parent texture and retain
the native view and parent storage through completion.

## Pipelines And Shaders

Graphics and compute pipelines are compiled with `MTL4::Compiler` and Metal 4
pipeline descriptors. Validation-enabled devices request
`MTL::ShaderValidationEnabled` in pipeline options.

Slang remains the only shader source of truth. A production Apple build
performs the following offline build pipeline:

```text
Slang -> generated MSL -> AIR -> metallib -> embedded C++ bytes
```

At runtime `EmbeddedShaderLibrary` selects the metallib corresponding to the
same embedded SPIR-V module used by Vulkan. Every compute pipeline supplies an
explicit Metal threads-per-threadgroup size.

Metal-specific blit and indexed-indirect-count kernels live in
`MetalBuiltins.slang`. Slang's Metal target generates temporary `.metal` files
only in the build directory; Xcode compiles them to one embedded metallib.
Runtime code loads those bytes with metal-cpp and creates pipeline states with
`MTL4::Compiler`. It never compiles MSL source at runtime.

The indexed-indirect builder uses Slang's target interoperation facilities.
`__target_intrinsic(metal, ...)` maps Slang wrapper types to Metal shader-side
`command_buffer` and `render_command`, and `ParameterBlock<T>` lowers to the
argument buffer that carries the ICB reference. These are GPU shader types used
to write ICB commands. They are unrelated to the CPU-side
`MTL4::CommandBuffer` and `MTL4::RenderCommandEncoder` objects.

Render pipelines that opt into GPU-generated commands select a second Slang
Metal variant compiled with `DEMO_METAL_ICB=1`. That variant places the material
texture array and shared sampler in a `ParameterBlock`, because Xcode 27 rejects
fragment ICB pipelines with those resources at top level. The Metal RHI encodes
the existing material argument table into that nested argument buffer and binds
its GPU address through the ordinary Metal 4 argument table. Renderer binding
schemas and the Vulkan/D3D12 shader variants are unchanged.

## Copy, Blit, And Mipmaps

The Metal 4 compute encoder handles copies, fills, and blits.

- Equal-size, forward, same-format 2D regions use `copyFromTexture`.
- Scaling or a negative source/destination extent uses the private Metal 4
  compute pipeline with bilinear filtering.
- The compute path binds source texture, destination texture, and constants
  through `MTL4::ArgumentTable` and dispatches 8 by 8 threads.
- Mipmap generation uses the same compute path between adjacent mip levels.
- Color aspect, mip bounds, layer bounds, offsets, sample count, usage flags,
  compressed formats, overlapping subresources, and 2D texture type are
  validated before encoding.

The current portable blit contract is color-only, single-sample, single-layer
2D. Block-compressed textures support exact copies but not scaling or flipping.

## Drawable Presentation

`CAMetalLayer` uses `framebufferOnly = false` because the final copy writes its
drawable texture from a Metal 4 compute encoder. The RHI-visible swapchain
images are per-frame, offscreen BGRA8 composition textures. Scene scaling and
flipping plus ImGui rendering target those composition textures, so the backend
never uses a drawable texture as a render attachment and never issues a draw
command against one.

Before the frame command buffer is submitted, `MetalSwapchain::encodePresent()`
appends an equal-size `copyFromTexture` from the completed composition texture to
the drawable. This folds presentation into the frame's existing Metal 4 command
buffer instead of allocating and committing a second command buffer every frame.
Each presentation slot owns a residency set containing its composition texture.
The frame command buffer also binds `CAMetalLayer::residencySet()`. Metal updates
that set as drawable backing resources change, including during a window resize;
without it, a valid copy submission can target a drawable whose resources are not
resident and intermittently present an empty frame.

The layer also sets `allowsNextDrawableTimeout = false`. During live resize,
returning `nil` from a timed-out `nextDrawable()` would skip presentation just as
Core Animation replaces the layer backing, exposing an empty refresh between
valid frames. Zero-sized and iconified windows are rejected before acquisition,
so blocking here cannot turn those states into a drawable wait.

The required order is:

```text
wait(drawable) -> encode scene/UI work -> encode drawable compute copy
-> commit frame command buffer -> signalDrawable(drawable)
-> drawable->present()
```

`MetalSwapchain::acquireNextImage()` waits for the drawable before exposing the
corresponding composition texture. `Queue::submit()` commits the encoded frame
and signals its drawable. `MetalSwapchain::present()` then presents it.

The queue's presentation completion event is scheduled after `signalDrawable`.
Consequently, waiting for a presentation token (including `waitIdle()` during
resize) covers both the copy command buffer and the drawable signal before the
layer backing or retained drawable is released.

## Validation Smoke Test

`metal4_validation_smoke_tests` enables validation before any Metal device is
created:

```text
MTL_DEBUG_LAYER=1
MTL_DEBUG_LAYER_ERROR_MODE=assert
MTL_SHADER_VALIDATION=1
MTL_SHADER_VALIDATION_DEFAULT_STATE=all
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
```

`MetalDevice::init()` applies the same settings when
`DeviceCreateInfo::enableValidationLayers` is true. The smoke test additionally
verifies:

- buffer upload and readback through the Metal 4 compute encoder;
- scaled, horizontally flipped mip generation through the private compute
  pipeline;
- compute and render argument-table updates after command-buffer end and before
  queue submit, including destruction of the replaced buffer;
- direct indexed, ordinary indexed-indirect, and GPU-count/ICB indexed draws in
  three independent viewport regions;
- an offscreen Metal 4 render pass, draw, render-to-transfer barrier, and texture
  readback without issuing any draw command against a drawable;
- API and shader validation while the work executes.

The executable returns `77` when the Metal 4 runtime or device is unavailable,
and CTest declares `SKIP_RETURN_CODE 77`. This lets an Xcode 26 compile-only host
produce the test, while a separate macOS 26 host performs GPU validation.

## Xcode Project Generation

Use the repository generator instead of assigning `DEVELOPER_DIR` manually:

```sh
./tools/generate_xcode_project.py --platform macos
```

The generator scans the system and user application directories, including
`~/Downloads`, and selects the highest installed Xcode version supported by the
backend. Xcode prerelease applications are eligible, so an Xcode 27 Beta is
preferred over an Xcode 26 release. The default output directory includes the
selected major version, for example `build-macos-xcode27`, so a project never
reuses an older Xcode's CMake cache.

Inspect the selection without generating files:

```sh
./tools/generate_xcode_project.py --platform macos --dry-run
```

Force a particular installation when needed:

```sh
./tools/generate_xcode_project.py \
  --xcode '/Applications/Xcode 26.app' \
  --platform macos
```

The same entry point supports `ios-simulator` and `ios-device`. Additional
CMake options may be passed after `--`.

## Metal Toolchain

Install the optional shader tools once:

```sh
DEVELOPER_DIR='/path/to/Xcode.app/Contents/Developer' \
  xcodebuild -downloadComponent MetalToolchain
```

After generation, use the selected developer directory printed by the script
when building from a terminal. The script also prints the exact build and
`open -a` commands for the selected Xcode application.

```sh
DEVELOPER_DIR='/path/to/Xcode.app/Contents/Developer' \
  cmake --build build-macos-xcode27 --target Demo --parallel
```

Compile-only validation must not invoke the produced Metal executables. Move or
rebuild the artifacts on the macOS 26 machine and run:

```sh
ctest --test-dir build-macos-xcode27 -R 'metal4_(source_contract|validation_smoke)' \
  --output-on-failure
```
