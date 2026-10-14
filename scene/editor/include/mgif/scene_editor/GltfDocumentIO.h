#pragma once

#include <mgif/scene_editor/EditorFaultInjection.h>
#include <mgif/scene_editor/EditorLimits.h>
#include <mgif/scene_editor/GltfDocument.h>
#include <mgif/scene_editor/GltfValidator.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace demo::editor {

enum class GltfLoadError : std::uint8_t
{
	none = 0,
	unsupportedFormat,
	sourceUnavailable,
	sourceTooLarge,
	invalidContainer,
	unsafeDependency,
	dependencyUnavailable,
	dependencyTooLarge,
	parseFailed,
	validationFailed,
	injectedFailure,
	allocationFailed,
	internalError
};

enum class GltfSaveError : std::uint8_t
{
	none = 0,
	unsupportedFormat,
	invalidDocument,
	unsafeTarget,
	dependencyUnavailable,
	stagingUnavailable,
	serializationFailed,
	stagedValidationFailed,
	semanticMismatch,
	publishFailed,
	injectedFailure,
	allocationFailed,
	internalError
};

struct GltfLoadOptions final
{
	EditorLimits limits{kDefaultEditorLimits};
	EditorFaultInjector* faultInjector{nullptr};
};

struct GltfSaveOptions final
{
	EditorLimits limits{kDefaultEditorLimits};
	EditorFaultInjector* faultInjector{nullptr};
	bool prettyPrint{true};
};

class GltfLoadResult final
{
public:
	GltfLoadResult(GltfLoadResult&&) noexcept;
	GltfLoadResult& operator=(GltfLoadResult&&) noexcept;
	~GltfLoadResult();

	GltfLoadResult(const GltfLoadResult&) = delete;
	GltfLoadResult& operator=(const GltfLoadResult&) = delete;

	[[nodiscard]] bool succeeded() const noexcept;
	[[nodiscard]] GltfLoadError error() const noexcept;
	[[nodiscard]] const std::string& message() const noexcept;
	[[nodiscard]] const std::string& warnings() const noexcept;
	[[nodiscard]] const GltfValidationReport& validationReport() const noexcept;
	[[nodiscard]] GltfDocument* candidate() noexcept;
	[[nodiscard]] const GltfDocument* candidate() const noexcept;
	[[nodiscard]] std::unique_ptr<GltfDocument> takeCandidate() && noexcept;

	// A failed or already-consumed result cannot replace active. Destruction of
	// the previous document occurs only after a fully validated candidate exists.
	[[nodiscard]] bool publishTo(std::unique_ptr<GltfDocument>& active) && noexcept;

private:
	GltfLoadResult(GltfLoadError error,
	               std::string message,
	               std::string warnings,
	               GltfValidationReport validationReport,
	               std::unique_ptr<GltfDocument> candidate) noexcept;

	GltfLoadError m_error{GltfLoadError::internalError};
	std::string m_message;
	std::string m_warnings;
	GltfValidationReport m_validationReport;
	std::unique_ptr<GltfDocument> m_candidate;

	friend class GltfDocumentIO;
};

class GltfSaveResult final
{
public:
	[[nodiscard]] bool succeeded() const noexcept;
	[[nodiscard]] GltfSaveError error() const noexcept;
	[[nodiscard]] const std::string& message() const noexcept;
	[[nodiscard]] const GltfValidationReport& validationReport() const noexcept;
	[[nodiscard]] bool usedRecoverableReplaceFallback() const noexcept;
	[[nodiscard]] const std::filesystem::path& recoveryPath() const noexcept;

private:
	GltfSaveResult(GltfSaveError error,
	               std::string message,
	               GltfValidationReport validationReport = {},
	               bool usedRecoverableReplaceFallback = false,
	               std::filesystem::path recoveryPath = {}) noexcept;

	GltfSaveError m_error{GltfSaveError::internalError};
	std::string m_message;
	GltfValidationReport m_validationReport;
	bool m_usedRecoverableReplaceFallback{false};
	std::filesystem::path m_recoveryPath;

	friend class GltfDocumentIO;
};

class GltfDocumentIO final
{
public:
	// Create a validated, self-contained document for the editor's New Scene
	// command. The candidate has one active empty scene and no dependencies.
	[[nodiscard]] static GltfLoadResult createEmptyCandidate(
		std::string_view sceneName = "Scene");

	[[nodiscard]] static GltfLoadResult loadCandidate(
		const std::filesystem::path& sourcePath,
		const GltfLoadOptions& options = {});

	// Save As is the only persistence operation. The target is published last.
	// Windows publishes an existing regular target with one flushed, same-volume
	// atomic replacement call; no intermediate move removes the selected path.
	[[nodiscard]] static GltfSaveResult saveAs(
		GltfDocument& document,
		const std::filesystem::path& targetPath,
		const GltfSaveOptions& options = {});

private:
	[[nodiscard]] static GltfLoadResult failure(
		GltfLoadError error,
		std::string message,
		std::string warnings = {},
		GltfValidationReport validationReport = {});
	[[nodiscard]] static GltfSaveResult saveFailure(
		GltfSaveError error,
		std::string message,
		GltfValidationReport validationReport = {},
		bool usedRecoverableReplaceFallback = false,
		std::filesystem::path recoveryPath = {});
};

} // namespace demo::editor
