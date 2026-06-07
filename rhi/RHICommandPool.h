#pragma once

#include "RHIQueue.h"

#include <cstdint>

namespace demo {
namespace rhi {

class CommandPool
{
public:
  virtual ~CommandPool() = default;

  virtual void init(void* nativeDevice, QueueClass queueClass, uint32_t queueFamilyIndex) = 0;
  virtual void deinit()                                                                   = 0;
  virtual void reset()                                                                    = 0;

  virtual uint64_t getBackendHandle() const = 0;
};

}  // namespace rhi
}  // namespace demo
