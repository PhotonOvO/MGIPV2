#pragma once

// RHIBoundary.h — Phase 1 fatal-abort baseline (BCK-02).
// Defines RHI_UNIMPLEMENTED: LOGE + std::abort in both debug and release builds,
// replacing unreliable assert(false) stubs that NDEBUG silently strips.
// Phase 6 may upgrade the macro body to a Result<T> path without changing any call site.

#include "../common/logger.h"
#include <cstdlib>

// RHI_UNIMPLEMENTED(method_name)
// Logs a fatal error and aborts the process immediately.
// method_name must be a string literal (e.g. "createBuffer") — never pass pointers,
// addresses, or runtime user data (per threat model T-02-01).
// Behaviour is identical in debug and release builds (no #ifdef NDEBUG branching).
#define RHI_UNIMPLEMENTED(method_name)                                                             \
  do                                                                                               \
  {                                                                                                \
    LOGE("[RHI] Unimplemented: %s — check Device::supports() before calling this method.",        \
         method_name);                                                                             \
    std::abort();                                                                                  \
  } while(false)
