#pragma once

namespace demo::rhi::metal {

struct Metal4Availability
{
  bool sdkHasCoreApi{false};
  bool runtimeHasCoreApi{false};
  bool deviceAvailable{false};
};

[[nodiscard]] Metal4Availability queryMetal4Availability() noexcept;

}  // namespace demo::rhi::metal
