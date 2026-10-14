#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace demo {

// Copies retain immutable storage. Builders publish a completed vector; edits
// replace the owner instead of exposing writable aliases to queued snapshots.
class ImmutableByteBuffer final {
public:
  using value_type = std::uint8_t;
  using const_iterator = std::vector<value_type>::const_iterator;

  ImmutableByteBuffer() = default;
  ImmutableByteBuffer(std::vector<value_type> bytes)
      : m_storage(std::make_shared<Storage>(std::move(bytes))) {}
  ImmutableByteBuffer(std::initializer_list<value_type> bytes)
      : ImmutableByteBuffer(std::vector<value_type>(bytes)) {}

  ImmutableByteBuffer &operator=(std::vector<value_type> bytes) {
    ImmutableByteBuffer replacement(std::move(bytes));
    m_storage.swap(replacement.m_storage);
    return *this;
  }
  ImmutableByteBuffer &operator=(std::initializer_list<value_type> bytes) {
    return *this = std::vector<value_type>(bytes);
  }

  [[nodiscard]] static ImmutableByteBuffer concatenate(std::vector<ImmutableByteBuffer> parts) {
    if (parts.empty())
      return {};
    if (parts.size() == 1u)
      return std::move(parts.front());
    ImmutableByteBuffer result;
    result.m_storage = std::make_shared<Storage>(std::move(parts));
    return result;
  }

  [[nodiscard]] std::vector<std::span<const value_type>> chunks() const {
    std::vector<std::span<const value_type>> result;
    appendChunks(result);
    return result;
  }

  [[nodiscard]] const std::vector<value_type> &vector() const {
    static const std::vector<value_type> empty;
    if (!m_storage)
      return empty;
    if (m_storage->parts.empty())
      return m_storage->bytes;
    // Incremental consumers use chunks; full-snapshot consumers request a
    // contiguous view lazily. All copies observe the same immutable result.
    std::call_once(m_storage->flattenedOnce, [&] {
      std::vector<value_type> bytes;
      bytes.reserve(size());
      for (const auto chunk : chunks())
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
      m_storage->flattened = std::move(bytes);
    });
    return m_storage->flattened;
  }
  [[nodiscard]] operator const std::vector<value_type> &() const { return vector(); }
  [[nodiscard]] operator std::span<const value_type>() const { return vector(); }
  [[nodiscard]] const value_type *data() const { return vector().data(); }
  [[nodiscard]] std::size_t size() const noexcept { return m_storage ? m_storage->size : 0u; }
  [[nodiscard]] bool empty() const noexcept { return size() == 0u; }
  [[nodiscard]] const_iterator begin() const { return vector().begin(); }
  [[nodiscard]] const_iterator end() const { return vector().end(); }
  [[nodiscard]] value_type operator[](std::size_t index) const { return vector()[index]; }
  [[nodiscard]] value_type front() const { return vector().front(); }
  void clear() noexcept { m_storage.reset(); }

  [[nodiscard]] bool operator==(const ImmutableByteBuffer &other) const {
    if (m_storage == other.m_storage)
      return true;
    if (size() != other.size())
      return false;
    const auto left = chunks();
    const auto right = other.chunks();
    std::size_t l = 0u, r = 0u, lo = 0u, ro = 0u;
    while (l < left.size() && r < right.size()) {
      const auto count = std::min(left[l].size() - lo, right[r].size() - ro);
      if (std::memcmp(left[l].data() + lo, right[r].data() + ro, count) != 0)
        return false;
      lo += count;
      ro += count;
      if (lo == left[l].size()) {
        ++l;
        lo = 0u;
      }
      if (ro == right[r].size()) {
        ++r;
        ro = 0u;
      }
    }
    return l == left.size() && r == right.size();
  }

private:
  struct Storage final {
    explicit Storage(std::vector<value_type> value) : bytes(std::move(value)), size(bytes.size()) {}
    explicit Storage(std::vector<ImmutableByteBuffer> value) : parts(std::move(value)) {
      for (const auto &part : parts) {
        if (part.size() > std::numeric_limits<std::size_t>::max() - size)
          throw std::length_error("immutable byte buffer is too large");
        size += part.size();
      }
    }
    std::vector<value_type> bytes;
    std::vector<ImmutableByteBuffer> parts;
    std::size_t size{0u};
    mutable std::once_flag flattenedOnce;
    mutable std::vector<value_type> flattened;
  };

  void appendChunks(std::vector<std::span<const value_type>> &result) const {
    if (!m_storage)
      return;
    if (m_storage->parts.empty()) {
      if (!m_storage->bytes.empty())
        result.emplace_back(m_storage->bytes);
    } else {
      for (const auto &part : m_storage->parts)
        part.appendChunks(result);
    }
  }

  std::shared_ptr<const Storage> m_storage;
};

} // namespace demo
