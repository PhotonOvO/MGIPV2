#include "EditorUiOsReplayPlan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <utility>

namespace demo::editor::testing::osreplay {
namespace {

void appendJsonString(std::string &output, std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20u) {
        output += "\\u00";
        output.push_back(hex[(character >> 4u) & 0x0fu]);
        output.push_back(hex[character & 0x0fu]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

void appendBool(std::string &output, bool value) {
  output += value ? "true" : "false";
}

void appendHexUInt(std::string &output, std::uint64_t value) {
  static constexpr char hex[] = "0123456789abcdef";
  char buffer[2u * sizeof(value)]{};
  std::size_t size = 0u;
  do {
    buffer[size++] = hex[value & 0x0fu];
    value >>= 4u;
  } while (value != 0u);
  while (size != 0u)
    output.push_back(buffer[--size]);
}

void appendRect(std::string &output, const ScreenRect &rect) {
  output += "{\"coordinateSpace\":\"screen-physical-px\",\"left\":" +
            std::to_string(rect.left) + ",\"top\":" +
            std::to_string(rect.top) + ",\"right\":" +
            std::to_string(rect.right) + ",\"bottom\":" +
            std::to_string(rect.bottom) + "}";
}

void appendProbeRect(std::string &output, const ui::EditorUiProbeRect &rect) {
  output += "{\"coordinateSpace\":\"imgui-screen-logical\","
            "\"minimumX\":" +
            std::to_string(rect.minimumX) + ",\"minimumY\":" +
            std::to_string(rect.minimumY) + ",\"maximumX\":" +
            std::to_string(rect.maximumX) + ",\"maximumY\":" +
            std::to_string(rect.maximumY) + "}";
}

[[nodiscard]] std::string_view objectKindName(EditorObjectKind kind) noexcept {
  switch (kind) {
  case EditorObjectKind::scene:
    return "Scene";
  case EditorObjectKind::node:
    return "Node";
  case EditorObjectKind::mesh:
    return "Mesh";
  case EditorObjectKind::primitive:
    return "Primitive";
  case EditorObjectKind::material:
    return "Material";
  case EditorObjectKind::texture:
    return "Texture";
  case EditorObjectKind::image:
    return "Image";
  case EditorObjectKind::sampler:
    return "Sampler";
  case EditorObjectKind::camera:
    return "Camera";
  case EditorObjectKind::light:
    return "Light";
  case EditorObjectKind::accessor:
    return "Accessor";
  case EditorObjectKind::bufferView:
    return "BufferView";
  case EditorObjectKind::buffer:
    return "Buffer";
  case EditorObjectKind::skin:
    return "Skin";
  case EditorObjectKind::animation:
    return "Animation";
  case EditorObjectKind::invalid:
  case EditorObjectKind::count:
    return "Invalid";
  }
  return "Invalid";
}

void appendObjectId(std::string &output, EditorObjectId id) {
  output += "{\"kind\":";
  appendJsonString(output, objectKindName(id.kind));
  output += ",\"slot\":" + std::to_string(id.slot) +
            ",\"generation\":" + std::to_string(id.generation) +
            ",\"authority\":\"" + std::to_string(id.authority.value) +
            "\"}";
}

void appendGraphRowKey(std::string &output, const ui::GraphRowKey &key) {
  output += "{\"scene\":";
  appendObjectId(output, key.scene.untyped());
  output += ",\"object\":";
  appendObjectId(output, key.object);
  output += ",\"directParent\":";
  if (key.directParent)
    appendObjectId(output, key.directParent->untyped());
  else
    output += "null";
  output += ",\"occurrence\":" + std::to_string(key.occurrence) + "}";
}

[[nodiscard]] bool validToken(std::string_view value) noexcept {
  if (value.empty())
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == ':';
  });
}

[[nodiscard]] bool validText(std::string_view value) noexcept {
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20u;
  });
}

[[nodiscard]] bool lowercaseHex(std::string_view value,
                                std::size_t length) noexcept {
  return value.size() == length &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool graphKeyValid(const ui::GraphRowKey &key) noexcept {
  return key.scene.valid() && key.object.valid() &&
         (!key.directParent || key.directParent->valid()) &&
         key.scene.authority() == key.object.authority &&
         (!key.directParent ||
          key.directParent->authority() == key.scene.authority());
}

[[nodiscard]] bool pointerOffsetValid(std::string_view value) noexcept {
  const auto comma = value.find(',');
  if (comma == std::string_view::npos)
    return false;
  for (const auto part : {value.substr(0u, comma), value.substr(comma + 1u)}) {
    int offset = 0;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), offset);
    if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() ||
        offset < -1000 || offset > 1000)
      return false;
  }
  return true;
}

[[nodiscard]] std::string_view
fixedProbeLabel(TargetSemantic semantic) noexcept {
  switch (semantic) {
  case TargetSemantic::gizmoTranslateY:
  case TargetSemantic::viewportOutside:
    return {};
  case TargetSemantic::sceneAddNode:
    return "Add Node";
  case TargetSemantic::nodeDuplicate:
    return "Duplicate";
  case TargetSemantic::nodeDelete:
  case TargetSemantic::deleteAccept:
    return "Delete";
  case TargetSemantic::nodeAddChildMenu:
    return "Add Child";
  case TargetSemantic::nodeAddChildNode:
    return "Empty Node";
  case TargetSemantic::nodeRename:
    return "Rename";
  case TargetSemantic::renameInput:
    return "##name";
  case TargetSemantic::client:
  case TargetSemantic::graphRow:
  case TargetSemantic::nodeDragSource:
  case TargetSemantic::hierarchyDropTarget:
  case TargetSemantic::viewport:
  case TargetSemantic::gizmoTranslate:
  case TargetSemantic::gizmoRotate:
  case TargetSemantic::gizmoScale:
  case TargetSemantic::inspectorTranslationX:
  case TargetSemantic::inspectorTranslationY:
  case TargetSemantic::inspectorTranslationZ:
  case TargetSemantic::inspectorRotationX:
  case TargetSemantic::inspectorRotationY:
  case TargetSemantic::inspectorRotationZ:
  case TargetSemantic::inspectorScaleX:
  case TargetSemantic::inspectorScaleY:
  case TargetSemantic::inspectorScaleZ:
  case TargetSemantic::toolbarGrid:
  case TargetSemantic::toolbarGizmo:
  case TargetSemantic::toolbarSnap:
    return {};
  }
  return {};
}

[[nodiscard]] bool popupSemantic(TargetSemantic semantic) noexcept {
  return semantic == TargetSemantic::sceneAddNode ||
         semantic == TargetSemantic::nodeDuplicate ||
         semantic == TargetSemantic::nodeDelete ||
         semantic == TargetSemantic::nodeAddChildMenu ||
         semantic == TargetSemantic::nodeAddChildNode ||
         semantic == TargetSemantic::nodeRename;
}

[[nodiscard]] bool modalSemantic(TargetSemantic semantic) noexcept {
  return semantic == TargetSemantic::renameInput ||
         semantic == TargetSemantic::deleteAccept;
}

[[nodiscard]] bool passiveSemantic(TargetSemantic semantic) noexcept {
  switch (semantic) {
  case TargetSemantic::viewport:
  case TargetSemantic::gizmoTranslate:
  case TargetSemantic::gizmoRotate:
  case TargetSemantic::gizmoScale:
  case TargetSemantic::inspectorTranslationX:
  case TargetSemantic::inspectorTranslationY:
  case TargetSemantic::inspectorTranslationZ:
  case TargetSemantic::inspectorRotationX:
  case TargetSemantic::inspectorRotationY:
  case TargetSemantic::inspectorRotationZ:
  case TargetSemantic::inspectorScaleX:
  case TargetSemantic::inspectorScaleY:
  case TargetSemantic::inspectorScaleZ:
  case TargetSemantic::toolbarGrid:
  case TargetSemantic::toolbarGizmo:
  case TargetSemantic::toolbarSnap:
    return true;
  default:
    return false;
  }
}

TargetSpec target(TargetSemantic semantic, std::string role,
                  std::string graphRowRole = {}) {
  return TargetSpec{semantic, std::move(role), std::move(graphRowRole)};
}

InputSpec input(InputKind kind, std::string value = {}) {
  return InputSpec{kind, std::move(value)};
}

PhaseSpec phase(std::string id, ProbePhase probePhase, TargetSpec targetSpec,
                InputSpec inputSpec, bool hovered = false,
                bool focused = false, bool dropAccepted = false,
                CapturePoint capture = CapturePoint::none) {
  return PhaseSpec{0u, std::move(id), probePhase, std::move(targetSpec),
                   std::move(inputSpec), hovered, focused, dropAccepted,
                   capture};
}

StepSpec oneDispatch(StepSpec step, ui::CommandSource source,
                     DispatchKind kind, std::string operation,
                     std::string targetRole,
                     ui::ProjectionRefresh refresh,
                     std::string secondaryRole = {}, std::string text = {});
StepSpec dragDispatches(StepSpec step);

StepSpec clickStep(std::string id, std::string subcase,
                   TargetSemantic semantic, std::string role,
                   std::string graphRole, InputKind click,
                   EffectKind effect = EffectKind::presentationOnly,
                   std::int32_t revisionDelta = 0,
                   HistoryEffect history = HistoryEffect::unchanged,
                   bool orderDigestChanges = false) {
  const TargetSpec clickTarget =
      target(semantic, std::move(role), std::move(graphRole));
  const TargetSpec client = target(TargetSemantic::client, "client");
  StepSpec step;
  step.id = std::move(id);
  step.subcase = std::move(subcase);
  step.effect = effect;
  step.expectedRevisionDelta = revisionDelta;
  step.history = history;
  step.expectHierarchyOrderDigestChange = orderDigestChanges;
  step.phases = {
      phase("target", ProbePhase::target, clickTarget,
            input(InputKind::movePointer), false, false, false,
            CapturePoint::before),
      phase("hover", ProbePhase::hover, clickTarget, input(click), true),
      phase("settled", ProbePhase::settled, client,
            input(InputKind::none), false, false, false,
            CapturePoint::after),
  };
  if (clickTarget.semantic == TargetSemantic::graphRow &&
      click == InputKind::leftClick) {
    step = oneDispatch(std::move(step), ui::CommandSource::sceneBrowser,
                       DispatchKind::selectObject, "select",
                       clickTarget.graphRowRole,
                       ui::ProjectionRefresh::selectionOnly);
  } else if (clickTarget.semantic == TargetSemantic::sceneAddNode ||
             clickTarget.semantic == TargetSemantic::nodeAddChildNode ||
             clickTarget.semantic == TargetSemantic::nodeDuplicate) {
    const std::string operation =
        clickTarget.semantic == TargetSemantic::sceneAddNode
            ? "add-root"
            : clickTarget.semantic == TargetSemantic::nodeAddChildNode
                  ? "add-child"
                  : "duplicate";
    step = oneDispatch(std::move(step), ui::CommandSource::sceneBrowser,
                       DispatchKind::hierarchyAction, operation,
                       clickTarget.graphRowRole,
                       ui::ProjectionRefresh::hierarchy);
  } else if (clickTarget.semantic == TargetSemantic::deleteAccept) {
    step = oneDispatch(std::move(step), ui::CommandSource::dialog,
                       DispatchKind::hierarchyAction, "delete",
                       clickTarget.graphRowRole,
                       effect == EffectKind::documentNoopHistoryEnvelope
                           ? ui::ProjectionRefresh::none
                           : ui::ProjectionRefresh::hierarchy);
  }
  return step;
}

StepSpec contextOpenStep(std::string id, std::string subcase,
                         std::string graphRole,
                         TargetSemantic popupSemantic,
                         std::string popupRole) {
  const TargetSpec graphTarget =
      target(TargetSemantic::graphRow, graphRole, graphRole);
  const TargetSpec popupTarget =
      target(popupSemantic, std::move(popupRole), std::move(graphRole));
  StepSpec step;
  step.id = std::move(id);
  step.subcase = std::move(subcase);
  step.phases = {
      phase("target", ProbePhase::target, graphTarget,
            input(InputKind::movePointer), false, false, false,
            CapturePoint::before),
      phase("hover", ProbePhase::hover, graphTarget,
            input(InputKind::rightDown), true),
      phase("release", ProbePhase::hover, graphTarget,
            input(InputKind::rightUp), true),
      phase("settled", ProbePhase::settled, popupTarget,
            input(InputKind::none), false, false, false,
            CapturePoint::after),
  };
  return step;
}

StepSpec chordStep(std::string id, std::string subcase, std::string chord,
                   EffectKind effect, std::int32_t revisionDelta,
                   HistoryEffect history, bool orderDigestChanges = false) {
  const TargetSpec client = target(TargetSemantic::client, "client");
  StepSpec step;
  step.id = std::move(id);
  step.subcase = std::move(subcase);
  step.effect = effect;
  step.expectedRevisionDelta = revisionDelta;
  step.history = history;
  step.expectHierarchyOrderDigestChange = orderDigestChanges;
  step.phases = {
      phase("focus", ProbePhase::focus, client,
            input(InputKind::keyChord, std::move(chord)), false, true, false,
            CapturePoint::before),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after),
  };
  const std::string operation = step.id.find("undo") != std::string::npos
                                    ? "undo"
                                    : "redo";
  step = oneDispatch(std::move(step), ui::CommandSource::shortcut,
                     DispatchKind::invokeCommand, operation, {},
                     revisionDelta == 0 ? ui::ProjectionRefresh::none
                                        : ui::ProjectionRefresh::properties);
  return step;
}

