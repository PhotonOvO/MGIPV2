#pragma once

// Backend-neutral common header. Vulkan/VMA helpers that used to live here are
// isolated under rhi/vulkan/internal as transitional backend-private debt.

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include "logger.h"

#ifdef NDEBUG
#define ASSERT(condition, message)                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    if(!(condition))                                                                                                   \
    {                                                                                                                  \
      throw std::runtime_error(message);                                                                               \
    }                                                                                                                  \
  } while(false)
#else
#define ASSERT(condition, message) assert((condition) && (message))
#endif

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
