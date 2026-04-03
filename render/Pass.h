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
  TextureHandle    textureHandle{};
  BufferHandle     bufferHandle{};

  static PassResourceDependency texture(TextureHandle handle, ResourceAccess accessMode, rhi::ShaderStage stages)
  {
    PassResourceDependency dependency{};
    dependency.type          = PassResourceType::texture;
    dependency.access        = accessMode;
    dependency.stageMask     = stages;
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
inline constexpr TextureHandle kPassGBufferColorHandle{0xF101u, 1u};
inline constexpr TextureHandle kPassGBufferDepthHandle{0xF102u, 1u};
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