StepSpec dragStep(std::string id, std::string subcase,
                  std::string sourceRole, TargetSemantic targetSemantic,
                  std::string targetRole, std::string targetGraphRole,
                  EffectKind effect, std::int32_t revisionDelta,
                  HistoryEffect history) {
  const TargetSpec source = target(TargetSemantic::nodeDragSource, sourceRole,
                                   sourceRole);
  const TargetSpec destination = target(targetSemantic, std::move(targetRole),
                                        std::move(targetGraphRole));
  StepSpec step;
  step.id = std::move(id);
  step.subcase = std::move(subcase);
  step.effect = effect;
  step.expectedRevisionDelta = revisionDelta;
  step.history = history;
  step.expectHierarchyOrderDigestChange =
      effect == EffectKind::documentMutation;
  step.phases = {
      phase("source-target", ProbePhase::target, source,
            input(InputKind::movePointer), false, false, false,
            CapturePoint::before),
      phase("source-hover", ProbePhase::hover, source,
            input(InputKind::leftDown), true),
      phase("drag-target", ProbePhase::dragTarget, destination,
            input(InputKind::movePointer)),
      phase("drop-hover", ProbePhase::dropHover, destination,
            input(InputKind::leftUp), true, false, true),
      phase("settled", ProbePhase::settled, destination,
            input(InputKind::none), false, false, false,
            CapturePoint::after),
  };
  step = dragDispatches(std::move(step));
  return step;
}

StepSpec oneDispatch(StepSpec step, ui::CommandSource source,
                     DispatchKind kind, std::string operation,
                     std::string targetRole, ui::ProjectionRefresh refresh,
                     std::string secondaryRole, std::string text) {
  step.expectedDispatches.push_back(
      DispatchExpectation{source, kind, std::move(operation),
                          std::move(targetRole), std::move(secondaryRole),
                          std::move(text), true, refresh});
  return step;
}

StepSpec dragDispatches(StepSpec step) {
  step.expectedDispatches = {
      {ui::CommandSource::sceneBrowser, DispatchKind::selectObject, "select",
       step.phases.front().target.graphRowRole, {}, {}, true,
       ui::ProjectionRefresh::selectionOnly},
      {ui::CommandSource::sceneBrowser, DispatchKind::reparentNode, "reparent",
       step.phases.front().target.graphRowRole,
       step.phases[2u].target.graphRowRole, {}, true,
       step.effect == EffectKind::documentNoopHistoryEnvelope
           ? ui::ProjectionRefresh::none
           : ui::ProjectionRefresh::hierarchy},
  };
  return step;
}

[[nodiscard]] std::string_view
commandSourceName(ui::CommandSource source) noexcept {
  switch (source) {
  case ui::CommandSource::menu:
    return "menu";
  case ui::CommandSource::toolbar:
    return "toolbar";
  case ui::CommandSource::shortcut:
    return "shortcut";
  case ui::CommandSource::sceneBrowser:
    return "scene-browser";
  case ui::CommandSource::inspector:
    return "inspector";
  case ui::CommandSource::dialog:
    return "dialog";
  case ui::CommandSource::settings:
    return "settings";
  }
  return "invalid";
}

[[nodiscard]] std::string_view
projectionRefreshName(ui::ProjectionRefresh refresh) noexcept {
  switch (refresh) {
  case ui::ProjectionRefresh::none:
    return "none";
  case ui::ProjectionRefresh::selectionOnly:
    return "selection-only";
  case ui::ProjectionRefresh::properties:
    return "properties";
  case ui::ProjectionRefresh::hierarchy:
    return "hierarchy";
  case ui::ProjectionRefresh::topology:
    return "topology";
  case ui::ProjectionRefresh::fullScene:
    return "full-scene";
  }
  return "invalid";
}

[[nodiscard]] std::string dispatchTrace(const StepSpec &step) {
  std::string output;
  for (std::size_t index = 0u; index < step.expectedDispatches.size();
       ++index) {
    if (index != 0u)
      output.push_back(',');
    output += commandSourceName(step.expectedDispatches[index].source);
    output.push_back(':');
    output += dispatchKindName(step.expectedDispatches[index].kind);
    output.push_back(':');
    output += step.expectedDispatches[index].operation;
    output.push_back(':');
    output += step.expectedDispatches[index].targetRole;
    output.push_back(':');
    output += step.expectedDispatches[index].secondaryRole;
    output.push_back(':');
    output += step.expectedDispatches[index].text;
    output += step.expectedDispatches[index].accepted ? ":1:" : ":0:";
    output += projectionRefreshName(step.expectedDispatches[index].refresh);
  }
  return output;
}

void appendStep(ReplayPlan &plan, StepSpec step, std::uint32_t &ordinal) {
  step.index = static_cast<std::uint32_t>(plan.steps.size());
  for (PhaseSpec &value : step.phases)
    value.ordinal = ordinal++;
  plan.steps.push_back(std::move(step));
}

} // namespace

std::string_view targetSemanticName(TargetSemantic value) noexcept {
  switch (value) {
  case TargetSemantic::gizmoTranslateY:
    return "gizmo-translate-y";
  case TargetSemantic::viewportOutside:
    return "viewport-outside";
  case TargetSemantic::client:
    return "client";
  case TargetSemantic::graphRow:
    return "graph-row";
  case TargetSemantic::sceneAddNode:
    return "scene-add-node";
  case TargetSemantic::nodeDuplicate:
    return "node-duplicate";
  case TargetSemantic::nodeDelete:
    return "node-delete";
  case TargetSemantic::nodeAddChildMenu:
    return "node-add-child-menu";
  case TargetSemantic::nodeAddChildNode:
    return "node-add-child-node";
  case TargetSemantic::nodeRename:
    return "node-rename";
  case TargetSemantic::renameInput:
    return "rename-input";
  case TargetSemantic::deleteAccept:
    return "delete-accept";
  case TargetSemantic::nodeDragSource:
    return "node-drag-source";
  case TargetSemantic::hierarchyDropTarget:
    return "hierarchy-drop-target";
  case TargetSemantic::viewport:
    return "viewport";
  case TargetSemantic::gizmoTranslate:
    return "gizmo-translate";
  case TargetSemantic::gizmoRotate:
    return "gizmo-rotate";
  case TargetSemantic::gizmoScale:
    return "gizmo-scale";
  case TargetSemantic::inspectorTranslationX:
    return "inspector-translation-x";
  case TargetSemantic::inspectorTranslationY:
    return "inspector-translation-y";
  case TargetSemantic::inspectorTranslationZ:
    return "inspector-translation-z";
  case TargetSemantic::inspectorRotationX:
    return "inspector-rotation-x";
  case TargetSemantic::inspectorRotationY:
    return "inspector-rotation-y";
  case TargetSemantic::inspectorRotationZ:
    return "inspector-rotation-z";
  case TargetSemantic::inspectorScaleX:
    return "inspector-scale-x";
  case TargetSemantic::inspectorScaleY:
    return "inspector-scale-y";
  case TargetSemantic::inspectorScaleZ:
    return "inspector-scale-z";
  case TargetSemantic::toolbarGrid:
    return "toolbar-grid";
  case TargetSemantic::toolbarGizmo:
    return "toolbar-gizmo";
  case TargetSemantic::toolbarSnap:
    return "toolbar-snap";
  }
  return "invalid";
}

std::optional<ui::EditorUiProbeSemantic>
editorProbeSemantic(TargetSemantic value) noexcept {
  switch (value) {
  case TargetSemantic::gizmoTranslateY:
  case TargetSemantic::viewportOutside:
    return std::nullopt;
  case TargetSemantic::client:
    return std::nullopt;
  case TargetSemantic::graphRow:
    return ui::EditorUiProbeSemantic::graphRow;
  case TargetSemantic::sceneAddNode:
    return ui::EditorUiProbeSemantic::sceneAddNode;
  case TargetSemantic::nodeDuplicate:
    return ui::EditorUiProbeSemantic::nodeDuplicate;
  case TargetSemantic::nodeDelete:
    return ui::EditorUiProbeSemantic::nodeDelete;
  case TargetSemantic::nodeAddChildMenu:
    return ui::EditorUiProbeSemantic::nodeAddChildMenu;
  case TargetSemantic::nodeAddChildNode:
    return ui::EditorUiProbeSemantic::nodeAddChildNode;
  case TargetSemantic::nodeRename:
    return ui::EditorUiProbeSemantic::nodeRename;
  case TargetSemantic::renameInput:
    return ui::EditorUiProbeSemantic::renameInput;
  case TargetSemantic::deleteAccept:
    return ui::EditorUiProbeSemantic::deleteAccept;
  case TargetSemantic::nodeDragSource:
    return ui::EditorUiProbeSemantic::nodeDragSource;
  case TargetSemantic::hierarchyDropTarget:
    return ui::EditorUiProbeSemantic::hierarchyDropTarget;
  case TargetSemantic::viewport:
  case TargetSemantic::gizmoTranslate:
  case TargetSemantic::gizmoRotate:
  case TargetSemantic::gizmoScale:
  case TargetSemantic::toolbarGrid:
  case TargetSemantic::toolbarGizmo:
  case TargetSemantic::toolbarSnap:
    return std::nullopt;
  case TargetSemantic::inspectorTranslationX:
    return ui::EditorUiProbeSemantic::inspectorTranslationX;
  case TargetSemantic::inspectorTranslationY:
    return ui::EditorUiProbeSemantic::inspectorTranslationY;
  case TargetSemantic::inspectorTranslationZ:
    return ui::EditorUiProbeSemantic::inspectorTranslationZ;
  case TargetSemantic::inspectorRotationX:
    return ui::EditorUiProbeSemantic::inspectorRotationX;
  case TargetSemantic::inspectorRotationY:
    return ui::EditorUiProbeSemantic::inspectorRotationY;
  case TargetSemantic::inspectorRotationZ:
    return ui::EditorUiProbeSemantic::inspectorRotationZ;
  case TargetSemantic::inspectorScaleX:
    return ui::EditorUiProbeSemantic::inspectorScaleX;
  case TargetSemantic::inspectorScaleY:
    return ui::EditorUiProbeSemantic::inspectorScaleY;
  case TargetSemantic::inspectorScaleZ:
    return ui::EditorUiProbeSemantic::inspectorScaleZ;
  }
  return std::nullopt;
}

std::string_view probePhaseName(ProbePhase value) noexcept {
  switch (value) {
  case ProbePhase::target:
    return "target";
  case ProbePhase::hover:
    return "hover";
  case ProbePhase::focus:
    return "focus";
  case ProbePhase::dragTarget:
    return "drag-target";
  case ProbePhase::dropHover:
    return "drop-hover";
  case ProbePhase::settled:
    return "settled";
  case ProbePhase::boundary:
    return "boundary";
  }
  return "invalid";
}

std::string_view inputKindName(InputKind value) noexcept {
  switch (value) {
  case InputKind::none:
    return "none";
  case InputKind::movePointer:
    return "move-pointer";
  case InputKind::leftClick:
    return "left-click";
  case InputKind::rightClick:
    return "right-click";
  case InputKind::rightDown:
    return "right-down";
  case InputKind::rightUp:
    return "right-up";
  case InputKind::leftDown:
    return "left-down";
  case InputKind::leftUp:
    return "left-up";
  case InputKind::keyChord:
    return "key-chord";
  case InputKind::replaceText:
    return "replace-text";
  case InputKind::pressKey:
    return "press-key";
  case InputKind::keyDown:
    return "key-down";
  case InputKind::keyUp:
    return "key-up";
  case InputKind::escapeReleaseBatch:
    return "escape-release-batch";
  case InputKind::focusLoss:
    return "focus-loss";
  case InputKind::focusRestore:
    return "focus-restore";
  case InputKind::leftUpFocusLoss:
    return "left-up-focus-loss";
  }
  return "invalid";
}

std::string_view capturePointName(CapturePoint value) noexcept {
  switch (value) {
  case CapturePoint::none:
    return "none";
  case CapturePoint::before:
    return "before";
  case CapturePoint::after:
    return "after";
  }
  return "invalid";
}

std::string_view effectKindName(EffectKind value) noexcept {
  switch (value) {
  case EffectKind::presentationOnly:
    return "presentation-only";
  case EffectKind::selectionOnly:
    return "selection-only";
  case EffectKind::documentMutation:
    return "document-mutation";
  case EffectKind::historyMove:
    return "history-move";
  case EffectKind::documentNoopHistoryEnvelope:
    return "document-noop-history-envelope";
  case EffectKind::authorityReset:
    return "authority-reset";
  }
  return "invalid";
}

