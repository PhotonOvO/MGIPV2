#pragma once

#include "../RHIDescriptor.h"

#include <cstdint>
#include <vector>

namespace demo::rhi::vulkan {

// Adapters that let an externally-managed VkDescriptorSet / VkDescriptorSetLayout
// be registered as an rhi BindGroup. They own nothing — the underlying objects
// are created and destroyed elsewhere. Only getNativeHandle() is meaningful; the
// binding resolver uses it at bind time. This is the rhi-layer seam that keeps
// raw descriptor-set handles out of render passes.
class AdoptedBindTableLayout final : public BindTableLayout
{
public:
  explicit AdoptedBindTableLayout(uint64_t nativeLayout) : m_native(nativeLayout) {}
  void                   init(void*, const std::vector<BindTableLayoutEntry>&) override {}
  void                   deinit() override {}
  [[nodiscard]] uint64_t getNativeHandle() const override { return m_native; }

private:
  uint64_t m_native{0};
};

class AdoptedBindTable final : public BindTable
{
public:
  explicit AdoptedBindTable(uint64_t nativeSet) : m_native(nativeSet) {}
  void                   init(void*, const BindTableLayout&, uint32_t) override {}
  void                   deinit() override {}
  void                   update(uint32_t, const BindTableWrite*) override {}
  [[nodiscard]] uint64_t getNativeHandle() const override { return m_native; }

private:
  uint64_t m_native{0};
};

}  // namespace demo::rhi::vulkan
