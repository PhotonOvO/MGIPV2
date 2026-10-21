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

#include <cstring>

#ifdef VKDEMO_HAS_TRACY
// The Tracy client includes windows.h on Windows, so NOMINMAX is restored around it.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#define VKDEMO_PROFILER_TRACY_RESTORE_NOMINMAX
#endif
#include <tracy/Tracy.hpp>
#ifdef VKDEMO_PROFILER_TRACY_RESTORE_NOMINMAX
#undef VKDEMO_PROFILER_TRACY_RESTORE_NOMINMAX
#undef NOMINMAX
#endif
#endif

#define VKDEMO_PROFILE_CONCAT_IMPL(left, right) left##right
#define VKDEMO_PROFILE_CONCAT(left, right) VKDEMO_PROFILE_CONCAT_IMPL(left, right)

namespace demo::profiling {

// A CPU range covering the remainder of the enclosing scope.
//
// NVTX and Tracy are independent sinks: either, both or neither may be compiled in, so the
// call sites need no guards of their own.
//
// Tracy's zone must be opened by an object that lives in the calling scope. Holding a
// tracy::ScopedZone as a member gives exactly that: the constructor opens the zone and the
// destructor closes it, so the range stays correct across early returns. The member is a plain
// object, so no range allocates.
class ScopedCpuRange
{
public:
  // Most call sites spell the object directly, so the one-argument form has to stay. Tracy
  // then records the header's own source location, which is enough to name the zone.
  explicit ScopedCpuRange(const char* name)
    : ScopedCpuRange(name, 0, "ProfilerMarkers", "ScopedCpuRange")
  {
  }

  ScopedCpuRange(const char* name, int line, const char* source, const char* function)
#ifdef VKDEMO_HAS_TRACY
    : m_zone(static_cast<uint32_t>(line),
             source,
             std::strlen(source),
             function,
             std::strlen(function),
             name,
             std::strlen(name),
             0,
             -1,
             true)
#endif
  {
#ifdef VKDEMO_HAS_NVTX
    nvtxRangePushA(name);
#else
    (void)name;
    (void)line;
    (void)source;
    (void)function;
#endif
  }

  ~ScopedCpuRange()
  {
#ifdef VKDEMO_HAS_NVTX
    nvtxRangePop();
#endif
  }

  ScopedCpuRange(const ScopedCpuRange&)            = delete;
  ScopedCpuRange& operator=(const ScopedCpuRange&) = delete;
  ScopedCpuRange(ScopedCpuRange&&)                 = delete;
  ScopedCpuRange& operator=(ScopedCpuRange&&)      = delete;

private:
#ifdef VKDEMO_HAS_TRACY
  tracy::ScopedZone m_zone;
#endif
};

// Kept for callers that push and pop explicitly. ScopedCpuRange is preferred because an early
// return cannot unbalance it.
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

// Marks a frame boundary. Tracy builds its frame timeline and per-frame statistics from it, so
// it belongs in the main loop rather than inside a pass.
inline void markFrame()
{
#ifdef VKDEMO_HAS_TRACY
  FrameMark;
#endif
}

}  // namespace demo::profiling

// The call sites are statements, so the macro declares an object and relies on the call site's
// semicolon.
#define VKDEMO_CPU_SCOPE(name)                                                                            \
  [[maybe_unused]] const ::demo::profiling::ScopedCpuRange VKDEMO_PROFILE_CONCAT(vkdemoCpuRange_, __LINE__)( \
    name, __LINE__, __FILE__, __func__)

// A per-frame plot value. Cheap and idempotent, so it stays a no-op rather than requiring call
// sites to guard it.
#ifdef VKDEMO_HAS_TRACY
#define VKDEMO_PLOT(name, value) TracyPlot(name, static_cast<int64_t>(value))
#else
#define VKDEMO_PLOT(name, value)                                                                          \
  do                                                                                                      \
  {                                                                                                       \
    (void)(value);                                                                                        \
  } while (false)
#endif

// A named value attached to the enclosing zone, for counts that explain where its time went.
#ifdef VKDEMO_HAS_TRACY
#define VKDEMO_ZONE_VALUE(name, value)                                                                    \
  do                                                                                                      \
  {                                                                                                       \
    ZoneValue(static_cast<int64_t>(value));                                                               \
    ZoneName(name, sizeof(name) - 1);                                                                     \
  } while (false)
#else
#define VKDEMO_ZONE_VALUE(name, value)                                                                    \
  do                                                                                                      \
  {                                                                                                       \
    (void)(name);                                                                                         \
    (void)(value);                                                                                        \
  } while (false)
#endif