std::string_view historyEffectName(HistoryEffect value) noexcept {
  switch (value) {
  case HistoryEffect::unchanged:
    return "unchanged";
  case HistoryEffect::pushUndoClearRedo:
    return "push-undo-clear-redo";
  case HistoryEffect::moveUndoToRedo:
    return "move-undo-to-redo";
  case HistoryEffect::moveRedoToUndo:
    return "move-redo-to-undo";
  case HistoryEffect::reset:
    return "reset";
  }
  return "invalid";
}

std::string_view dispatchKindName(DispatchKind value) noexcept {
  switch (value) {
  case DispatchKind::invokeCommand:
    return "InvokeCommand";
  case DispatchKind::selectObject:
    return "SelectObject";
  case DispatchKind::hierarchyAction:
    return "HierarchyAction";
  case DispatchKind::renameObject:
    return "RenameObject";
  case DispatchKind::reparentNode:
    return "ReparentNode";
  case DispatchKind::propertyEdit:
    return "PropertyEdit";
  }
  return "Invalid";
}

std::size_t ReplayPlan::phaseCount() const noexcept {
  std::size_t count = 0u;
  for (const StepSpec &step : steps)
    count += step.phases.size();
  return count;
}

PlanValidation ReplayPlan::validate() const {
  const auto fail = [](std::string message) {
    return PlanValidation{false, std::move(message)};
  };
  if (schemaVersion != kSchemaVersion || protocol != kProtocol ||
      (caseId != kU02Case && caseId != kU03Case &&
       caseId != kU03InspectorCase && caseId != kU03GizmoCase &&
       !isEscapeGizmoCase(caseId) && !isFocusGizmoCase(caseId)) ||
      graphRoles.empty() ||
      steps.empty())
    return fail("plan envelope is invalid");

  std::set<std::string> graphRoleNames;
  for (const GraphRoleSpec &role : graphRoles) {
    if (!validToken(role.role) || !validToken(role.subcase) ||
        !validToken(role.introducedByStep) ||
        !validText(role.labelContains) ||
        (role.objectKind != EditorObjectKind::scene &&
         role.objectKind != EditorObjectKind::node) ||
        !graphRoleNames.insert(role.role).second)
      return fail("graph role identity/constraint is invalid");
  }
  for (const GraphRoleSpec &role : graphRoles) {
    for (const std::string *reference :
         {&role.sameObjectAsRole, &role.directParentRole,
          &role.distinctFromRole}) {
      if (!reference->empty() && !graphRoleNames.contains(*reference))
        return fail("graph role references an unknown role");
    }
  }

  std::set<std::string> stepIds;
  std::uint32_t expectedOrdinal = 0u;
  for (std::size_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
    const StepSpec &step = steps[stepIndex];
    if (step.index != stepIndex || !validToken(step.id) ||
        !validToken(step.subcase) || !stepIds.insert(step.id).second ||
        step.phases.empty())
      return fail("step identity or cardinality is invalid");
    if (step.effect == EffectKind::documentMutation &&
        step.expectedRevisionDelta != 1)
      return fail("document mutation must advance revision exactly once");
    if (step.effect == EffectKind::documentNoopHistoryEnvelope &&
        (step.expectedRevisionDelta != 0 ||
         step.history != HistoryEffect::pushUndoClearRedo))
      return fail("document no-op must preserve revision and push history");
    if ((step.effect == EffectKind::authorityReset) !=
        step.relaunchProcessBefore)
      return fail("authority reset must be an explicit process relaunch");
    if (step.relaunchProcessBefore &&
        (stepIndex == 0u ||
         steps[stepIndex - 1u].id != "main-process-boundary"))
      return fail("process relaunch is missing its preceding boundary");
    const std::size_t dispatchLimit = caseId == kU02Case ? 2u : 4u;
    if (step.expectedDispatches.size() > dispatchLimit)
      return fail("step dispatch cardinality exceeds the OS replay contract");
    if (caseId == kU02Case &&
        ((step.effect == EffectKind::presentationOnly ||
          step.effect == EffectKind::authorityReset) !=
         step.expectedDispatches.empty()))
      return fail("step effect and expected dispatch cardinality disagree");
    for (const DispatchExpectation &dispatch : step.expectedDispatches) {
      if (commandSourceName(dispatch.source) == "invalid" ||
          dispatchKindName(dispatch.kind) == "Invalid" ||
          !validToken(dispatch.operation) ||
          (!dispatch.targetRole.empty() &&
           !validToken(dispatch.targetRole)) ||
          (!dispatch.secondaryRole.empty() &&
           !validToken(dispatch.secondaryRole)) ||
          !validText(dispatch.text) || !dispatch.accepted ||
          projectionRefreshName(dispatch.refresh) == "invalid")
        return fail("step contains an invalid expected dispatch");
      if (dispatch.kind == DispatchKind::propertyEdit &&
          (dispatch.propertyValue.empty() ||
           (dispatch.propertyPhase != "begin" &&
           dispatch.propertyPhase != "preview" &&
           dispatch.propertyPhase != "commit")))
        return fail("PropertyEdit dispatch has an invalid phase");
      for (const std::string *roleName :
           {&dispatch.targetRole, &dispatch.secondaryRole}) {
        if (roleName->empty())
          continue;
        const auto role = std::find_if(
            graphRoles.begin(), graphRoles.end(), [&](const GraphRoleSpec &item) {
              return item.role == *roleName;
            });
        if (role == graphRoles.end() || role->subcase != step.subcase)
          return fail("expected dispatch references a foreign graph role");
      }
    }

    std::set<std::string> phaseIds;
    std::uint32_t previousDispatchCount = 0u;
    for (const PhaseSpec &value : step.phases) {
      if (value.gizmo && ((caseId != kU03GizmoCase &&
                           !isEscapeGizmoCase(caseId) &&
                           !isFocusGizmoCase(caseId)) ||
                          value.gizmo->phase > 3u ||
          value.gizmo->commitCount > value.gizmo->beginCount ||
          value.gizmo->detachCount > value.gizmo->beginCount ||
          value.gizmo->latchCount > value.gizmo->beginCount ||
          (value.gizmo->noSelection && value.gizmo->phase != 0u) ||
          (value.gizmo->captureHeld != (value.gizmo->phase == 2u))))
        return fail("invalid gizmo phase expectation");
      if (caseId != kU02Case &&
          (!value.expectedDispatchCount ||
           *value.expectedDispatchCount < previousDispatchCount ||
           *value.expectedDispatchCount > step.expectedDispatches.size()))
        return fail("phase has an invalid expected dispatch prefix");
      previousDispatchCount = value.expectedDispatchCount.value_or(0u);
      if (value.ordinal != expectedOrdinal++ || !validToken(value.id) ||
          !phaseIds.insert(value.id).second ||
          !validToken(value.target.role) ||
          (!value.target.graphRowRole.empty() &&
           !validToken(value.target.graphRowRole)))
        return fail("phase identity or target role is invalid");
      if (value.target.semantic == TargetSemantic::client &&
          value.target.requiresGraphRowKey())
        return fail("client target cannot require GraphRowKey");
      if (value.target.semantic != TargetSemantic::client &&
          !passiveSemantic(value.target.semantic) &&
          !value.target.requiresGraphRowKey())
        return fail("UI target must be anchored by GraphRowKey role");
      if (value.target.requiresGraphRowKey()) {
        const auto role = std::find_if(
            graphRoles.begin(), graphRoles.end(), [&](const GraphRoleSpec &item) {
              return item.role == value.target.graphRowRole;
            });
        if (role == graphRoles.end() || role->subcase != step.subcase)
          return fail("phase GraphRowKey role is absent from its subcase");
      }
      if (value.requireHovered && value.phase != ProbePhase::hover &&
          value.phase != ProbePhase::dropHover &&
          value.phase != ProbePhase::dragTarget &&
          value.phase != ProbePhase::focus)
        return fail("hover confirmation is attached to the wrong phase");
      if (value.requireFocused && value.phase != ProbePhase::focus)
        return fail("focus confirmation is attached to the wrong phase");
      if (value.requireDropAccepted && value.phase != ProbePhase::dropHover)
        return fail("drop acceptance is attached to the wrong phase");
      if ((value.phase == ProbePhase::settled ||
           value.phase == ProbePhase::boundary) &&
          value.input.kind != InputKind::none)
        return fail("settled/boundary phase cannot inject input");
      const bool pointerOffset = value.input.kind == InputKind::movePointer &&
                                 !value.input.value.empty();
      if (pointerOffset &&
          (caseId == kU02Case || !pointerOffsetValid(value.input.value)))
        return fail("invalid physical pointer offset");
      if (!pointerOffset && (value.input.kind == InputKind::keyChord ||
           value.input.kind == InputKind::replaceText ||
           value.input.kind == InputKind::pressKey ||
           value.input.kind == InputKind::keyDown ||
           value.input.kind == InputKind::keyUp ||
           value.input.kind == InputKind::escapeReleaseBatch ||
           value.input.kind == InputKind::focusLoss ||
           value.input.kind == InputKind::focusRestore ||
           value.input.kind == InputKind::leftUpFocusLoss) !=
              !value.input.value.empty())
        return fail("input value presence does not match input kind");
      if (value.input.kind == InputKind::escapeReleaseBatch &&
          (!isSameFrameGizmoCase(caseId) || value.input.value !=
              (caseId == kU03EscapeReleaseCase ? "escape-first" : "release-first")))
        return fail("terminal batch order does not match its scoped case");
      if (value.input.kind == InputKind::focusLoss &&
          (!isFocusGizmoCase(caseId) || value.input.value != "sink"))
        return fail("focus-loss input is outside its scoped case");
      if (value.input.kind == InputKind::focusRestore &&
          (!isFocusGizmoCase(caseId) || value.input.value != "editor"))
        return fail("focus-restore input is outside its scoped case");
      if (value.input.kind == InputKind::leftUpFocusLoss &&
          (!isFocusGizmoCase(caseId) ||
           value.input.value != "release-then-focus-loss"))
        return fail("combined focus-loss input is outside its scoped case");
      if (isFocusGizmoCase(caseId) && value.gizmo) {
        const GizmoExpectation &gizmo = *value.gizmo;
        // Focus partitions never clear selection or detach.  Their terminal
        // counters are mutually exclusive: a delivered release commits in
        // the preceding frame, while a focus loss latches and any later
        // physical ups are suppression-only observations.
        if (gizmo.noSelection || gizmo.detachCount != 0u ||
            gizmo.deliveredReleaseCount > 1u ||
            gizmo.escapePressCount != 0u || gizmo.focusLossCount > 1u ||
            gizmo.suppressedReleaseCount > 2u || gizmo.commitCount > 1u ||
            gizmo.latchCount > 1u ||
            (gizmo.deliveredReleaseCount != 0u &&
             (gizmo.commitCount == 0u || gizmo.latchCount != 0u ||
              gizmo.focusLossCount != 0u || gizmo.suppressedReleaseCount != 0u)) ||
            (gizmo.commitCount != 0u && gizmo.deliveredReleaseCount == 0u) ||
            (gizmo.focusLossCount != 0u && gizmo.latchCount != 1u) ||
            (gizmo.latchCount != 0u &&
             (gizmo.commitCount != 0u || gizmo.deliveredReleaseCount != 0u)) ||
            (gizmo.suppressedReleaseCount != 0u &&
             (gizmo.focusLossCount == 0u || gizmo.latchCount == 0u)) ||
            (caseId == kU03FocusDeliveredReleaseCase &&
             (gizmo.focusLossCount != 0u ||
              gizmo.suppressedReleaseCount != 0u || gizmo.latchCount != 0u)) ||
            (caseId != kU03FocusDeliveredReleaseCase &&
             (gizmo.deliveredReleaseCount != 0u || gizmo.commitCount != 0u)))
          return fail("focus-loss terminal counter expectation is invalid");
      }
    }
    if (caseId != kU02Case &&
        (step.phases.front().expectedDispatchCount != 0u ||
         step.phases.back().expectedDispatchCount != step.expectedDispatches.size()))
      return fail("phase boundaries lack complete dispatch coverage");
    if (step.phases.front().capture != CapturePoint::before ||
        step.phases.back().capture != CapturePoint::after)
      return fail("every step must publish pre/post PNG boundaries");
  }
  for (const GraphRoleSpec &role : graphRoles) {
    const auto introduced = std::find_if(
        steps.begin(), steps.end(), [&](const StepSpec &step) {
          return step.id == role.introducedByStep;
        });
    if (introduced == steps.end() || introduced->subcase != role.subcase)
      return fail("graph role introduction step is invalid");
    for (const std::string *reference :
         {&role.sameObjectAsRole, &role.directParentRole,
          &role.distinctFromRole}) {
      if (reference->empty())
        continue;
      const auto found = std::find_if(
          graphRoles.begin(), graphRoles.end(), [&](const GraphRoleSpec &item) {
            return item.role == *reference;
          });
      if (found == graphRoles.end() || found->subcase != role.subcase)
        return fail("graph role crosses a process/subcase boundary");
    }
  }
  return PlanValidation{true, {}};
}

