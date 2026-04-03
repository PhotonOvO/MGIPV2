#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace demo {
namespace rhi {

enum class ResourceLifetimeTier : uint8_t
{
  Device = 0,
  Swapchain,
  PerFrame,
  PerPass,
  Immediate,
};

struct OwnershipSplit
{
  bool rendererOwnsLogicalLifetime{true};
  bool rhiOwnsPhysicalLifetime{true};
};

struct BindlessResidencyPolicy
{
  bool keepResidentAfterLogicalRelease{true};
  bool reuseIndexOnlyAfterRetirement{true};
};

enum class ResourceKind : uint8_t
{
  Unknown = 0,
  Buffer,
  Texture,
  TextureView,
  BufferView,
  Sampler,
  Pipeline,
  BindTable,
};

struct ResourceHandle
{
  ResourceKind kind{ResourceKind::Unknown};
  uint32_t     index{0};
  uint32_t     generation{0};

  [[nodiscard]] constexpr bool isValid() const noexcept { return index != 0 || generation != 0; }

  constexpr bool operator==(const ResourceHandle& rhs) const noexcept
  {
    return kind == rhs.kind && index == rhs.index && generation == rhs.generation;
  }
};

struct ResourceRetirement
{
  ResourceHandle resource{};
  uint64_t       retireTimelineValue{0};
};

struct RetirementPolicy
{
  enum class Mode : uint8_t
  {
    TimelineValue = 0,
    FrameCount,
  };

  Mode     mode{Mode::TimelineValue};
  uint64_t value{0};

  [[nodiscard]] static constexpr RetirementPolicy timeline(uint64_t timelineValue)
  {
    return RetirementPolicy{Mode::TimelineValue, timelineValue};
  }

  [[nodiscard]] static constexpr RetirementPolicy frameCount(uint32_t frameDelay)
  {
    return RetirementPolicy{Mode::FrameCount, frameDelay};
  }
};

[[nodiscard]] constexpr uint64_t calculateRetirementTimelineValue(uint64_t nextSubmitTimelineValue, RetirementPolicy policy)
{
  if(policy.mode == RetirementPolicy::Mode::TimelineValue)
  {
    return policy.value;
  }

  return nextSubmitTimelineValue + policy.value;
}

[[nodiscard]] constexpr bool hasReachedRetirementPoint(uint64_t currentTimelineValue, uint64_t retirementTimelineValue)
{
  return currentTimelineValue >= retirementTimelineValue;
}

class DeferredDestructionQueue
{
public:
  virtual ~DeferredDestructionQueue() = default;

  virtual void                                                 enqueue(ResourceRetirement retirement) = 0;
  virtual uint32_t                                             process(uint64_t currentTimelineValue) = 0;
  virtual void                                                 clear()                                = 0;
  [[nodiscard]] virtual bool                                   empty() const                          = 0;
  [[nodiscard]] virtual const std::vector<ResourceHandle>&     drainedResources() const               = 0;
  [[nodiscard]] virtual const std::vector<ResourceRetirement>& pendingRetirements() const             = 0;
};

class InlineDeferredDestructionQueue final : public DeferredDestructionQueue
{
public:
  void enqueue(ResourceRetirement retirement) override { m_pending.push_back(retirement); }

  uint32_t process(uint64_t currentTimelineValue) override
  {
    m_drained.clear();

    auto it = std::remove_if(m_pending.begin(), m_pending.end(), [this, currentTimelineValue](const ResourceRetirement& retirement) {
      if(!hasReachedRetirementPoint(currentTimelineValue, retirement.retireTimelineValue))
      {
        return false;
      }

      m_drained.push_back(retirement.resource);
      return true;
    });

    m_pending.erase(it, m_pending.end());
    return static_cast<uint32_t>(m_drained.size());
  }

  void clear() override
  {
    m_pending.clear();
    m_drained.clear();
  }

  [[nodiscard]] bool empty() const override { return m_pending.empty(); }

  [[nodiscard]] const std::vector<ResourceHandle>& drainedResources() const override { return m_drained; }

  [[nodiscard]] const std::vector<ResourceRetirement>& pendingRetirements() const override { return m_pending; }

private:
  std::vector<ResourceRetirement> m_pending;
  std::vector<ResourceHandle>     m_drained;
};

}  // namespace rhi
}  // namespace demo
