#pragma once

#include "../Pass.h"
#include "../editor/EditorOverlayShaderInterop.h"
#include "../editor/EditorOverlayTypes.h"
#include "../editor/GridModel.h"
#include "../../rhi/RHIDevice.h"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace demo {

inline constexpr TextureHandle kPassEditorOverlayHelperDepthHandle{0xF301u,
                                                                    1u};

enum class EditorOverlayPassError : std::uint8_t {
  none = 0,
  invalidArgument,
  notInitialized,
  alreadyInitialized,
  frameBusy,
  staleTicket,
  invalidSubmission,
  resourceFailure,
  recordingFailure,
};

enum class EditorOverlayFrameDisposition : std::uint8_t {
  notSubmitted = 0,
  submitted,
};

enum class EditorOverlayRecordingStage : std::uint8_t {
  none = 0,
  beginEvent,
  beginRenderPass,
  dynamicState,
  argumentTable,
  gridDraw,
  axesDraw,
  gizmoDraw,
  endEncoding,
};

struct EditorOverlayFrameTicket final {
  std::uint32_t slot{0u};
  std::uint32_t generation{0u};
  std::uint64_t frameSerial{0u};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return generation != 0u && frameSerial != 0u;
  }

  auto operator<=>(const EditorOverlayFrameTicket &) const = default;
};

struct EditorOverlayPassCreateInfo final {
  std::uint32_t frameSlotCount{0u};
  rhi::ShaderIRFormat shaderFormat{rhi::ShaderIRFormat::unknown};
  std::span<const std::byte> shaderData{};
  rhi::TextureFormat helperDepthFormat{rhi::TextureFormat::d32Sfloat};
};

struct EditorOverlayPassTargets final {
  rhi::TextureHandle outputTexture{};
  rhi::TextureViewHandle outputView{};
  rhi::TextureFormat outputFormat{rhi::TextureFormat::undefined};
  rhi::SampleCount outputSamples{rhi::SampleCount::count1};
  rhi::TextureHandle sceneDepthTexture{};
  rhi::TextureViewHandle sceneDepthView{};
  rhi::TextureFormat sceneDepthFormat{rhi::TextureFormat::undefined};
  rhi::SampleCount sceneDepthSamples{rhi::SampleCount::count1};
};

// Spans are consumed and copied into the selected frame slot before
// prepareFrame returns. The pass never retains caller-owned storage.
struct EditorOverlayPassFrame final {
  std::uint64_t frameSerial{0u};
  editor::EditorViewportMetrics viewport{};
  editor::OverlayMatrix4 viewProjection{};
  bool gridVisible{false};
  editor::GridDrawPacket grid{};
  std::span<const editor::GridLinePacket> gridLines{};
  std::span<const editor::GridLinePacket> gridAxes{};
  bool gizmoVisible{false};
  editor::GizmoDrawPacket gizmo{};
  EditorOverlayPassTargets targets{};
};

struct EditorOverlayHelperDepthBinding final {
  TextureHandle passHandle{kPassEditorOverlayHelperDepthHandle};
  rhi::TextureHandle texture{};
  rhi::TextureViewHandle view{};
  rhi::ResourceState initialState{rhi::ResourceState::Undefined};
  rhi::TextureFormat format{rhi::TextureFormat::undefined};
  rhi::Extent2D extent{};
  rhi::SampleCount samples{rhi::SampleCount::count1};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return !texture.isNull() && !view.isNull() &&
           format != rhi::TextureFormat::undefined && extent.width != 0u &&
           extent.height != 0u;
  }
};

struct EditorOverlayPrepareResult final {
  EditorOverlayPassError error{EditorOverlayPassError::none};
  EditorOverlayFrameTicket ticket{};
  EditorOverlayHelperDepthBinding helperDepth{};

  [[nodiscard]] constexpr bool succeeded() const noexcept {
    return error == EditorOverlayPassError::none;
  }
};

struct EditorOverlayPassDiagnostics final {
  std::uint64_t initializeCalls{0u};
  std::uint64_t prepareCalls{0u};
  std::uint64_t recordedFrames{0u};
  std::uint64_t submittedFrames{0u};
  std::uint64_t completedFrames{0u};
  std::uint64_t abandonedFrames{0u};
  std::uint64_t rejectedFrames{0u};
  std::uint64_t gridDraws{0u};
  std::uint64_t axesDraws{0u};
  std::uint64_t gizmoDraws{0u};
  std::uint64_t pipelineCreations{0u};
  std::uint64_t helperDepthCreations{0u};
  std::uint64_t helperDepthReplacements{0u};
  std::uint64_t frameTableRebuilds{0u};
  std::uint64_t descriptorWrites{0u};
  std::uint64_t queueCompletionQueries{0u};
  std::uint64_t failuresBeforeSubmit{0u};
  std::uint64_t failuresAfterSubmit{0u};
  std::uint64_t surfaceReleases{0u};
  std::uint64_t shutdownCalls{0u};
  std::uint64_t deviceIdleCalls{0u};
  std::uint64_t nativeEscapeCalls{0u};
  EditorOverlayPassError lastError{EditorOverlayPassError::none};
  EditorOverlayRecordingStage lastRecordingStage{
      EditorOverlayRecordingStage::none};
};

class EditorOverlayPass final : public RenderPassNode {
public:
  EditorOverlayPass() = default;
  ~EditorOverlayPass() override;

  EditorOverlayPass(const EditorOverlayPass &) = delete;
  EditorOverlayPass &operator=(const EditorOverlayPass &) = delete;

  [[nodiscard]] rhi::RHIResult
  initialize(rhi::Device &device,
             const EditorOverlayPassCreateInfo &createInfo) noexcept;

