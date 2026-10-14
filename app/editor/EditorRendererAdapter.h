#pragma once

#include "EditorController.h"

#include "../../render/RendererFacade.h"

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace demo::editor::ui {

// Production-only assembly adapter. It converts immutable editor publications
// into the MGIF-owned renderer transaction contract without exposing the RHI
// or renderer handles to the Dear ImGui layer.
class EditorRendererAdapter final {
public:
  struct FlushResult final {
    bool consumed{false};
    bool accepted{false};
    bool fullSnapshot{false};
    ProjectionRefresh refresh{ProjectionRefresh::none};
    SceneApplyError error{SceneApplyError::InvalidState};
    std::string message;
    std::optional<RendererTargetFence> resultingFence;
    std::optional<SceneProjectionResult> projection;
    std::optional<SceneUploadPlan> uploadPlan;
  };

  explicit EditorRendererAdapter(rhi::BackendType backend) noexcept;
  void bindRendererIdentity(RendererInstanceId renderer,
                            RendererDeviceId device) noexcept;

  [[nodiscard]] bool
  enqueue(const EditorController::ProjectionPublication &publication,
          std::string *error = nullptr,
          SceneApplyError *typedError = nullptr) noexcept;
  [[nodiscard]] FlushResult flush(RendererFacade &renderer) noexcept;

  // A reset is a document/load lifecycle boundary. It may drain the device;
  // ordinary edits never call this path.
  void reset(RendererFacade &renderer) noexcept;
  void clearPending() noexcept;

  [[nodiscard]] bool initialized() const noexcept { return m_initialized; }
  [[nodiscard]] bool hasPending() const noexcept { return !m_pending.empty(); }
  [[nodiscard]] bool requiresAuthoritativeRebuild() const noexcept {
    return m_requiresAuthoritativeRebuild;
  }
  [[nodiscard]] const std::optional<RendererTargetFence> &
  activeFence() const noexcept {
    return m_activeFence;
  }
  [[nodiscard]] const SceneDigest &activeGenerationDigest() const noexcept {
    return m_activeGenerationDigest;
  }

private:
  struct PendingPublication final {
    SceneProjectionResult projection;
    ProjectionRefresh refresh{ProjectionRefresh::none};
    std::optional<SceneCanonicalizationResult> canonical;
  };

  struct MappingState final {
    std::map<StableSceneId, RendererObjectHandle> handles;
    std::array<std::uint32_t, 8> nextHandleIndices{};
  };

  struct MappingBuild final {
    RendererSceneMappingSnapshot mapping;
    MappingState state;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept { return error.empty(); }
  };

  [[nodiscard]] RendererTargetFence
  targetFence(const SceneProjectionResult &projection,
              std::uint64_t mappingEpoch) const noexcept;
  [[nodiscard]] MappingBuild
  buildMapping(const SceneProjectionResult &projection,
               const RendererTargetFence &fence,
               const MappingState &baseState,
               std::uint32_t objectGeneration) const;
  [[nodiscard]] std::optional<SceneUploadPlan>
  buildPlan(const SceneProjectionResult &projection,
            const RendererTargetFence &fence, std::string &error) const;
  [[nodiscard]] SceneCanonicalizationResult makeFullSnapshot(
      const RendererTargetFence &baseFence,
      const SceneDigest &baseDocumentDigest,
      const SceneProjectionResult &target,
      FullRebuildReason reason) const;
  [[nodiscard]] FlushResult bootstrap(RendererFacade &renderer,
                                      PendingPublication pending) noexcept;
  [[nodiscard]] FlushResult apply(RendererFacade &renderer,
                                  PendingPublication pending) noexcept;

  rhi::BackendType m_backend{rhi::BackendType::vulkan};
  RendererInstanceId m_rendererIdentity{};
  RendererDeviceId m_deviceIdentity{};
  bool m_initialized{false};
  bool m_requiresAuthoritativeRebuild{false};
  std::uint32_t m_objectGeneration{1u};
  std::optional<RendererTargetFence> m_activeFence;
  SceneDigest m_activeDocumentDigest{};
  SceneDigest m_activeGenerationDigest{};
  MappingState m_mappingState;
  std::deque<PendingPublication> m_pending;
};

} // namespace demo::editor::ui
