#pragma once

#include "../RHIArgumentTable.h"
#include "../RHIBackend.h"
#include "../RHICommandBuffer.h"
#include "../RHIQueue.h"
#include "../RHIResidency.h"

#include <cstdint>

namespace demo::rhi::metal {

enum class Metal4CommandBufferState : uint8_t
{
  idle = 0,
  recording,
  executable,
  submitted,
  reusable,
};

struct Metal4QueueRecord
{
  void* nativeQueue{nullptr};
  QueueClass queueClass{QueueClass::graphics};
  QueueCapabilities capabilities{};
};

struct Metal4CommandAllocatorRecord
{
  void* nativeAllocator{nullptr};
  QueueClass queueClass{QueueClass::graphics};
  SubmissionToken reusableAfter{};
};

struct Metal4CommandBufferRecord
{
  void* nativeCommandBuffer{nullptr};
  Metal4CommandBufferState state{Metal4CommandBufferState::idle};
  SubmissionToken submission{};
};

struct Metal4RenderEncoderRecord
{
  void* nativeEncoder{nullptr};
  ShaderStage activeStages{ShaderStage::none};
};

struct Metal4ComputeEncoderRecord
{
  void* nativeEncoder{nullptr};
};

struct Metal4ArgumentTableBinding
{
  ArgumentTableHandle table{};
  ShaderStage stages{ShaderStage::none};
  uint32_t slot{0};
};

struct Metal4ResidencySetRecord
{
  void* nativeResidencySet{nullptr};
  uint32_t maxResources{0};
  bool committed{false};
};

struct Metal4TextureViewPoolRecord
{
  void* nativeTextureViewPool{nullptr};
  uint32_t capacity{0};
};

struct Metal4GpuAddress
{
  uint64_t value{0};

  [[nodiscard]] constexpr GpuPtr toRhi() const noexcept { return GpuPtr{value}; }
};

struct Metal4NativeContractStatus
{
  bool sdkTypesCompiled{false};
  bool runtimeTypesAvailable{false};
  bool deviceAvailable{false};
};

[[nodiscard]] Metal4NativeContractStatus queryMetal4NativeContract() noexcept;

}  // namespace demo::rhi::metal