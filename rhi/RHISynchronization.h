#pragma once

#include "RHICommandList.h"

#include <cstdint>

namespace demo {
namespace rhi {

class Fence
{
public:
  virtual ~Fence() = default;

  virtual void init(void* nativeDevice, bool signaled = false) = 0;
  virtual void deinit()                                        = 0;

  virtual void wait(uint64_t timeout = UINT64_MAX) = 0;
  virtual void reset()                             = 0;

  virtual bool isSignaled() const = 0;

  virtual uint64_t getNativeHandle() const = 0;
};

class TimelineSemaphore
{
public:
  virtual ~TimelineSemaphore() = default;

  virtual void init(void* nativeDevice, uint64_t initialValue = 0) = 0;
  virtual void deinit()                                            = 0;

  virtual void signal(uint64_t value)                              = 0;
  virtual void wait(uint64_t value, uint64_t timeout = UINT64_MAX) = 0;

  virtual uint64_t getCurrentValue() const = 0;

  virtual uint64_t getNativeHandle() const = 0;
};

struct SubmissionReceipt
{
  uint64_t timelineValue{0};
};

struct SubmissionRequest
{
  CommandList* commandList{nullptr};
};

class SubmissionQueue
{
public:
  virtual ~SubmissionQueue() = default;

  virtual SubmissionReceipt submitCommandLists(const SubmissionRequest* requests, uint32_t requestCount) = 0;
  virtual void              waitForSubmission(SubmissionReceipt receipt)                                 = 0;
};

}  // namespace rhi
}  // namespace demo
