#pragma once

#ifdef VKDEMO_HAS_NVTX
#include <nvtx3/nvToolsExt.h>
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
};

}  // namespace demo::profiling
