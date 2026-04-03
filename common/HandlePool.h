#pragma once

#include "Handles.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace demo {

template <typename Handle, typename Value>
class HandlePool
{
public:
  HandlePool() { m_slots.push_back(Slot{}); }

  template <typename... Args>
  Handle emplace(Args&&... args)
  {
    const uint32_t slotIndex = acquireSlot();
    Slot&          slot      = m_slots[slotIndex];
    slot.value               = Value{std::forward<Args>(args)...};
    slot.occupied            = true;
    ++m_liveCount;
    return Handle{slotIndex, slot.generation};
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
    slot->generation = nextGeneration(slot->generation);
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
        std::forward<Fn>(fn)(Handle{index, slot.generation}, slot.value);
      }
    }
  }

  [[nodiscard]] uint32_t liveCount() const { return m_liveCount; }

private:
  struct Slot
  {
    Value    value{};
    uint32_t generation{1};
    uint32_t nextFree{0};
    bool     occupied{false};
  };

  [[nodiscard]] static uint32_t nextGeneration(uint32_t generation)
  {
    uint32_t next = generation + 1;
    if(next == 0)
    {
      next = 1;
    }
    return next;
  }

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
    if(!slot.occupied || slot.generation != handle.generation)
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
    if(!slot.occupied || slot.generation != handle.generation)
    {
      return nullptr;
    }

    return &slot;
  }

  std::vector<Slot> m_slots;
  uint32_t          m_freeHead{0};
  uint32_t          m_liveCount{0};
};

}  // namespace demo
