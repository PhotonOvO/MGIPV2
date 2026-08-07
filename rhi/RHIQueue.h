#pragma once

#include "RHIHandles.h"

#include <cstdint>
#include <span>

namespace demo::rhi {

class CommandBuffer;

enum class QueueClass : uint8_t
{
  graphics = 0,
  compute,
  transfer,
};

enum class QueueOperation : uint8_t
{
  render = 0,
  compute,
  transfer,
};

[[nodiscard]] constexpr bool supportsQueueOperation(
  QueueClass queueClass, QueueOperation operation) noexcept
{
  switch(queueClass)
  {
  case QueueClass::graphics:
    return true;
  case QueueClass::compute:
    return operation != QueueOperation::render;
  case QueueClass::transfer:
    return operation == QueueOperation::transfer;
  }
  return false;
}

struct QueueIdentity
{
  uint32_t index{0};
  uint32_t generation{0};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return index != 0 && generation != 0;
  }

  constexpr bool operator==(const QueueIdentity&) const = default;
};

struct QueueCapabilities
{
  bool supportsTimestamps{false};
  bool supportsPresent{false};
  bool supportsSparseBinding{false};
};

struct QueueInfo
{
  QueueClass        queueClass{QueueClass::graphics};
  QueueCapabilities capabilities{};
  bool              dedicated{false};
  bool              available{false};

  [[nodiscard]] constexpr bool isValid() const noexcept { return available; }
};

struct SubmissionToken
{
  QueueIdentity queue{};
  uint64_t      value{0};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return queue.isValid() && value != 0;
  }

  constexpr bool operator==(const SubmissionToken&) const = default;
};

enum class SubmitStage : uint8_t
{
  allCommands = 0,
  colorOutput,
  compute,
  transfer,
};

struct QueueSyncPoint
{
  QueueSyncHandle sync{};
  uint64_t        value{0};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return sync.isValid();
  }
};

struct SubmitWaitPoint
{
  SubmissionToken submission{};
  QueueSyncPoint  external{};
  SubmitStage     stage{SubmitStage::allCommands};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return submission.isValid() != external.isValid();
  }
};

struct SubmitSignalPoint
{
  QueueSyncPoint external{};

  [[nodiscard]] constexpr bool isValid() const noexcept
  {
    return external.isValid();
  }
};

struct SubmitBatch
{
  std::span<CommandBuffer* const>    commandBuffers{};
  std::span<const SubmitWaitPoint>   waitPoints{};
  std::span<const SubmitSignalPoint> signalPoints{};
  const char*                        debugLabel{nullptr};
};

class Queue
{
public:
  virtual ~Queue() = default;

  [[nodiscard]] virtual QueueIdentity identity() const noexcept = 0;
  [[nodiscard]] virtual QueueInfo info() const noexcept = 0;
  [[nodiscard]] virtual SubmissionToken submit(const SubmitBatch& batch) = 0;
  [[nodiscard]] virtual bool isComplete(SubmissionToken token) const = 0;
  virtual void wait(SubmissionToken token) = 0;
  virtual void waitIdle() = 0;
  [[nodiscard]] virtual SubmissionToken lastSubmittedToken() const noexcept = 0;
};

}  // namespace demo::rhi