std::string ReplayPlan::contractTrace() const {
  std::string output = "schema=" + std::to_string(schemaVersion) +
                       "\nprotocol=" + protocol + "\ncase=" + caseId + "\n";
  for (const GraphRoleSpec &role : graphRoles) {
    output += "role\t" + role.role + "\t" + role.subcase + "\t" +
              std::string(objectKindName(role.objectKind)) + "\t" +
              role.labelContains + "\t" + role.introducedByStep + "\t" +
              role.sameObjectAsRole + "\t" + role.directParentRole + "\t" +
              role.distinctFromRole + "\t" +
              (role.requireRoot ? "1" : "0") + "\n";
  }
  for (const StepSpec &step : steps) {
    for (const PhaseSpec &value : step.phases) {
      output += std::to_string(step.index) + "\t" + step.id + "\t" +
                step.subcase + "\t" + std::string(effectKindName(step.effect)) +
                "\t" + std::to_string(step.expectedRevisionDelta) + "\t" +
                std::string(historyEffectName(step.history)) + "\t" +
                dispatchTrace(step) + "\t" +
                (step.expectHierarchyOrderDigestChange ? "1" : "0") + "\t" +
                (step.relaunchProcessBefore ? "1" : "0") + "\t" +
                std::to_string(value.ordinal) + "\t" + value.id + "\t" +
                std::string(probePhaseName(value.phase)) + "\t" +
                std::string(targetSemanticName(value.target.semantic)) + "\t" +
                value.target.role + "\t" + value.target.graphRowRole + "\t" +
                std::string(inputKindName(value.input.kind)) + "\t" +
                value.input.value + "\t" +
                (value.requireHovered ? "1" : "0") + "\t" +
                (value.requireFocused ? "1" : "0") + "\t" +
                (value.requireDropAccepted ? "1" : "0") + "\t" +
                std::string(capturePointName(value.capture));
      if (caseId != kU02Case) {
        output.push_back('\t');
        for (const DispatchExpectation &dispatch : step.expectedDispatches)
          output += dispatch.propertyPhase + "=" + dispatch.propertyValue + ";";
        output.push_back('\t');
        if (value.expectedDispatchCount)
          output += std::to_string(*value.expectedDispatchCount);
      }
      if (caseId == kU03GizmoCase || isEscapeGizmoCase(caseId) ||
          isFocusGizmoCase(caseId)) {
        output.push_back('\t');
        if (value.gizmo) {
          const auto &g = *value.gizmo;
          output += std::to_string(g.phase) + ":" + (g.captureHeld ? "1" : "0") +
                    ":" + std::to_string(g.beginCount) + ":" +
                    std::to_string(g.previewCount) + ":" +
                    std::to_string(g.commitCount) + ":";
          if (g.pointerOutside) output += *g.pointerOutside ? "1" : "0";
        }
      }
      if (isEscapeGizmoCase(caseId)) {
        output.push_back('\t');
        if (value.gizmo) {
          output += std::to_string(value.gizmo->detachCount) + ":" +
                    std::to_string(value.gizmo->latchCount) + ":" +
                    (value.gizmo->noSelection ? "1" : "0");
        }
      }
      if (isFocusGizmoCase(caseId)) {
        output.push_back('\t');
        if (value.gizmo) {
          output += std::to_string(value.gizmo->deliveredReleaseCount) + ":" +
                    std::to_string(value.gizmo->escapePressCount) + ":" +
                    std::to_string(value.gizmo->focusLossCount) + ":" +
                    std::to_string(value.gizmo->suppressedReleaseCount);
        }
      }
      output.push_back('\n');
    }
  }
  return output;
}

std::string ReplayPlan::toJson() const {
  std::string output = "{\"documentKind\":\"mgif.editor-ui-os-replay-plan\","
                       "\"schemaVersion\":" +
                       std::to_string(schemaVersion) + ",\"protocol\":";
  appendJsonString(output, protocol);
  output += ",\"case\":";
  appendJsonString(output, caseId);
  output += ",\"description\":";
  appendJsonString(output, description);
  output += ",\"dispatchMode\":\"win32-send-input\","
            "\"postMessage\":false,\"mouseEvent\":false,\"graphRoles\":[";
  for (std::size_t roleIndex = 0u; roleIndex < graphRoles.size(); ++roleIndex) {
    if (roleIndex != 0u)
      output.push_back(',');
    const GraphRoleSpec &role = graphRoles[roleIndex];
    output += "{\"role\":";
    appendJsonString(output, role.role);
    output += ",\"subcase\":";
    appendJsonString(output, role.subcase);
    output += ",\"objectKind\":";
    appendJsonString(output, objectKindName(role.objectKind));
    output += ",\"labelContains\":";
    appendJsonString(output, role.labelContains);
    output += ",\"introducedByStep\":";
    appendJsonString(output, role.introducedByStep);
    output += ",\"sameObjectAsRole\":";
    appendJsonString(output, role.sameObjectAsRole);
    output += ",\"directParentRole\":";
    appendJsonString(output, role.directParentRole);
    output += ",\"distinctFromRole\":";
    appendJsonString(output, role.distinctFromRole);
    output += ",\"requireRoot\":";
    appendBool(output, role.requireRoot);
    output += "}";
  }
  output += "],\"steps\":[";
  for (std::size_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
    if (stepIndex != 0u)
      output.push_back(',');
    const StepSpec &step = steps[stepIndex];
    output += "{\"index\":" + std::to_string(step.index) + ",\"id\":";
    appendJsonString(output, step.id);
    output += ",\"subcase\":";
    appendJsonString(output, step.subcase);
    output += ",\"effect\":";
    appendJsonString(output, effectKindName(step.effect));
    output += ",\"expectedRevisionDelta\":" +
              std::to_string(step.expectedRevisionDelta) + ",\"history\":";
    appendJsonString(output, historyEffectName(step.history));
    output += ",\"expectedDispatches\":[";
    for (std::size_t dispatchIndex = 0u;
         dispatchIndex < step.expectedDispatches.size(); ++dispatchIndex) {
      if (dispatchIndex != 0u)
        output.push_back(',');
      output += "{\"source\":";
      appendJsonString(output,
                       commandSourceName(step.expectedDispatches[dispatchIndex]
                                             .source));
      output += ",\"payloadKind\":";
      appendJsonString(output,
                       dispatchKindName(step.expectedDispatches[dispatchIndex]
                                            .kind));
      output += ",\"operation\":";
      appendJsonString(output,
                       step.expectedDispatches[dispatchIndex].operation);
      output += ",\"targetRole\":";
      appendJsonString(output,
                       step.expectedDispatches[dispatchIndex].targetRole);
      output += ",\"secondaryRole\":";
      appendJsonString(output,
                       step.expectedDispatches[dispatchIndex].secondaryRole);
      output += ",\"text\":";
      appendJsonString(output, step.expectedDispatches[dispatchIndex].text);
      output += ",\"accepted\":";
      appendBool(output, step.expectedDispatches[dispatchIndex].accepted);
      output += ",\"refresh\":";
      appendJsonString(output,
                       projectionRefreshName(
                           step.expectedDispatches[dispatchIndex].refresh));
      output += ",\"propertyPhase\":";
      appendJsonString(output,
                       step.expectedDispatches[dispatchIndex].propertyPhase);
      output += ",\"propertyValue\":";
      appendJsonString(output,
                       step.expectedDispatches[dispatchIndex].propertyValue);
      output += "}";
    }
    output += "]";
    output += ",\"expectHierarchyOrderDigestChange\":";
    appendBool(output, step.expectHierarchyOrderDigestChange);
    output += ",\"relaunchProcessBefore\":";
    appendBool(output, step.relaunchProcessBefore);
    output += ",\"phases\":[";
    for (std::size_t phaseIndex = 0; phaseIndex < step.phases.size();
         ++phaseIndex) {
      if (phaseIndex != 0u)
        output.push_back(',');
      const PhaseSpec &value = step.phases[phaseIndex];
      output += "{\"ordinal\":" + std::to_string(value.ordinal) +
                ",\"id\":";
      appendJsonString(output, value.id);
      output += ",\"phase\":";
      appendJsonString(output, probePhaseName(value.phase));
      output += ",\"target\":{\"semantic\":";
      appendJsonString(output, targetSemanticName(value.target.semantic));
      output += ",\"role\":";
      appendJsonString(output, value.target.role);
      output += ",\"graphRowRole\":";
      if (value.target.graphRowRole.empty())
        output += "null";
      else
        appendJsonString(output, value.target.graphRowRole);
      output += "},\"input\":{\"kind\":";
      appendJsonString(output, inputKindName(value.input.kind));
      output += ",\"value\":";
      if (value.input.value.empty())
        output += "null";
      else
        appendJsonString(output, value.input.value);
      output += "},\"requireHovered\":";
      appendBool(output, value.requireHovered);
      output += ",\"requireFocused\":";
      appendBool(output, value.requireFocused);
      output += ",\"requireDropAccepted\":";
      appendBool(output, value.requireDropAccepted);
      output += ",\"capture\":";
      appendJsonString(output, capturePointName(value.capture));
      if (value.gizmo) {
        const auto &g = *value.gizmo;
        output += ",\"gizmo\":{\"phase\":" + std::to_string(g.phase) +
                  ",\"captureHeld\":" + (g.captureHeld ? "true" : "false") +
                  ",\"beginCount\":" + std::to_string(g.beginCount) +
                  ",\"previewCount\":" + std::to_string(g.previewCount) +
                  ",\"commitCount\":" + std::to_string(g.commitCount) +
                  ",\"pointerOutside\":";
        output += g.pointerOutside ? (*g.pointerOutside ? "true" : "false") : "null";
        if (isEscapeGizmoCase(caseId)) {
          output += ",\"detachCount\":" + std::to_string(g.detachCount) +
                    ",\"latchCount\":" + std::to_string(g.latchCount) +
                    ",\"noSelection\":" + (g.noSelection ? "true" : "false");
        }
        if (isFocusGizmoCase(caseId)) {
          output += ",\"deliveredReleaseCount\":" +
                    std::to_string(g.deliveredReleaseCount) +
                    ",\"escapePressCount\":" +
                    std::to_string(g.escapePressCount) +
                    ",\"focusLossCount\":" +
                    std::to_string(g.focusLossCount) +
                    ",\"suppressedReleaseCount\":" +
                    std::to_string(g.suppressedReleaseCount);
        }
        output += "}";
      }
      if (caseId != kU02Case) {
        output += ",\"expectedDispatchCount\":";
        output += value.expectedDispatchCount
                      ? std::to_string(*value.expectedDispatchCount) : "null";
      }
      output += "}";
    }
    output += "]}";
  }
  output += "]}";
  return output;
}

