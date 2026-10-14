#pragma once

#include <mgif/scene_editor/EditorLimits.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tinygltf {
class Model;
}

namespace demo::editor {

class GltfDocument;
class GltfDocumentIO;

namespace detail {
class GltfModelValidator;
class GltfDocumentAccess;
class SceneCommandsAccess;
}

enum class GltfValidationErrorCode : std::uint8_t
{
	none = 0,
	limitExceeded,
	indexOutOfRange,
	duplicateParent,
	cycle,
	depthExceeded,
	invalidTransform,
	invalidBufferRange,
	invalidAccessor,
	invalidReference,
	invalidValue,
	invalidExtensionDeclaration,
	invalidAccessorPayload,
	accessorBoundsMismatch,
	invalidSparseIndex,
	invalidPrimitiveTopology,
	invalidPrimitiveIndex,
	attributeCountMismatch,
	invalidAnimationInput,
	invalidJointIndex,
	invalidWeight
};

struct GltfValidationIssue final
{
	GltfValidationErrorCode code{GltfValidationErrorCode::none};
	std::string path;
	std::string message;
};

class GltfValidationReport final
{
public:
	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] const std::vector<GltfValidationIssue>& issues() const noexcept;

private:
	bool m_valid{true};
	std::vector<GltfValidationIssue> m_issues;

	friend class GltfValidator;
	friend class detail::GltfModelValidator;
};

class GltfValidator final
{
public:
	[[nodiscard]] static GltfValidationReport validate(
		const GltfDocument& document,
		const EditorLimits& limits = kDefaultEditorLimits);

private:
	[[nodiscard]] static GltfValidationReport validateModel(
		const tinygltf::Model& model,
		const EditorLimits& limits);

	friend class detail::GltfDocumentAccess;
	friend class detail::SceneCommandsAccess;
	friend class GltfDocumentIO;
};

} // namespace demo::editor
