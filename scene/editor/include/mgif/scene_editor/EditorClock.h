#pragma once

#include <chrono>

namespace demo::editor {

using EditorTimePoint = std::chrono::steady_clock::time_point;

class EditorClock
{
public:
	virtual ~EditorClock() = default;

	[[nodiscard]] virtual EditorTimePoint now() const noexcept = 0;
};

class SystemEditorClock final : public EditorClock
{
public:
	[[nodiscard]] EditorTimePoint now() const noexcept override;
};

} // namespace demo::editor
