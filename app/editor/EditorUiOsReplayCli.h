#pragma once

#include "EditorUiOsReplayPlan.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace demo::editor::testing::osreplay {

inline constexpr std::string_view kPlanContractSha256 =
    "15b7e1037139631e477b6e2ffb9bc0421ccb6932c5616ba2a3ef46c664769526";
inline constexpr std::string_view kCapabilitySourceRole =
    "editor-ui-os-replay-cli-v1";

enum class CliMode : std::uint8_t {
  disabled = 0,
  capability,
  run,
  invalid,
};

struct CliOptions final {
  std::string caseId;
  std::string subcase;
  std::uint32_t startStep{0};
  std::uint32_t stopStep{0};
  std::filesystem::path evidenceDirectory;
  std::filesystem::path syncDirectory;
  std::string runNonce;
  std::string planSha256;
  std::string executableSha256;
  std::string sourceSha256;
  std::uint32_t width{0};
  std::uint32_t height{0};
  std::uint32_t timeoutMilliseconds{0};
};

struct CliParseResult final {
  CliMode mode{CliMode::disabled};
  CliOptions options;
  std::vector<std::string> unconsumed;
  std::string error;

  [[nodiscard]] bool accepted() const noexcept {
    return mode != CliMode::invalid;
  }
  [[nodiscard]] bool runRequested() const noexcept {
    return mode == CliMode::run;
  }
};

class CapabilityIntegrationToken final {
private:
  CapabilityIntegrationToken() = default;
#if defined(MGIF_EDITOR_OS_REPLAY)
  friend CapabilityIntegrationToken editorUiOsReplayIntegrationToken();
#endif
};

#if defined(MGIF_EDITOR_OS_REPLAY)
[[nodiscard]] inline CapabilityIntegrationToken
editorUiOsReplayIntegrationToken() {
  return CapabilityIntegrationToken{};
}
#endif

// Arguments exclude argv[0]. Only the --editor-os-* namespace is owned here;
// unrelated application arguments are returned unchanged in unconsumed.
[[nodiscard]] CliParseResult
parseEditorUiOsReplayCli(std::span<const std::string_view> arguments);

[[nodiscard]] std::string editorUiOsReplayCapabilityJson();
[[nodiscard]] std::string
editorUiOsReplayCapabilityJson(CapabilityIntegrationToken);
[[nodiscard]] std::string editorUiOsReplayCapabilityJson(
    CapabilityIntegrationToken, std::string_view caseId);

} // namespace demo::editor::testing::osreplay
