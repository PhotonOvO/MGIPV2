#pragma once

#include <mgif/scene_editor/EditorPickingTypes.h>

#include "../../rhi/RHIQueue.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace demo::editor {

struct EditorPickingPassTicket final {
  std::uint64_t value{0};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }

  auto operator<=>(const EditorPickingPassTicket &) const = default;
};

struct EditorPickingBackendSubmission final {
  bool accepted{false};
  EditorPickingPassTicket ticket;
  rhi::SubmissionToken submission;
  PickResultReason reason{PickResultReason::backendRejected};
};

enum class EditorPickingBackendReadbackStatus : std::uint8_t {
  sampled = 0,
  miss,
  error,
  cancelled,
};

struct EditorPickingBackendReadback final {
  EditorPickingPassTicket ticket;
  rhi::SubmissionToken submission;
  EditorPickingBackendReadbackStatus status{
      EditorPickingBackendReadbackStatus::error};
  FramebufferPickToken token;
  std::optional<EditorWorldPoint> worldHit;
  std::optional<double> rayDistance;
  PickResultReason reason{PickResultReason::backendFailure};
};

// Implemented by the renderer/RHI integration owner. The interface has no
// synchronous wait operation: readiness is observed only through pollOne().
class IEditorPickingPassBackend {
public:
  virtual ~IEditorPickingPassBackend() = default;

  [[nodiscard]] virtual EditorPickingBackendSubmission
  enqueue(const EditorPickRequest &request) noexcept = 0;
  [[nodiscard]] virtual std::optional<EditorPickingBackendReadback>
  pollOne() noexcept = 0;
  virtual void cancel(EditorPickingPassTicket ticket) noexcept = 0;
  virtual void retireReadback(EditorPickingPassTicket ticket,
                              rhi::SubmissionToken submission) noexcept = 0;
};

struct EditorPickingPassCompletion final {
  EditorPickRequest request;
  EditorPickingBackendReadbackStatus status{
      EditorPickingBackendReadbackStatus::error};
  FramebufferPickToken token;
  std::optional<EditorWorldPoint> worldHit;
  std::optional<double> rayDistance;
  PickResultReason reason{PickResultReason::backendFailure};
};

using EditorPickingPassShutdownCallback = void (*)(
    void *context, const EditorPickingPassCompletion &completion) noexcept;

struct EditorPickingPassStats final {
  std::uint64_t submitCalls{0};
  std::uint64_t acceptedRequests{0};
  std::uint64_t rejectedRequests{0};
  std::uint64_t terminalCompletions{0};
  std::uint64_t cancelledRequests{0};
  std::uint64_t ignoredUnknownOrDuplicateReadbacks{0};
  std::uint64_t retirementCalls{0};
  std::uint64_t synchronousWaitCalls{0};

  bool operator==(const EditorPickingPassStats &) const = default;
};

class EditorPickingPass final {
public:
  explicit EditorPickingPass(IEditorPickingPassBackend &backend) noexcept;
  ~EditorPickingPass() noexcept;

  EditorPickingPass(const EditorPickingPass &) = delete;
  EditorPickingPass &operator=(const EditorPickingPass &) = delete;

  [[nodiscard]] PickSubmitResult submit(const EditorPickRequest &request);
  void poll(std::vector<EditorPickingPassCompletion> &out);
  void cancelAuthority(EditorDocumentAuthority authority);
  void cancelAll();
  void shutdown(void *context,
                EditorPickingPassShutdownCallback callback) noexcept;

  [[nodiscard]] std::size_t pendingCount() const noexcept;
  [[nodiscard]] const EditorPickingPassStats &stats() const noexcept;

private:
  struct Pending final {
    EditorPickRequest request;
    EditorPickingPassTicket ticket;
    rhi::SubmissionToken submission;
  };

  void pump();
  [[nodiscard]] std::size_t
  pendingTicketIndex(EditorPickingPassTicket ticket) const noexcept;
  [[nodiscard]] std::size_t
  pendingIndex(EditorPickingPassTicket ticket,
               rhi::SubmissionToken submission) const noexcept;

  IEditorPickingPassBackend *m_backend{nullptr};
  std::vector<Pending> m_pending;
  std::vector<EditorPickingPassCompletion> m_ready;
  std::uint64_t m_lastAcceptedSerial{0};
  EditorPickingPassStats m_stats;
  bool m_shutdown{false};
};

} // namespace demo::editor
