#pragma once

#include "RHICommandList.h"
#include "RHIResourceLifetime.h"
#include "RHISwapchain.h"
#include "RHISynchronization.h"

#include <cstdint>

namespace demo {
namespace rhi {

struct FrameData
{
  CommandList* commandList{nullptr};
  uint64_t     lastSignalValue{0};
  void*        userData{nullptr};
};

class FrameContext : public SubmissionQueue
{
public:
  virtual ~FrameContext() = default;

  virtual void init(void* nativeDevice, uint32_t queueFamilyIndex, uint32_t frameCount) = 0;
  virtual void deinit()                                                                 = 0;

  virtual void              beginFrame()                       = 0;
  virtual SubmissionReceipt endFrame(CommandList* cmdList)     = 0;
  virtual void              setSwapchain(Swapchain* swapchain) = 0;

  virtual FrameData& getCurrentFrame()    = 0;
  virtual void       advanceToNextFrame() = 0;

  virtual void waitCurrentFrame()                = 0;
  virtual void waitForFrame(uint64_t frameIndex) = 0;

  virtual uint32_t getFrameCount() const        = 0;
  virtual uint32_t getCurrentFrameIndex() const = 0;
  virtual uint64_t getCurrentFrameValue() const = 0;

  virtual void* getTimelineSemaphore() const = 0;

  virtual void     enqueueRetirement(ResourceHandle resource, uint64_t timelineValue) = 0;
  virtual uint32_t processRetirements(uint64_t currentTimelineValue)                  = 0;

  virtual DeferredDestructionQueue&       getDestructionQueue()       = 0;
  virtual const DeferredDestructionQueue& getDestructionQueue() const = 0;
};

}  // namespace rhi
}  // namespace demo
