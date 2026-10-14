#include <mgif/scene_editor/EditorClock.h>
#include <mgif/scene_editor/EditorFaultInjection.h>

namespace demo::editor {

EditorTimePoint SystemEditorClock::now() const noexcept
{
	return std::chrono::steady_clock::now();
}

const char* editorFaultPointName(EditorFaultPoint point) noexcept
{
	switch(point)
	{
		case EditorFaultPoint::parse:
			return "parse";
		case EditorFaultPoint::postParseValidation:
			return "post-parse-validation";
		case EditorFaultPoint::allocation:
			return "allocation";
		case EditorFaultPoint::stagingFileCreate:
			return "staging-file-create";
		case EditorFaultPoint::write:
			return "write";
		case EditorFaultPoint::flush:
			return "flush";
		case EditorFaultPoint::preAtomicReplace:
			return "pre-atomic-replace";
		case EditorFaultPoint::candidateUpload:
			return "candidate-upload";
		case EditorFaultPoint::preCandidatePublish:
			return "pre-candidate-publish";
		case EditorFaultPoint::count:
			break;
	}
	return "unknown";
}

bool NoEditorFaultInjector::shouldFail(EditorFaultPoint) noexcept
{
	return false;
}

} // namespace demo::editor