PlanValidation ReadyProbe::validateAgainst(const ReplayPlan &plan) const {
  const auto fail = [](std::string message) {
    return PlanValidation{false, std::move(message)};
  };
  const PlanValidation planValidation = plan.validate();
  if (!planValidation.valid)
    return fail("ready probe references an invalid plan");
  if (schemaVersion != kSchemaVersion || protocol != kProtocol ||
      caseId != plan.caseId || !lowercaseHex(runNonce, 32u) ||
      !lowercaseHex(planSha256, 64u) ||
      !lowercaseHex(executableSha256, 64u) ||
      !lowercaseHex(sourceSha256, 64u))
    return fail("ready probe envelope or identity is invalid");
  if (step >= plan.steps.size())
    return fail("ready probe step is outside the plan");
  const StepSpec &expectedStep = plan.steps[step];
  if (stepId != expectedStep.id)
    return fail("ready probe step id does not match the plan");
  const auto found = std::find_if(
      expectedStep.phases.begin(), expectedStep.phases.end(),
      [this](const PhaseSpec &value) { return value.id == phaseId; });
  if (found == expectedStep.phases.end())
    return fail("ready probe phase does not match the plan");
  if (client.hwnd == 0u || client.pid == 0u || client.width == 0u ||
      client.height == 0u || client.dpi == 0u || !client.screenRect.valid())
    return fail("ready probe client identity is invalid");
  if (!ui::isKnownLifecycleState(frame.lifecycle) ||
      frame.documentRevision != authority.documentRevision ||
      frame.selectionRevision != authority.selectionRevision ||
      frame.frameSerial == 0u || frame.mainViewportId == 0u ||
      !(frame.viewportSize.x > 0.0F) || !(frame.viewportSize.y > 0.0F) ||
      !(frame.displaySize.x > 0.0F) || !(frame.displaySize.y > 0.0F) ||
      !(frame.framebufferScale.x > 0.0F) ||
      !(frame.framebufferScale.y > 0.0F))
    return fail("ready probe frame binding is invalid");
  if (authority.documentAuthority == 0u ||
      target.revision != authority.documentRevision)
    return fail("ready probe authority/revision binding is invalid");
  if (!lowercaseHex(authority.documentDigest, 64u) ||
      !lowercaseHex(authority.projectionDigest, 64u) ||
      !lowercaseHex(authority.runtimeContentDigest, 64u) ||
      !lowercaseHex(authority.hierarchyOrderDigest, 64u) ||
      authority.projectionDigest != authority.runtimeContentDigest ||
      authority.selectionKind.empty() ||
      !validText(authority.selectionKind) ||
      !validText(authority.undoDescription) ||
      !validText(authority.redoDescription))
    return fail("ready probe digest/selection/history evidence is invalid");
  if ((authority.selectedGraphRowKey &&
       !graphKeyValid(*authority.selectedGraphRowKey)) ||
      (authority.selectedGraphRowKey &&
       authority.selectedGraphRowKey->scene.authority().value !=
           authority.documentAuthority) ||
      (authority.selectedObject && !authority.selectedObject->valid()) ||
      (authority.selectedObject &&
       authority.selectedObject->authority.value !=
           authority.documentAuthority) ||
      ((authority.selectionKind == "None") !=
       !authority.selectedObject.has_value()))
    return fail("ready probe selected identity is invalid");
  if (target.semantic != found->target.semantic ||
      target.role != found->target.role ||
      target.graphRowRole != found->target.graphRowRole ||
      target.matchCount != 1u || !target.visible || !target.enabled ||
      !target.rect.valid())
    return fail("ready probe target is not unique, visible, and enabled");
  if (target.semantic == TargetSemantic::client) {
    if (target.itemId != 0u || target.order != 0u ||
        target.windowToken != 0u || target.popupToken != 0u ||
        target.modalToken != 0u)
      return fail("synthetic client target contains an ImGui item identity");
  } else if (target.semantic == TargetSemantic::gizmoTranslateY ||
             target.semantic == TargetSemantic::viewportOutside) {
    if ((caseId != kU03GizmoCase && !isEscapeGizmoCase(caseId) &&
         !isFocusGizmoCase(caseId)) || !gizmo || !found->gizmo ||
        target.observationSource != "editor-gizmo-adapter-v1" ||
        target.itemId != 0u || target.order != 0u || target.windowToken != 0u ||
        target.popupToken != 0u || target.modalToken != 0u ||
        !target.probeRect.valid() || !target.graphRowKey ||
        !gizmo->node || target.graphRowKey->object != *gizmo->node ||
        target.probeRect != (target.semantic == TargetSemantic::gizmoTranslateY
                                ? gizmo->componentRect : gizmo->outsideRect))
      return fail("native target lost its observed geometry/identity");
  } else if (editorProbeSemantic(target.semantic).has_value()) {
    const std::string_view fixedLabel = fixedProbeLabel(target.semantic);
    if (target.itemId == 0u || target.order == 0u ||
        target.windowToken == 0u || !target.probeRect.valid() ||
        target.label.empty() || !validText(target.label) ||
        (!fixedLabel.empty() && target.label != fixedLabel) ||
        (popupSemantic(target.semantic) && target.popupToken == 0u) ||
        (modalSemantic(target.semantic) && target.modalToken == 0u))
      return fail("ready probe lost its actual ImGui item identity");
  } else {
    return fail("target has no implemented observation source");
  }
  if (found->target.requiresGraphRowKey() != target.graphRowKey.has_value() ||
      (target.graphRowKey && !graphKeyValid(*target.graphRowKey)))
    return fail("ready probe GraphRowKey binding is invalid");
  if ((found->requireHovered && !target.hovered) ||
      (found->requireFocused && !target.focused) ||
      (found->requireDropAccepted && !target.dropAccepted))
    return fail("ready probe interaction confirmation is incomplete");
  if (found->gizmo) {
    if (!gizmo || gizmo->renderedFrame != frame.frameSerial ||
        gizmo->inputFrame == 0u || gizmo->mappingEpoch == 0u ||
        gizmo->documentRevision != authority.documentRevision ||
        gizmo->selectionRevision != authority.selectionRevision ||
        !gizmo->viewportRect.valid() ||
        gizmo->phase != found->gizmo->phase ||
        gizmo->captureHeld != found->gizmo->captureHeld ||
        gizmo->beginCount != found->gizmo->beginCount ||
        gizmo->previewCount != found->gizmo->previewCount ||
        gizmo->commitCount != found->gizmo->commitCount ||
        gizmo->detachCount != found->gizmo->detachCount ||
        gizmo->latchCount != found->gizmo->latchCount)
      return fail("ready gizmo observation differs from phase expectation");
    if (isFocusGizmoCase(caseId) &&
        (gizmo->deliveredReleaseCount != found->gizmo->deliveredReleaseCount ||
         gizmo->escapePressCount != found->gizmo->escapePressCount ||
         gizmo->focusLossCount != found->gizmo->focusLossCount ||
         gizmo->suppressedReleaseCount != found->gizmo->suppressedReleaseCount))
      return fail("ready focus-loss counters differ from phase expectation");
    if (found->gizmo->noSelection) {
      if (!isEscapeGizmoCase(caseId) || gizmo->node || authority.selectionKind != "None" ||
          authority.selectedObject || authority.selectedGraphRowKey || !gizmo->gestureNode ||
          !gizmo->gestureNode->valid() ||
          gizmo->gestureNode->authority.value != authority.documentAuthority)
        return fail("detached gizmo lost its retained gesture identity");
    } else if (!gizmo->node || !gizmo->node->valid() ||
               gizmo->node->authority.value != authority.documentAuthority ||
               gizmo->node != authority.selectedObject) {
      return fail("attached gizmo lost its selection identity");
    }
    for (const auto &values : {gizmo->translation, gizmo->scale})
      for (const double value : values)
        if (!std::isfinite(value)) return fail("non-finite gizmo transform");
    for (const double value : gizmo->rotation)
      if (!std::isfinite(value)) return fail("non-finite gizmo rotation");
  }
  if (observedDispatches.size() > expectedStep.expectedDispatches.size())
    return fail("ready probe observed excess UI dispatches");
  if (found->expectedDispatchCount &&
      observedDispatches.size() != *found->expectedDispatchCount)
    return fail("ready probe is missing its exact phase dispatch prefix");
  std::uint64_t previousSequence = 0u;
  for (std::size_t index = 0u; index < observedDispatches.size(); ++index) {
    const ObservedDispatch &observed = observedDispatches[index];
    const DispatchExpectation &expected = expectedStep.expectedDispatches[index];
    const bool observedTextMatches = observed.text == expected.text;
    if (observed.sequence == 0u || observed.sequence <= previousSequence ||
        observed.source != expected.source || observed.kind != expected.kind ||
        observed.operation != expected.operation ||
        observed.targetRole != expected.targetRole ||
        observed.secondaryRole != expected.secondaryRole ||
        !observedTextMatches ||
        observed.accepted != expected.accepted ||
        observed.refresh != expected.refresh ||
        observed.propertyPhase != expected.propertyPhase ||
        observed.propertyValue != expected.propertyValue ||
        !observed.error.empty())
      return fail("ready probe UI dispatch prefix is invalid");
    previousSequence = observed.sequence;
  }
  if ((found->phase == ProbePhase::settled ||
       found->phase == ProbePhase::boundary) &&
      observedDispatches.size() != expectedStep.expectedDispatches.size())
    return fail("terminal ready probe lacks the expected UI dispatches");
  return PlanValidation{true, {}};
}

std::string ReadyProbe::toJson() const {
  std::string output =
      "{\"documentKind\":\"mgif.editor-ui-os-replay-ready\","
      "\"schemaVersion\":" +
      std::to_string(schemaVersion) + ",\"protocol\":";
  appendJsonString(output, protocol);
  output += ",\"integration\":true,\"dispatchMode\":"
            "\"win32-send-input\",\"runNonce\":";
  appendJsonString(output, runNonce);
  output += ",\"case\":";
  appendJsonString(output, caseId);
  output += ",\"planSha256\":";
  appendJsonString(output, planSha256);
  output += ",\"identity\":{\"executableSha256\":";
  appendJsonString(output, executableSha256);
  output += ",\"sourceSha256\":";
  appendJsonString(output, sourceSha256);
  output += "},\"step\":{\"index\":" + std::to_string(step) +
            ",\"id\":";
  appendJsonString(output, stepId);
  output += ",\"phase\":";
  appendJsonString(output, phaseId);
  output += "},\"renderedFrame\":" + std::to_string(renderedFrame) +
            ",\"frame\":{\"lifecycle\":";
  appendJsonString(output, ui::lifecycleStateName(frame.lifecycle));
  output += ",\"lifecycleRevision\":" +
            std::to_string(frame.lifecycleRevision) +
            ",\"documentRevision\":" +
            std::to_string(frame.documentRevision) +
            ",\"selectionRevision\":" +
            std::to_string(frame.selectionRevision) +
            ",\"frameSerial\":" + std::to_string(frame.frameSerial) +
            ",\"mainViewportId\":" +
            std::to_string(frame.mainViewportId) +
            ",\"viewportPosition\":{\"x\":" +
            std::to_string(frame.viewportPosition.x) + ",\"y\":" +
            std::to_string(frame.viewportPosition.y) +
            "},\"viewportSize\":{\"x\":" +
            std::to_string(frame.viewportSize.x) + ",\"y\":" +
            std::to_string(frame.viewportSize.y) +
            "},\"displaySize\":{\"x\":" +
            std::to_string(frame.displaySize.x) + ",\"y\":" +
            std::to_string(frame.displaySize.y) +
            "},\"framebufferScale\":{\"x\":" +
            std::to_string(frame.framebufferScale.x) + ",\"y\":" +
            std::to_string(frame.framebufferScale.y) +
            "}},\"client\":{\"hwnd\":\"0x";
  appendHexUInt(output, client.hwnd);
  output += "\",\"pid\":" + std::to_string(client.pid) +
            ",\"width\":" + std::to_string(client.width) +
            ",\"height\":" + std::to_string(client.height) +
            ",\"dpi\":" + std::to_string(client.dpi) +
            ",\"screenRect\":";
  appendRect(output, client.screenRect);
  output += "},\"authority\":{\"documentAuthority\":\"" +
            std::to_string(authority.documentAuthority) +
            "\",\"documentRevision\":" +
            std::to_string(authority.documentRevision) +
            ",\"selectionRevision\":" +
            std::to_string(authority.selectionRevision) +
            ",\"undoCount\":" + std::to_string(authority.undoCount) +
            ",\"redoCount\":" + std::to_string(authority.redoCount) +
            ",\"documentDigest\":";
  appendJsonString(output, authority.documentDigest);
  output += ",\"projectionDigest\":";
  appendJsonString(output, authority.projectionDigest);
  output += ",\"runtimeContentDigest\":";
  appendJsonString(output, authority.runtimeContentDigest);
  output += ",\"hierarchyOrderDigest\":";
  appendJsonString(output, authority.hierarchyOrderDigest);
  output += ",\"selectionKind\":";
  appendJsonString(output, authority.selectionKind);
  output += ",\"selectedGraphRowKey\":";
  if (authority.selectedGraphRowKey)
    appendGraphRowKey(output, *authority.selectedGraphRowKey);
  else
    output += "null";
  output += ",\"selectedObject\":";
  if (authority.selectedObject)
    appendObjectId(output, *authority.selectedObject);
  else
    output += "null";
  output += ",\"undoDescription\":";
  appendJsonString(output, authority.undoDescription);
  output += ",\"redoDescription\":";
  appendJsonString(output, authority.redoDescription);
  output += "},\"target\":{\"semantic\":";
  appendJsonString(output, targetSemanticName(target.semantic));
  output += ",\"role\":";
  appendJsonString(output, target.role);
  output += ",\"graphRowRole\":";
  if (target.graphRowRole.empty())
    output += "null";
  else
    appendJsonString(output, target.graphRowRole);
  output += ",\"graphRowKey\":";
  if (target.graphRowKey)
    appendGraphRowKey(output, *target.graphRowKey);
  else
    output += "null";
  output += ",\"probe\":{\"label\":";
  appendJsonString(output, target.label);
  if (!target.observationSource.empty()) {
    output += ",\"source\":";
    appendJsonString(output, target.observationSource);
  }
  output += ",\"order\":" + std::to_string(target.order) +
            ",\"itemId\":" + std::to_string(target.itemId) +
            ",\"windowToken\":" + std::to_string(target.windowToken) +
            ",\"popupToken\":" + std::to_string(target.popupToken) +
            ",\"modalToken\":" + std::to_string(target.modalToken) +
            ",\"rect\":";
  appendProbeRect(output, target.probeRect);
  output += "},\"revision\":" + std::to_string(target.revision) +
            ",\"rect\":";
  appendRect(output, target.rect);
  output += ",\"matchCount\":" + std::to_string(target.matchCount) +
            ",\"visible\":";
  appendBool(output, target.visible);
  output += ",\"enabled\":";
  appendBool(output, target.enabled);
  output += ",\"hovered\":";
  appendBool(output, target.hovered);
  output += ",\"focused\":";
  appendBool(output, target.focused);
  output += ",\"dropAccepted\":";
  appendBool(output, target.dropAccepted);
  output += "},\"observedDispatches\":[";
  for (std::size_t index = 0u; index < observedDispatches.size(); ++index) {
    if (index != 0u)
      output.push_back(',');
    output += "{\"sequence\":" +
              std::to_string(observedDispatches[index].sequence) +
              ",\"source\":";
    appendJsonString(output, commandSourceName(observedDispatches[index].source));
    output += ",\"payloadKind\":";
    appendJsonString(output, dispatchKindName(observedDispatches[index].kind));
    output += ",\"operation\":";
    appendJsonString(output, observedDispatches[index].operation);
    output += ",\"targetRole\":";
    appendJsonString(output, observedDispatches[index].targetRole);
    output += ",\"secondaryRole\":";
    appendJsonString(output, observedDispatches[index].secondaryRole);
    output += ",\"text\":";
    appendJsonString(output, observedDispatches[index].text);
    output += ",\"accepted\":";
    appendBool(output, observedDispatches[index].accepted);
    output += ",\"refresh\":";
    appendJsonString(output,
                     projectionRefreshName(observedDispatches[index].refresh));
    output += ",\"error\":";
    appendJsonString(output, observedDispatches[index].error);
    output += ",\"propertyPhase\":";
    appendJsonString(output, observedDispatches[index].propertyPhase);
    output += ",\"propertyValue\":";
    appendJsonString(output, observedDispatches[index].propertyValue);
    output += "}";
  }
  output += "]";
  if (gizmo) output += ",\"gizmo\":" + gizmo->toJson();
  output += "}";
  return output;
}

