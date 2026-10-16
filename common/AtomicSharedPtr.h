#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace demo
{
	// Portable stand-in for std::atomic<std::shared_ptr<T>>.
	//
	// The C++20 specialization (P0718R2) is implemented by MSVC, but not by
	// libc++, where std::atomic<T> falls back to the primary template and
	// rejects the non-trivially-copyable std::shared_ptr. That makes
	// std::atomic<std::shared_ptr<T>> unusable on Apple platforms.
	//
	// Each operation is serialized by a mutex, which is the same guarantee the
	// standard specialization provides on platforms that cannot make
	// shared_ptr atomicity lock-free. The memory_order parameters are accepted
	// so existing call sites stay source compatible; the mutex supplies the
	// ordering.
	template<typename T>
	class AtomicSharedPtr
	{
	public:
		using Value = std::shared_ptr<T>;

		AtomicSharedPtr() noexcept = default;

		explicit AtomicSharedPtr(Value value) noexcept
			: m_value(std::move(value))
		{
		}

		AtomicSharedPtr(const AtomicSharedPtr&) = delete;
		AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

		void store(Value value,
			std::memory_order = std::memory_order_seq_cst) noexcept
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_value = std::move(value);
		}

		[[nodiscard]] Value load(
			std::memory_order = std::memory_order_seq_cst) const noexcept
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_value;
		}

		[[nodiscard]] Value exchange(Value value,
			std::memory_order = std::memory_order_seq_cst) noexcept
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return std::exchange(m_value, std::move(value));
		}

	private:
		mutable std::mutex m_mutex;
		Value m_value;
	};
}
