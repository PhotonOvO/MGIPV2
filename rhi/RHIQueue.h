#pragma once

#include <cstdint>

namespace demo::rhi {

enum class QueueClass : uint8_t
{
  graphics = 0,
  compute,
  transfer,
};

struct QueueInfo
{
  uint32_t familyIndex{~0u};
  uint32_t queueIndex{0};
  uint32_t queueCount{0};
  uint64_t backendHandle{0};
  QueueClass queueClass{QueueClass::graphics};
  bool       dedicated{false};

  [[nodiscard]] bool isValid() const { return backendHandle != 0 && familyIndex != ~0u; }
};

struct QueueSubmitInfo
{
  QueueClass queueClass{QueueClass::graphics};
  uint64_t   waitTimelineValue{0};
  uint64_t   signalTimelineValue{0};
};

}  // namespace demo::rhi
