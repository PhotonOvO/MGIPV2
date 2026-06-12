# DDGI 实施计划（基于 LuxGI 方案）

> 最后更新：2026-06-13
> 状态：**草稿，等待 RHI 纯净化里程碑完结后激活**
>
> 本文件是落地到代码的可执行任务计划，对标 `rhi-migration-plan.md` 的分节风格。
> 每个 Wave 结束都能编译运行，可独立提交与回滚。
> 执行顺序：Phase D0 → D1 → D2 → D3 → D4（Phase D5 为可选后续里程碑）。

---

## 0. 背景与前置依赖

### 0.1 本里程碑定位

DDGI（Dynamic Diffuse Global Illumination）是本仓库 RHI 纯净化里程碑之后的**下一个主要里程碑**。
本文档是落地计划，聚焦"如何在 MGIF 引擎上按 Wave 增量实施 DDGI"，不是设计文档。

**前置依赖（不可跳过）：**

- RHI 纯净化 Phase 3（render/ 残留 Vulkan 原生调用清零）和 Phase 4（边界守卫 signal 降至 0/0/0）**必须全部完成后**，本里程碑才可启动。
- 启动检查：`tools/check_rhi_boundary.py` 在 `app/`、`render/`、`scene/`、`loader/`、`gfx/` 中三类 signal（backend_include、vk_token、native_getter）均为 0；`.rhi-boundary-allow` 中不存在标记为 `migration_residual` 的待清除条目。
- 满足上述条件后，在 `render/Pass.h` 新增 DDGI PassHandle 常量，标志 DDGI 里程碑正式启动。

### 0.2 与 rhi-migration-plan.md 的关系

本计划**复用**已有 RHI 接口（`rhi::Device`、`rhi::TextureFormat`、`GpuPtr`、`ArgumentTable`、`PassExecutor`），不重复 RHI 迁移工作。所有新增 render/ 代码只许通过 `rhi::` 接口访问 GPU 资源——这是本计划的强制硬约束（见 2.3 节）。

### 0.3 LuxGI 本地克隆

LuxGI 本地克隆：`F:\GitHub\LuxGI`（实施阶段可直接对照源码；本文档全部参数均已经 LuxGI 实际源码核实，非 README 推断值）。

---

## 1. LuxGI 架构解读

本章参数全部以 LuxGI 实际源码为准（本地克隆 `F:\GitHub\LuxGI`），标注文件路径便于实施时对照。

### 1.0 管线编排顺序（RenderGraph.cpp:74-115）

LuxGI 管线编排顺序：

AccelerationStructure → Bindless → ShadowMap → GBuffer → **GlobalDistanceField**（Mesh SDF 合成 + Global SDF）→ **GlobalSurfaceAtlas**（Surface Cache + Light Cache）→ **DDGI**（trace_rays → probe_update → border_update → sample_probe → end_frame ping-pong）→ RaytracedReflection → RaytracedShadow → DeferredLighting → Atmosphere/Skybox → DDGIVisualization/SDFVisualizer → Final。

MGIF 对应顺序：GBuffer → GlobalSDF（D1 阶段引入）→ DDGI（D2 引入）→ DeferredLighting（D3 采样接入）。

### 1.1 DDGI 核心参数（DDGIRenderer.h/.cpp 核实）

以下参数全部来自 LuxGI 实际源码，非估算：

- **八面体映射分辨率**：irradiance 8×8 texel（`IrradianceOctSize = 8`，DDGIRenderer.h:17）；depth 16×16 texel（`DepthOctSize = 16`，DDGIRenderer.h:18）；各 +2px padding（border），atlas 布局 `(OctSize+2) × probeCounts.x*y` 宽、`(OctSize+2) × probeCounts.z` 高（DDGIRenderer.cpp:182-185）。
- **纹理格式**：irradiance atlas **RGBA16F**（双缓冲 ×2）；depth atlas **RG16F**（存 mean depth + mean depth²，双缓冲 ×2）；radiance 中间纹理 **RGBA16F**（raysPerProbe × totalProbes 布局）；directionDepth **RGBA16F**（DDGIRenderer.cpp:173-204）。
- **Probe grid 场景 AABB 自适应**：`probeCounts = ivec3(sceneLength / probeDistance) + 2`，`probeDistance = 1.5f` 默认（DDGIRenderer.cpp:520-527）。
- **每 probe 光线数**：运行时默认 **256**，push constant 默认 128（DDGIRenderer.h:41,48）。
- **hysteresis = 0.98**：时域混合系数，`mix(result, prevResult, hysteresis)`（ProbeUpdate.glsl:145）。
- **ddgiGamma = 5.0**：irradiance 存储伽马编码/解码（DDGICommon.glsl:217,230）。
- **depthSharpness = 50.0**：depth update 权重 `pow(dot, sharpness)`。
- **normalBias = 1.0 / 0.1**；**maxDistance = probeDistance × 1.5**。
- **Ray 方向**：spherical Fibonacci 均匀分布 + 每帧随机旋转矩阵（push constant 传入，DDGICommon.glsl:42-52，GIRays.rgen:49）。
- **LuxGI 未实现 probe relocation / classification**：无 offset/state 字段，使用固定网格——MGIF 首版同样不做（Phase D5 后续如需参考 RTXGI SDK）。
- **无限反弹**：ray 命中点着色时 `indirectLighting()` 调用 `sampleIrradiance()` 采**上一帧** probe atlas（Lighting.glsl:79-84）；end_frame 做 ping-pong 翻转。

### 1.2 软件光追 SDF 路径（GISDFRays.comp:63-128）

LuxGI 软件光追为独立 compute pass，不依赖硬件 RT：

- compute local_size 16×1×1；dispatchX = ceil(raysPerProbe/16)；dispatchY = 总 probe 数。
- `tracyGlobalSDF()` sphere-march 策略：先采 **mip 纹理粗查询**（若距离小于 chunk 阈值则切换细纹理），最大 **250 步/级联**，命中阈值 `minSurfaceThickness` 随行进距离自适应，步进为 `max(dist × stepScale, voxelSize)`（SDFCommon.glsl:98-194）。
- **命中**：采 GlobalSurfaceAtlas（Surface Cache）取辐射度；**未命中**：采 skybox/IBL。
- 硬件 RT 路径（GIRays.rgen/rchit）与软件路径结构相同，仅 trace 与命中取材质方式不同——MGIF 只移植软件路径。

**SDF 基础设施（源码核实）：**

- **Mesh SDF**（SDFBaker.cpp:52-205）：离线烘焙，`targetTexelPerMeter = 3.0`，分辨率 clamp 到 32–128³，`acc::BVHTree` 加速（bvh_tree.h 外部库），R16F、归一化 `(d+1)/2`，最多 3 级 mip。
- **Global SDF**（GlobalDistanceField.h:19-27，.cpp:82-191）：2–4 级联（按 Quality），分辨率 **192³**，chunk 大小 32 体素 + margin 4，mip 缩小因子 4；chunk 级动态烘焙更新。
- **Surface Atlas**（GlobalSurfaceAtlas.cpp:37-91）：atlas 4096²，每物体 6 面正交投影捕捉，tile 尺寸 32–128px；**分帧更新：静态面 120 帧/光照 15 帧轮换**——本计划首版用简化命中着色替代（Phase D5 后置）。

### 1.3 SampleProbe 采样权重公式（DDGICommon.glsl:163-233）

8 邻居 probe 加权采样，完整权重链如下（顺序相乘，任何分量接近 0 均可提前跳过该 probe）：

1. **三线性权重**：根据采样点在 probe 网格中的位置计算双三线性插值系数。
2. **法线权重**：`square((dot(dirToProbe, N) + 1) * 0.5) + 0.2`，确保背向 probe 不会完全剔除。
3. **切比雪夫可见性**：`Var / (Var + (d - mean)²)` 立方，其中 Var = mean² - mean²（depth atlas 存 mean + mean²）。
4. **crush 压缩**：weight < 0.2 时三次多项式平滑压缩到 0，消除漏光。

irradiance 读取后解码：`pow(x, ddgiGamma * 0.5)` 后再平方（等价于 pow(x, ddgiGamma)），最终 `2π × netIrradiance`。

### 1.4 Shader 模块清单（LuxGI 命名，MGIF 翻译参考）

LuxGI 使用 GLSL 460；MGIF 需翻译为 Slang（.slang 文件，CMake compile_slang 离线编译 SPIR-V）。

**DDGI/ 组（`Assets/shaders/DDGI/`）：**

| LuxGI shader | MGIF 翻译目标 | 说明 |
|---|---|---|
| GIRays.rgen/rchit | 不移植（HW RT，移动端无法用） | — |
| GISDFRays.comp | shaders/ddgi/GISDFRays.slang | 软件 SDF trace，核心移植目标 |
| IrradianceProbeUpdate.comp | shaders/ddgi/IrradianceProbeUpdate.slang | hysteresis=0.98，ddgiGamma=5.0 伽马编码 |
| DepthProbeUpdate.comp | shaders/ddgi/DepthProbeUpdate.slang | depthSharpness=50 权重 |
| IrradianceBorderUpdate.comp | shaders/ddgi/IrradianceBorderUpdate.slang | border texel 复制 |
| DepthBorderUpdate.comp | shaders/ddgi/DepthBorderUpdate.slang | border texel 复制 |
| SampleProbe.comp | shaders/ddgi/SampleProbe.slang | 8 邻居加权采样 + 切比雪夫 |
| ProbeVisualization.vert/.frag | shaders/ddgi/ProbeVisualization.slang | 调试小球渲染 |

