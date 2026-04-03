#pragma once

#include "RHICommandList.h"
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

  virtual CommandList* allocateCommandList()                 = 0;
  virtual void         freeCommandList(CommandList* cmdList) = 0;

  virtual void allocateCommandLists(uint32_t count, CommandList** cmdLists) = 0;
  virtual void freeCommandLists(uint32_t count, CommandList** cmdLists)     = 0;

  virtual uint64_t getNativeHandle() const = 0;
};

}  // namespace rhi
}  // namespace demo
