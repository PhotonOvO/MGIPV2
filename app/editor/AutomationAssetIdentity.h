#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace demo::editor::automation {

inline constexpr std::string_view kSceneReadyModel =
    "resources/GLTF_Sponza/sponza.gltf";

namespace detail {

[[nodiscard]] inline bool isValidPathText(std::string_view value) noexcept
{
  return !value.empty() && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] inline std::optional<std::filesystem::path> normalizedAbsolutePath(
    const std::filesystem::path& value)
{
  std::error_code error;
  std::filesystem::path normalized = std::filesystem::weakly_canonical(value, error);
  if(!error)
  {
    return normalized.lexically_normal();
  }

  error.clear();
  normalized = std::filesystem::absolute(value, error);
  if(error)
  {
    return std::nullopt;
  }
  return normalized.lexically_normal();
}

} // namespace detail

[[nodiscard]] inline bool sameAssetIdentity(std::string_view requestedPath,
                                            std::string_view loadedPath) noexcept
{
  if(!detail::isValidPathText(requestedPath)
     || !detail::isValidPathText(loadedPath))
  {
    return false;
  }

  try
  {
    const std::filesystem::path requested{std::string(requestedPath)};
    const std::filesystem::path loaded{std::string(loadedPath)};

    std::error_code equivalentError;
    const bool equivalent =
        std::filesystem::equivalent(requested, loaded, equivalentError);
    if(!equivalentError && equivalent)
    {
      return true;
    }

    const std::optional<std::filesystem::path> normalizedRequested =
        detail::normalizedAbsolutePath(requested);
    const std::optional<std::filesystem::path> normalizedLoaded =
        detail::normalizedAbsolutePath(loaded);
    return normalizedRequested.has_value() && normalizedLoaded.has_value()
        && *normalizedRequested == *normalizedLoaded;
  }
  catch(...)
  {
    return false;
  }
}

[[nodiscard]] inline bool isSceneReadyModel(std::string_view loadedPath) noexcept
{
  return sameAssetIdentity(kSceneReadyModel, loadedPath);
}

[[nodiscard]] constexpr std::string_view sceneReadyMarkerModel() noexcept
{
  return kSceneReadyModel;
}

} // namespace demo::editor::automation
