#pragma once

#include <cstdint>

namespace demo::editor {

enum class HistoryPolicy : std::uint8_t
{
	unclassified = 0,
	notApplicable,
	undoable,
	nonHistory,
	clearOnSuccessfulCommit,
};

} // namespace demo::editor