**SDF/ 组（`Assets/shaders/SDF/`）：**

| LuxGI shader | MGIF 翻译目标 | 说明 |
|---|---|---|
| GlobalSDFClear.comp | shaders/sdf/GlobalSDFClear.slang | 清零 Global SDF volume |
| GlobalSDFMipmap.comp | shaders/sdf/GlobalSDFMipmap.slang | mip 取 min（非 avg） |
| SDFCommon.glsl | shaders/sdf/SDFCommon.slang | tracyGlobalSDF 球进函数，包含 mip 粗查询逻辑 |

---

## 2. MGIF 现状映射

### 2.1 可直接复用项

| 现有能力 | 证据 | 备注 |
|---|---|---|
| Compute 全链路 | `rhi/RHIEncoder.h:136-159` ComputeEncoder（dispatch/dispatchIndirect/setRootConstants/setRootPointer） | DDGI 全为 compute pass |
| Buffer Device Address / GpuPtr | `rhi/RHIDevice.h` `getBufferGpuAddress(BufferHandle)` 返回 GpuPtr | ray trace pass 需要 probe 位置 BDA |
| Pass 框架 | `render/Pass.h`（PassNode + 预定义 Handle 常量）、`render/PassExecutor.h/.cpp` | 新增 DDGI Pass Handle 常量即可接入 |
| Compute pass 参考样板 | `render/passes/GPUDrivenAOPass.cpp`（trace+denoise 双 compute pass，storage image 读写） | DDGI pass 布局对照此样板 |
| Atlas 管理参考 | `render/IBLResources.h/.cpp`（compute 生成 irradiance/prefilter，双缓冲） | irradiance/depth atlas 管理仿照此模式 |
| GPU 场景数据 | `render/GPUSceneRegistry.h`（对象 buffer + 地址表）、`render/GPUMeshletBuffer.h`（meshlet 几何） | ray shading 读取每对象平均反照率 |
| Slang shader 体系 | shaders/*.slang，CMake compile_slang | LuxGI GLSL 需翻译为 Slang |
| 时域计数器 | m_temporalFrameCounter（单调递增） | ping-pong 奇偶判断必须用此计数器（见 2.3 节约束） |
| TAA motion vector | 已有 velocity 纹理（kVelocityIndex=7） | DDGI 不直接使用，但与 TAA 并存需注意 barrier 顺序 |

### 2.2 缺失项（DDGI 需新建）

- 无任何 RT（无 ray query/AS/BLAS/TLAS）
- 无任何 SDF 基础设施（无 Mesh SDF、无 Global SDF volume）
- 无 light probe / irradiance 探针系统
- 无 SH 缓存、无 SSGI
- 间接光现状：IBL + 固定 ambient `vec3(0.1, 0.12, 0.15)`（`render/RenderTypes.h:48`）——DDGI 采样将替换/混合此项

### 2.3 硬约束（所有 Wave 强制执行）

以下约束贯穿本里程碑全程，每 Wave 验收时均须检查：

**约束 1：RHI 边界守卫（最高优先级）**

所有新增 render/ 代码**只许用 `rhi::` 接口**，严禁 Vulkan 原生调用（VkImage/VkBuffer/vkCmd*/VkDescriptorSet 等 Vk 前缀符号）。`tools/check_rhi_boundary.py` 棘轮基线**只减不增**——每 Wave 提交前必须运行守卫脚本，确认三类 signal 均未新增。违反此约束意味着 DDGI 里程碑目标（上层代码图形 API 无关）失败。

**约束 2：构建环境**

MSVC 必须经 vcvars64 环境构建（直接运行 cmake --build 会因 cstdint 缺失而失败，这是已验证的既有环境约束）。执行节奏：**每 Wave 编辑→构建通过→提交**（原子提交），构建失败不提交。

**约束 3：双平台可构建（Windows + Android）**

移动端（Adreno/Mali）无硬件 RT，因此 MGIF DDGI **只走 SDF 软件 trace 路径**（即 GISDFRays 路径），不移植 LuxGI 的 GIRays 硬件 RT 路径。所有"是否支持硬件 RT"的路径分支通过 `DeviceCapabilities::supportsHardwareRT` 门控，**禁止使用 `#ifdef VULKAN`** 等平台宏。

**约束 4：frameIndex 陷阱——必须使用 m_temporalFrameCounter**

时域 ping-pong、奇偶判断、"是否为首帧"判断，**必须使用单调递增的 `m_temporalFrameCounter`**，严禁使用 0,1,2 环形 frameIndex（飞行帧索引）。

前车之鉴：TAA 修复（260612-nki）根因正是 frameIndex 环索引导致 `&1` 奇偶错乱，引发描述符滞后一帧和 ping-pong 未定义行为。DDGI atlas 双缓冲翻转（end_frame ping-pong）同样依赖奇偶判断，误用环索引会导致每帧读写同一张 atlas（未定义行为→收敛失败→画面闪烁）。

**约束 5：前置里程碑依赖**

DDGI 里程碑启动的前提是 RHI 纯净化 Phase 3/4 全部完成，`check_rhi_boundary` 基线达到 0/0/0。在此之前，本文档仅作规划参考，不得启动任何 Wave 实施。

**约束 6：验证手段**

每 Wave 视觉验收使用 renderdoc-mcp 帧捕获。注意：`get_cbuffer_contents` 不计 dynamic offset，cbuffer 值以 `debug_pixel` 结果为准。

---

## 3. 总体设计决策

### 3.1 移动端裁剪决策表

| 维度 | LuxGI 实际值（源码核实） | MGIF 移动端方案 | 理由 |
|---|---|---|---|
| 每 probe 光线数 | 256（运行时默认，DDGIRenderer.h:41） | 32–64（可配置，分帧摊销） | 移动端带宽/compute 预算有限 |
| Probe grid | 场景 AABB 自适应，probeDistance=1.5，+2 边界 | 同方案，另加上限钳制防爆显存 | 方案已经验证且自适应，无需更改 |
| Irradiance texel | 8×8 + 2 border，RGBA16F | 同；低配可降到 6×6 | 8×8 已是 DDGI 理论下限，建议保留 |
| Depth texel | 16×16 + 2 border，RG16F | 同或 14×14 | 切比雪夫精度依赖 depth texel 数 |
| hysteresis | 0.98（ProbeUpdate.glsl:145） | 0.97–0.98 可调 | 移动端帧率不稳可适当降低 |
| 伽马编码 | ddgiGamma=5.0（DDGICommon.glsl:217,230） | 同（ddgiGamma=5.0 保留） | 带宽不变，精度更优 |
| Mesh SDF | 离线 SDFBaker，32–128³ R16F，BVH 加速，3 级 mip | 离线（tools/），32–64³ | 移动端纹理带宽/内存预算 |
| Global SDF | 192³ × 2–4 级联，chunk 32 体素动态烘焙 | 先单级联 128³ 全量合成，chunk 动态更新后置 | 首版简化，D5 阶段再升级级联 |
| Surface Cache | 4096² atlas，6 面捕捉，120/15 帧分帧 | **后置/可选**——首版简化命中着色 | Surface Cache 复杂度高，单独列为 Phase D5 |
| 简化命中着色 | — | SDF 梯度估算法线 + 太阳光 SDF 阴影 + 每对象平均反照率 + miss 采天空/IBL | Surface Cache 缺失时的合理 fallback |
| Ray march 步数 | 250 步/级联（SDFCommon.glsl:98） | 64–128 步，保留 mip 粗查询策略 | 移动端 ALU 预算；mip 粗查询抵消精度损失 |
| 更新频率 | probe 每帧全量；Surface Cache 分帧 | probe 分帧轮转更新子集（1/N，N=4–8） | 移动端帧时间预算 |
| 反射/软阴影/SVGF/DFAO | 完整实现 | **明确划出本计划范围外**（远期扩展） | 本里程碑只做 diffuse GI |
| relocation/classification | LuxGI 未实现 | 同样不做；如需参考 RTXGI SDK，列为 Phase D5 远期 | LuxGI 本身无此实现，固定网格已足够 |
| 硬件 RT | LuxGI 有 GIRays HW 路径 | **不实现**（移动端无此能力） | Adreno/Mali 不支持 VK_KHR_ray_tracing |

### 3.2 明确不做事项

| 决策 | 理由 |
|---|---|
| 不实现反射/软阴影/SVGF | 列为远期扩展；本里程碑聚焦 diffuse GI |
| 不在本里程碑启用 Surface Cache | Phase D5 后续；首版简化命中着色已足够验证 DDGI 收敛 |
| 不实现硬件 RT（VK_KHR_ray_tracing） | 移动端（Adreno/Mali）无此能力；SDF 软件路径已能工作 |
| 不新增 rhi::DeviceSize 或其他无意义别名 | VkDeviceSize == uint64_t，别名无类型安全增益 |
| 不在 render/ 引入任何 Vulkan 原生调用 | 所有 render/ 代码走 rhi:: 接口（硬约束 1） |
| 不实现 probe relocation/classification | LuxGI 本身未实现；固定网格对大部分场景足够 |