  [[nodiscard]] EditorOverlayPrepareResult
  prepareFrame(std::uint32_t frameSlot,
               const EditorOverlayPassFrame &frame) noexcept;

  [[nodiscard]] rhi::RHIResult
  resolveFrame(EditorOverlayFrameTicket ticket,
               EditorOverlayFrameDisposition disposition,
               rhi::SubmissionToken submission = {}) noexcept;

  // Makes completed frame slots reusable without waiting. Returns the number
  // of slots that transitioned back to idle.
  [[nodiscard]] std::uint32_t collectCompletedFrames() noexcept;

  // Releases helper-depth and format-dependent pipeline resources. Pending
  // physical uses remain owned by the RHI retirement queue.
  [[nodiscard]] rhi::RHIResult releaseSurfaceResources() noexcept;
  [[nodiscard]] rhi::RHIResult shutdown() noexcept;

  [[nodiscard]] bool initialized() const noexcept { return m_device != nullptr; }
  [[nodiscard]] EditorOverlayHelperDepthBinding helperDepthBinding() const noexcept;
  [[nodiscard]] const EditorOverlayPassDiagnostics &diagnostics() const noexcept {
    return m_diagnostics;
  }

  [[nodiscard]] const char *getName() const override {
    return "EditorOverlay";
  }
  [[nodiscard]] HandleSlice<PassResourceDependency>
  getDependencies() const override;
  void execute(const PassContext &context) const override;

private:
  enum class FrameState : std::uint8_t { idle = 0, prepared, submitted };

  enum class PipelineFamily : std::uint8_t {
    grid = 0,
    axes,
    gizmoSolidForward,
    gizmoDitherForward,
    gizmoPlaneForward,
    gizmoSolidReverse,
    gizmoDitherReverse,
    gizmoPlaneReverse,
    count,
  };

  struct SurfaceSignature final {
    rhi::TextureFormat outputFormat{rhi::TextureFormat::undefined};
    rhi::TextureFormat helperDepthFormat{rhi::TextureFormat::undefined};
    rhi::SampleCount samples{rhi::SampleCount::count1};

    auto operator<=>(const SurfaceSignature &) const = default;
  };

  struct SurfaceResources final {
    SurfaceSignature signature{};
    rhi::Extent2D extent{};
    rhi::TextureHandle helperDepth{};
    rhi::TextureViewHandle helperDepthView{};
    std::array<rhi::PipelineHandle,
               static_cast<std::size_t>(PipelineFamily::count)>
        pipelines{};
    rhi::ResourceState helperState{rhi::ResourceState::Undefined};

    [[nodiscard]] bool valid() const noexcept;
  };

  struct FrameSlot final {
    FrameState state{FrameState::idle};
    EditorOverlayFrameTicket ticket{};
    bool recorded{false};
    bool noDraw{false};
    bool tablePoisoned{false};
    std::uint32_t gridLineCount{0u};
    std::uint32_t axisLineCount{0u};
    editor::EditorOverlayFrameUniforms uniforms{};
    std::vector<editor::EditorOverlayLineInstance> encodedLines{};
    editor::GizmoDrawPacket gizmo{};
    EditorOverlayPassTargets targets{};
    rhi::BufferHandle uniformBuffer{};
    rhi::BufferHandle lineBuffer{};
    rhi::ArgumentTableHandle argumentTable{};
    std::uint64_t submissionBaselineValue{0u};
    rhi::SubmissionToken submission{};
  };

  [[nodiscard]] rhi::RHIResult createStaticResources(
      const EditorOverlayPassCreateInfo &createInfo) noexcept;
  [[nodiscard]] rhi::RHIResult createFrameResources(
      std::uint32_t frameSlotCount) noexcept;
  [[nodiscard]] rhi::RHIResult ensureSurfaceResources(
      const EditorOverlayPassFrame &frame) noexcept;
  [[nodiscard]] rhi::RHIResult ensureFrameTable(FrameSlot &slot) noexcept;
  [[nodiscard]] rhi::RHIResult encodeFrame(
      FrameSlot &slot, const EditorOverlayPassFrame &frame) noexcept;
  [[nodiscard]] bool destroySurfaceResources() noexcept;
  [[nodiscard]] bool
  destroySurfaceResourceSet(SurfaceResources &surface) noexcept;
  [[nodiscard]] bool destroyFrameResources() noexcept;
  [[nodiscard]] bool destroyStaticResources() noexcept;
  [[nodiscard]] bool destroyAll() noexcept;
  void recordError(EditorOverlayPassError error) const noexcept;

  [[nodiscard]] rhi::PipelineHandle pipelineFor(
      const editor::GizmoComponentDrawPacket &draw,
      const editor::GizmoDrawPacket &packet,
      const editor::EditorProjectionConvention &convention) const noexcept;

  rhi::Device *m_device{nullptr};
  rhi::Queue *m_graphicsQueue{nullptr};
  rhi::TextureFormat m_helperDepthFormat{rhi::TextureFormat::d32Sfloat};
  rhi::ShaderLibraryHandle m_shaderLibrary{};
  rhi::SamplerHandle m_sceneDepthSampler{};
  rhi::ArgumentLayoutHandle m_argumentLayout{};
  rhi::BufferHandle m_gizmoVertexBuffer{};
  rhi::BufferHandle m_gizmoIndexBuffer{};
  SurfaceResources m_surface{};
  mutable std::vector<FrameSlot> m_frames{};
  std::uint64_t m_lastFrameSerial{0u};
  std::uint32_t m_nextTicketGeneration{1u};
  mutable EditorOverlayPassDiagnostics m_diagnostics{};
};

} // namespace demo
