# MGIF Agent Instructions

在回答或修改代码之前，先停顿一下，确认用户真正要解决的问题、涉及的模块、当前后端状态和需要什么级别的证据。把“设计建议”“代码修改”“验证结论”分开表达。

本文件是仓库级代理契约。更深目录中的 `AGENTS.md` 可以补充本文件；发生冲突时，离目标文件更近的规则优先。API 规范只定义语义依据，不替代仓库中的实现和测试。

## 1. 项目定位与事实来源

MGIF 是 C++20/CMake/CTest 的多后端 GPU 工程，包含公共 RHI、Vulkan/D3D12/Metal 4 后端、渲染器、场景编辑器、资源加载器和 Slang shader 管线。

按以下顺序确认当前事实：

1. 目标文件和相邻模块的实际代码。
2. `CMakeLists.txt`、`CMakePresets.json`、相关 `README.md` 和运行手册。
3. 对应测试、边界检查和本次运行产生的证据。
4. 外部 API 规范。

如果实现、文档和历史计划冲突，先以当前代码和可重复验证为准，并在同一变更中修正文档。不要把历史验收数字、旧目录名或计划中的功能当作当前能力。特别是后端 README 可能落后于源码；修改 D3D12 或 Metal 时必须核对源文件、CMake 条件和实际 smoke 结果。

当前能力声明必须保持准确：

- Vulkan 是当前必须保持可运行并经过 smoke/validation 验证的参考后端。
- D3D12 的真实实现范围以当前源代码和 Windows Debug Layer smoke 为准；仅有 scaffold 或 compile-only 证据时，不得宣称完整运行时或跨功能 parity。
- Metal 4 只在 Apple SDK/Xcode 环境中进行真实编译和运行验证；Windows 上的 contract/source 检查不能宣称 Metal 4 backend complete。
- Android 当前走 Vulkan；Metal 4 需要 Apple 平台和 SDK 26 或更新版本。

不要因为工作区中存在 `build/`、`out/`、`captures/`、`node_modules/` 或缓存文件就扩大源码范围。先用 `git status --short` 和 `rg --files` 确认 tracked source、生成物和用户已有改动。

## 2. 修改前的通用规则

- 保留用户已有的未提交改动。禁止为了“整理工作区”使用 `git reset --hard`、`git checkout --`、`git clean` 或删除未知目录。
- 先建立影响面：公共契约、后端 lowering、渲染器、shader、编辑器、工具和测试分别列出，再修改代码。
- 小改动保持最小补丁；跨模块或改变公共语义的改动先阅读相关设计文档并更新契约/映射/验证说明。
- 不把临时诊断、捕获文件、生成的二进制、个人路径和本机缓存提交到仓库。
- 失败时报告实际命令、构建目录、平台、配置、返回码和缺失的外部条件；不要用“看起来通过”替代证据。
- 没有必要时不要增加新的抽象、fallback、feature flag、全局锁或隐式同步。新能力必须说明它服务的现有用户路径和跨后端语义。

## 3. 模块边界

| 模块 | 代理应保持的责任 |
| --- | --- |
| `rhi/*.h` | 与后端无关的公共对象、句柄、能力、队列提交、命令录制、编码器、资源状态和同步契约 |
| `rhi/vulkan/` | Vulkan 对象创建、枚举/格式/布局/descriptor/barrier 转换、WSI、validation 和 Vulkan interop |
| `rhi/d3d12/` | D3D12 resource state、descriptor heap/root signature、fence、command list 和 debug layer lowering |
| `rhi/metal/` | Metal 4 availability、queue/allocator/command buffer/encoder、argument table、residency、barrier 和 drawable lowering |
| `render/` | frame scheduling、pass 编排、upload、draw stream、GPU-driven 数据和 renderer 级策略 |
| `scene/` | 场景/编辑器 CPU 状态、命令历史、选择/拾取和运行时映射 |
| `loader/` | glTF/KTX2/SDF 等资源读取和 CPU 侧转换 |
| `shaders/` | Slang 源码、跨后端 binding/reflection/线程组契约 |
| `tools/`、`tests/` | 机器可验证的边界、合同、运行、捕获和证据生成 |