### 3.3 核心多后端不变量

- render/ 中所有新代码的资源类型只使用 `BufferHandle`、`TextureHandle`、`GpuPtr`、`rhi::TextureFormat`、`rhi::PassHandle`，不使用任何 Vk 前缀类型。
- Pipeline 创建只通过 `ComputePipelineDesc` + `rhi::Device::createComputePipeline()`。
- Dispatch 只通过 `ComputeEncoder::dispatch()` 或 `dispatchIndirect()`。
- Root constants 通过 `setRootConstants()` 传递；大型常量 buffer 通过 `GpuPtr`（BDA）传递。
- 新增纹理格式使用 `rhi::TextureFormat::rgba16Sfloat` / `rhi::TextureFormat::rg16Sfloat` 等枚举，不使用 VkFormat 字面量。

---

## 4. Phase / Wave 分解

每个 Wave 包含：目标、前置依赖、新增与改动文件清单、shader 清单、数据结构与接口草案、验收标准、提交样例。

---

### Phase D0：前置准备

**Phase D0 目标**：确认 RHI 纯净化完结，建立 DDGI 里程碑骨架（配置结构体 + Pass Handle 常量）。

---

#### Wave D0-1：DDGI 配置骨架与 Pass Handle 注册

**状态**：[x] 完成（2026-06-13）

**目标**

确认 `check_rhi_boundary` 基线 0/0/0 且 `.rhi-boundary-allow` 无 `migration_residual` 待清条目；在 `render/Pass.h` 追加 DDGI 相关 PassHandle 常量；新增 `render/DDGIConfig.h` 定义所有可调参数。

**前置依赖**

RHI 纯净化 Phase 3/4 全部完成（STATE.md 显示 Phase 3/4 complete，守卫 0/0/0）。

**新增文件**

- `render/DDGIConfig.h`

**改动文件**

- `render/Pass.h`（追加 DDGI PassHandle 常量）

**数据结构草案**

DDGIConfig 结构体包含以下字段：gridDims（uvec3，probe grid 三维尺寸）、probeSpacing（float，默认 1.5，单位为世界坐标单位）、irradianceTexelSize（uint，默认 8）、depthTexelSize（uint，默认 16）、raysPerProbe（uint，默认 64，移动端建议值；LuxGI 桌面默认 256）、hysteresis（float，默认 0.98）、ddgiGamma（float，默认 5.0）、depthSharpness（float，默认 50.0）、normalBias（float，默认 0.3）、maxDistance（float，默认 probeSpacing × 1.5，运行时计算）、updateStride（uint，默认 4，分帧轮转更新步长）、ddgiWeight（float，默认 0.5，与 IBL 混合系数）。

Pass Handle 常量（追加到 render/Pass.h）：DDGIRayTrace、DDGIProbeUpdate、DDGIBorderUpdate、DDGISampleProbe、DDGIEndFrame、DDGIDebugVisualize、GlobalSDFCompose、GlobalSDFClear、GlobalSDFMipmap。

**验收标准**

- [x] MSVC vcvars64 构建通过，无 C2 错误
- [ ] `check_rhi_boundary.py` 运行后三类 signal 均为 0，无新增
- [x] DDGIConfig 可在 `RenderDevice` 中声明成员变量而不报编译错误
- [x] 新增 PassHandle 常量与已有常量无命名冲突（DDGI 段取 0xF401–0xF409，避开 TransientAllocator 已占用的 0xF301）

**提交样例**：`feat(ddgi): add DDGIConfig + Pass handle constants`

**实施备注**：DDGIConfig 增加 enabled 字段（默认 false，行为不变门控）；用户豁免前置里程碑检查。

---

### Phase D1：SDF 基础设施

**Phase D1 目标**：建立软件 SDF trace 所需的全部 GPU 资产——离线 Mesh SDF 生成工具、资产加载器、Global SDF 合成 compute pass。

---

#### Wave D1-1：离线 Mesh SDF 生成工具与资产加载器

**状态**：[x] 完成（2026-06-13）

**目标**

离线工具 `tools/sdf_baker/`（独立构建目标）：遍历输入网格，使用 BVH 加速查询，生成 32–64³ R16F SDF，归一化存储 `(d+1)/2`，写出 `.bin` 资产文件（含包围盒 + 分辨率元数据）。`loader/SDFLoader.h/.cpp` 加载 `.bin` 并以 `TextureHandle` 注册到 RHI。

**前置依赖**

Wave D0-1 完成。

**新增文件**

- `tools/sdf_baker/main.cpp`
- `tools/sdf_baker/SDFBaker.h`
- `tools/sdf_baker/SDFBaker.cpp`
- `loader/SDFLoader.h`
- `loader/SDFLoader.cpp`

**Shader 清单**

无（纯 CPU 离线生成，不涉及 GPU shader）。

**数据结构草案**

SDFBakerConfig 包含：inputMeshPath（std::string）、outputPath（std::string）、resolution（uint，32–64 clamp）、targetTexelPerMeter（float，默认 3.0，对标 LuxGI SDFBaker.cpp:52）。

MeshSDFAsset 包含：worldBounds（AABB，float3 min + float3 max）、resolution（uvec3）、normalizedSDFHandle（TextureHandle，加载后由 RHI 持有）、isValid（bool）。

SDFLoadResult 包含：asset（MeshSDFAsset）、errorMessage（std::string）。

**验收标准**

- [x] `tools/sdf_baker` 可在 vcvars64 环境独立构建（cmake --build 指向 tools/sdf_baker target）
- [ ] 对 Stanford Bunny 或类似测试网格运行生成，输出 `.bin` 文件非空
- [ ] SDFLoader 加载后 `TextureHandle.isValid()` 为 true
- [ ] RenderDoc 帧捕获可见 SDF 3D 纹理资源，内容非全零
- [x] `check_rhi_boundary.py` 零新增（构建内守卫三类 signal 均 0/0/0 PASS）

**提交样例**：`feat(tools): add offline Mesh SDF baker + SDFLoader`

**实施备注**：
- **验收口径（用户授权偏离）**：本 Wave 唯一验收门槛为 MSVC 构建通过；实跑烘焙/RenderDoc 类验收项跳过，保留未勾，待 D1-2 接入 Global SDF 合成时一并实证。
- **BVH/距离场实现**：自包含中位切分三角形 BVH（最长轴 nth_element 切分，叶节点 ≤4 三角形），不依赖 LuxGI 的 bvh_tree.h。距离查询为 BVH 加速的点-三角形最近点（Ericson 5.1.5），近子节点优先遍历剪枝。
- **符号计算**：LuxGI 式伪法线背面投票——每体素发射 sampleCount²（默认 6²=36）个球面方向射线，取最近命中三角形几何法线判背面（dot(dir, N) > 0），背面命中数 > 50% 且有命中则取负号（对照 LuxGI SDFBaker.cpp:113-144）。
- **归一化与存储**：d / maxDistance（padded AABB 最大边长）clamp 到 [-1,1]，存 (d+1)/2，自包含 float→half（RNE 舍入）。分辨率 per-axis ceil(extent × targetTexelPerMeter=3.0) clamp 32–64（注：LuxGI 原码此处为除法，语义与参数名不符，本实现按"每米体素数"取乘法，clamp 后实际差异很小）。Z 切片多线程并行烘焙。
- **网格加载**：自包含最简 OBJ 解析器（v/f 记录、扇形三角化、支持 v//vn 等索引形式与负索引），未引入 tinygltf——离线工具保持零依赖（仅链 glm）。
- **.bin 格式**：magic "MSDF" + version(1) + resolution uvec3 + worldBounds min/max float3×2 + R16F payload（x-major），格式常量在 SDFBaker.h 与 SDFLoader.h 双侧注释强制同步。
- **3D 纹理无阻塞点**：rhi::TextureDesc 已支持 TextureDimension::e3D + Extent3D.depth，rhi::TextureFormat::r16Sfloat 已存在，VulkanDevice::createTexture 已正确处理 VK_IMAGE_TYPE_3D——SDFLoader 直接经 rhi::Device 创建 R16F Texture3D 并经 staging buffer + copyBufferToTexture 上传（Undefined→TransferDst→General 屏障链，复用 loadAndCreateImage 的 staging 退役契约），未改动任何 rhi/ 接口。

---

#### Wave D1-2：Global SDF 合成 Compute Pass

**状态**：[x] 完成（2026-06-13）

**目标**

新增 Global SDF 单级联 128³ 合成 pipeline：GlobalSDFClear（清零）→ GlobalSDFCompose（遍历 Mesh SDF 取 min 合成）→ GlobalSDFMipmap（mip 取 min 而非 avg）三个 compute dispatch，由 `render/passes/GlobalSDFPass.h/.cpp` 管理。RenderDoc 可视化 Global SDF 切片。

**前置依赖**

Wave D1-1 完成（MeshSDFAsset 与 SDFLoader 就绪）。

**新增文件**

- `render/passes/GlobalSDFPass.h`
- `render/passes/GlobalSDFPass.cpp`
- `shaders/sdf/GlobalSDFClear.slang`
- `shaders/sdf/GlobalSDFCompose.slang`
- `shaders/sdf/GlobalSDFMipmap.slang`

