#pragma once

#include "EditorUiOsReplayPlan.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace demo::editor::testing::osreplay {

struct ContinueReceipt final {
  std::uint32_t schemaVersion{kSchemaVersion};
  std::string protocol{std::string(kProtocol)};
  std::string runNonce;
  std::string caseId{std::string(kU02Case)};
  std::string planSha256;
  std::uint32_t step{0};
  std::string stepId;
  std::string phaseId;
  std::string readySha256;
  InputKind inputKind{InputKind::none};
  std::uint32_t sendInputCount{0};
};

struct RuntimeHostSnapshot final {
  ClientProbe client;
  AuthorityProbe authority;
  bool rendererSettled{false};
  bool clientFocused{false};
  std::optional<GizmoProbe> gizmo;
};

struct ReadyPublication final {
  bool published{false};
  std::string readySha256;
  std::string error;
};

struct RuntimeCallbacks final {
  std::function<std::optional<RuntimeHostSnapshot>()> snapshot;
  std::function<ReadyPublication(const ReadyProbe &)> publishReady;
};

struct RuntimeConfig final {
  ReplayPlan plan;
  std::string subcase;
  std::uint32_t startStep{0};
  std::uint32_t stopStep{0};
  std::string runNonce;
  std::string planSha256;
  std::string executableSha256;
  std::string sourceSha256;
  std::uint32_t requiredStableFrames{1};
};

enum class RuntimeStatus : std::uint8_t {
  running = 0,
  waitingForContinue,
  complete,
  failed,
};

class EditorUiOsReplayRuntime final : public ui::EditorUiProbe {
public:
  EditorUiOsReplayRuntime(RuntimeConfig config, RuntimeCallbacks callbacks);
  ~EditorUiOsReplayRuntime() override;

  EditorUiOsReplayRuntime(const EditorUiOsReplayRuntime &) = delete;
  EditorUiOsReplayRuntime &
  operator=(const EditorUiOsReplayRuntime &) = delete;
  EditorUiOsReplayRuntime(EditorUiOsReplayRuntime &&) noexcept;
  EditorUiOsReplayRuntime &
  operator=(EditorUiOsReplayRuntime &&) noexcept;

  void beginFrame(const ui::EditorUiProbeFrameBinding &binding) override;
  void observeItem(const ui::EditorUiProbeItem &item) override;
  void observeDispatch(const ui::UiCommand &command,
                       const ui::RuntimeActionResult &result) override;
  void endFrame() override;

  // SceneEditorUi::endFrame only seals CPU observations. The host calls this
  // after the corresponding renderer frame has returned, so a published ready
  // file and its PNG boundary always describe the same completed frame.
  [[nodiscard]] bool onFrameRendered();
  [[nodiscard]] bool acceptContinue(const ContinueReceipt &receipt);
  [[nodiscard]] RuntimeStatus status() const noexcept;
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool waitingForContinue() const noexcept;
  [[nodiscard]] std::string_view error() const noexcept;
  [[nodiscard]] std::uint32_t currentStep() const noexcept;
  [[nodiscard]] std::size_t currentPhase() const noexcept;
  [[nodiscard]] std::uint32_t pendingStableFrameCount() const noexcept;
  [[nodiscard]] std::uint64_t pendingStableFrameSerial() const noexcept;
  [[nodiscard]] std::string_view pendingReason() const noexcept;
  [[nodiscard]] const std::optional<ReadyProbe> &lastReady() const noexcept;
  [[nodiscard]] std::optional<ui::GraphRowKey>
  graphRole(std::string_view role) const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace demo::editor::testing::osreplay
