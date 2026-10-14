#pragma once

#include "EditorUiOsReplayCli.h"
#include "EditorUiOsReplayRuntime.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

struct GLFWwindow;

namespace demo::editor::testing::osreplay {

class EditorUiOsReplayHost final {
public:
  EditorUiOsReplayHost(GLFWwindow *window, CliOptions options,
                      std::function<std::size_t()> pendingNativeTerminals = {},
                      std::function<bool()> nativeFocusInterrupted = {});
  ~EditorUiOsReplayHost();

  EditorUiOsReplayHost(const EditorUiOsReplayHost &) = delete;
  EditorUiOsReplayHost &operator=(const EditorUiOsReplayHost &) = delete;
  EditorUiOsReplayHost(EditorUiOsReplayHost &&) noexcept;
  EditorUiOsReplayHost &operator=(EditorUiOsReplayHost &&) noexcept;

  [[nodiscard]] std::optional<RuntimeHostSnapshot>
  snapshot(AuthorityProbe authority, bool rendererSettled) const;
  [[nodiscard]] ReadyPublication publishReady(const ReadyProbe &ready);
  [[nodiscard]] ContinueReceipt waitForContinue(const ReadyProbe &ready);
  void confirmAcceptedContinue(const ContinueReceipt &receipt);
  void writeResult(bool gpuIdleWaited);

  [[nodiscard]] const CliOptions &options() const noexcept;
  [[nodiscard]] const std::filesystem::path &evidenceDirectory() const noexcept;
  [[nodiscard]] std::size_t validatedReceiptCount() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace demo::editor::testing::osreplay