**Shader 清单**

- GlobalSDFClear：将 Global SDF Texture3D（128³）清零（写入最大距离值 1.0）；dispatch (128/8, 128/8, 128/8)。
- GlobalSDFCompose：遍历场景中所有已注册 MeshSDFAsset，对每个体素采样 Mesh SDF（含 AABB 变换），取 min 合成到 Global SDF；对照 LuxGI GlobalDistanceField.cpp:82-191。
- GlobalSDFMipmap：逐级 mip 下采样，核函数取 min（不是 avg），确保保守距离——对照 LuxGI GlobalSDFMipmap.comp。

**数据结构草案**

GlobalSDFVolume 包含：sdfTexture（TextureHandle，R16F 128³）、sdfMip（TextureHandle，mip chain）、worldBounds（AABB）、resolution（uint，128）、voxelSize（float，worldBounds.size / resolution）。

GlobalSDFPassData 包含：globalSDFVolume（GlobalSDFVolume）、meshSDFList（span 或 GPU buffer，存每个 MeshSDF 的 AABB + GpuPtr）、numMeshSDFs（uint）。

**验收标准**

- [x] MSVC 构建通过
- [ ] RenderDoc 帧捕获可见 GlobalSDFClear、GlobalSDFCompose、GlobalSDFMipmap 三次 dispatch，pass 标签正确
- [ ] Global SDF Texture3D 切片颜色合理（距离场形状可辨，非全零/全 max）
- [x] `check_rhi_boundary.py` 零新增（构建内守卫 PASS）

**提交样例**：`feat(render): add GlobalSDF compose/clear/mipmap compute passes`

**实施备注**：
- **验收口径（用户授权偏离）**：本 Wave 唯一验收门槛为 MSVC 构建通过（含 compile_slang 构建期 shader 编译）；RenderDoc 类验收项跳过保留未勾，待场景注册真实 MeshSDFAsset 后一并实证。
- **Shader 平铺路径（用户授权偏离）**：CMake 只 glob `shaders/*.slang` 不递归子目录，三个 shader 平铺为 `shaders/sdf_global_clear.slang`、`shaders/sdf_global_compose.slang`、`shaders/sdf_global_mipmap.slang`（替代计划中的 `shaders/sdf/` 子目录路径）。
- **Mip 方案**：未采用独立低分辨率纹理——`rhi::TextureDesc::mipLevels=3` + 逐 mip `createTextureView`（`ImageViewType::e3D`）在单纹理上建 min mip 链（HiZDepthPyramid 已验证该 RHI 能力），Mipmap shader 每级 2×2×2 取 min（保守距离），逐级 dispatch + compute→compute barrier。未改动任何 rhi/ 接口。
- **Compose 空列表 early-out**：`GlobalSDFPassData` 以 `GlobalSDFMeshEntry`（AABB + TextureHandle）列表实现（`setMeshSDFList`）；列表为空时跳过 Compose dispatch（仅 Clear + Mipmap），体素保持清除值 1.0（最大归一化距离），可编译可运行。本 Wave 场景尚无注册资产。
- **纹理数组访问方式**：mesh SDF 经 `combinedImageSampler` 固定数组（`LGlobalSDFMaxMeshSDFs=8` 槽，`Sampler3D[]` + 三线性 clamp 采样器）绑定，未用 bindless；空槽复用最后一个有效 view（HiZ mip 数组同款写满模式）；mesh 数量经 `GlobalSDFComposeUniforms`（per-frame uniform buffer）传递而非 root constants（per-mesh AABB 数组超 128B push 预算）。AABB 外体素用到 AABB 距离作保守下界；mesh 解码距离 = (s×2−1)×meshMaxDistance（meshMaxDistance = padded AABB 最大边长，与 sdf_baker 归一化约定同步）。
- **enabled 门控**：`GlobalSDFPass::execute` 开头 `if (!m_renderer->getDDGIConfig().enabled) return;`（默认 false），默认渲染行为零改变；`RenderDevice`/`GPUDrivenRenderer` 新增 `getDDGIConfig()` 访问器。
- **接入点**：pass 注册于 `GPUDrivenRenderer::init` 的 `m_passExecutor.addPass` 序列（clusteredLightCulling 之后、CSM shadow 之前，帧内早期）；资源 init 在 `initPhase7Resources/bindPhase7PassResources` 之后；依赖声明为空（资源全 pass 私有，barrier 内部管理），Undefined→General 首帧惰性转换（HiZ 同款 mutable 标志）。
- **世界包围盒**：暂用固定默认 [-16,16]³（voxelSize=0.25），场景驱动包围盒留待后续 Wave。

---

### Phase D2：DDGI 探针系统

**Phase D2 目标**：完整的 probe atlas 资源管理 + SDF ray trace + probe irradiance/depth 更新 + border 更新。

---

#### Wave D2-1：DDGIProbeVolume 资源管理

**状态**：[x] 完成（2026-06-13）

**目标**

`render/DDGIProbeVolume.h/.cpp` 管理 probe grid 的三组 atlas 纹理（irradiance atlas、depth atlas、radiance 中间纹理）以及 probe 世界坐标位置 buffer（BDA 可访问）。双缓冲结构（current + history），用于后续 ping-pong 无限反弹。

**前置依赖**

Wave D0-1 完成（DDGIConfig 可用）。

**新增文件**

- `render/DDGIProbeVolume.h`
- `render/DDGIProbeVolume.cpp`

**Shader 清单**

无（仅资源管理，本 Wave 无 compute dispatch）。

**数据结构草案**

DDGIProbeVolumeDesc 包含：gridDims（uvec3）、probeSpacing（float）、irradianceTexelSize（uint，默认 8）、depthTexelSize（uint，默认 16）、sceneBoundsMin（float3）、sceneBoundsMax（float3）。

DDGIProbeVolume 持有：irradianceAtlas（TextureHandle，RGBA16F，current）、irradianceAtlasHistory（TextureHandle，RGBA16F，history）、depthAtlas（TextureHandle，RG16F，current）、depthAtlasHistory（TextureHandle，RG16F，history）、radianceBuffer（TextureHandle，RGBA16F，raysPerProbe × totalProbes）、probePositionBuffer（BufferHandle，BDA 可访问，存 totalProbes 个 float4）、config（DDGIConfig 副本）、totalProbes（uint，gridDims.x × gridDims.y × gridDims.z）。

probe 世界坐标由 `sceneBoundsMin + ivec3(probeIndex) × probeSpacing` 算出，存入 probePositionBuffer（upload 一次或按场景变化重算）。

**验收标准**

- [x] MSVC 构建通过
- [x] DDGIProbeVolume 构造/析构全程无 Vulkan 原生调用（vkCreate*/vmaCreate* 等不出现在 render/ 代码中）
- [x] `check_rhi_boundary.py` 零新增（构建内守卫 PASS）
- [ ] RenderDoc 可见 irradianceAtlas、depthAtlas、radianceBuffer 三个纹理资源已分配

**提交样例**：`feat(render): add DDGIProbeVolume resource management`

**实施备注**：
- **验收口径（用户授权偏离）**：本 Wave 唯一验收门槛为 MSVC 构建通过；RenderDoc 验收项跳过保留未勾（enabled 默认 false，资源默认不分配，待启用后实证）。
- **enabled 门控（用户授权偏离）**：资源分配在 `GPUDrivenRenderer::init` 中以 `getDDGIConfig().enabled` 门控——enabled=false（默认）时不分配任何 GPU 资源，默认渲染行为与显存占用零改变；`shutdown` 中 `deinit()` 对未初始化状态是 no-op。
- **gridDims 自适应与钳制**：`computeGridDims` = `ivec3(sceneLength / probeSpacing) + 2`，逐轴钳制到 `kMaxGridDims{24,16,24}`（每轴下限 2）；解析后的 gridDims 回写 `m_ddgiConfig.gridDims` 作为后续 Wave 的单一事实源。世界包围盒暂沿用 GlobalSDFPass 的默认 [-16,16]³，场景驱动包围盒留待后续 Wave。
- **Atlas 布局（LuxGI）**：宽 = (texelSize+2) × gridDims.x×gridDims.y + 2、高 = (texelSize+2) × gridDims.z + 2；irradiance RGBA16F / depth RG16F 各双份（current+history），radiance 中间纹理 RGBA16F（宽=raysPerProbe、高=totalProbes）；usage 均为 `storage | sampled`（compute 写 + 后续采样），单 mip、gpuOnly。
- **probePositionBuffer 上传路径**：`cpuToGpu + storage | shaderDeviceAddress + allowGpuAddress=true`（沿用 gpuCullingObjectBuffer 的 host-visible BDA 模式），`mapBuffer + memcpy` 一次性上传，无需 staging/命令缓冲；CPU 端按 x→y→z 线性化计算 `sceneBoundsMin + ivec3(x,y,z) × probeSpacing`，存 float4（w=1）；地址经 `getBufferGpuAddress` 缓存为 `GpuPtr`。
- **swapAtlases 已预置**：`std::swap` 交换 irradiance/depth 的 current↔history 句柄（不动 radiance），供 D4-1 ping-pong 使用；调用方按约束 4 必须以 `m_temporalFrameCounter`（单调递增）驱动翻转，严禁飞行帧环索引。
- **接入点**：`GPUDrivenRenderer` 直接持有 `DDGIProbeVolume m_ddgiProbeVolume` 成员（值语义，仿 HiZDepthPyramid/IBLResources 资源类模式而非 pass 模式），提供 `getDDGIProbeVolume()` 访问器；`DDGIProbeVolume::makeDesc(config, boundsMin, boundsMax)` 从 DDGIConfig 构造 desc。

