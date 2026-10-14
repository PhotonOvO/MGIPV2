#pragma once

#include <cstdint>

namespace demo::editor {

enum class EditorFaultPoint : uint8_t
{
	parse,
	postParseValidation,
	allocation,
	stagingFileCreate,
	write,
	flush,
	preAtomicReplace,
	candidateUpload,
	preCandidatePublish,
	count
};

[[nodiscard]] const char* editorFaultPointName(EditorFaultPoint point) noexcept;

class EditorFaultInjector
{
public:
	virtual ~EditorFaultInjector() = default;

	[[nodiscard]] virtual bool shouldFail(EditorFaultPoint point) noexcept = 0;
};

class NoEditorFaultInjector final : public EditorFaultInjector
{
public:
	[[nodiscard]] bool shouldFail(EditorFaultPoint point) noexcept override;
};

} // namespace demo::editor
