#pragma once

#include "ImGuiRhiTypes.h"

#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"

#include <cstdint>
#include <memory>

namespace demo
{
  class ImGuiRhiRenderer
  {
  public:
    using TextureID = uint64_t;

    struct InitInfo
    {
      rhi::Device* device{nullptr};
      rhi::TextureFormat swapchainFormat{rhi::TextureFormat::undefined};
      uint32_t frameCount{0};
      void* window{nullptr};
    };

    ImGuiRhiRenderer();
    ~ImGuiRhiRenderer();

    ImGuiRhiRenderer(const ImGuiRhiRenderer&) = delete;
    ImGuiRhiRenderer& operator=(const ImGuiRhiRenderer&) = delete;

    void init(const InitInfo& info);
    void shutdown();
    void newFrame();

    void render(
      rhi::CommandBuffer& commandBuffer,
      rhi::TextureHandle target,
      rhi::TextureViewHandle targetView,
      rhi::Extent2D targetExtent,
      uint32_t frameIndex);

    [[nodiscard]] bool isInitialized() const;

    [[nodiscard]] TextureID registerTexture(
      rhi::TextureViewHandle view,
      rhi::ArgumentAccessIntent accessIntent = rhi::ArgumentAccessIntent::readWrite);
    void unregisterTexture(TextureID textureId);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace demo