---

#### Wave D2-2：GISDFRays SDF Ray Trace Compute Pass

**状态**：[x] 完成（2026-06-13）

**目标**

实现 `shaders/ddgi/GISDFRays.slang`：对每个 probe 发射 N 条光线（spherical Fibonacci 分布 + 每帧随机旋转矩阵），通过 Global SDF sphere-march（mip 粗查询→细纹理，最大 64–128 步，命中阈值自适应），简化命中着色（SDF 梯度估算法线 + 太阳光 SDF 阴影 + 每对象平均反照率；miss 采天空/IBL），写入 radianceBuffer（RGBA16F）。新增 `render/passes/DDGIRayTracePass.h/.cpp`。

**前置依赖**

Wave D1-2（Global SDF pass 与 GlobalSDFVolume 就绪）；Wave D2-1（DDGIProbeVolume 就绪）。

**新增文件**

- `render/passes/DDGIRayTracePass.h`
- `render/passes/DDGIRayTracePass.cpp`
- `shaders/ddgi/GISDFRays.slang`
- `shaders/sdf/SDFCommon.slang`（sphere-march 公共函数，对照 LuxGI SDFCommon.glsl:98-194）

**Shader 清单**

GISDFRays：输入 probePositionBuffer（GpuPtr/BDA）、globalSDFVolume（Texture3D + mip）、sunDirection（float3）、skyTexture（TextureCube/IBL）、rotationMatrix（float3x3，每帧随机，通过 root constants 传入）；输出 radianceBuffer（RwTexture2D RGBA16F，raysPerProbe × totalProbes 布局，Y 轴 = probe index，X 轴 = ray index）；dispatch 配置：localSize 16×1×1，dispatchX = ceil(raysPerProbe/16)，dispatchY = totalProbes（对照 LuxGI GISDFRays.comp:63-128）；ray 方向：spherical Fibonacci 基础方向经 rotationMatrix 旋转（每帧随机旋转，DDGICommon.glsl:42-52）；sphere-march：先采 sdfMip 粗查询，若距离 < chunk 阈值则采 sdfTexture 细纹理（双级采样策略，对照 SDFCommon.glsl tracyGlobalSDF）；不使用 Vulkan RT 扩展。

**数据结构草案**

DDGIRayTracePassData 包含：probePositionBufferGpuPtr（GpuPtr）、globalSDFVolume（const GlobalSDFVolume 引用）、radianceBufferHandle（TextureHandle）、numRaysPerProbe（uint）、totalProbes（uint）、rotationMatrix（float3x3，CPU 每帧更新）、sunDirection（float3）。

随机旋转矩阵生成：CPU 每帧使用 `m_temporalFrameCounter` 为随机种子，生成随机 float3x3 旋转矩阵，通过 `setRootConstants()` 传入 shader。

**验收标准**

- [x] MSVC 构建通过（含 compile_slang 构建期 shader 编译）
- [ ] RenderDoc 可见 GISDFRays dispatch，Y 轴 dispatch count = totalProbes
- [ ] radianceBuffer 绿色通道非全零（场景中有漫反射物体时存在有效辐射度值）
- [x] 完全不调用 Vulkan RT 扩展（vkGetAccelerationStructure*/vkCmdTraceRays* 不出现，纯 compute sphere-march）
- [x] `check_rhi_boundary.py` 零新增（构建内守卫 PASS）

**提交样例**：`feat(render): add DDGI SDF ray trace pass (GISDFRays)`

**实施备注**：
- **验收口径（用户授权偏离）**：本 Wave 唯一验收门槛为 MSVC 构建通过；RenderDoc 类验收项跳过保留未勾（enabled 默认 false），待启用后统一实证。
- **Shader 平铺路径（用户授权偏离）**：CMake 只 glob `shaders/*.slang` 不递归子目录，两个 shader 平铺为 `shaders/ddgi_gi_sdf_rays.slang`、`shaders/sdf_common.slang`（替代计划中的 `shaders/ddgi/GISDFRays.slang`、`shaders/sdf/SDFCommon.slang`）；`sdf_common.slang` 为 include-only 模块，仿 `shader.ibl_common.slang` 在 CMakeLists.txt 中 EXCLUDE 出 compile_slang 列表。
- **sphere-march（单级联适配 LuxGI tracyGlobalSDF）**：粗 mip 优先查询——先采最粗 min mip（mip 2），解码距离低于阈值（2 个粗体素 = 8×voxelSize）才采 mip 0 细化；自适应命中阈值 `minSurfaceThickness = voxelHalf × clamp(stepTime/voxelSize, 0, 1)`；步进 `max(dist × stepScale, voxelSize)`；**maxSteps 实际值 96**（uniform 可调，预算区间 64–128，`DDGIRayTracePass::kDefaultMaxSteps`）。mip 链经单一 Texture3D 全 mip view + mipmapMode=nearest 采样器以 SampleLevel 显式 lod 访问。
- **BDA 读取方式**：probePositionBuffer 的 `GpuPtr.value`（uint64_t）经 per-frame uniform buffer 字段 `probePositionAddress` 传入，shader 内 `(float4*)rayTraceUniforms.probePositionAddress` 指针索引（仿 shader.rast.slang 的 sceneInfo.dataBufferAddress 模式），未用 setRootPointer。
- **Uniforms 而非 root constants**：旋转矩阵（3×vec4 列）+ 体积参数 + 光照参数 + BDA 地址合计 160B 超 128B push 预算，仿 GlobalSDFCompose 走 per-frame uniform buffer（`shaderio::DDGIRayTraceUniforms`）。
- **命中着色降级（用户授权偏离）**：反照率为常量 0.5（uniform 可调，`kDefaultAlbedo`），未接 GPUSceneRegistry 每对象平均反照率——留待后续 Wave；着色 = SDF 梯度法线（6 次中心差分）× Lambert 太阳直射 × SDF 阴影射线（命中即遮挡，二值可见性）；太阳方向/颜色取自 `RenderParams::lightSettings`（运行时真实方向光，非占位）。
- **miss 天空降级（用户授权偏离）**：miss 采常量天空色 (0.35, 0.45, 0.65)（uniform 传入）而非 IBL cubemap——IBL 绑定接线复杂度高，留待 D4-1 history/IBL fallback 一并接入。radianceBuffer 写 `rgb=radiance, a=hitDistance`（miss/probe-inside 写体积对角线长度作 max trace distance）。
- **随机旋转矩阵**：CPU 端 Shoemake 均匀随机四元数，种子为单调递增 `m_temporalFrameCounter`（约束 4，新增 `GPUDrivenRenderer::getTemporalFrameCounter()` 访问器），经 3 个 vec4 列传入，shader 内显式列组合避免矩阵布局约定问题。
- **接入点与 barrier**：pass 注册在 GlobalSDFPass 之后（Global SDF 写→读由 GlobalSDFPass 尾部 barrier 覆盖）；radianceBuffer 首帧惰性 Undefined→General 转换；dispatch 后 compute→compute textureWrites barrier 供 D2-3 probe update 消费。资源 init 在 probe volume init 之后、同受 `enabled` 门控（默认 false 不分配）。

---

#### Wave D2-3：Probe Irradiance/Depth 更新 + Border 更新

**状态**：[x] 完成（2026-06-13）

**目标**

四个 compute shader 完成 probe atlas 更新：IrradianceProbeUpdate（八面体展开，hysteresis 时域混合，ddgiGamma=5.0 伽马编码存储）、DepthProbeUpdate（存 mean depth + mean depth²，depthSharpness=50 权重）、IrradianceBorderUpdate、DepthBorderUpdate（border texel 复制，保证双线性采样正确）。新增 `render/passes/DDGIProbeUpdatePass.h/.cpp`。

**前置依赖**

Wave D2-2（radianceBuffer 写入就绪）。

**新增文件**

- `render/passes/DDGIProbeUpdatePass.h`
- `render/passes/DDGIProbeUpdatePass.cpp`
- `shaders/ddgi/IrradianceProbeUpdate.slang`
- `shaders/ddgi/DepthProbeUpdate.slang`
- `shaders/ddgi/IrradianceBorderUpdate.slang`
- `shaders/ddgi/DepthBorderUpdate.slang`

**Shader 清单**

IrradianceProbeUpdate：读取 radianceBuffer，对每个 probe 八面体展开（8×8 texel），对当前 octahedral 方向加权累积新辐照度，与历史做 `mix(newResult, prevAtlas, hysteresis)`（hysteresis=0.98，通过 DDGIConfig 传入）；存储前按 `pow(irradiance, 1.0/ddgiGamma)` 伽马编码（ddgiGamma=5.0）；对照 LuxGI ProbeUpdate.glsl:145。

DepthProbeUpdate：同样八面体展开（16×16 texel），对每条 ray 按 `pow(max(0, dot(rayDir, N)), depthSharpness)` 加权（depthSharpness=50），累积 mean depth（R 通道）和 mean depth²（G 通道，供切比雪夫可见性测试）。