公共层不能 include 或保存 `Vk*`、`VK_*`、`Vma*`、`ID3D12*`、`D3D12_*`、`MTL*`、`CAMetalLayer` 等后端类型，也不能提供 native getter 让 `app/`、`render/`、`scene/` 或 `loader/` 绕过 RHI。后端转换集中在对应 `rhi/<backend>/` 内；必要的 Vulkan interop 只能留在 backend-internal allowlist 中。

Renderer 不缓存 native handle，不根据 `VkImageLayout`、D3D12 resource state 或 Metal encoder 细节写分支。跨后端差异通过公共 capability tier、明确的 `unsupported`/错误结果或后端内部 lowering 表达，禁止静默降级。

## 4. 公共 RHI 不变量

这些规则同时适用于 Vulkan、D3D12 和 Metal 4。修改公共头或后端实现时必须保持它们成立：

- 资源使用 typed `index + generation` handle。stale handle、错误资源种类、越界 slot 和错误 parent 关系必须在 Debug/contract 路径可诊断。
- `CommandBuffer` 的合法状态遵循 `idle/reusable -> recording -> executable -> submitted -> reusable`。recording 期间只允许一个活跃 encoder；end、submit、reset 的非法顺序必须可检测。
- `CommandAllocator` 只能在其关联提交的 GPU 工作完成后 reset。不能按 CPU frame index 猜测完成；使用 `SubmissionToken`、fence/timeline 或等价的 backend completion evidence。
- 物理资源、view、argument table、descriptor/index 和 pipeline 的复用/销毁必须延迟到最后一次 GPU 使用完成。父资源必须覆盖子 view/table 的生命周期。
- Queue 提交表达 wait -> command buffers -> signal 的批次语义。不要在录制或普通 submit 中隐藏 `waitIdle`、全局 flush 或设备级同步。
- `StageBarrier` 表达常规 producer/consumer 阶段依赖；`ResourceBarrier` 表达资源状态、layout、ownership、present 或 aliasing 等需要资源范围的转换。公共枚举不等于任何一个后端的整数枚举。
- ArgumentTable/descriptor 内容在 GPU 仍可能读取时不得原地改写；需要更新时使用版本化、快照或明确的 retirement 规则。
- `GpuPtr` 只表示可寻址 buffer memory。纹理、sampler、view、pipeline 和 native object 不能伪装成 GPU pointer。
- FrameContext、UploadManager、RenderGraph、DrawStream 是 renderer/service 组合，不把它们重新塞回 `Device` 的隐式接口。

设计这些不变量时参考 Vulkan 的对象/命令缓冲/同步有效用法、Metal 4 的可复用 command buffer 和 allocator、DirectX 12 enhanced barriers/resource binding 的生命周期要求。外部规范不能成为绕过本项目契约的理由。

## 5. 后端实现规则

### Vulkan

- Vulkan 专有枚举、扩展结构、`VkImageLayout`、`VkDescriptorSet`、`VkShaderModule` 和 native handle 只在 `rhi/vulkan/` lowering。
- 开发/诊断构建启用 `VK_LAYER_KHRONOS_validation`。device lost、`VkResult`、queue、command buffer 和相关资源上下文必须随错误记录。
- command buffer pending 时不能 reset、修改或销毁其依赖对象。资源释放和 allocator reset 以 completion token 为准。
- Vulkan 改动至少执行适用的 contract/单元测试和 validation smoke；涉及渲染结果、同步、资源状态或 GPU-driven 数据时，按 `tools/VULKAN_SMOKE_RUNBOOK.md` 生成独立证据目录。

### D3D12

- 资源状态、enhanced barrier、descriptor heap、root signature、fence、command signature 和 DXGI 细节只在 D3D12 backend。
- descriptor heap 中仍可能被 GPU 引用的条目不能 CPU 原地复用；indirect command、root constants 和 descriptor table 的布局变更必须同步 shader/reflection/command signature。
- 修改 D3D12 真实路径时，在 Windows 上启用 D3D12 Debug Layer/InfoQueue，并记录 validation smoke；仅编译通过只能证明接口形状。

### Metal 4

