#pragma once

#include "Handles.h"

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace demo {

namespace detail {

inline std::atomic<uint32_t> g_nextHandlePoolOwner{1};

[[nodiscard]] inline uint16_t acquireHandlePoolOwner() noexcept
{
  uint32_t owner = g_nextHandlePoolOwner.fetch_add(1, std::memory_order_relaxed);
  owner &= 0xFFFFu;
  return static_cast<uint16_t>(owner == 0 ? 1u : owner);
}

[[nodiscard]] constexpr uint32_t encodeHandleGeneration(
  uint16_t owner, uint16_t generation) noexcept
{
  return (static_cast<uint32_t>(owner) << 16u) | generation;
}

}  // namespace detail

template <typename Handle, typename Value>
class HandlePool
{
public:
  HandlePool() : m_owner(detail::acquireHandlePoolOwner()) { m_slots.push_back(Slot{}); }

  HandlePool(const HandlePool&) = delete;
  HandlePool& operator=(const HandlePool&) = delete;
  HandlePool(HandlePool&&) noexcept = default;
  HandlePool& operator=(HandlePool&&) noexcept = default;

  template <typename... Args>
  Handle emplace(Args&&... args)
  {
    const uint32_t slotIndex = acquireSlot();
    Slot&          slot      = m_slots[slotIndex];
    slot.value               = Value{std::forward<Args>(args)...};
    slot.occupied            = true;
    ++m_liveCount;
    return Handle{slotIndex, encodedGeneration(slot)};
  }

  bool destroy(Handle handle)
  {
    Slot* slot = slotForHandle(handle);
    if(slot == nullptr)
    {
      return false;
    }

    slot->value      = Value{};
    slot->occupied   = false;
    slot->nextFree   = m_freeHead;
    m_freeHead       = handle.index;
    ++slot->generation;
    if(slot->generation == 0)
      slot->generation = 1;
    --m_liveCount;
    return true;
  }

  [[nodiscard]] Value* tryGet(Handle handle)
  {
    Slot* slot = slotForHandle(handle);
    return slot != nullptr ? &slot->value : nullptr;
  }

  [[nodiscard]] const Value* tryGet(Handle handle) const
  {
    const Slot* slot = slotForHandle(handle);
    return slot != nullptr ? &slot->value : nullptr;
  }

  [[nodiscard]] bool isAlive(Handle handle) const { return slotForHandle(handle) != nullptr; }

  template <typename Fn>
  void forEachActive(Fn&& fn)
  {
    for(uint32_t index = 1; index < static_cast<uint32_t>(m_slots.size()); ++index)
    {
      Slot& slot = m_slots[index];
      if(slot.occupied)
      {
        std::forward<Fn>(fn)(Handle{index, encodedGeneration(slot)}, slot.value);
      }
    }
  }

  [[nodiscard]] uint32_t liveCount() const { return m_liveCount; }

private:
  struct Slot
  {
    Value    value{};
    uint16_t generation{1};
    uint32_t nextFree{0};
    bool     occupied{false};
  };

  [[nodiscard]] uint32_t acquireSlot()
  {
    if(m_freeHead != 0)
    {
      const uint32_t slotIndex    = m_freeHead;
      m_freeHead                  = m_slots[slotIndex].nextFree;
      m_slots[slotIndex].nextFree = 0;
      return slotIndex;
    }

    m_slots.push_back(Slot{});
    return static_cast<uint32_t>(m_slots.size() - 1);
  }

  [[nodiscard]] Slot* slotForHandle(Handle handle)
  {
    if(handle.isNull() || handle.index >= static_cast<uint32_t>(m_slots.size()))
    {
      return nullptr;
    }

    Slot& slot = m_slots[handle.index];
    if(!slot.occupied || encodedGeneration(slot) != handle.generation)
    {
      return nullptr;
    }

    return &slot;
  }

  [[nodiscard]] const Slot* slotForHandle(Handle handle) const
  {
    if(handle.isNull() || handle.index >= static_cast<uint32_t>(m_slots.size()))
    {
      return nullptr;
    }

    const Slot& slot = m_slots[handle.index];
    if(!slot.occupied || encodedGeneration(slot) != handle.generation)
    {
      return nullptr;
    }

    return &slot;
  }

  [[nodiscard]] uint32_t encodedGeneration(const Slot& slot) const noexcept
  {
    return detail::encodeHandleGeneration(m_owner, slot.generation);
  }

  std::vector<Slot> m_slots;
  uint32_t          m_freeHead{0};
  uint32_t          m_liveCount{0};
  uint16_t          m_owner{0};
};

}  // namespace demo
