#pragma once

#include "RHIBackend.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace demo::rhi {

enum class HotPathCounter : uint8_t
{
  commandBufferBegins = 0,
  encoderBegins,
  queueSubmits,
  submittedCommandBuffers,
  descriptorUpdates,
  descriptorAllocations,
  tableVersionAllocations,
  pipelineCreations,
  argumentLayoutCreations,
  textureViewCreations,
  commandRecordingHeapAllocations,
  commandRecordingNativeObjectCreations,
  count,
};

struct RHIHotPathCounters
{
  uint64_t commandBufferBegins{0};
  uint64_t encoderBegins{0};
  uint64_t queueSubmits{0};
  uint64_t submittedCommandBuffers{0};
  uint64_t descriptorUpdates{0};
  uint64_t descriptorAllocations{0};
  uint64_t tableVersionAllocations{0};
  uint64_t pipelineCreations{0};
  uint64_t argumentLayoutCreations{0};
  uint64_t textureViewCreations{0};
  uint64_t commandRecordingHeapAllocations{0};
  uint64_t commandRecordingNativeObjectCreations{0};

  [[nodiscard]] constexpr bool stableRecordingBudgetMet() const noexcept
  {
    return commandRecordingHeapAllocations == 0 &&
           commandRecordingNativeObjectCreations == 0 &&
           pipelineCreations == 0 && argumentLayoutCreations == 0 &&
           textureViewCreations == 0 && descriptorAllocations == 0;
  }
};

namespace detail {

inline constexpr size_t kBackendCount = 3;
inline constexpr size_t kCounterCount = static_cast<size_t>(HotPathCounter::count);
using CounterRow = std::array<std::atomic<uint64_t>, kCounterCount>;
inline std::array<CounterRow, kBackendCount> g_hotPathCounters{};

[[nodiscard]] constexpr size_t backendIndex(BackendType backend) noexcept
{
  return static_cast<size_t>(backend);
}

}  // namespace detail

inline void incrementHotPathCounter(
  BackendType backend, HotPathCounter counter, uint64_t amount = 1) noexcept
{
  detail::g_hotPathCounters[detail::backendIndex(backend)]
    [static_cast<size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

inline void resetHotPathCounters(BackendType backend) noexcept
{
  for(auto& counter : detail::g_hotPathCounters[detail::backendIndex(backend)])
    counter.store(0, std::memory_order_relaxed);
}

[[nodiscard]] inline RHIHotPathCounters snapshotHotPathCounters(BackendType backend) noexcept
{
  const auto& row = detail::g_hotPathCounters[detail::backendIndex(backend)];
  const auto load = [&row](HotPathCounter counter) {
    return row[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
  };
  return RHIHotPathCounters{
    .commandBufferBegins = load(HotPathCounter::commandBufferBegins),
    .encoderBegins = load(HotPathCounter::encoderBegins),
    .queueSubmits = load(HotPathCounter::queueSubmits),
    .submittedCommandBuffers = load(HotPathCounter::submittedCommandBuffers),
    .descriptorUpdates = load(HotPathCounter::descriptorUpdates),
    .descriptorAllocations = load(HotPathCounter::descriptorAllocations),
    .tableVersionAllocations = load(HotPathCounter::tableVersionAllocations),
    .pipelineCreations = load(HotPathCounter::pipelineCreations),
    .argumentLayoutCreations = load(HotPathCounter::argumentLayoutCreations),
    .textureViewCreations = load(HotPathCounter::textureViewCreations),
    .commandRecordingHeapAllocations = load(HotPathCounter::commandRecordingHeapAllocations),
    .commandRecordingNativeObjectCreations = load(HotPathCounter::commandRecordingNativeObjectCreations),
  };
}

}  // namespace demo::rhi