- 使用 MTL4 queue/allocator/command buffer/encoder 族，不能把旧 `MTLCommandBuffer` 语义悄悄混入公共契约。
- Metal 4 command buffer 可重录重用，但不替资源建立强引用。backend 必须保持资源到 submission token 完成；allocator 只有在 GPU 完成后才能 reset。
- encoder 通过 argument table 绑定资源；不要恢复逐资源绑定的隐藏路径。正在执行的 table 需要版本化/快照或拒绝更新。
- Metal 4 资源默认按 untracked 处理。写后读、写后写、跨 encoder/queue 使用都要通过 RHI barrier/event/residency 语义显式同步。
- Metal 4 的 availability、SDK、Xcode、运行时和硬件证据分开记录。Windows 上只做 source/contract 检查，不发布 Metal 运行时结论。

### 翻译层与 workaround

参考 KosmicKrisp 的分层经验：API 语义差异集中在 backend lowering/bridge/workaround，而不是散落到 Renderer。shader 或 compute emulation 只能在 capability-gated、独立可测并有明确成本的前提下加入。

每个平台 workaround 至少记录：编号、平台/OS/driver/SDK、可复现输入或 CTS、观察到的错误、代码位置、外部 ticket（如有）、当前状态和移除条件。代码保留短编号，详细原因放在 backend 文档中。

## 6. Shader 规则

- `shaders/*.slang` 是 shader source of truth。跨后端修改必须同时检查生成的 SPIR-V/DXIL/MSL/metallib 路径、binding schema、reflection 和 threads-per-threadgroup。
- Metal 运行时不得偷偷编译手写 `.metal/.mm` 作为生产 shader 路径；遵循 `rhi/metal/README.md` 定义的离线生成/缓存/嵌入流程。
- shader ABI、argument table slot、root constants、indirect command layout 改动必须同时更新 CPU 结构、shader、reflection 和对应 contract test。
- 编译器 workaround 或 lowering 不得改变公共 RHI 的资源生命周期和同步语义。

## 7. 构建、测试与证据

优先使用 `CMakePresets.json` 的 `x64-debug`、`x64-release`、`x64-debug-tests` 和 `x64-release-tests`。Windows MSVC/Ninja 环境按 `build_debug_with_vsdevcmd.cmd` 或相关 runbook 建立；不要把已有 IDE 缓存当作验证结果。

按变更影响选择验证层级：

1. **公共头/纯 CPU 改动**：编译、相关 CTest/contract test、必要的 Python contract。
2. **RHI 边界或后端转换**：运行 `python tools/check_rhi_boundary.py --root .`，确认 `backend_include`、`vk_token`、`native_getter` 没有新增违规；baseline 缺失时记录 warning，不要未经审阅生成 baseline。
3. **shader/binding/command layout**：Slang 生成/静态 contract、对应 backend 编译和 ABI/layout 测试。
4. **Vulkan/D3D12/Metal runtime**：分别使用 validation/debug layer/API validation 和对应平台 smoke；只报告实际运行平台覆盖的后端。
5. **渲染结果或同步修复**：遵循 Observe -> Diagnose -> Patch -> Rebuild -> Recapture -> Verify；报告 capture、frame、资源/命令上下文和 evidence ID。
6. **性能改动**：先记录同一构建、同一场景、同一窗口的基线；跨 trace 使用归一化比较，不用不同持续时间的绝对毫秒直接下结论。

当前仓库的 `BUILD_TESTING=ON` 可能依赖未随当前 checkout 提供的测试源。运行 CTest 前先确认目标测试源和 build tree 存在；缺失时报告环境/checkout 不完整，不要把配置失败误诊为实现回归。

构建/测试失败不通过改写证据或放宽 guard 来“修复”。如果测试源、SDK、GPU、Xcode、RenderDoc 或 profiler 不可用，明确标记为未验证，并说明下一步需要的环境。

## 8. 性能与并发

- command recording、资源查找和普通 submit 应尽量支持独立线程；不要在热路径加入隐藏的全局锁、`waitIdle`、临时 descriptor/PSO/view 分配或每次调用的堆分配。
- 复用 allocator、command buffer、argument table 和 scratch storage 时，必须同时满足 GPU completion 和公共生命周期契约；“这一帧通常已经结束”不是同步证明。
- GPU-driven、indirect、bindless 和 descriptor 更新要保留可观察的 allocation/version/retirement counters；性能结论必须说明构建类型、validation/profiler 开销、场景、窗口和 trace 时长。
- 只有同一场景、同一窗口和相同 workload 的基线与归一化对比，才能支持性能回归或优化结论。不要用一次局部计时替代提交/等待/渲染整体证据。