IrradianceBorderUpdate：将 probe 内边缘 texel 复制到 2px border，保证 atlas 内跨 probe 双线性采样不产生错误混合；border 复制规则对照 LuxGI IrradianceBorderUpdate.comp（行/列镜像）。

DepthBorderUpdate：同上，针对 depth atlas。

**数据结构草案**

DDGIProbeUpdatePassData 包含：radianceBufferHandle（TextureHandle）、irradianceAtlasCurrent（TextureHandle，可写）、depthAtlasCurrent（TextureHandle，可写）、irradianceAtlasHistory（TextureHandle，只读）、depthAtlasHistory（TextureHandle，只读）、temporalFrameCounter（uint32，用于首帧 fallback 判断：if temporalFrameCounter == 0 则跳过 hysteresis 直接写入）、config（DDGIConfig 引用）。

注意：`temporalFrameCounter == 0` 判断首帧，**不用 frameIndex & 1**；hysteresis alpha 通过 DDGIConfig.hysteresis 传入。

**验收标准**

- [x] MSVC 构建通过（含 compile_slang 构建期 shader 编译）
- [ ] RenderDoc 帧捕获可见四次 probe update dispatch（IrradianceProbeUpdate、DepthProbeUpdate、IrradianceBorderUpdate、DepthBorderUpdate）
- [ ] 第 2 帧起 irradianceAtlas 显示 hysteresis 混合后的颜色变化趋势（非突变）
- [ ] border texel 颜色与相邻 probe 对称（RenderDoc 纹理检视器可验证）
- [x] `check_rhi_boundary.py` 零新增（构建内守卫 PASS）

**提交样例**：`feat(render): add DDGI probe irradiance/depth update + border passes`

**实施备注**：
- **验收口径（用户授权偏离）**：本 Wave 唯一验收门槛为 MSVC 构建通过（slang 编译失败即构建失败）；RenderDoc 类验收项跳过保留未勾（enabled 默认 false），待启用后统一实证。
- **Shader 平铺路径（用户授权偏离）**：CMake 只 glob `shaders/*.slang` 不递归子目录，四个 shader 平铺为 `shaders/ddgi_irradiance_probe_update.slang`、`shaders/ddgi_depth_probe_update.slang`、`shaders/ddgi_irradiance_border_update.slang`、`shaders/ddgi_depth_border_update.slang`（替代计划中的 `shaders/ddgi/` 子目录路径）。
- **公共模块 ddgi_common.slang（用户授权偏离）**：新增 include-only 模块（CMake EXCLUDE，仿 sdf_common.slang）承载八面体 octEncode/octDecode、normalizedOctCoord、getProbeId、probeAtlasInteriorOrigin（probe 索引↔atlas texel 换算，与 DDGIProbeVolume::atlasExtent 布局契约同步注释）以及 sphericalFibonacci——后者从 ddgi_gi_sdf_rays.slang 原样迁出，rays shader 改为 include，确保 trace 与 update 两侧 ray 方向重建逐位一致。
- **Ray 方向重建而非存储**：D2-2 radianceBuffer 只存 rgb=radiance/a=hitDistance（未存方向，LuxGI 的 directionDepth 纹理省略）；update kernel 经共享 sphericalFibonacci + 同帧随机旋转列重建方向。旋转矩阵经新增 `DDGIRayTracePass::makeRayRotation(temporalFrameCounter)` 静态方法共享（两个 pass 同帧同种子，严禁飞行帧环索引——约束 4）。
- **参数走 push constants**：`shaderio::DDGIProbeUpdatePush`（80B < 128B push 预算）经 setRootConstants 传入两个 update pipeline（仿 GlobalSDFDispatchPush），未走 uniform buffer——argument table 全静态、无 per-frame 资源；border pipeline 零常量（dispatch 形状即覆盖全部 probe）。
- **首帧判断方式**：`firstFrame = (temporalFrameCounter == 0) || 首次记录 dispatch（atlas Undefined→General 惰性转换 latch）`——latch 扩展覆盖运行时中途启用 DDGI 的场景（此时 counter > 0 但 atlas 无有效历史，必须直写跳过 hysteresis）。
- **读写 atlas 接线现状**：本 Wave 固定"读 history atlas、写 current atlas"（DDGIProbeVolume 双缓冲句柄），swapAtlases 留待 D4-1 启用；启用 ping-pong 时必须重建/重写本 pass 的静态 argument table（头文件已注明）。
- **Depth ray distance 约定**：D2-2 miss 写 maxTraceDistance（体积对角线，非 LuxGI 的 -1 哨兵），故 depth update 只需 `min(maxDistance, hitDist - 0.01)` 钳制，无 -1 重映射分支。
- **Barrier 链**：irradiance/depth update 两次 dispatch 相互独立（不同 atlas、只读 radiance）连续编码；之后 compute→compute textureWrites barrier 隔开 border 复制（border 读 interior 写 border ring，dispatch 内无自冒险）；尾部 compute→compute|fragment barrier 供 D3-1 SampleProbe / 调试可视化消费。radiance 写→读由 D2-2 尾部 barrier 覆盖。
- **接入与门控**：pass 注册于 DDGIRayTracePass 之后、CSM shadow 之前；execute 以 `DDGIConfig::enabled` 门控（默认 false，默认帧零改变）；initResources 仅在 enabled 时调用且校验 config texel 尺寸（8/16）与 shader numthreads 编译期值一致，不符则跳过并告警。

---

### Phase D3：采样集成

**Phase D3 目标**：将 DDGI irradiance 接入 lighting pass，替换固定 ambient；添加 probe 可视化调试 pass。

---

#### Wave D3-1：SampleProbe 接入 Lighting Pass

**状态**：[x] 完成（2026-06-13）

**目标**

新增 `shaders/ddgi/SampleProbe.slang` 实现 8 邻居完整加权采样（三线性 × 法线 × 切比雪夫 × crush）；接入 `render/passes/GPUDrivenLightPass.cpp`，替换固定 ambient `vec3(0.1, 0.12, 0.15)`（render/RenderTypes.h:48），与 IBL 做权重混合（系数为 DDGIConfig.ddgiWeight，默认 0.5）。

**前置依赖**

Wave D2-3 完成（irradianceAtlas/depthAtlas 正常更新）。

**新增文件**

- `shaders/ddgi/SampleProbe.slang`

**改动文件**

- `render/passes/GPUDrivenLightPass.cpp`（引入 DDGI 采样路径，含 DDGIConfig.ddgiWeight 门控）
- `shaders/lighting/`（相关 Slang 文件追加 DDGI irradiance 项）

**Shader 清单**

SampleProbe：输入 worldPos（float3）、normal（float3）、ddgiVolume（DDGIProbeVolumeGPU 常量）、irradianceAtlas（Texture2D）、depthAtlas（Texture2D）；输出 irradiance（float3）。

完整权重链（对照 LuxGI DDGICommon.glsl:163-233）：遍历 8 邻居 probe，权重为三线性系数 × `square((dot(dirToProbe, N) + 1) * 0.5) + 0.2` × `(Var / (Var + (d - mean)^2))^3` × crush（weight < 0.2 时三次平滑）；采样 irradianceAtlas 时加 normalBias 偏移（worldPos += normal × DDGIConfig.normalBias）；irradiance 解码：`pow(sample, ddgiGamma * 0.5)` 后平方，最终乘 `2π`。

DDGIProbeVolumeGPU 为 GPU-side 常量结构（通过 root constants 或 uniform buffer 传入）：包含 irradianceAtlasSRVIndex（uint，bindless 索引）、depthAtlasSRVIndex（uint）、gridDims（uvec3）、probeSpacing（float）、irradianceTexelSize（uint）、depthTexelSize（uint）、probePositionBufferGpuPtr（GpuPtr）、ddgiGamma（float）、normalBias（float）。

**数据结构草案**

SampleProbeInput 包含：worldPos（float3）、normal（float3）、ddgiVolume（DDGIProbeVolumeGPU，inline 值传入 shader）。

lighting pass 改动：增加 `if (ddgiEnabled) { irradiance = sampleProbe(...) * ddgiWeight; } else { irradiance = ambientColor; }`；与 IBL 合并：`finalIndirect = lerp(iblIrradiance, ddgiIrradiance, ddgiWeight)`。

**验收标准**

- [x] MSVC 构建通过
- [ ] RenderDoc debug_pixel 采样点 DDGI irradiance 项非零（可见有效间接光贡献）—— 用户授权跳过，留待视觉验收
- [ ] 关闭 DDGI（ddgiWeight=0）后画面退回 IBL + ambient，与 DDGI 接入前渲染结果一致 —— 用户授权跳过；运行时门控保证 enabled=false / weight=0 时数值路径与现状逐位一致
- [x] `check_rhi_boundary.py` 零新增（构建内置 RHI boundary guard 通过）

**实施备注（2026-06-13）**

