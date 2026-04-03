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
  uint64_t nativeHandle{0};

  [[nodiscard]] bool isValid() const { return nativeHandle != 0 && familyIndex != ~0u; }
};

}  // namespace demo::rhi
