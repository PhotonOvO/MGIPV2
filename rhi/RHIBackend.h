#pragma once

#include <cstdint>

namespace demo::rhi {

enum class BackendType : uint8_t
{
  vulkan = 0,
  d3d12,
  metal,
};

struct BackendVersion
{
  uint32_t major{0};
  uint32_t minor{0};
  uint32_t patch{0};
  uint32_t nativeValue{0};
};

struct BackendInfo
{
  BackendType    type{BackendType::vulkan};
  const char*    apiName{"unknown"};
  BackendVersion version{};
};

}  // namespace demo::rhi