std::string GizmoProbe::toJson() const {
  std::string output = "{\"source\":\"editor-gizmo-adapter-v1\"";
  const auto number = [&](std::string_view name, std::uint64_t value) {
    output += ",\"" + std::string(name) + "\":" + std::to_string(value);
  };
  number("renderedFrame", renderedFrame); number("inputFrame", inputFrame);
  number("documentRevision", documentRevision); number("selectionRevision", selectionRevision);
  number("mappingEpoch", mappingEpoch);
  number("hoveredComponent", hoveredComponent); number("activeComponent", activeComponent);
  number("phase", phase); number("beginCount", beginCount);
  number("previewCount", previewCount); number("commitCount", commitCount);
  number("detachCount", detachCount); number("latchCount", latchCount);
  number("lastTerminalFrame", lastTerminalFrame); number("lastTerminalSequence", lastTerminalSequence);
  output += ",\"node\":";
  if (node) appendObjectId(output, *node); else output += "null";
  output += ",\"gestureNode\":";
  if (gestureNode) appendObjectId(output, *gestureNode); else output += "null";
  output += ",\"viewportRect\":"; appendProbeRect(output, viewportRect);
  output += ",\"componentRect\":"; appendProbeRect(output, componentRect);
  output += ",\"outsideRect\":"; appendProbeRect(output, outsideRect);
  output += ",\"pointer\":{\"x\":" + std::to_string(pointer.x) +
            ",\"y\":" + std::to_string(pointer.y) + "}";
  output += ",\"hasPointer\":"; appendBool(output, hasPointer);
  output += ",\"componentValid\":"; appendBool(output, componentValid);
  output += ",\"componentEnabled\":"; appendBool(output, componentEnabled);
  output += ",\"ndcYDown\":"; appendBool(output, ndcYDown);
  output += ",\"captureHeld\":"; appendBool(output, captureHeld);
  output += ",\"terminal\":"; appendJsonString(output, terminal);
  number("deliveredReleaseCount", deliveredReleaseCount);
  number("escapePressCount", escapePressCount);
  number("focusLossCount", focusLossCount);
  number("suppressedReleaseCount", suppressedReleaseCount);
  number("nativeCallbackSequence", nativeCallbackSequence);
  output += ",\"lastTerminalEventsOverflowed\":";
  appendBool(output, lastTerminalEventsOverflowed);
  output += ",\"lastTerminalEvents\":[";
  for (std::size_t index = 0u; index < lastTerminalEvents.size(); ++index) {
    if (index != 0u) output += ',';
    output += "{\"kind\":";
    appendJsonString(output, lastTerminalEvents[index].kind);
    output += ",\"sequence\":" + std::to_string(lastTerminalEvents[index].sequence);
    output += ",\"callbackSequence\":" +
              std::to_string(lastTerminalEvents[index].callbackSequence) + "}";
  }
  output += ']';
  const auto vector = [&](std::string_view name, const auto &values) {
    output += ",\"" + std::string(name) + "\":[";
    bool first = true;
    for (const double value : values) {
      if (!first) output += ',';
      first = false;
      char bytes[64];
      const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value);
      if (std::isfinite(value) && result.ec == std::errc{})
        output.append(bytes, result.ptr);
      else output += "null";
    }
    output += ']';
  };
  vector("translation", translation); vector("rotation", rotation); vector("scale", scale);
  output += '}';
  return output;
}

ReplayPlan makeU02OsReplayPlan() {
  ReplayPlan plan;
  plan.description =
      "true Win32 SendInput hierarchy replay with per-phase live UI probes";
  plan.graphRoles = {
      {"primary-scene", "main", EditorObjectKind::scene, "R09 Scene",
       "scene-context-open", {}, {}, {}, false},
      {"primary-node", "main", EditorObjectKind::node, "Node",
       "scene-add-node", {}, {}, {}, true},
      {"cross-parent-source", "main", EditorObjectKind::node, "Selected",
       "bootstrap", {}, {}, {}, true},
      {"cross-parent-target", "main", EditorObjectKind::node, "Shared Peer",
       "bootstrap", {}, {}, "cross-parent-source", true},
      {"same-parent-child", "main", EditorObjectKind::node, "Node",
       "node-add-child-empty", {}, "primary-node", {}, false},
      {"same-parent-parent", "main", EditorObjectKind::node, "",
       "scene-add-node", "primary-node", {}, {}, true},
      {"root-noop-source", "main", EditorObjectKind::node, "No Material",
       "bootstrap", {}, {}, "cross-parent-source", true},
      {"regular-delete-node", "main", EditorObjectKind::node,
       "OS Replay Hierarchy Node", "node-duplicate-menu", {}, {},
       "primary-node", true},
      {"unique-root-node", "unique-root", EditorObjectKind::node, "Root",
       "unique-root-select", {}, {}, {}, true},
  };
  std::uint32_t ordinal = 0u;

  const TargetSpec client = target(TargetSemantic::client, "client");
  StepSpec bootstrap;
  bootstrap.id = "bootstrap";
  bootstrap.subcase = "main";
  bootstrap.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("boundary", ProbePhase::boundary, client, input(InputKind::none),
            false, false, false, CapturePoint::after),
  };
  appendStep(plan, std::move(bootstrap), ordinal);

  appendStep(plan,
             contextOpenStep("scene-context-open", "main", "primary-scene",
                             TargetSemantic::sceneAddNode, "menu:add-node"),
             ordinal);
  appendStep(plan,
             clickStep("scene-add-node", "main",
                       TargetSemantic::sceneAddNode, "menu:add-node",
                       "primary-scene", InputKind::leftClick,
                       EffectKind::documentMutation, 1,
                       HistoryEffect::unchanged, true),
             ordinal);
  appendStep(plan,
             contextOpenStep("node-context-add-child", "main",
                             "primary-node",
                             TargetSemantic::nodeAddChildMenu,
                             "menu:add-child"),
             ordinal);

  StepSpec submenu = clickStep(
      "node-add-child-submenu", "main", TargetSemantic::nodeAddChildMenu,
      "menu:add-child", "primary-node", InputKind::none);
  submenu.phases[1].input = input(InputKind::none);
  appendStep(plan, std::move(submenu), ordinal);
  appendStep(plan,
             clickStep("node-add-child-empty", "main",
                       TargetSemantic::nodeAddChildNode,
                       "menu:add-child-empty", "primary-node",
                       InputKind::leftClick, EffectKind::documentMutation, 1,
                       HistoryEffect::pushUndoClearRedo, true),
             ordinal);
  appendStep(plan,
             contextOpenStep("node-context-rename", "main", "primary-node",
                             TargetSemantic::nodeRename, "menu:rename"),
             ordinal);
  appendStep(plan,
             clickStep("node-rename-menu", "main",
                       TargetSemantic::nodeRename, "menu:rename",
                       "primary-node", InputKind::leftClick),
             ordinal);

  const TargetSpec renameInput =
      target(TargetSemantic::renameInput, "rename:text", "primary-node");
  StepSpec renameText;
  renameText.id = "rename-replace-text";
  renameText.subcase = "main";
  renameText.phases = {
      phase("target", ProbePhase::target, renameInput,
            input(InputKind::movePointer), false, false, false,
            CapturePoint::before),
      phase("hover", ProbePhase::hover, renameInput,
            input(InputKind::leftClick), true),
      phase("focus", ProbePhase::focus, renameInput,
            input(InputKind::replaceText, "OS Replay Hierarchy Node"), false,
            true),
      phase("settled", ProbePhase::settled, renameInput,
            input(InputKind::none), false, false, false,
            CapturePoint::after),
  };
  appendStep(plan, std::move(renameText), ordinal);

  StepSpec renameSubmit;
  renameSubmit.id = "rename-submit";
  renameSubmit.subcase = "main";
  renameSubmit.effect = EffectKind::documentMutation;
  renameSubmit.expectedRevisionDelta = 1;
  renameSubmit.history = HistoryEffect::pushUndoClearRedo;
  renameSubmit.expectedDispatches = {
      {ui::CommandSource::dialog, DispatchKind::renameObject, "rename",
       "primary-node", {}, "OS Replay Hierarchy Node", true,
       ui::ProjectionRefresh::hierarchy}};
  const TargetSpec clientTarget = target(TargetSemantic::client, "client");
  renameSubmit.phases = {
      phase("focus", ProbePhase::focus, renameInput,
            input(InputKind::pressKey, "Enter"), false, true, false,
            CapturePoint::before),
      phase("settled", ProbePhase::settled, clientTarget,
            input(InputKind::none), false, false, false,
            CapturePoint::after),
  };
  appendStep(plan, std::move(renameSubmit), ordinal);

  appendStep(plan,
             contextOpenStep("node-context-duplicate", "main",
                             "primary-node", TargetSemantic::nodeDuplicate,
                             "menu:duplicate"),
             ordinal);
  appendStep(plan,
             clickStep("node-duplicate-menu", "main",
                       TargetSemantic::nodeDuplicate, "menu:duplicate",
                       "primary-node", InputKind::leftClick,
                       EffectKind::documentMutation, 1,
                       HistoryEffect::pushUndoClearRedo, true),
             ordinal);
  appendStep(plan,
             dragStep("cross-parent-drag", "main", "cross-parent-source",
                      TargetSemantic::hierarchyDropTarget, "cross-parent-target",
                      "cross-parent-target", EffectKind::documentMutation, 1,
                      HistoryEffect::pushUndoClearRedo),
             ordinal);
  appendStep(plan,
             chordStep("undo-cross-parent", "main", "Ctrl+Z",
                       EffectKind::historyMove, 1,
                       HistoryEffect::moveUndoToRedo, true),
             ordinal);
  appendStep(plan,
             chordStep("redo-cross-parent", "main", "Ctrl+Y",
                       EffectKind::historyMove, 1,
                       HistoryEffect::moveRedoToUndo, true),
             ordinal);
  appendStep(plan,
             dragStep("same-parent-drag-noop", "main", "same-parent-child",
                      TargetSemantic::hierarchyDropTarget, "same-parent-parent",
                      "same-parent-parent",
                      EffectKind::documentNoopHistoryEnvelope, 0,
                      HistoryEffect::pushUndoClearRedo),
             ordinal);
  appendStep(plan,
             dragStep("root-root-drag-noop", "main", "root-noop-source",
                      TargetSemantic::hierarchyDropTarget, "primary-scene",
                      "primary-scene",
                      EffectKind::documentNoopHistoryEnvelope, 0,
                      HistoryEffect::pushUndoClearRedo),
             ordinal);
  appendStep(plan,
             contextOpenStep("node-context-delete", "main",
                             "regular-delete-node", TargetSemantic::nodeDelete,
                             "menu:delete"),
             ordinal);
  appendStep(plan,
             clickStep("node-delete-menu", "main",
                       TargetSemantic::nodeDelete, "menu:delete",
                       "regular-delete-node", InputKind::leftClick),
             ordinal);
  appendStep(plan,
             clickStep("delete-confirm", "main", TargetSemantic::deleteAccept,
                       "delete:confirm", "regular-delete-node",
                       InputKind::leftClick, EffectKind::documentMutation, 1,
                       HistoryEffect::pushUndoClearRedo, true),
             ordinal);

  StepSpec mainBoundary;
  mainBoundary.id = "main-process-boundary";
  mainBoundary.subcase = "main";
  mainBoundary.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("boundary", ProbePhase::boundary, client, input(InputKind::none),
            false, false, false, CapturePoint::after),
  };
  appendStep(plan, std::move(mainBoundary), ordinal);

  StepSpec reset;
  reset.id = "unique-root-subcase";
  reset.subcase = "unique-root";
  reset.effect = EffectKind::authorityReset;
  reset.history = HistoryEffect::reset;
  reset.relaunchProcessBefore = true;
  reset.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after),
  };
  appendStep(plan, std::move(reset), ordinal);
  appendStep(plan,
             clickStep("unique-root-select", "unique-root",
                       TargetSemantic::graphRow, "unique-root-node",
                       "unique-root-node", InputKind::leftClick,
                       EffectKind::selectionOnly, 0,
                       HistoryEffect::unchanged),
             ordinal);
  appendStep(plan,
             contextOpenStep("unique-root-context-delete", "unique-root",
                             "unique-root-node", TargetSemantic::nodeDelete,
                             "menu:delete"),
             ordinal);
  appendStep(plan,
             clickStep("unique-root-delete-menu", "unique-root",
                       TargetSemantic::nodeDelete, "menu:delete",
                       "unique-root-node", InputKind::leftClick),
             ordinal);
  appendStep(plan,
             clickStep("unique-root-delete-confirm", "unique-root",
                       TargetSemantic::deleteAccept, "delete:confirm",
                       "unique-root-node", InputKind::leftClick,
                       EffectKind::documentNoopHistoryEnvelope, 0,
                       HistoryEffect::pushUndoClearRedo),
             ordinal);
  appendStep(plan,
             chordStep("unique-root-undo", "unique-root", "Ctrl+Z",
                       EffectKind::historyMove, 0,
                       HistoryEffect::moveUndoToRedo),
             ordinal);
  appendStep(plan,
             chordStep("unique-root-redo", "unique-root", "Ctrl+Y",
                       EffectKind::historyMove, 0,
                       HistoryEffect::moveRedoToUndo),
             ordinal);

  StepSpec finalBoundary;
  finalBoundary.id = "final-boundary";
  finalBoundary.subcase = "unique-root";
  finalBoundary.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("boundary", ProbePhase::boundary, client, input(InputKind::none),
            false, false, false, CapturePoint::after),
  };
  appendStep(plan, std::move(finalBoundary), ordinal);
  return plan;
}

