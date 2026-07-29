#pragma once

#ifdef VKDEMO_HAS_NVTX
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#define VKDEMO_PROFILER_RESTORE_NOMINMAX
#endif
#include <nvtx3/nvToolsExt.h>
#ifdef VKDEMO_PROFILER_RESTORE_NOMINMAX
#undef VKDEMO_PROFILER_RESTORE_NOMINMAX
#undef NOMINMAX
#endif
#endif

namespace demo::profiling {

inline void pushCpuRange(const char* name)
{
#ifdef VKDEMO_HAS_NVTX
  nvtxRangePushA(name);
#else
  (void)name;
#endif
}

inline void popCpuRange()
{
#ifdef VKDEMO_HAS_NVTX
  nvtxRangePop();
#endif
}

class ScopedCpuRange
{
public:
  explicit ScopedCpuRange(const char* name) { pushCpuRange(name); }
  ~ScopedCpuRange() { popCpuRange(); }

  ScopedCpuRange(const ScopedCpuRange&)            = delete;
  ScopedCpuRange& operator=(const ScopedCpuRange&) = delete;
  ScopedCpuRange(ScopedCpuRange&&)                 = delete;
  ScopedCpuRange& operator=(ScopedCpuRange&&)      = delete;
};

}  // namespace demo::profiling

#define VKDEMO_PROFILE_CONCAT_IMPL(left, right) left##right
#define VKDEMO_PROFILE_CONCAT(left, right) VKDEMO_PROFILE_CONCAT_IMPL(left, right)
#define VKDEMO_CPU_SCOPE(name)                                                                            \
  [[maybe_unused]] const ::demo::profiling::ScopedCpuRange VKDEMO_PROFILE_CONCAT(vkdemoCpuRange_, __LINE__)(name)
