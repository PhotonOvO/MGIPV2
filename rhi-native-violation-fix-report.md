# RHI 边界违规修复报告（Round 1–7 终版）

> 修复日期：2026-06-11
> 基线：审计报告 `rhi-native-violation-audit.md`（基线提交 91f7ddb 之前的状态）
> 终态提交：9f3026f
> 验收命令：`rg "Vk|VK_|Vma|vma|vkCreate|vkDestroy|rhi/vulkan|VulkanCommon|getBackendDeviceToken|getAllocatorToken" render loader app scene`（`gfx/` 目录不存在）
> 构建：vcvars64 + Ninja，0 error，Demo.exe 链接成功；`check_rhi_boundary.py` PASS 0/0/0

---

## 一、修复轮次与提交链

| 轮次 | 内容 | 提交 |
|------|------|------|
| Phase 3 W1 | GPUMeshletBuffer/GPUSceneRegistry VMA→RHI；sceneView 句柄化 | 80fb9d8 |
| Phase 3 W2 | GPUDrivenRenderer native 清零；token escape hatch 根除 | 31d9d7f |
| Phase 3 W3 | UploadUtils NativeUploadContext 删除；MeshPool arena owned 化 | 61285d8 |
| Round 1 | ImGui-only `queryImGuiNativeContext` RHI seam | 267b813 |
| Round 3 | LightResources 死 token 存储清除 | 81b002a |
| Round 4 | render 层 sampler token 清零 | d9939cf |
| Round 5 | Vulkan/VMA 初始化下沉 VulkanDevice + RHI 工厂 | 6d998de |
| Round 6 | SceneResources VkExtent2D/interop 清除；view debugName 直通 | c6c3ced |
| Round 7 | swapchain 控制接口提升（deinit/vsync/fullscreen/presentModeName） | 9f3026f |

## 二、已清理文件（验收扫描代码级零命中）

`render/GPUMeshletBuffer.{h,cpp}`、`render/GPUSceneRegistry.{h,cpp}`、`render/MeshPool.{h,cpp}`、
`render/UploadUtils.{h,cpp}`、`render/LightResources.{h,cpp}`、`render/SceneResources.cpp`、
`render/GPUDrivenRenderer.h`、`render/RenderTypes.h`、`render/passes/*`（全部）、`loader/`、`app/`、`scene/`（代码级）。
`render/GPUDrivenRenderer.cpp` 仅剩 1 条豁免 include；`render/RenderDevice.cpp/.h` 仅剩后端 include 与注释（见下表）。

## 三、剩余命中分类表

| 文件:行 | token | 分类 | 豁免 | 理由 |
|---------|-------|------|------|------|
| `render/DebugInteropBridge.cpp`（37 处） | `Vk*`/`VK_*`/`vkCreate/DestroyDescriptorPool`/`ImGui_ImplVulkan_*`/2 个后端 include | 明确豁免 | ✅ debug_bridge | D-08/D-09：唯一允许的 ImGui native 互操作文件 |
| `render/DebugInteropBridge.h:33,59` | 注释（`VK_IMAGE_LAYOUT_GENERAL` 语义、shutdown 顺序文档） | 注释残留 | ✅ | 头文件本体只暴露 RHI 类型，注释为文档 |
| `render/RHIFormatBridge.h`（37 处） | `VkFormat`/`VK_FORMAT_*` 映射表 + VulkanCommon include | 明确豁免 | ✅ render_bridge | 永久格式 seam |
| `loader/Ktx2Loader.cpp` | `vkFormat`（小写，KTX2 文件头字段名） | 明确豁免 | ✅ 文件格式 | 验收正则实际零命中；纯文件格式语义 |
| `render/GPUDrivenRenderer.cpp:7` | `rhi/vulkan/VulkanDevice.h` include | 明确豁免 | ✅ interop_cast | 行 92-93 `VulkanDeviceInterop` 纹理解析（已登记豁免） |
| `render/RenderDevice.h:4` | `VulkanCommon.h` include | 待后续修 | ❌ | 提供 `utils::findFile` 等工具符号；需把通用工具迁出 VulkanCommon（资源表里程碑一并处理） |
| `render/RenderDevice.h:47` | `VulkanResourceTable.h` include + `m_device.resourceTable` 成员 | 待后续修 | ❌ | 资源表当前由 render 层持有并注入后端；归属下沉是下一里程碑的核心项 |
| `render/RenderDevice.cpp:3-5` | `VulkanSwapchain/VulkanDevice/VulkanFrameContext.h` include | 待后续修 | ❌ | 剩余消费点：swapchain `nativeImage/nativeImageView` 注册（2 处）、`setResourceTable`、`processRetirements`、frame context `dynamic_cast`+`setResourceTable`、`VulkanDeviceInterop` 纹理 resolve、`BufferRecord` 互操作（行 6904 区） |
| `render/RenderDevice.cpp` 9 处注释（966/1055/1349/1543-44/2475×3/5164） | `VkImageView`/`ImGui_ImplVulkan`/`VK_ATTACHMENT_*`/`vmaInvalidateAllocation`/`VkDescriptorPool` 字样 | 注释残留 | 人工判断：保留 | 历史/语义说明，无代码效力；守卫不计 |
| `render/PassExecutor.cpp:39,328` | 注释（`VkMemoryBarrier2` 语义说明） | 注释残留 | 保留 | 同上 |
| `render/HiZDepthPyramid.cpp:6` | 注释（解释为何不 include VulkanCommon） | 注释残留 | 保留 | 反向说明文，无代码效力 |
| `render/FrameSubmission.h:71` | 注释（`VK_EXT_present_timing` 展望） | 注释残留 | 保留 | 文档性 |
| `scene/SceneAssetSerializer.h:12` | 注释（序列化版本变更记录提及 VkFormat） | 注释残留 | 保留 | 版本历史记录 |

