#pragma once

#include "Handles.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace demo {

namespace detail {

inline constexpr uint32_t kMaxHandlePoolOwner =
  (std::numeric_limits<uint16_t>::max)();

class HandlePoolOwnerAllocator
{
public:
  explicit HandlePoolOwnerAllocator(uint32_t firstOwner = 1) noexcept
    : m_nextOwner(firstOwner)
  {
  }

  [[nodiscard]] uint16_t acquire()
  {
    uint32_t owner = m_nextOwner.load(std::memory_order_relaxed);
    for(;;)
    {
      if(owner == 0 || owner > kMaxHandlePoolOwner)
      {
        throw std::overflow_error("HandlePool owner identity space exhausted");
      }

      if(m_nextOwner.compare_exchange_weak(
           owner, owner + 1, std::memory_order_relaxed, std::memory_order_relaxed))
      {
        return static_cast<uint16_t>(owner);
      }
    }
  }

private:
  std::atomic<uint32_t> m_nextOwner;
};

[[nodiscard]] inline uint16_t acquireHandlePoolOwner()
{
  static HandlePoolOwnerAllocator owners;
  return owners.acquire();
}

[[nodiscard]] constexpr uint32_t encodeHandleGeneration(
  uint16_t owner, uint16_t generation) noexcept
{
  return (static_cast<uint32_t>(owner) << 16u) | generation;
}

}  // namespace detail

template <
  typename Handle,
  typename Value,
  uint16_t MaxSlotGeneration = (std::numeric_limits<uint16_t>::max)()>
class HandlePool
{
public:
  static_assert(MaxSlotGeneration != 0, "HandlePool requires a non-zero slot generation");
  static_assert(std::is_nothrow_destructible_v<Value>,
                "HandlePool requires a non-throwing Value destructor");
  static_assert(std::is_nothrow_move_constructible_v<Value> ||
                  std::is_copy_constructible_v<Value>,
                "HandlePool requires a nothrow-movable or copyable Value");
  static_assert(std::is_nothrow_copy_constructible_v<Handle> &&
                  std::is_nothrow_move_constructible_v<Handle>,
                "HandlePool requires a non-throwing handle value type");

  HandlePool()
  {
    m_slots.push_back(Slot{});
    m_owner = detail::acquireHandlePoolOwner();
  }

  HandlePool(const HandlePool&) = delete;
  HandlePool& operator=(const HandlePool&) = delete;
  HandlePool(HandlePool&& other) noexcept
    : m_slots(std::move(other.m_slots)),
      m_freeHead(std::exchange(other.m_freeHead, 0)),
      m_liveCount(std::exchange(other.m_liveCount, 0)),
      m_owner(std::exchange(other.m_owner, uint16_t{0}))
  {
  }

  HandlePool& operator=(HandlePool&& other) noexcept
  {
    // Owner zero carries no transferable identity. Preserving the destination
    // avoids making it unusable or revalidating one of its stale handles.
    if(this != &other && other.m_owner != 0)
    {
      m_slots     = std::move(other.m_slots);
      m_freeHead  = std::exchange(other.m_freeHead, 0);
      m_liveCount = std::exchange(other.m_liveCount, 0);
      m_owner     = std::exchange(other.m_owner, uint16_t{0});
    }
    return *this;
  }

  template <typename... Args>
  Handle emplace(Args&&... args)
  {
    if(m_owner == 0)
    {
      throw std::logic_error("cannot emplace into a moved-from HandlePool");
    }

    if(m_liveCount == (std::numeric_limits<uint32_t>::max)())
    {
      throw std::overflow_error("HandlePool live handle space exhausted");
    }
    if(m_freeHead == 0 &&
       m_slots.size() > (std::numeric_limits<uint32_t>::max)())
    {
      throw std::overflow_error("HandlePool slot index space exhausted");
    }

    // Consume arguments before any vector growth, so references into this pool
    // remain valid while Value is constructed. From this point until commit,
    // exceptions leave the free list, slot set, and live count unchanged.
    // As with standard emplace operations, forwarded rvalue arguments may have
    // been consumed even when a later allocation or copy fails.
    Slot candidate;
    candidate.value.emplace(std::forward<Args>(args)...);
    candidate.occupied = true;

    if(m_freeHead != 0)
    {
      const uint32_t slotIndex = m_freeHead;
      Slot&          slot      = m_slots[slotIndex];
      Handle         handle{slotIndex, encodedGeneration(slot)};

      slot.value.emplace(std::move_if_noexcept(*candidate.value));
      slot.occupied = true;
      m_freeHead    = slot.nextFree;
      slot.nextFree = 0;
      ++m_liveCount;
      return handle;
    }

    const uint32_t slotIndex = static_cast<uint32_t>(m_slots.size());
    Handle         handle{slotIndex, encodedGeneration(candidate)};
    m_slots.push_back(std::move_if_noexcept(candidate));
    ++m_liveCount;
    return handle;
  }

  bool destroy(Handle handle)
  {
    Slot* slot = slotForHandle(handle);
    if(slot == nullptr)
    {
      return false;
    }

    slot->value.reset();
    slot->occupied = false;
    if(slot->generation == MaxSlotGeneration)
    {
      // A retired slot is never placed back on the free list, so an issued
      // owner/index/generation tuple can never become live again.
      slot->nextFree = 0;
    }
    else
    {
      ++slot->generation;
      slot->nextFree = m_freeHead;
      m_freeHead     = handle.index;
    }
    --m_liveCount;
    return true;
  }

  [[nodiscard]] Value* tryGet(Handle handle)
  {
    Slot* slot = slotForHandle(handle);
    return slot != nullptr ? &*slot->value : nullptr;
  }

  [[nodiscard]] const Value* tryGet(Handle handle) const
  {
    const Slot* slot = slotForHandle(handle);
    return slot != nullptr ? &*slot->value : nullptr;
  }

  [[nodiscard]] bool isAlive(Handle handle) const { return slotForHandle(handle) != nullptr; }

  template <typename Fn>
  void forEachActive(Fn&& fn)
  {
    if(m_owner == 0)
    {
      return;
    }

    for(std::size_t index = 1; index < m_slots.size(); ++index)
    {
      Slot& slot = m_slots[index];
      if(slot.occupied)
      {
        std::forward<Fn>(fn)(
          Handle{static_cast<uint32_t>(index), encodedGeneration(slot)},
          *slot.value);
      }
    }
  }

  [[nodiscard]] uint32_t liveCount() const { return m_liveCount; }

private:
  struct Slot
  {
    std::optional<Value> value;
    uint16_t             generation{1};
    uint32_t             nextFree{0};
    bool                 occupied{false};
  };

  [[nodiscard]] Slot* slotForHandle(Handle handle)
  {
    if(m_owner == 0 || handle.isNull() || handle.index >= m_slots.size())
    {
      return nullptr;
    }

    Slot& slot = m_slots[handle.index];
    if(!slot.occupied || !slot.value ||
       encodedGeneration(slot) != handle.generation)
    {
      return nullptr;
    }

    return &slot;
  }

  [[nodiscard]] const Slot* slotForHandle(Handle handle) const
  {
    if(m_owner == 0 || handle.isNull() || handle.index >= m_slots.size())
    {
      return nullptr;
    }

    const Slot& slot = m_slots[handle.index];
    if(!slot.occupied || !slot.value ||
       encodedGeneration(slot) != handle.generation)
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
