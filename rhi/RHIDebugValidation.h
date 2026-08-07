#pragma once

#include "RHIStageBarrier.h"

#include <array>
#include <cstdint>

namespace demo::rhi {

enum class ResourceStateValidationError : uint8_t
{
  none = 0,
  invalidHandle,
  invalidDestinationState,
  stateMismatch,
  invalidRange,
  capacityExceeded,
};

struct ResourceStateValidationResult
{
  ResourceStateValidationError error{ResourceStateValidationError::none};
  ResourceState expected{ResourceState::Undefined};
  ResourceState actual{ResourceState::Undefined};

  [[nodiscard]] constexpr bool valid() const noexcept
  {
    return error == ResourceStateValidationError::none;
  }
};

class DebugResourceStateTracker
{
public:
  static constexpr uint32_t kCapacity = 1024;

  void reset() noexcept { m_count = 0; }

  [[nodiscard]] ResourceStateValidationResult transition(const TextureBarrier& barrier) noexcept
  {
    if(!barrier.texture.isValid())
      return {ResourceStateValidationError::invalidHandle};
    if(barrier.after == ResourceState::Undefined ||
       barrier.after == ResourceState::IndirectArgument)
      return {ResourceStateValidationError::invalidDestinationState};
    if(barrier.range.levelCount == 0 || barrier.range.layerCount == 0)
      return {ResourceStateValidationError::invalidRange};
    return transition(ResourceKey{
      0, barrier.texture.index, barrier.texture.generation,
      barrier.range.baseMipLevel, barrier.range.levelCount,
      barrier.range.baseArrayLayer, barrier.range.layerCount, 0, 0},
      barrier.before, barrier.after);
  }

  [[nodiscard]] ResourceStateValidationResult transition(const BufferBarrier& barrier) noexcept
  {
    if(!barrier.buffer.isValid())
      return {ResourceStateValidationError::invalidHandle};
    if(barrier.after == ResourceState::Undefined ||
       barrier.after == ResourceState::ColorAttachment ||
       barrier.after == ResourceState::DepthStencilAttachment ||
       barrier.after == ResourceState::DepthStencilReadOnly ||
       barrier.after == ResourceState::Present)
      return {ResourceStateValidationError::invalidDestinationState};
    return transition(ResourceKey{
      1, barrier.buffer.index, barrier.buffer.generation,
      0, 0, 0, 0, barrier.offset, barrier.size},
      barrier.before, barrier.after);
  }

private:
  struct ResourceKey
  {
    uint8_t kind{0};
    uint32_t index{0};
    uint32_t generation{0};
    uint32_t baseMipLevel{0};
    uint32_t levelCount{0};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{0};
    uint64_t offset{0};
    uint64_t size{0};
    constexpr bool operator==(const ResourceKey&) const = default;
  };

  struct Entry
  {
    ResourceKey key{};
    ResourceState state{ResourceState::Undefined};
  };

  [[nodiscard]] ResourceStateValidationResult transition(
    ResourceKey key, ResourceState before, ResourceState after) noexcept
  {
    for(uint32_t index = 0; index < m_count; ++index)
    {
      Entry& entry = m_entries[index];
      if(entry.key == key)
      {
        if(before != ResourceState::Undefined && before != entry.state)
          return {ResourceStateValidationError::stateMismatch, entry.state, before};
        entry.state = after;
        return {};
      }
    }
    if(m_count == kCapacity)
      return {ResourceStateValidationError::capacityExceeded};
    m_entries[m_count++] = Entry{key, after};
    return {};
  }

  std::array<Entry, kCapacity> m_entries{};
  uint32_t m_count{0};
};

}  // namespace demo::rhi
