#pragma once

#include "SceneEditorUiModel.h"

#include <cmath>
#include <compare>
#include <cstdint>
#include <string_view>

namespace demo::editor::ui {

enum class EditorUiProbeSemantic : std::uint8_t {
  graphRow = 0,
  sceneAddNode,
  sceneAddPointLight,
  sceneAddDirectionalLight,
  sceneAddSpotLight,
  nodeDuplicate,
  nodeDelete,
  nodeAddChildMenu,
  nodeAddChildNode,
  nodeAddChildPointLight,
  nodeAddChildDirectionalLight,
  nodeAddChildSpotLight,
  nodeRename,
  renameInput,
  renameAccept,
  renameCancel,
  deleteAccept,
  deleteCancel,
  nodeDragSource,
  hierarchyDropTarget,
  inspectorTranslationX,
  inspectorTranslationY,
  inspectorTranslationZ,
  inspectorRotationX,
  inspectorRotationY,
  inspectorRotationZ,
  inspectorScaleX,
  inspectorScaleY,
  inspectorScaleZ,
  inspectorBaseColorSwatch,
  inspectorMetallic,
  inspectorRoughness,
};

enum class EditorUiProbeCoordinateSpace : std::uint8_t {
  imguiScreenLogical = 0,
};

struct EditorUiProbePoint final {
  float x{0.0F};
  float y{0.0F};

  auto operator<=>(const EditorUiProbePoint &) const = default;
};

struct EditorUiProbeFrameBinding final {
  LifecycleState lifecycle{LifecycleState::noDocument};
  std::uint64_t lifecycleRevision{0};
  std::uint64_t documentRevision{0};
  SelectionRevision selectionRevision{0};
  std::uint64_t frameSerial{0};
  std::uint32_t mainViewportId{0};
  EditorUiProbePoint viewportPosition;
  EditorUiProbePoint viewportSize;
  EditorUiProbePoint displaySize;
  EditorUiProbePoint framebufferScale{1.0F, 1.0F};

  auto operator<=>(const EditorUiProbeFrameBinding &) const = default;
};

struct EditorUiProbeItemKey final {
  EditorUiProbeSemantic semantic{EditorUiProbeSemantic::graphRow};
  GraphRowKey row;
  EditorObjectId object;

  auto operator<=>(const EditorUiProbeItemKey &) const = default;
};

struct EditorUiProbeRect final {
  float minimumX{0.0F};
  float minimumY{0.0F};
  float maximumX{0.0F};
  float maximumY{0.0F};

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(minimumX) && std::isfinite(minimumY) &&
           std::isfinite(maximumX) && std::isfinite(maximumY) &&
           maximumX > minimumX && maximumY > minimumY;
  }

  auto operator<=>(const EditorUiProbeRect &) const = default;
};

struct EditorUiProbeItem final {
  EditorUiProbeItemKey key;
  // The label view is valid only for the duration of observeItem(). Consumers
  // that retain a frame must copy it.
  std::string_view label;
  std::uint64_t order{0};
  std::uint32_t itemId{0};
  std::uint32_t windowToken{0};
  std::uint32_t popupToken{0};
  std::uint32_t modalToken{0};
  EditorUiProbeCoordinateSpace rectCoordinateSpace{
      EditorUiProbeCoordinateSpace::imguiScreenLogical};
  EditorUiProbeRect rect;
  bool visible{false};
  bool hovered{false};
  bool active{false};
  bool focused{false};
  bool enabled{false};
  bool dropAccepted{false};

  [[nodiscard]] bool valid() const noexcept {
    return itemId != 0u && rect.valid();
  }
};

// Acceptance consumers own duplicate/missing semantic policy. Frame
// boundaries and validity-bearing item records let them reject ambiguous or
// incomplete OS-input targets without exposing Dear ImGui implementation
// types outside SceneEditorUi.cpp.
class EditorUiProbe {
public:
  virtual ~EditorUiProbe() = default;

  virtual void beginFrame(const EditorUiProbeFrameBinding &binding) = 0;
  virtual void observeItem(const EditorUiProbeItem &item) = 0;
  virtual void observeDispatch(const UiCommand &command,
                               const RuntimeActionResult &result) = 0;
  virtual void endFrame() = 0;
};

} // namespace demo::editor::ui