## 9. RenderDoc 调查

- 使用项目级 `renderdoc-debug` skill 和 typed `rdc_*` tools。
- 不通过 shell 调用 `rdc` 或 `renderdoccmd`。
- 先收集 RenderDoc 证据，再提出根因；每个假设和根因结论都引用 evidence ID。
- 用最小源代码补丁检验当前假设。只有 rebuild、recapture 和 RenderDoc verification 全部完成，才能称为修复确认。

## 10. Superluminal CPU profiling 调查

- 使用项目级 `superluminal-perf` skill 和 typed `slp_*` tools。
- 不通过 shell 执行 `slp.py dump-json`；使用有界 `slp_*` 聚合工具。
- 每个新 trace 先调用 `slp_info`。主可执行文件符号未解析时，停止并要求带 PDB 重新捕获；不要根据以 `?` 结尾的猜测名称下结论。
- 先区分每帧 `cpu_ms` 与 `wall_ms` 的 CPU-bound、GPU-bound 或 wait-bound，再讨论 CPU 优化。不同持续时间的 trace 使用 `slp_compare` 归一化比较。
- 报告排除的 Vulkan validation/profiler overhead；Debug 构建的测量开销不能外推到 Release。
- 每个 hotspot/假设引用 tool、trace、thread、window 和数值。只有 rebuild、re-profile 和 normalized `slp_compare` 完成，才能称为性能修复确认。

环境变量：`SLP_PY` 指向 `slp.py`，`SLP_CAPTURES` 指向 trace 目录，`SLP_PYTHON` 可选；事件分析需要 `zstandard` 或系统 `libzstd`。

## 11. 完成前自检

结束任务前回答以下问题，并把答案写入最终回复：

- 改动属于哪个模块和哪条公共/后端契约？
- 是否泄露了 native API、引入了隐式同步、提前释放或 GPU 期间复用资源？
- 是否需要更新 README、映射表、workaround、shader reflection 或 runbook？
- 跑了哪些命令和测试？哪些后端/平台没有验证？
- 渲染、同步、性能或调试结论对应哪些 evidence ID、trace、capture 或日志？
- 工作区是否保留了用户原有改动，是否没有提交生成物？

## 12. 规范与项目参考

这些资料用于解释语义和验证方向；它们不能把尚未实现的能力变成项目承诺：

- [Apple: Understanding the Metal 4 core API](https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api?language=objc)：command queue/buffer/allocator、encoder、argument table、resource synchronization/barriers。
- [Microsoft DirectX-Specs](https://microsoft.github.io/DirectX-Specs/)：重点参考 [Enhanced Barriers](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html)、[Resource Binding](https://microsoft.github.io/DirectX-Specs/d3d/ResourceBinding.html) 和 CPU efficiency 相关设计。
- [Khronos Vulkan 1.4 specification](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html)：对象/内存生命周期、command buffer 状态、queue submission、同步和有效用法（重点 §3.3.1、§5.3.5、§6.1、§7）。
- [NoGraphicsAPI AGENTS.md](https://github.com/sebbbi/NoGraphicsAPI/blob/main/AGENTS.md)：作为“短、明确、可执行代理规则”的写法参考；其单一 Vulkan 目标、无标准库等项目取舍不直接适用于 MGIF。
- `README.md`、`rhi-public-contract-refactor-plan.md`：MGIF 公共 RHI 的设计目标、对象职责和迁移约束。
- `rhi/metal/README.md`、`rhi/d3d12/README.md`：后端实现、编译/运行条件和已知限制；若 README 与源码冲突，以源码和验证结果为准，并修正文档。
- `tools/VULKAN_SMOKE_RUNBOOK.md`：Vulkan Debug/Ninja/validation smoke、固定场景、marker 和证据目录协议。
- `F:\\GitHub\\mesa\\kosmickrisp-summary.md`：KosmicKrisp 的可选外部参考，说明 Vulkan→Metal 4 分层、shader lowering、encoder 分段和 workaround 文档化；当前 MGIF checkout 不包含此路径，也不依赖它才能构建或验证。
