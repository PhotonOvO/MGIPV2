#pragma once

#include "../RHISurface.h"

namespace CA {
class MetalLayer;
}

namespace MTL {
class Device;
}

struct SDL_Window;

namespace demo::rhi::metal {

class MetalSurface final : public Surface
{
public:
  MetalSurface() = default;
  ~MetalSurface() override;

  void init(MTL::Device* device, const WindowHandle& window);
  void deinit() override;
  [[nodiscard]] SurfaceCapabilities queryCapabilities() const override;

  void updateDrawableSize();
  [[nodiscard]] CA::MetalLayer* layer() const noexcept { return m_layer; }
  [[nodiscard]] Extent2D drawableExtent() const noexcept { return m_extent; }

private:
  MTL::Device* m_device{nullptr};
  CA::MetalLayer* m_layer{nullptr};
  SDL_Window* m_window{nullptr};
  void* m_metalView{nullptr};
  Extent2D m_extent{};
};

}  // namespace demo::rhi::metal