**结论：非 RHI 层已无任何代码级 `Vk*`/`VK_*`/`Vma*`/`vk*`/`vma*` 函数调用、类型持有或枚举使用**（豁免文件除外）。剩余实质项只有 5 个后端 include 及其背后的 4 类消费点（资源表归属、swapchain native image 注册、frame context 类型、VulkanCommon 工具符号），属同一个"资源表/帧上下文归属下沉"后续里程碑。

## 四、本系列新增的 RHI API

| API | 说明 |
|-----|------|
| `rhi::ImGuiNativeContext` + `Device::queryImGuiNativeContext()` | ImGui-only native 查询（不含 allocator/资源句柄），唯一消费者 DebugInteropBridge.cpp |
| `rhi::createDevice()` / `rhi::createSurface()`（RHIFactory） | 后端中立构造入口 |
| `Device::initSurface(Surface&, WindowHandle)` | surface 初始化下沉（native instance/physDevice 不出后端） |
| `Device::createSwapchain(Surface&, vSync)` | swapchain 创建下沉（native device/queue/surface/cmdPool 不出后端） |
| `Device::createFrameContext(Swapchain*, frameCount)` | 帧上下文创建+接线下沉 |
| `Device::getTimestampPeriodNs()` | GPU 时间戳周期查询（替代 render 侧 `VkPhysicalDeviceProperties2`） |
| `Swapchain::deinit/setVSync/setFullscreen/getPresentModeName` | 控制接口提升（替代 4 处 VulkanSwapchain cast 与 `VK_PRESENT_MODE_*` switch） |
| `TextureViewCreateDesc::debugName` 后端消费 | view 调试命名直通（替代 render 侧 interop resolve） |

## 五、删除的 escape hatch

- `RenderDevice::getBackendDeviceToken()` / `getAllocatorToken()`（含 GPUDrivenRenderer 转发与全部 ~20 处解引用）
- `LightResources`/`MeshPool` 的 `m_backendDeviceToken/m_backendAllocatorToken` uintptr_t 存储
- `upload::NativeUploadContext` / `NativeUploadBuffer`（uintptr_t 伪装结构，整族删除）
- `DEMO_RHI_VK` / `DEMO_RHI_ALLOCATOR` 宏
- `rebindFrameBufferHandle(rhi::BufferHandle&, VkBuffer)`
- `VulkanDevice::setAllocator`（allocator 不再以任何形式出后端）
- `utils::Buffer` 在 render 层的全部使用；`fromNativeHandle<T>`、`toVkExtent` 等转换辅助
- **未新增**任何 `getNativeDevice`/`backendToken`/uintptr_t 替代通道（扫描验证 0）

## 六、ImGui interop 豁免说明

`render/DebugInteropBridge.cpp` 是唯一的 ImGui native 互操作文件（已验证 `imgui_impl_vulkan`/`ImGui_ImplVulkan` 全库仅此一处实现引用）：
- 初始化 native（instance/physDevice/device/queue/queueFamily）全部来自 `queryImGuiNativeContext()`，bridge 是唯一把 `void*` 转回 `Vk*` 的非 RHI 文件；
- ImGui 专用 `VkDescriptorPool`（`FREE_DESCRIPTOR_SET_BIT`）在 bridge 内自建自毁（D-09）；
- 纹理注册经 `VulkanDeviceInterop::resolveTextureView`（后端内部接口）；
- `GPUDrivenImguiPass.cpp` 验证零 `Vk*`/`VK_*`/`Vma*` 命中，只经 bridge 的 C++ wrapper 间接工作；
- 不返回 `VmaAllocator`，不暴露普通资源 native 句柄。

## 七、未完成事项

1. **资源表/帧上下文归属下沉**（下一里程碑）：`VulkanResourceTable` 成员、`setResourceTable`、`processRetirements`、frame context `dynamic_cast`、swapchain `nativeImage(View)` 注册、`VulkanCommon.h` 工具符号（`utils::findFile`）→ 消除后 `RenderDevice.cpp/.h` 5 个后端 include 可全部移除。
2. **`rhi::CommandBuffer::getBackendHandle()`**：仅 DebugInteropBridge 使用（取 `VkCommandBuffer` 喂 ImGui），建议收敛为 ImGui-only 命名或并入 `ImGuiNativeContext`。
3. **渲染验证未运行**（里程碑约定末尾统一做）。重点回归项：visibility-sort 上传路径（修复 ef335ce 映射指针回归后该路径重新激活）、ImGui 显示、全场景渲染输出比对。
4. **Android 构建未验证**（WSI 扩展分支与 validation 逻辑已等价保留）。
5. 注释残留（约 16 处）保留未改写——无代码效力，守卫不计；如需文案净化可一次性批处理。
