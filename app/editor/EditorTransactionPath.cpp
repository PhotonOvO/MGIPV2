#include "EditorTransactionPath.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <filesystem>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace demo::editor::automation {
namespace {

using std::filesystem::file_status;
using std::filesystem::file_type;
using std::filesystem::path;

[[noreturn]] void fail(std::string message, const std::error_code& error = {}) {
  if(error) {
    message += ": ";
    message += error.message();
  }
  throw std::invalid_argument(std::move(message));
}

[[nodiscard]] path normalizeAbsolute(const path& value,
                                     std::string_view label) {
  if(value.empty() || !value.is_absolute()) {
    fail(std::string(label) + " must be an absolute path");
  }
  std::error_code error;
  path absolute = std::filesystem::absolute(value, error);
  if(error || absolute.empty()) {
    fail(std::string("Could not normalize ") + std::string(label), error);
  }
  path normalized = absolute.lexically_normal();
  if(normalized != normalized.root_path() && normalized.filename().empty()) {
    normalized = normalized.parent_path();
  }
  return normalized;
}

[[nodiscard]] bool pathComponentEqual(const path& left, const path& right) {
#ifdef _WIN32
  const std::wstring leftText = left.native();
  const std::wstring rightText = right.native();
  return _wcsicmp(leftText.c_str(), rightText.c_str()) == 0;
#else
  return left == right;
#endif
}

[[nodiscard]] bool samePath(const path& left, const path& right) {
  const path normalizedLeft = left.lexically_normal();
  const path normalizedRight = right.lexically_normal();
  auto leftIt = normalizedLeft.begin();
  auto rightIt = normalizedRight.begin();
  for(; leftIt != normalizedLeft.end() && rightIt != normalizedRight.end();
      ++leftIt, ++rightIt) {
    if(!pathComponentEqual(*leftIt, *rightIt)) {
      return false;
    }
  }
  return leftIt == normalizedLeft.end() && rightIt == normalizedRight.end();
}

[[nodiscard]] std::vector<path> strictRelativeComponents(const path& root,
                                                          const path& candidate) {
  const path normalizedRoot = root.lexically_normal();
  const path normalizedCandidate = candidate.lexically_normal();
  std::vector<path> components;
  auto rootIt = normalizedRoot.begin();
  auto candidateIt = normalizedCandidate.begin();
  for(; rootIt != normalizedRoot.end(); ++rootIt, ++candidateIt) {
    if(candidateIt == normalizedCandidate.end() ||
       !pathComponentEqual(*rootIt, *candidateIt)) {
      return {};
    }
  }
  if(candidateIt == normalizedCandidate.end()) {
    return {};
  }
  for(; candidateIt != normalizedCandidate.end(); ++candidateIt) {
    if(*candidateIt == path{"."} || *candidateIt == path{".."} ||
       candidateIt->empty()) {
      return {};
    }
    components.push_back(*candidateIt);
  }
  return components;
}

enum class EntryState {
  absent,
  directory,
  alias,
  nonDirectory,
  error,
};

struct InspectedEntry final {
  EntryState state{EntryState::error};
  std::error_code error{};
};

[[nodiscard]] InspectedEntry inspectEntry(const path& value) {
  std::error_code error;
  const file_status status = std::filesystem::symlink_status(value, error);
  if(error && error != std::errc::no_such_file_or_directory) {
    return {EntryState::error, error};
  }
  if(std::filesystem::is_symlink(status)) {
    return {EntryState::alias, {}};
  }
  if(status.type() == file_type::not_found ||
     error == std::errc::no_such_file_or_directory) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(value.c_str());
    if(attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD winError = GetLastError();
      if(winError != ERROR_FILE_NOT_FOUND && winError != ERROR_PATH_NOT_FOUND) {
        return {EntryState::error,
                std::error_code(static_cast<int>(winError),
                                std::system_category())};
      }
      return {EntryState::absent, {}};
    }
    if((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
      return {EntryState::alias, {}};
    }
    if((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
      return {EntryState::nonDirectory, {}};
    }
    return {EntryState::directory, {}};
#else
    return {EntryState::absent, {}};
#endif
  }
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(value.c_str());
  if(attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD winError = GetLastError();
    return {EntryState::error,
            std::error_code(static_cast<int>(winError), std::system_category())};
  }
  if((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
    return {EntryState::alias, {}};
  }
#endif
  if(!std::filesystem::is_directory(status)) {
    return {EntryState::nonDirectory, {}};
  }
  return {EntryState::directory, {}};
}

void requireRealDirectory(const path& value, std::string_view label) {
  const InspectedEntry inspected = inspectEntry(value);
  switch(inspected.state) {
    case EntryState::directory:
      return;
    case EntryState::absent:
      fail(std::string(label) + " does not exist", inspected.error);
    case EntryState::alias:
      fail(std::string(label) + " cannot be a symlink or reparse point");
    case EntryState::nonDirectory:
      fail(std::string(label) + " is not a directory");
    case EntryState::error:
      fail(std::string("Could not inspect ") + std::string(label),
           inspected.error);
  }
  fail(std::string("Could not inspect ") + std::string(label));
}

void requireAbsoluteDirectoryChain(const path& value, std::string_view label) {
  std::vector<path> ancestors;
  const path filesystemRoot = value.root_path();
  for(path current = value;; current = current.parent_path()) {
    ancestors.push_back(current);
    const path parent = current.parent_path();
    if((!filesystemRoot.empty() && samePath(current, filesystemRoot)) ||
       parent.empty() || samePath(parent, current)) {
      break;
    }
  }
  for(auto current = ancestors.rbegin(); current != ancestors.rend(); ++current) {
    requireRealDirectory(*current, label);
  }
}

[[nodiscard]] path canonicalExisting(const path& value,
                                     std::string_view label);

#ifdef _WIN32
[[nodiscard]] path rawEnvironmentPath(const wchar_t* variableName,
                                      std::string_view label) {
  SetLastError(ERROR_SUCCESS);
  const DWORD required = GetEnvironmentVariableW(variableName, nullptr, 0u);
  if(required == 0u) {
    fail(std::string(label) + " must be present and non-empty");
  }

  std::wstring value(required, L'\0');
  SetLastError(ERROR_SUCCESS);
  const DWORD length = GetEnvironmentVariableW(variableName, value.data(),
                                                required);
  if(length == 0u) {
    fail(std::string(label) + " must be present and non-empty");
  }
  if(length >= required) {
    fail(std::string(label) + " changed during validation");
  }
  value.resize(length);
  return normalizeAbsolute(path{std::move(value)}, label);
}

[[nodiscard]] path canonicalEnvironmentRoot(const wchar_t* variableName,
                                            std::string_view label) {
  const path root = rawEnvironmentPath(variableName, label);
  const std::string ancestorLabel = std::string(label) + " ancestor";
  requireAbsoluteDirectoryChain(root, ancestorLabel);
  const path canonical = canonicalExisting(root, label);
  requireAbsoluteDirectoryChain(root, ancestorLabel);
  return canonical;
}

[[nodiscard]] path resolveWindowsConfiguredRoot() {
  // Validate both raw values before temp_directory_path() can resolve a
  // relative value against the process working directory.
  const path canonicalTemp =
      canonicalEnvironmentRoot(L"TEMP", "process TEMP");
  const path canonicalTmp =
      canonicalEnvironmentRoot(L"TMP", "process TMP");
  if(!samePath(canonicalTemp, canonicalTmp)) {
    fail("Process TEMP and TMP must identify the same existing real directory");
  }

  std::error_code error;
  const path libraryRoot = std::filesystem::temp_directory_path(error);
  if(error || libraryRoot.empty()) {
    fail("Could not resolve the process-configured temporary root", error);
  }
  const path normalizedLibraryRoot =
      normalizeAbsolute(libraryRoot, "standard-library temporary root");
  requireAbsoluteDirectoryChain(normalizedLibraryRoot,
                                "standard-library temporary root ancestor");
  const path canonicalLibraryRoot =
      canonicalExisting(normalizedLibraryRoot, "standard-library temporary root");
  if(!samePath(canonicalLibraryRoot, canonicalTemp)) {
    fail("Standard-library temporary root did not match process TEMP and TMP");
  }
  return canonicalTemp;
}
#endif

[[nodiscard]] path resolveConfiguredRoot(const path* explicitRoot) {
  if(explicitRoot != nullptr) {
    return normalizeAbsolute(*explicitRoot, "configured temporary root");
  }
#ifdef _WIN32
  return resolveWindowsConfiguredRoot();
#else
  std::error_code error;
  const path root = std::filesystem::temp_directory_path(error);
  if(error || root.empty()) {
    fail("Could not resolve the process-configured temporary root", error);
  }
  return normalizeAbsolute(root, "configured temporary root");
#endif
}

[[nodiscard]] path canonicalExisting(const path& value, std::string_view label) {
  std::error_code error;
  const path result = std::filesystem::canonical(value, error);
  if(error || result.empty()) {
    fail(std::string("Could not canonicalize ") + std::string(label), error);
  }
  return result.lexically_normal();
}

[[nodiscard]] EditorTransactionDirectoryValidation validateWithRoot(
    const path& requestedDirectory, const path& sourcePath,
    const path& explicitRoot) {
  if(requestedDirectory.empty() || !requestedDirectory.is_absolute()) {
    fail("--editor-transaction-dir must be an absolute isolated directory");
  }

  const path root = resolveConfiguredRoot(&explicitRoot);
  requireAbsoluteDirectoryChain(root, "configured temporary root ancestor");
  const path canonicalRoot = canonicalExisting(root, "configured temporary root");
  requireAbsoluteDirectoryChain(root, "configured temporary root ancestor");

  const path requested = normalizeAbsolute(requestedDirectory,
                                            "--editor-transaction-dir");
  const std::vector<path> components = strictRelativeComponents(root, requested);
  if(components.empty()) {
    fail("--editor-transaction-dir must be a strict child of configured temporary root " +
         root.string());
  }

  std::error_code sourceAbsoluteError;
  const path sourceAbsolute = std::filesystem::absolute(sourcePath,
                                                        sourceAbsoluteError)
                                  .lexically_normal();
  if(sourceAbsoluteError || sourceAbsolute.empty()) {
    fail("Could not normalize strict Sponza source", sourceAbsoluteError);
  }
  requireAbsoluteDirectoryChain(sourceAbsolute.parent_path(),
                                "strict Sponza source ancestor");
  const InspectedEntry sourceState = inspectEntry(sourceAbsolute);
  if(sourceState.state == EntryState::alias) {
    fail("strict Sponza source cannot be a symlink or reparse point");
  }
  std::error_code sourceTypeError;
  if(sourceState.state != EntryState::nonDirectory ||
     !std::filesystem::is_regular_file(sourceAbsolute, sourceTypeError) ||
     sourceTypeError) {
    fail("strict Sponza source is not a regular file", sourceState.error);
  }
  const path canonicalSource = canonicalExisting(sourceAbsolute,
                                                 "strict Sponza source");

  // Inspect every pre-existing component before any creation. Missing
  // components are created one at a time, with a no-alias recheck immediately
  // after each create so create_directories cannot follow an alias chain.
  path current = root;
  for(const path& component : components) {
    current /= component;
    const InspectedEntry beforeCreate = inspectEntry(current);
    if(beforeCreate.state == EntryState::alias) {
      fail("--editor-transaction-dir cannot traverse a symlink or reparse point");
    }
    if(beforeCreate.state == EntryState::nonDirectory) {
      fail("--editor-transaction-dir contains a non-directory component");
    }
    if(beforeCreate.state == EntryState::error) {
      fail("Could not inspect --editor-transaction-dir component",
           beforeCreate.error);
    }
    if(beforeCreate.state == EntryState::absent) {
      requireAbsoluteDirectoryChain(
          current.parent_path(), "--editor-transaction-dir ancestor");
      std::error_code createError;
      const bool created = std::filesystem::create_directory(current, createError);
      if(createError && createError != std::errc::file_exists) {
        fail("Could not create --editor-transaction-dir component", createError);
      }
      (void)created;
    }
    const InspectedEntry afterCreate = inspectEntry(current);
    if(afterCreate.state == EntryState::alias) {
      fail("--editor-transaction-dir cannot traverse a symlink or reparse point");
    }
    if(afterCreate.state != EntryState::directory) {
      fail("--editor-transaction-dir component was not a real directory",
           afterCreate.error);
    }
    requireAbsoluteDirectoryChain(current,
                                  "--editor-transaction-dir ancestor");
  }

  // Recheck the whole chain before canonical() and again after it. This keeps
  // canonical containment from becoming a check-then-follow alias check.
  requireAbsoluteDirectoryChain(current, "--editor-transaction-dir ancestor");
  const path canonicalDirectory = canonicalExisting(current,
                                                    "--editor-transaction-dir");
  requireAbsoluteDirectoryChain(current, "--editor-transaction-dir ancestor");
  if(!samePath(canonicalRoot, canonicalExisting(root,
                                                 "configured temporary root")) ||
     strictRelativeComponents(canonicalRoot, canonicalDirectory).empty()) {
    fail("--editor-transaction-dir canonical path escaped configured temporary root " +
         canonicalRoot.string());
  }

  const path target = canonicalDirectory / "sponza-transaction.glb";
  const InspectedEntry targetState = inspectEntry(target);
  if(targetState.state != EntryState::absent) {
    fail("Editor transaction output target must be absent",
         targetState.error);
  }

  if(samePath(canonicalDirectory, canonicalSource.parent_path()) ||
     samePath(target, canonicalSource)) {
    fail("Editor transaction output cannot alias the source asset");
  }

  std::error_code emptyError;
  if(!std::filesystem::is_empty(canonicalDirectory, emptyError) || emptyError) {
    fail("--editor-transaction-dir must be empty at startup", emptyError);
  }
  requireAbsoluteDirectoryChain(current, "--editor-transaction-dir ancestor");
  if(!samePath(canonicalDirectory,
               canonicalExisting(current, "--editor-transaction-dir"))) {
    fail("--editor-transaction-dir changed during validation");
  }

  return {canonicalRoot, canonicalDirectory, target};
}

} // namespace

std::filesystem::path configuredTemporaryRoot() {
  const path root = resolveConfiguredRoot(nullptr);
  requireAbsoluteDirectoryChain(root, "configured temporary root ancestor");
  return canonicalExisting(root, "configured temporary root");
}

EditorTransactionDirectoryValidation validateEditorTransactionDirectory(
    const std::filesystem::path& requestedDirectory,
    const std::filesystem::path& sourcePath) {
  const path root = resolveConfiguredRoot(nullptr);
  return validateWithRoot(requestedDirectory, sourcePath, root);
}

EditorTransactionDirectoryValidation validateEditorTransactionDirectory(
    const std::filesystem::path& requestedDirectory,
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& configuredRoot) {
  return validateWithRoot(requestedDirectory, sourcePath, configuredRoot);
}

} // namespace demo::editor::automation
