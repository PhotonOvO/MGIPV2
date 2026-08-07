#pragma once

#include "RHIQueue.h"

#include <memory>

namespace demo::rhi {

class CommandBuffer;

class CommandAllocator
{
public:
  virtual ~CommandAllocator() = default;

  [[nodiscard]] virtual QueueClass queueClass() const noexcept = 0;
  virtual void reset(SubmissionToken completedToken = {}) = 0;
  [[nodiscard]] virtual std::unique_ptr<CommandBuffer> createCommandBuffer() = 0;
};

}  // namespace demo::rhi