#pragma once

#include <optional>
#include <utility>

namespace demo
{
	template<typename T>
	class FrameDeferredValue
	{
	public:
		void defer(T value)
		{
			m_pending = std::move(value);
		}

		[[nodiscard]] bool hasPending() const noexcept
		{
			return m_pending.has_value();
		}

		[[nodiscard]] std::optional<T> consume()
		{
			std::optional<T> value = std::move(m_pending);
			m_pending.reset();
			return value;
		}

	private:
		std::optional<T> m_pending;
	};
} // namespace demo
