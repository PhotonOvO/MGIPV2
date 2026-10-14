#pragma once

#include <filesystem>

namespace demo::editor::automation {

// The result is canonical only after the directory chain has been checked for
// aliases. Callers should use directory for all subsequent transaction I/O.
struct EditorTransactionDirectoryValidation final {
  std::filesystem::path root;
  std::filesystem::path directory;
  std::filesystem::path outputTarget;
};

// Resolve the process-configured temporary root. On Windows, raw TEMP and TMP
// must both be present, absolute, and identify the same existing real
// directory; the standard-library temporary root must match them. This
// function never creates the root.
[[nodiscard]] std::filesystem::path configuredTemporaryRoot();

// Validate and, when necessary, create one isolated child below the configured
// temporary root. The overload accepting a root exists for dependency-free
// tests and uses exactly the same validation implementation as production.
[[nodiscard]] EditorTransactionDirectoryValidation
validateEditorTransactionDirectory(const std::filesystem::path& requestedDirectory,
                                   const std::filesystem::path& sourcePath);

[[nodiscard]] EditorTransactionDirectoryValidation
validateEditorTransactionDirectory(const std::filesystem::path& requestedDirectory,
                                   const std::filesystem::path& sourcePath,
                                   const std::filesystem::path& configuredRoot);

} // namespace demo::editor::automation
