#pragma once

#include "../common/Handles.h"
#include "DrawStream.h"
#include "../rhi/RHICommandList.h"

#include <cstdint>
#include <vector>

namespace demo {

// Forward declaration to allow PassContext to carry render params without a
// hard dependency on the full RenderParams type from Renderer.
struct RenderParams;
struct GltfUploadResult;
class TransientAllocator;

struct PassContext
{
  rhi::CommandList*   cmd{nullptr};
  TransientAllocator* transientAllocator{nullptr};
  uint32_t            frameIndex{0};
  uint32_t            passIndex{0};
  // Optional per-pass rendering parameters. This enables passes to access
  // high-level render state without needing direct access to Renderer.
  const RenderParams*       params{nullptr};
  std::vector<StreamEntry>* drawStream{nullptr};
  // glTF model data for rendering meshes
  const GltfUploadResult*   gltfModel{nullptr};
  // Global bindless resource (bind once at pass start)
  BindGroupHandle           globalBindlessGroup{};
};

enum class ResourceAccess : uint8_t
{
  read,
  write,
  readWrite,
};

enum class PassResourceType : uint8_t
{
  texture,
  buffer,
};

struct PassResourceDependency
{
  PassResourceType type{PassResourceType::buffer};
  ResourceAccess   access{ResourceAccess::read};
  rhi::ShaderStage stageMask{rhi::ShaderStage::none};
  rhi::ResourceState requiredState{rhi::ResourceState::Undefined};
  TextureHandle    textureHandle{};
  BufferHandle     bufferHandle{};

  static PassResourceDependency texture(TextureHandle handle, ResourceAccess accessMode, rhi::ShaderStage stages,
                                        rhi::ResourceState textureState = rhi::ResourceState::Undefined)
  {
    PassResourceDependency dependency{};
    dependency.type          = PassResourceType::texture;
    dependency.access        = accessMode;
    dependency.stageMask     = stages;
    dependency.requiredState = textureState;
    dependency.textureHandle = handle;
    return dependency;
  }

  static PassResourceDependency buffer(BufferHandle handle, ResourceAccess accessMode, rhi::ShaderStage stages)
  {
    PassResourceDependency dependency{};
    dependency.type         = PassResourceType::buffer;
    dependency.access       = accessMode;
    dependency.stageMask    = stages;
    dependency.bufferHandle = handle;
    return dependency;
  }
};

inline constexpr BufferHandle  kPassVertexBufferHandle{0xF001u, 1u};
inline constexpr BufferHandle  kPassPointLightBufferHandle{0xF002u, 1u};
inline constexpr BufferHandle  kPassSpotLightBufferHandle{0xF003u, 1u};
inline constexpr BufferHandle  kPassPointLightCoarseBoundsHandle{0xF004u, 1u};
inline constexpr BufferHandle  kPassSpotLightCoarseBoundsHandle{0xF005u, 1u};
inline constexpr BufferHandle  kPassLightCoarseCullingUniformHandle{0xF006u, 1u};
inline constexpr BufferHandle  kPassGPUCullObjectBufferHandle{0xF007u, 1u};
inline constexpr BufferHandle  kPassGPUCullIndirectBufferHandle{0xF008u, 1u};
inline constexpr BufferHandle  kPassGPUCullStatsBufferHandle{0xF009u, 1u};
inline constexpr BufferHandle  kPassGPUCullUniformBufferHandle{0xF00Au, 1u};
inline constexpr BufferHandle  kPassGPUCullResultBufferHandle{0xF00Bu, 1u};
inline constexpr TextureHandle kPassGBuffer0Handle{0xF101u, 1u};
inline constexpr TextureHandle kPassGBuffer1Handle{0xF102u, 1u};
inline constexpr TextureHandle kPassGBuffer2Handle{0xF103u, 1u};
inline constexpr TextureHandle kPassSceneDepthHandle{0xF104u, 1u};
inline constexpr TextureHandle kPassShadowHandle{0xF105u, 1u};
inline constexpr TextureHandle kPassOutputHandle{0xF106u, 1u};      // OutputTexture (PBR result)
inline constexpr TextureHandle kPassCSMShadowHandle{0xF107u, 1u};    // CSM cascade array
inline constexpr TextureHandle kPassDepthPyramidHandle{0xF108u, 1u};
inline constexpr TextureHandle kPassSwapchainHandle{0xF201u, 1u};

class PassNode
{
public:
  template <typename T>
  struct HandleSlice
  {
    const T* data{nullptr};
    uint32_t count{0};
  };

  virtual ~PassNode() = default;

  [[nodiscard]] virtual const char*                         getName() const                           = 0;
  [[nodiscard]] virtual HandleSlice<PassResourceDependency> getDependencies() const                   = 0;
  virtual void                                              execute(const PassContext& context) const = 0;
};

class ComputePassNode : public PassNode
{
};

class RenderPassNode : public PassNode
{
};

}  // namespace demo