ReplayPlan makeU03OsReplayPlan() {
  ReplayPlan plan;
  plan.caseId = std::string(kU03Case);
  plan.description =
      "true Win32 SendInput viewport, transform gizmo, and Inspector replay";
  plan.graphRoles = {
      {"primary-scene", "main", EditorObjectKind::scene, "R09 Scene",
       "bootstrap", {}, {}, {}, false},
      {"transform-node", "main", EditorObjectKind::node, "Selected",
       "bootstrap", {}, {}, {}, true},
  };
  std::uint32_t ordinal = 0u;
  const TargetSpec client = target(TargetSemantic::client, "client");
  auto boundary = [&](std::string id) {
    StepSpec step;
    step.id = std::move(id);
    step.subcase = "main";
    step.phases = {phase("target", ProbePhase::target, client,
                          input(InputKind::none), false, false, false,
                          CapturePoint::before),
                   phase("boundary", ProbePhase::boundary, client,
                         input(InputKind::none), false, false, false,
                         CapturePoint::after)};
    appendStep(plan, std::move(step), ordinal);
  };
  boundary("bootstrap");

  StepSpec selectNode;
  selectNode.id = "select-transform-node";
  selectNode.subcase = "main";
  selectNode.effect = EffectKind::selectionOnly;
  selectNode.expectedDispatches = {
      {ui::CommandSource::sceneBrowser, DispatchKind::selectObject, "select",
       "transform-node", {}, {}, true, ui::ProjectionRefresh::selectionOnly}};
  const TargetSpec nodeRow =
      target(TargetSemantic::graphRow, "transform-node", "transform-node");
  selectNode.phases = {
      phase("target", ProbePhase::target, nodeRow, input(InputKind::movePointer),
            false, false, false, CapturePoint::before),
      phase("hover", ProbePhase::hover, nodeRow, input(InputKind::leftClick),
            true),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  appendStep(plan, std::move(selectNode), ordinal);

  auto passiveClick = [&](std::string id, TargetSemantic semantic,
                          std::string role, InputKind kind,
                          EffectKind effect = EffectKind::presentationOnly) {
    StepSpec step;
    step.id = std::move(id);
    step.subcase = "main";
    step.effect = effect;
    const std::string targetRole = role;
    step.phases = {phase("target", ProbePhase::target,
                          target(semantic, targetRole),
                          input(InputKind::movePointer), false, false, false,
                          CapturePoint::before),
                   phase("hover", ProbePhase::hover,
                         target(semantic, targetRole), input(kind), true),
                   phase("settled", ProbePhase::settled, client,
                         input(InputKind::none), false, false, false,
                         CapturePoint::after)};
    appendStep(plan, std::move(step), ordinal);
  };
  // Viewport click classifier: hit, toggle, and blank-space clear are all
  // observed through the passive viewport channel (no UI dispatch envelope).
  passiveClick("viewport-single-hit", TargetSemantic::viewport, "viewport",
               InputKind::leftClick, EffectKind::selectionOnly);
  passiveClick("viewport-toggle", TargetSemantic::viewport, "viewport",
               InputKind::leftClick, EffectKind::selectionOnly);
  passiveClick("viewport-blank", TargetSemantic::viewport, "viewport-blank",
               InputKind::leftClick, EffectKind::selectionOnly);

  const std::array<std::pair<TargetSemantic, const char *>, 3> gizmos{{
      {TargetSemantic::gizmoTranslate, "gizmo-translate"},
      {TargetSemantic::gizmoRotate, "gizmo-rotate"},
      {TargetSemantic::gizmoScale, "gizmo-scale"}}};
  for (const auto &[semantic, role] : gizmos)
    passiveClick(std::string(role) + "-single-hit", semantic, role,
                 InputKind::leftClick);

  // Inspector edits expose the complete begin/preview/commit PropertyEdit
  // sequence. Values are retained as strings to avoid locale-dependent JSON.
  StepSpec trs;
  trs.id = "inspector-translation-x";
  trs.subcase = "main";
  trs.effect = EffectKind::documentMutation;
  trs.expectedRevisionDelta = 1;
  trs.history = HistoryEffect::pushUndoClearRedo;
  trs.expectedDispatches = {
      {ui::CommandSource::inspector, DispatchKind::propertyEdit, "translation",
       "transform-node", {}, {}, true,
       ui::ProjectionRefresh::none, "begin", "0.0,0.0,0.0"},
      {ui::CommandSource::inspector, DispatchKind::propertyEdit, "translation",
       "transform-node", {}, {}, true,
       ui::ProjectionRefresh::properties, "preview", "0.8,0.0,0.0"},
      {ui::CommandSource::inspector, DispatchKind::propertyEdit, "translation",
       "transform-node", {}, {}, true,
       ui::ProjectionRefresh::none, "commit", "0.8,0.0,0.0"}};
  const TargetSpec field = target(TargetSemantic::inspectorTranslationX,
                                 "inspector-translation-x", "transform-node");
  trs.phases = {
      phase("target", ProbePhase::target, field, input(InputKind::movePointer),
            false, false, false, CapturePoint::before),
      phase("focus", ProbePhase::focus, field, input(InputKind::leftDown),
            true, false),
      phase("preview", ProbePhase::focus, field, input(InputKind::movePointer),
            true, true),
      phase("commit", ProbePhase::focus, field, input(InputKind::leftUp),
            false, false),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  appendStep(plan, std::move(trs), ordinal);

  passiveClick("toolbar-gizmo-toggle", TargetSemantic::toolbarGizmo,
               "toolbar-gizmo", InputKind::leftClick);
  passiveClick("toolbar-grid-toggle", TargetSemantic::toolbarGrid,
               "toolbar-grid", InputKind::leftClick);
  passiveClick("toolbar-snap-toggle", TargetSemantic::toolbarSnap,
               "toolbar-snap", InputKind::leftClick);

  auto gizmoDrag = [&](std::string id, InputKind terminal,
                       EffectKind effect, HistoryEffect history,
                       std::int32_t revision, bool escape) {
    StepSpec step;
    step.id = std::move(id);
    step.subcase = "main";
    step.effect = effect;
    step.expectedRevisionDelta = revision;
    step.history = history;
    const TargetSpec gizmo = target(TargetSemantic::gizmoTranslate,
                                    "gizmo-translate");
    step.phases = {
        phase("target", ProbePhase::target, gizmo,
              input(InputKind::movePointer), false, false, false,
              CapturePoint::before),
        phase("press", ProbePhase::dragTarget, gizmo,
              input(InputKind::leftDown), true),
        phase("preview", ProbePhase::dragTarget, gizmo,
              input(InputKind::movePointer), true),
        phase("outside", ProbePhase::dragTarget,
              target(TargetSemantic::viewport, "viewport-outside"),
              input(InputKind::movePointer), false),
        phase("release", ProbePhase::dragTarget,
              target(TargetSemantic::viewport, "viewport-outside"),
              input(terminal), false),
    };
    if (escape)
      step.phases.push_back(phase("escape", ProbePhase::dragTarget, gizmo,
                                  input(InputKind::pressKey, "Escape"), false));
    step.phases.push_back(phase("settled", ProbePhase::settled, client,
                                input(InputKind::none), false, false, false,
                                CapturePoint::after));
    appendStep(plan, std::move(step), ordinal);
  };
  gizmoDrag("gizmo-drag-outside-release", InputKind::leftUp,
            EffectKind::documentMutation, HistoryEffect::pushUndoClearRedo, 1,
            false);
  boundary("undo-transform");
  plan.steps.back().effect = EffectKind::historyMove;
  plan.steps.back().expectedRevisionDelta = 1;
  plan.steps.back().history = HistoryEffect::moveUndoToRedo;
  plan.steps.back().expectedDispatches = {
      {ui::CommandSource::shortcut, DispatchKind::invokeCommand, "undo", {},
       {}, {}, true, ui::ProjectionRefresh::properties}};
  plan.steps.back().phases[0].input = input(InputKind::pressKey, "Ctrl+Z");
  plan.steps.back().phases[0].phase = ProbePhase::focus;
  boundary("redo-transform");
  plan.steps.back().effect = EffectKind::historyMove;
  plan.steps.back().expectedRevisionDelta = 1;
  plan.steps.back().history = HistoryEffect::moveRedoToUndo;
  plan.steps.back().expectedDispatches = {
      {ui::CommandSource::shortcut, DispatchKind::invokeCommand, "redo", {},
       {}, {}, true, ui::ProjectionRefresh::properties}};
  plan.steps.back().phases[0].input = input(InputKind::pressKey, "Ctrl+Y");
  plan.steps.back().phases[0].phase = ProbePhase::focus;
  gizmoDrag("gizmo-escape-only", InputKind::none, EffectKind::selectionOnly,
            HistoryEffect::unchanged, 0, true);
  gizmoDrag("gizmo-escape-release-same-frame", InputKind::leftUp,
            EffectKind::selectionOnly, HistoryEffect::unchanged, 0, true);
  // Focus loss is orchestrated by the Windows harness (foreground transfer),
  // while any physical release is still represented as a SendInput phase.
  StepSpec focusNoRelease;
  focusNoRelease.id = "focus-loss-no-release";
  focusNoRelease.subcase = "main";
  focusNoRelease.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("focus-loss", ProbePhase::boundary, client,
            input(InputKind::none)),
      phase("late-release", ProbePhase::dragTarget, client,
            input(InputKind::leftUp)),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  appendStep(plan, std::move(focusNoRelease), ordinal);
  StepSpec focusWithRelease;
  focusWithRelease.id = "focus-loss-with-release";
  focusWithRelease.subcase = "main";
  focusWithRelease.effect = EffectKind::documentMutation;
  focusWithRelease.expectedRevisionDelta = 1;
  focusWithRelease.history = HistoryEffect::pushUndoClearRedo;
  focusWithRelease.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("release", ProbePhase::dragTarget, client,
            input(InputKind::leftUp)),
      phase("focus-loss", ProbePhase::boundary, client,
            input(InputKind::none)),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  appendStep(plan, std::move(focusWithRelease), ordinal);
  boundary("final-boundary");
  for (StepSpec &step : plan.steps) {
    for (std::size_t index = 0u; index < step.phases.size(); ++index) {
      auto &value = step.phases[index];
      value.expectedDispatchCount = index + 1u == step.phases.size()
          ? static_cast<std::uint32_t>(step.expectedDispatches.size()) : 0u;
      if (step.expectedDispatches.size() == 3u && index > 1u)
        value.expectedDispatchCount = static_cast<std::uint32_t>(index - 1u);
      if (value.input.kind == InputKind::pressKey &&
          value.input.value.starts_with("Ctrl+"))
        value.input.kind = InputKind::keyChord;
      if (value.input.kind == InputKind::movePointer && value.id == "preview" &&
          value.target.semantic == TargetSemantic::inspectorTranslationX)
        value.input.value = "80,0";
    }
  }
  return plan;
}

ReplayPlan makeU03InspectorOsReplayPlan() {
  ReplayPlan plan;
  plan.caseId = std::string(kU03InspectorCase);
  plan.description = "partial U03: real Inspector TRS and history input only";
  const ReplayPlan draft = makeU03OsReplayPlan();
  plan.graphRoles = draft.graphRoles;
  std::uint32_t ordinal = 0u;
  const auto copyStep = [&](std::string_view id) {
    const auto found = std::find_if(draft.steps.begin(), draft.steps.end(),
        [&](const StepSpec &step) { return step.id == id; });
    return *found;
  };
  appendStep(plan, copyStep("bootstrap"), ordinal);
  appendStep(plan, copyStep("select-transform-node"), ordinal);
  const StepSpec translation = copyStep("inspector-translation-x");
  appendStep(plan, translation, ordinal);
  const auto componentEdit = [&](std::string id, TargetSemantic semantic,
                                 std::string operation, std::string before,
                                 std::string after) {
    StepSpec step = translation;
    step.id = id;
    for (PhaseSpec &value : step.phases) {
      if (value.target.semantic == TargetSemantic::inspectorTranslationX) {
        value.target.semantic = semantic;
        value.target.role = id;
      }
    }
    for (auto &dispatch : step.expectedDispatches) {
      dispatch.operation = operation;
      dispatch.propertyValue = dispatch.propertyPhase == "begin" ? before : after;
    }
    appendStep(plan, std::move(step), ordinal);
  };
  componentEdit("inspector-rotation-x", TargetSemantic::inspectorRotationX,
                "rotation", "0.0,0.0,0.0", "8.0,0.0,0.0");
  componentEdit("inspector-scale-x", TargetSemantic::inspectorScaleX,
                "scale", "1.0,1.0,1.0", "1.8,1.0,1.0");
  for (const auto id : {"undo-transform", "redo-transform"}) {
    StepSpec step = copyStep(id);
    step.phases.front().requireFocused = true;
    appendStep(plan, std::move(step), ordinal);
  }
  appendStep(plan, copyStep("final-boundary"), ordinal);
  return plan;
}

ReplayPlan makeU03GizmoOsReplayPlan() {
  const auto inspector = makeU03InspectorOsReplayPlan();
  ReplayPlan plan;
  plan.caseId = std::string(kU03GizmoCase);
  plan.description = "partial U03: native translate-Y outside release and history";
  plan.graphRoles = inspector.graphRoles;
  std::uint32_t ordinal = 0u;
  appendStep(plan, inspector.steps[0], ordinal);
  StepSpec select = inspector.steps[1];
  select.phases.back().gizmo = GizmoExpectation{1u, false, 0u, 0u, 0u, {}};
  appendStep(plan, std::move(select), ordinal);
  StepSpec drag;
  drag.id = "drag-outside-release";
  drag.subcase = "main";
  drag.effect = EffectKind::documentMutation;
  drag.expectedRevisionDelta = 1;
  drag.history = HistoryEffect::pushUndoClearRedo;
  const auto component = target(TargetSemantic::gizmoTranslateY,
                                "gizmo-translate-y", "transform-node");
  const auto outside = target(TargetSemantic::viewportOutside,
                              "viewport-outside", "transform-node");
  const auto client = target(TargetSemantic::client, "client");
  drag.phases = {
      phase("target", ProbePhase::target, component, input(InputKind::movePointer),
            false, false, false, CapturePoint::before),
      phase("hover", ProbePhase::hover, component, input(InputKind::leftDown), true),
      phase("outside", ProbePhase::dragTarget, outside, input(InputKind::movePointer)),
      phase("release", ProbePhase::dragTarget, client, input(InputKind::leftUp)),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  for (auto &value : drag.phases) value.expectedDispatchCount = 0u;
  drag.phases[0].gizmo = GizmoExpectation{1u, false, 0u, 0u, 0u, {}};
  drag.phases[1].gizmo = drag.phases[0].gizmo;
  drag.phases[2].gizmo = GizmoExpectation{2u, true, 1u, 0u, 0u, false};
  drag.phases[3].gizmo = GizmoExpectation{2u, true, 1u, 1u, 0u, true};
  drag.phases[4].gizmo = GizmoExpectation{1u, false, 1u, 1u, 1u, true};
  appendStep(plan, std::move(drag), ordinal);
  appendStep(plan, inspector.steps[5], ordinal);
  appendStep(plan, inspector.steps[6], ordinal);
  appendStep(plan, inspector.steps[7], ordinal);
  return plan;
}

ReplayPlan makeU03EscapeOsReplayPlan() {
  const ReplayPlan translate = makeU03GizmoOsReplayPlan();
  ReplayPlan plan;
  plan.caseId = std::string(kU03EscapeCase);
  plan.description = "partial U03 O05: Escape-only detach and late release retain preview without history";
  plan.graphRoles = translate.graphRoles;
  std::uint32_t ordinal = 0u;
  appendStep(plan, translate.steps[0], ordinal);
  appendStep(plan, translate.steps[1], ordinal);
  StepSpec drag = translate.steps[2];
  drag.id = "drag-escape-only";
  drag.history = HistoryEffect::unchanged;
  drag.phases[3].id = "escape";
  drag.phases[3].input = input(InputKind::keyDown, "Escape");
  const GizmoExpectation detached{0u, false, 1u, 1u, 0u, true, 1u, 0u, true};
  drag.phases.back().gizmo = detached;
  appendStep(plan, std::move(drag), ordinal);
  const auto client = target(TargetSemantic::client, "client");
  StepSpec late;
  late.id = "late-release";
  late.subcase = "main";
  late.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::keyUp, "Escape"),
            false, false, false, CapturePoint::before),
      phase("release", ProbePhase::dragTarget, client, input(InputKind::leftUp)),
      phase("settled", ProbePhase::settled, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  for (auto &value : late.phases) { value.expectedDispatchCount = 0u; value.gizmo = detached; }
  appendStep(plan, std::move(late), ordinal);
  auto boundary = translate.steps.back();
  for (auto &value : boundary.phases) value.gizmo = detached;
  appendStep(plan, std::move(boundary), ordinal);
  return plan;
}

ReplayPlan makeU03SameFrameOsReplayPlan(bool escapeFirst) {
  ReplayPlan plan = makeU03EscapeOsReplayPlan();
  plan.caseId = std::string(escapeFirst ? kU03EscapeReleaseCase : kU03ReleaseEscapeCase);
  plan.description = "partial U03 O06: ordered same-frame Escape and release detach without history";
  auto &drag = plan.steps[2];
  drag.id = "drag-same-frame";
  drag.phases[3].id = "terminal-batch";
  drag.phases[3].input = input(InputKind::escapeReleaseBatch,
                              escapeFirst ? "escape-first" : "release-first");
  return plan;
}

ReplayPlan makeU03FocusOsReplayPlan(std::string_view caseId) {
  if (!isFocusGizmoCase(caseId))
    return {};

  const ReplayPlan escape = makeU03EscapeOsReplayPlan();
  ReplayPlan plan;
  plan.caseId = std::string(caseId);
  plan.description =
      "partial U03 O07: native focus-loss release partitions preserve the preview";
  plan.graphRoles = escape.graphRoles;
  std::uint32_t ordinal = 0u;
  appendStep(plan, escape.steps[0], ordinal);
  appendStep(plan, escape.steps[1], ordinal);

  const bool deliveredRelease = caseId == kU03FocusDeliveredReleaseCase;
  const bool sameFrameRelease = caseId == kU03FocusSameFrameCase;
  const auto client = target(TargetSemantic::client, "client");
  const auto component = target(TargetSemantic::gizmoTranslateY,
                                "gizmo-translate-y", "transform-node");
  const auto outside = target(TargetSemantic::viewportOutside,
                              "viewport-outside", "transform-node");
  StepSpec drag;
  drag.id = deliveredRelease ? "drag-focus-delivered-release"
                             : sameFrameRelease ? "drag-focus-same-frame"
                                                 : "drag-focus-no-release";
  drag.subcase = "main";
  drag.effect = EffectKind::documentMutation;
  drag.expectedRevisionDelta = 1;
  drag.history = deliveredRelease ? HistoryEffect::pushUndoClearRedo
                                  : HistoryEffect::unchanged;
  drag.phases = {
      phase("target", ProbePhase::target, component,
            input(InputKind::movePointer), false, false, false,
                  CapturePoint::before),
      phase("hover", ProbePhase::hover, component,
            input(InputKind::leftDown), true),
      phase("outside", ProbePhase::dragTarget, outside,
            input(InputKind::movePointer)),
  };
  if (deliveredRelease) {
    drag.phases.push_back(phase("release", ProbePhase::dragTarget, client,
                                input(InputKind::leftUp)));
    drag.phases.push_back(phase("focus-loss", ProbePhase::dragTarget, client,
                                input(InputKind::focusLoss, "sink")));
    drag.phases.push_back(phase("focus-restore", ProbePhase::dragTarget, client,
                                input(InputKind::focusRestore, "editor")));
  } else if (sameFrameRelease) {
    drag.phases.push_back(phase("focus-loss", ProbePhase::dragTarget, client,
                                input(InputKind::leftUpFocusLoss,
                                      "release-then-focus-loss")));
    drag.phases.push_back(phase("focus-restore", ProbePhase::dragTarget, client,
                                input(InputKind::focusRestore, "editor")));
    drag.phases.push_back(phase("settle-focus", ProbePhase::dragTarget, client,
                                input(InputKind::none)));
  } else {
    drag.phases.push_back(phase("focus-loss", ProbePhase::dragTarget, client,
                                input(InputKind::focusLoss, "sink")));
    drag.phases.push_back(phase("focus-restore", ProbePhase::dragTarget, client,
                                input(InputKind::focusRestore, "editor")));
    drag.phases.push_back(phase("late-release", ProbePhase::dragTarget, client,
                                input(InputKind::leftUp)));
  }
  drag.phases.push_back(phase("settled", ProbePhase::settled, client,
                              input(InputKind::none), false, false, false,
                              CapturePoint::after));

  const GizmoExpectation idle{1u, false, 0u, 0u, 0u, std::nullopt,
                              0u, 0u, false, 0u, 0u, 0u, 0u};
  const GizmoExpectation dragging{2u, true, 1u, 0u, 0u, false,
                                  0u, 0u, false, 0u, 0u, 0u, 0u};
  const GizmoExpectation preview{2u, true, 1u, 1u, 0u, true,
                                 0u, 0u, false, 0u, 0u, 0u, 0u};
  // A focus-loss callback is its own application-visible terminal batch.  In
  // the no-release partition the polling/synthetic mouse-up cannot be
  // observed until the editor has regained focus and a subsequent frame is
  // assembled, so keep this first latched snapshot at zero suppressed ups.
  const GizmoExpectation latched{3u, false, 1u, 1u, 0u, std::nullopt,
                                 0u, 1u, false, 0u, 0u, 1u, 0u};
  const GizmoExpectation latchedFocusBatch{
      3u, false, 1u, 1u, 0u, std::nullopt, 0u, 1u, false, 0u, 0u, 1u,
      1u};
  const GizmoExpectation latchedSuppressed{3u, false, 1u, 1u, 0u, true,
                                            0u, 1u, false, 0u, 0u, 1u, 1u};
  const GizmoExpectation latchedLateSuppressed{3u, false, 1u, 1u, 0u, true,
                                               0u, 1u, false, 0u, 0u, 1u, 2u};
  const GizmoExpectation committed{1u, false, 1u, 1u, 1u, true,
                                    0u, 0u, false, 1u, 0u, 0u, 0u};
  const GizmoExpectation committedUnfocused{1u, false, 1u, 1u, 1u,
                                            std::nullopt, 0u, 0u, false,
                                            1u, 0u, 0u, 0u};
  drag.phases[0].gizmo = idle;
  drag.phases[1].gizmo = idle;
  drag.phases[2].gizmo = dragging;
  drag.phases[3].gizmo = preview;
  if (deliveredRelease) {
    drag.phases[4].gizmo = committed;
    drag.phases[5].gizmo = committedUnfocused;
    drag.phases[6].gizmo = committed;
  } else if (sameFrameRelease) {
    drag.phases[4].gizmo = latchedFocusBatch;
    drag.phases[5].gizmo = latchedSuppressed;
    drag.phases[6].gizmo = latchedSuppressed;
  } else {
    drag.phases[4].gizmo = latched;
    drag.phases[5].gizmo = latchedSuppressed;
    drag.phases[6].gizmo = latchedLateSuppressed;
  }
  if (deliveredRelease)
    drag.phases[6].gizmo = committed;
  for (auto &value : drag.phases)
    value.expectedDispatchCount = 0u;
  appendStep(plan, std::move(drag), ordinal);

  StepSpec boundary;
  boundary.id = "final-boundary";
  boundary.subcase = "main";
  boundary.phases = {
      phase("target", ProbePhase::target, client, input(InputKind::none),
            false, false, false, CapturePoint::before),
      phase("boundary", ProbePhase::boundary, client, input(InputKind::none),
            false, false, false, CapturePoint::after)};
  boundary.phases[0].gizmo = deliveredRelease ? committed
                                               : (sameFrameRelease
                                                      ? latchedSuppressed
                                                      : latchedLateSuppressed);
  boundary.phases[1].gizmo = boundary.phases[0].gizmo;
  boundary.phases[0].expectedDispatchCount = 0u;
  boundary.phases[1].expectedDispatchCount = 0u;
  appendStep(plan, std::move(boundary), ordinal);
  return plan;
}

} // namespace demo::editor::testing::osreplay