- **SampleProbe 改为 lighting 内联 include 模块**（用户授权偏离）：LuxGI 的 SampleProbe 是独立 fullscreen compute pass 写间接光纹理；MGIF 简化为 `shaders/ddgi_sample_probe.slang` include-only 函数库（CMake EXCLUDE，同 ddgi_common.slang 模式），由 `shader.light.slang` 直接内联调用 `sampleDDGIIrradiance()`，省去一次全屏 pass 与中间纹理带宽。权重链完整对照 LuxGI DDGICommon.glsl:163-233 翻译（8 邻居三线性 × square((dot+1)*0.5)+0.2 背面 × 切比雪夫三次方 × crush<0.2 三次压缩 × vBias=(N+3·Wo)·normalBias × pow(x,γ/2) 后平方 × 2π/sumWeight）；probe 位置经 probePositionBuffer BDA 读取（与 ray trace pass 同一数据源，数值与 LuxGI 解析式 origin+coord·spacing 等价）。include 链冲突处理：`shader.light.slang` 本地的 `signNotZero(float2)` 与 ddgi_common.slang 的同签名重载冲突，删除本地副本改用 ddgi_common 版本（实现逐位相同）。
- **绑定降级方案**：lighting 输入 bindless 数组扩到 23 项（+kDDGIIrradianceAtlasIndex=21 / kDDGIDepthAtlasIndex=22）。DDGI 关闭时 atlas 视图为 null → 绑定 gbuffer0 占位视图（沿用 `viewOr` fallback 先例），shader 侧 `ddgiGridDimsAndEnabled.w==0` 门控保证永不实质采样，enabled=false 不崩且渲染逐位不变。DDGI 开启时 atlas 处于 General 布局（probe update 以 storage image 写入且不做布局迁移），故两个 SRV 描述符用 `ArgumentAccessIntent::readWrite` → GENERAL 布局匹配。
- **能量公式对齐方式**：IBL 路径中 DDGI 替换 diffuse 的 irradiance 源，权重与现有约定一致——`ddgiDiffuse = kD * ddgiIrradiance * baseColor / PI`，再 `diffuse = lerp(diffuse, ddgiDiffuse, ddgiWeight)`，ao 与 IBL intensity 仍在末端统一乘；specular 不动。非 IBL（固定 ambient）路径：`lerp(ambient*baseColor*ao, ddgiIrradiance*baseColor/PI*ao, ddgiWeight)`。
- **uniform 传参**：`LightParams`（shader_io.h）追加 5 个 vec4 + uint64 BDA + 2 uint padding（总 480 字节，C++/std140 偏移一致，沿用 DDGIRayTraceUniforms 的 BDA-in-cbuffer 先例）；C++ 侧 `updateGPUDrivenLights` 仅在 volume/BDA/视图全就绪时置 enabled=1，否则字段保持零初始化走原路径。

**提交样例**：`feat(render): integrate DDGI SampleProbe into lighting pass, replace fixed ambient`

---

#### Wave D3-2：Probe 可视化调试 Pass

**状态**：[ ] 未开始

**目标**

新增 `render/passes/DDGIDebugPass.h/.cpp` 以 instanced sphere draw 渲染每个 probe 的平均 irradiance 颜色；ImGui 开关（`m_ddgiDebugVisualize`）控制可见性；不影响主渲染路径（guard `if (!m_ddgiDebugVisualize) return`）。

**前置依赖**

Wave D3-1 完成（irradianceAtlas 有效数据）。

**新增文件**

- `render/passes/DDGIDebugPass.h`
- `render/passes/DDGIDebugPass.cpp`
- `shaders/ddgi/ProbeVisualization.slang`

**Shader 清单**

ProbeVisualization：vertex shader 通过 probePositionBuffer（BDA）获取 probe 世界坐标，instanced 渲染单位球（scale 0.1）；fragment shader 采样 irradianceAtlas 当前 probe 中心 8×8 texel 平均色；最终输出为伽马解码后颜色（pow(sample, ddgiGamma * 0.5) 后平方）。

**数据结构草案**

DDGIDebugPassData 包含：probeCount（uint）、probePositionBufferGpuPtr（GpuPtr）、irradianceAtlasSRVIndex（uint，bindless 索引）、sphereIndexBuffer（BufferHandle，共享单位球 index buffer）、sphereVertexBuffer（BufferHandle）、ddgiGamma（float）。

**验收标准**

- [ ] MSVC 构建通过
- [ ] ImGui 开关开启后 RenderDoc 可见 probe 球体 instanced draw call
- [ ] probe 颜色与场景漫反射方向一致（天空方向 probe 偏蓝，地面方向 probe 偏暖）
- [ ] ImGui 开关关闭后无额外 draw call
- [ ] `check_rhi_boundary.py` 零新增

**提交样例**：`feat(render): add DDGI probe visualization debug pass`

---

### Phase D4：无限反弹与分帧轮转

**Phase D4 目标**：利用历史 probe atlas 实现无限反弹（LuxGI end_frame ping-pong 模式）；引入分帧轮转更新降低每帧 compute 开销。

---

#### Wave D4-1：无限反弹（History Atlas Ping-Pong）

**状态**：[ ] 未开始

**目标**

ray shading 阶段（GISDFRays）读取上一帧 probe atlas（historyAtlas）采样间接光入射，命中点着色时叠加一次 SampleProbe 调用（首帧 fallback 为 IBL）；DDGIProbeUpdatePass 更新完成后交换 current/history 指针（end_frame ping-pong，LuxGI 同名操作）。

**前置依赖**

Wave D3-1 完成（SampleProbe 函数已实现并验证）。

**改动文件**

- `render/DDGIProbeVolume.h/.cpp`（持有 irradianceAtlasHistory、depthAtlasHistory；新增 swapAtlases() 方法）
- `render/passes/DDGIRayTracePass.cpp`（GISDFRays 新增 historyIrradianceAtlas 绑定，命中时调用 SampleProbe 叠加间接光）
- `render/passes/DDGIProbeUpdatePass.cpp`（每帧 update 完成后调用 volume.swapAtlases()）

**Shader 清单**

GISDFRays（改动）：新增 historyIrradianceAtlasSRVIndex 和 historyDepthAtlasSRVIndex 绑定；命中点着色：`radiance += sampleProbeIndirect(hitWorldPos, hitNormal, historyAtlas)`（首帧判断：`if (temporalFrameCounter == 0) radiance += iblSample` 代替 probe 采样）；`frameFlip = temporalFrameCounter & 1`——注意此处 temporalFrameCounter 是单调递增值，`& 1` 只用于区分两组 atlas 的角色，不是飞行帧索引。

**数据结构草案**

DDGIProbeVolume 新增字段：irradianceAtlasHistory（TextureHandle）、depthAtlasHistory（TextureHandle）、方法 swapAtlases()（交换 current/history TextureHandle 值，O(1)，无 GPU 资源分配）。

ping-pong 规则：偶数帧（temporalFrameCounter & 1 == 0）ray trace 读 irradianceAtlasHistory，probe update 写 irradianceAtlas；奇数帧相反；端帧调用 swapAtlases() 后，下一帧 current/history 角色互换。

**验收标准**

- [ ] MSVC 构建通过
- [ ] 静止场景 RenderDoc 连续两帧对比，irradianceAtlas 颜色值单调趋近收敛（不同帧间有可见但递减的差异）
- [ ] 首帧（temporalFrameCounter == 0）无空白/黑色 probe（IBL fallback 工作）
- [ ] `check_rhi_boundary.py` 零新增

**提交样例**：`feat(render): DDGI infinite bounce via history atlas ping-pong`

---

#### Wave D4-2：分帧轮转 Probe 更新

**状态**：[ ] 未开始

**目标**

每帧只更新 1/updateStride 子集的 probe（默认 updateStride=4，即每帧更新 25% probe，4 帧后所有 probe 至少更新一次）；RenderDevice 维护 `m_ddgiUpdateOffset` 计数器（`m_ddgiUpdateOffset = (m_ddgiUpdateOffset + 1) % config.updateStride`）；shader early-out 跳过非本帧子集的 probe。

**前置依赖**

Wave D4-1 完成（ping-pong 机制稳定）。

**改动文件**

- `render/DDGIConfig.h`（新增 updateStride 字段，默认 4）
- `render/RenderDevice.h/.cpp`（新增 m_ddgiUpdateOffset，每帧递增取模）
- `render/passes/DDGIProbeUpdatePass.h/.cpp`（新增 updateOffset/updateStride 参数传入 shader）
- `shaders/ddgi/IrradianceProbeUpdate.slang`（新增 early-out：if (probeIndex % updateStride != updateOffset) return）
- `shaders/ddgi/DepthProbeUpdate.slang`（同上）

**数据结构草案**

DDGIConfig 新增：updateStride（uint，默认 4）。

DDGIProbeUpdatePassData 新增：updateOffset（uint）、updateStride（uint）。

m_ddgiUpdateOffset 在 RenderDevice 每帧 update 前递增：`m_ddgiUpdateOffset = (m_ddgiUpdateOffset + 1) % m_ddgiConfig.updateStride`；初始值 0。

**验收标准**

- [ ] MSVC 构建通过
- [ ] RenderDoc 可见每帧 IrradianceProbeUpdate dispatch 的实际写入 probe 数 ≈ totalProbes / updateStride（通过 RenderDoc 写入像素计数或 debug_pixel 验证）
- [ ] 4 帧后所有 probe 至少更新一次（通过连续 4 帧 RenderDoc 捕获对比所有 probe 颜色变化确认）
- [ ] `check_rhi_boundary.py` 零新增

