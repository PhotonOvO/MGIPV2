#pragma once

#include <cstring>
#include <memory>
#include <type_traits>

namespace demo::editor::detail {

template <typename To, typename From>
[[nodiscard]] To bitwiseCast(const From& source) noexcept
{
	static_assert(sizeof(To) == sizeof(From));
	static_assert(std::is_trivially_copyable_v<From>);
	static_assert(std::is_trivially_copyable_v<To>);
	static_assert(std::is_default_constructible_v<To>);

	To destination{};
	std::memcpy(std::addressof(destination), std::addressof(source), sizeof(To));
	return destination;
}

} // namespace demo::editor::detail
