#pragma once

#include "../RHISurface.h"

namespace demo::rhi::d3d12 {

class D3D12Surface final : public demo::rhi::Surface
{
public:
  D3D12Surface() = default;
  ~D3D12Surface() override { deinit(); }

  void                initD3D12(void* dxgiFactory, void* dxgiAdapter, const WindowHandle& window);
  void                deinit() override;
  SurfaceCapabilities queryCapabilities() const override;

  [[nodiscard]] void* nativeWindow() const { return m_nativeWindow; }

private:
  void* m_nativeWindow{nullptr};
};

}  // namespace demo::rhi::d3d12