**提交样例**：`feat(render): DDGI staggered per-frame probe update`

---

### Phase D5：可选后续里程碑

以下功能因复杂度高/移动端适用性有限，明确列为 Phase D5 后续里程碑，本计划不实施：

- **Surface Cache 完整实现**：对照 LuxGI GlobalSurfaceAtlas.cpp（4096² atlas、每物体 6 面正交投影捕捉、tile 32–128px、40³ chunk 空间网格查找、SDF 命中点 PBR 着色、LightCache 回写、静态面 120 帧/光照 15 帧分帧更新）。
- **Global SDF 级联**：对照 LuxGI GlobalDistanceField.cpp（192³ × 2–4 级联、chunk 32 体素动态烘焙），升级首版单级联 128³ 方案。
- **Probe relocation 与 classification**：LuxGI 本身未实现；如需参考 RTXGI SDK（https://github.com/NVIDIAGameWorks/RTXGI）。
- **移动端性能调优**：Adreno tile memory 复用（`VK_EXT_rasterization_order_attachment_access`）、Mali early-z 优化、实机 RenderDoc 帧率与 GPU 计数器带宽检查。
- **Android CI 集成**：Android build 验证、设备实机运行无崩溃、帧率基线测量。

---

## 5. 风险与回退策略

| 风险 | 表现 | 回退/缓解策略 |
|---|---|---|
| SDF sphere-march 精度不足（细薄网格漏光） | 细边、薄片物体 probe 采到错误辐射度 | 增大 Mesh SDF padding margin，降低分辨率到 32³（保守距离场），调小 minSurfaceThickness |
| probe atlas 带宽过高 | 移动端帧率低于目标 | 降 irradiance texel 到 6×6，降 raysPerProbe 到 32，增大 updateStride 到 8 |
| frameIndex 环索引误用导致时域翻转闪烁 | irradianceAtlas 每帧全量覆盖或读写同一张 atlas | 代码审查强制检查所有时域判断均使用 m_temporalFrameCounter（参照 TAA 修复 260612-nki 教训）；回滚当前 Wave 重写 |
| RHI 边界守卫触发 | `check_rhi_boundary.py` 新增 signal | 立即回滚当前 Wave，重写违规代码为 rhi:: 接口调用；不提交守卫失败状态 |
| Android 构建失败 | 移动端缺失 Vulkan 扩展或头文件错误 | 将硬件 RT 相关代码（虽本计划已排除）隔离到 `DeviceCapabilities::supportsHardwareRT` 门控；检查 NDK 版本与 Vulkan 头文件兼容性；**禁止使用 `#ifdef VULKAN` 平台宏** |
| Global SDF 合成时间过长（场景网格多） | 每帧 GlobalSDFCompose dispatch 时间超 1ms | 加入 CPU 侧 frustum 剔除，只传入可见 MeshSDF；增大 voxel size 到 Global SDF 世界范围 / 128 |
| probe atlas 内存超限（大场景 probe 数爆炸） | OOM 崩溃或分配失败 | 在 DDGIProbeVolumeDesc 构造时对 probeCounts 三轴分别钳制（如最大 16×8×16 = 2048 probe）；降低 probeSpacing 增大 probeDistance |

---

## 6. 验证方案

### 6.1 每 Wave 基础门（必须同时满足）

- [ ] MSVC vcvars64 环境构建通过（cmake --build，无 C2/LNK 错误）
- [ ] `tools/check_rhi_boundary.py` 三类 signal（backend_include、vk_token、native_getter）均无新增，棘轮基线只减不增

### 6.2 SDF 视觉验证

- 运行 RenderDoc 帧捕获，检查 Global SDF Texture3D 切片（Resource Inspector → Texture3D viewer，切换 Z slice）非全零、非全 max（1.0）。
- 对比场景物体位置与 SDF 零等值面是否对齐（距离场颜色由蓝转红过渡应发生在物体表面附近）。

### 6.3 DDGI 收敛验证

- 静止场景连续捕获 8 帧，对比 irradianceAtlas 同一 probe 的颜色值——应单调趋近收敛（相邻帧差异递减，而非随机跳变）。
- 首帧（temporalFrameCounter == 0）atlas 不应为全黑（IBL fallback 已接入）。

### 6.4 光照对比验证

- 记录 DDGI 接入前（固定 ambient）RenderDoc debug_pixel（选取场景中漫反射面）基线值。
- DDGI 接入后同位置 debug_pixel：diffuse indirect 项（DDGI irradiance × ddgiWeight）增量应大于 0。
- 切换 ddgiWeight=0 后画面应退回基线（差异 < 1%）。

### 6.5 Android 验证（Phase D5）

- Android NDK 构建无错误（cmake -DANDROID_ABI=arm64-v8a ...）。
- 设备实机运行无崩溃（adb logcat 无 FATAL/SIGSEGV）。
- Mali/Adreno GPU 计数器带宽检查：片上纹理带宽 < 目标阈值（设备相关）。
- 帧率测量：以稳定 30fps 为 Phase D5 目标（probe 分帧轮转 + raysPerProbe=32 配置下）。

---

## 7. 参考资料

### 7.1 LuxGI 关键源码索引

本地克隆：`F:\GitHub\LuxGI`

| 文件 | 用途 | 实施时对照要点 |
|---|---|---|
| Code/Maple/src/Engine/Renderer/RenderGraph.cpp:74-115 | 管线编排顺序 | 确认 DDGI pass 在 GBuffer 之后、DeferredLighting 之前 |
| Code/Maple/src/Engine/DDGI/DDGIRenderer.h/.cpp | probe grid/atlas 创建、全部默认参数、dispatch 编排 | IrradianceOctSize=8、DepthOctSize=16、hysteresis=0.98、ddgiGamma=5.0；AABB 自适应 grid 公式 |
| Code/Maple/src/Engine/DDGI/GlobalDistanceField.h/.cpp | Global SDF 级联与 chunk 动态烘焙 | Phase D5 级联升级参考；首版单级联 128³ 对应简化版 |
| Code/Maple/src/Engine/DDGI/GlobalSurfaceAtlas.cpp、SurfaceAtlasTile.h | Surface Cache（Phase D5 参考） | atlas 4096²、6 面捕捉、40³ chunk 查找、120/15 帧分帧 |
| Code/Maple/src/Engine/DDGI/SDFBaker.h/.cpp | Mesh SDF 离线烘焙 | targetTexelPerMeter=3.0；BVH 加速（bvh_tree.h）；R16F 归一化 |
| Assets/shaders/DDGI/GISDFRays.comp | 软件 SDF trace 核心 shader | local_size 16×1×1；dispatch 配置；sphere-march 两级采样 |
| Assets/shaders/DDGI/IrradianceProbeUpdate.comp（ProbeUpdate.glsl） | probe irradiance 更新 | hysteresis=0.98（ProbeUpdate.glsl:145）；伽马编码 |
| Assets/shaders/DDGI/DepthProbeUpdate.comp | depth 更新 | depthSharpness=50；mean + mean² 双通道存储 |
| Assets/shaders/DDGI/IrradianceBorderUpdate.comp | border texel 复制 | 行/列镜像规则 |
| Assets/shaders/DDGI/DDGICommon.glsl | SampleProbe 权重链、spherical Fibonacci、伽马常量 | DDGICommon.glsl:42-52（Fibonacci+旋转）；163-233（8邻居权重）；217,230（ddgiGamma） |
| Assets/shaders/SDF/SDFCommon.glsl | tracyGlobalSDF sphere-march 函数 | SDFCommon.glsl:98-194；mip 粗查询→细纹理；250 步上限（MGIF 降到 64–128） |
| Assets/shaders/SDF/GlobalSDFMipmap.comp | Global SDF mip 生成 | 取 min 而非 avg（保守距离场） |

### 7.2 外部参考

- **RTXGI SDK**：https://github.com/NVIDIAGameWorks/RTXGI（probe relocation/classification 参考，Phase D5 远期，LuxGI 未实现）
- **Dynamic Diffuse Global Illumination（Majercik et al. 2019）**：DDGI 原始论文，切比雪夫可见性与权重链的理论来源
- **Vulkan Buffer Device Address Guide**：https://docs.vulkan.org/guide/latest/buffer_device_address.html（probePositionBuffer BDA 访问）

### 7.3 MGIF 代码库参考

- `render/passes/GPUDrivenAOPass.cpp`：compute pass 布局参考样板（trace+denoise 双 dispatch，storage image 读写）
- `render/IBLResources.h/.cpp`：atlas 双缓冲管理参考（irradiance/prefilter compute 生成）
- `render/passes/GPUDrivenLightPass.cpp`：lighting pass 接入点（固定 ambient 所在位置，D3 阶段替换）
- `render/RenderTypes.h:48`：固定 ambient `vec3(0.1, 0.12, 0.15)`（D3-1 替换目标）

### 7.4 MGIF 项目教训

- **260612-nki TAA ping-pong 修复**：m_temporalFrameCounter 必要性实证——飞行帧环索引 `& 1` 奇偶错乱是 TAA 未能正确累积的根因。DDGI atlas ping-pong 同样依赖此计数器，任何时域奇偶判断均须使用 `m_temporalFrameCounter` 而非 `frameIndex`（见 2.3 节约束 4）。
