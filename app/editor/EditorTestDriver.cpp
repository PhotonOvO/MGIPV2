#include "EditorTestDriver.h"
#include "SceneEditorUi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace demo::editor::testing {
namespace {

constexpr std::size_t index(ActionKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

constexpr std::size_t index(UiState state) noexcept {
  return static_cast<std::size_t>(state);
}

[[nodiscard]] bool validText(std::string_view value) noexcept {
  if (value.empty())
    return false;
  for (const unsigned char character : value) {
    if (character < 0x20U)
      return false;
  }
  return true;
}

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
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
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
      if (character < 0x20U) {
        output += "\\u00";
        output.push_back(hex[(character >> 4U) & 0x0fU]);
        output.push_back(hex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  output.push_back('"');
}

void appendKey(std::string &output, std::string_view key, bool &first) {
  if (!first)
    output.push_back(',');
  first = false;
  appendJsonString(output, key);
  output.push_back(':');
}

void appendBool(std::string &output, bool value) {
  output += value ? "true" : "false";
}

void appendNumber(std::string &output, double value) {
  if (!std::isfinite(value)) {
    output += "null";
    return;
  }
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::setprecision(17) << value;
  output += stream.str();
}

void appendUInt(std::string &output, std::uint64_t value) {
  output += std::to_string(value);
}

[[nodiscard]] std::string selectionKindName(EditorSelectionKind kind) {
  switch (kind) {
  case EditorSelectionKind::none:
    return "None";
  case EditorSelectionKind::node:
    return "Node";
  case EditorSelectionKind::primitive:
    return "Primitive";
  case EditorSelectionKind::mesh:
    return "Mesh";
  case EditorSelectionKind::material:
    return "Material";
  case EditorSelectionKind::camera:
    return "Camera";
  case EditorSelectionKind::light:
    return "Light";
  }
  return "None";
}

[[nodiscard]] std::string graphKindName(ui::GraphRowKind kind) {
  switch (kind) {
  case ui::GraphRowKind::scene:
    return "Scene";
  case ui::GraphRowKind::node:
    return "Node";
  case ui::GraphRowKind::mesh:
    return "Mesh";
  case ui::GraphRowKind::primitive:
    return "Primitive";
  case ui::GraphRowKind::camera:
    return "Camera";
  case ui::GraphRowKind::light:
    return "Light";
  case ui::GraphRowKind::skin:
    return "Skin";
  }
  return "Unknown";
}

[[nodiscard]] std::string sectionKindName(ui::InspectorSectionKind kind) {
  switch (kind) {
  case ui::InspectorSectionKind::identity:
    return "Identity";
  case ui::InspectorSectionKind::transform:
    return "Transform";
  case ui::InspectorSectionKind::attachments:
    return "Attachments";
  case ui::InspectorSectionKind::primitive:
    return "Primitive";
  case ui::InspectorSectionKind::mesh:
    return "Mesh";
  case ui::InspectorSectionKind::material:
    return "Material";
  case ui::InspectorSectionKind::camera:
    return "Camera";
  case ui::InspectorSectionKind::light:
    return "Light";
  case ui::InspectorSectionKind::materialExtension:
    return "MaterialExtension";
  }
  return "Unknown";
}

[[nodiscard]] UiState mapLifecycle(ui::LifecycleState lifecycle,
                                   const ui::FrameModel &frame) noexcept {
  const auto parsed = parseUiState(ui::lifecycleStateName(lifecycle));
  if (parsed)
    return *parsed;
  // Unknown enum values are rejected rather than silently promoted to Ready.
  if (!frame.operation.error.empty())
    return UiState::error;
  if (frame.rendererBusy)
    return UiState::savingMergingCompacting;
  if (!frame.hasDocument)
    return UiState::noDocument;
  return frame.cleanRevision == frame.revision ? UiState::readyClean
                                                : UiState::readyModified;
}

void appendFrame(std::string &output, const StateSnapshot &snapshot) {
  const ui::FrameModel &frame = snapshot.frame;
  output += "{\"hasDocument\":";
  appendBool(output, frame.hasDocument);
  output += ",\"mutationEnabled\":";
  appendBool(output, frame.mutationEnabled);
  output += ",\"modalPending\":";
  appendBool(output, frame.modalPending);
  output += ",\"rendererBusy\":";
  appendBool(output, frame.rendererBusy);
  output += ",\"revision\":";
  appendUInt(output, frame.revision);
  output += ",\"cleanRevision\":";
  appendUInt(output, frame.cleanRevision);
  output += ",\"selectionRevision\":";
  appendUInt(output, frame.selectionRevision);
  output += ",\"canUndo\":";
  appendBool(output, frame.canUndo);
  output += ",\"canRedo\":";
  appendBool(output, frame.canRedo);
  output += ",\"selectionKind\":";
  appendJsonString(output, selectionKindName(frame.selection.kind()));
  output += ",\"operation\":{";
  bool first = true;
  appendKey(output, "kind", first);
  appendJsonString(output, ui::lifecycleOperationName(frame.operation.kind));
  appendKey(output, "label", first);
  appendJsonString(output, frame.operation.label);
  appendKey(output, "progress", first);
  if (frame.operation.progress)
    appendNumber(output, *frame.operation.progress);
  else
    output += "null";
  appendKey(output, "error", first);
  appendJsonString(output, frame.operation.error);
  appendKey(output, "dismissibleError", first);
  appendBool(output, frame.operation.dismissibleError);
  output += "},\"scenes\":[";
  for (std::size_t sceneIndex = 0; sceneIndex < frame.scenes.size();
       ++sceneIndex) {
    if (sceneIndex != 0)
      output.push_back(',');
    const ui::SceneChoice &scene = frame.scenes[sceneIndex];
    output += "{";
    bool sceneFirst = true;
    appendKey(output, "documentIndex", sceneFirst);
    output += std::to_string(scene.documentIndex);
    appendKey(output, "name", sceneFirst);
    appendJsonString(output, scene.name);
    appendKey(output, "current", sceneFirst);
    appendBool(output, scene.current);
    output += "}";
  }
  output += "],\"graphRows\":[";
  for (std::size_t rowIndex = 0; rowIndex < frame.browser.graphRows.size();
       ++rowIndex) {
    if (rowIndex != 0)
      output.push_back(',');
    const ui::GraphRow &row = frame.browser.graphRows[rowIndex];
    output += "{";
    bool rowFirst = true;
    appendKey(output, "kind", rowFirst);
    appendJsonString(output, graphKindName(row.kind));
    appendKey(output, "depth", rowFirst);
    output += std::to_string(row.depth);
    appendKey(output, "label", rowFirst);
    appendJsonString(output, row.label);
    appendKey(output, "selected", rowFirst);
    appendBool(output, row.selected);
    appendKey(output, "effectiveVisible", rowFirst);
    appendBool(output, row.effectiveVisible);
    output += "}";
  }
  output += "],\"inspectorSections\":[";
  for (std::size_t sectionIndex = 0;
       sectionIndex < frame.inspector.sections.size(); ++sectionIndex) {
    if (sectionIndex != 0)
      output.push_back(',');
    const ui::InspectorSection &section = frame.inspector.sections[sectionIndex];
    output += "{";
    bool sectionFirst = true;
    appendKey(output, "kind", sectionFirst);
    appendJsonString(output, sectionKindName(section.kind));
    appendKey(output, "label", sectionFirst);
    appendJsonString(output, section.label);
    appendKey(output, "defaultOpen", sectionFirst);
    appendBool(output, section.defaultOpen);
    appendKey(output, "fields", sectionFirst);
    output += "[";
    for (std::size_t fieldIndex = 0; fieldIndex < section.fields.size();
         ++fieldIndex) {
      if (fieldIndex != 0)
        output.push_back(',');
      const ui::InspectorField &field = section.fields[fieldIndex];
      output += "{";
      bool fieldFirst = true;
      appendKey(output, "id", fieldFirst);
      output += std::to_string(static_cast<unsigned>(field.id));
      appendKey(output, "label", fieldFirst);
      if (field.labelOverride.empty())
        appendJsonString(output, ui::fieldDescriptor(field.id).label);
      else
        appendJsonString(output, field.labelOverride);
      appendKey(output, "enabled", fieldFirst);
      appendBool(output, field.enabled);
      appendKey(output, "readOnly", fieldFirst);
      appendBool(output, field.readOnly);
      output += "}";
    }
    output += "]}";
  }
  output += "]}";
}

void appendPersistent(std::string &output,
                      const ui::PersistentState &persistent) {
  output += "{\"windows\":{";
  bool first = true;
  appendKey(output, "sceneBrowser", first);
  appendBool(output, persistent.windows.sceneBrowser);
  appendKey(output, "inspector", first);
  appendBool(output, persistent.windows.inspector);
  appendKey(output, "gridSnapSettings", first);
  appendBool(output, persistent.windows.gridSnapSettings);
  output += "},\"viewport\":{";
  first = true;
  appendKey(output, "showGrid", first);
  appendBool(output, persistent.viewport.showGrid);
  appendKey(output, "showGizmo", first);
  appendBool(output, persistent.viewport.showGizmo);
  appendKey(output, "snapEnabled", first);
  appendBool(output, persistent.viewport.snapEnabled);
  appendKey(output, "gridUnit", first);
  appendNumber(output, persistent.viewport.gridUnit);
  appendKey(output, "snapRotationDegrees", first);
  appendNumber(output, persistent.viewport.snapRotationDegrees);
  appendKey(output, "snapScale", first);
  appendNumber(output, persistent.viewport.snapScale);
  output += "},\"browserTab\":";
  output += std::to_string(static_cast<unsigned>(persistent.browserTab));
  output += "}";
}

[[nodiscard]] std::string
runtimeJson(const std::optional<ui::RuntimeActionResult> &runtime) {
  if (!runtime)
    return "null";
  std::string output = "{\"accepted\":";
  appendBool(output, runtime->accepted);
  output += ",\"refresh\":";
  output += std::to_string(static_cast<unsigned>(runtime->refresh));
  output += ",\"error\":";
  appendJsonString(output, runtime->error);
  output += "}";
  return output;
}

[[nodiscard]] std::string commandIdName(ui::CommandId id) {
  switch (id) {
  case ui::CommandId::newScene:
    return "newScene";
  case ui::CommandId::open:
    return "open";
  case ui::CommandId::importMerge:
    return "importMerge";
  case ui::CommandId::openRecent:
    return "openRecent";
  case ui::CommandId::saveAs:
    return "saveAs";
  case ui::CommandId::exit:
    return "exit";
  case ui::CommandId::undo:
    return "undo";
  case ui::CommandId::redo:
    return "redo";
  case ui::CommandId::duplicateSelection:
    return "duplicateSelection";
  case ui::CommandId::deleteSelection:
    return "deleteSelection";
  case ui::CommandId::fitScene:
    return "fitScene";
  case ui::CommandId::fitObject:
    return "fitObject";
  case ui::CommandId::toggleGrid:
    return "toggleGrid";
  case ui::CommandId::toggleGizmo:
    return "toggleGizmo";
  case ui::CommandId::toggleSnap:
    return "toggleSnap";
  case ui::CommandId::showGridSnapSettings:
    return "showGridSnapSettings";
  case ui::CommandId::toggleSceneBrowser:
    return "toggleSceneBrowser";
  case ui::CommandId::toggleInspector:
    return "toggleInspector";
  case ui::CommandId::compactScene:
    return "compactScene";
  case ui::CommandId::clearSelection:
    return "clearSelection";
  case ui::CommandId::count:
    break;
  }
  return "unknown";
}

[[nodiscard]] std::string_view commandSourceName(ui::CommandSource source) {
  switch (source) {
  case ui::CommandSource::menu:
    return "menu";
  case ui::CommandSource::toolbar:
    return "toolbar";
  case ui::CommandSource::shortcut:
    return "shortcut";
  case ui::CommandSource::sceneBrowser:
    return "sceneBrowser";
  case ui::CommandSource::inspector:
    return "inspector";
  case ui::CommandSource::dialog:
    return "dialog";
  case ui::CommandSource::settings:
    return "settings";
  }
  return "invalid";
}

void appendObjectId(std::string &output, EditorObjectId id) {
  output += "{";
  bool first = true;
  appendKey(output, "kind", first);
  appendJsonString(output, editorObjectKindName(id.kind));
  appendKey(output, "slot", first);
  output += std::to_string(id.slot);
  appendKey(output, "generation", first);
  output += std::to_string(id.generation);
  appendKey(output, "authority", first);
  appendUInt(output, id.authority.value);
  output += "}";
}

template <EditorObjectKind Kind>
void appendTypedId(std::string &output, TypedEditorObjectId<Kind> id) {
  appendObjectId(output, id.untyped());
}

void appendNodeTransform(std::string &output, const NodeTransform &transform) {
  output += "{\"translation\":[";
  for (std::size_t component = 0; component < transform.translation.size();
       ++component) {
    if (component != 0)
      output.push_back(',');
    appendNumber(output, transform.translation[component]);
  }
  output += "],\"rotation\":[";
  for (std::size_t component = 0; component < transform.rotation.size();
       ++component) {
    if (component != 0)
      output.push_back(',');
    appendNumber(output, transform.rotation[component]);
  }
  output += "],\"scale\":[";
  for (std::size_t component = 0; component < transform.scale.size();
       ++component) {
    if (component != 0)
      output.push_back(',');
    appendNumber(output, transform.scale[component]);
  }
  output += "]}";
}

void appendSceneTransform(std::string &output,
                          const SceneRootTransform &transform) {
  NodeTransform converted;
  converted.translation = transform.translation;
  converted.rotation = transform.rotation;
  converted.scale = transform.scale;
  appendNodeTransform(output, converted);
}

void appendValue(std::string &output, const ui::Value &value) {
  std::visit(
      [&output](const auto &item) {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::monostate>) {
          output += "null";
        } else if constexpr (std::is_same_v<Item, bool>) {
          appendBool(output, item);
        } else if constexpr (std::is_integral_v<Item>) {
          output += std::to_string(item);
        } else if constexpr (std::is_same_v<Item, double>) {
          appendNumber(output, item);
        } else if constexpr (std::is_same_v<Item, std::string>) {
          appendJsonString(output, item);
        } else if constexpr (std::is_same_v<Item, EditorObjectId>) {
          appendObjectId(output, item);
        } else {
          output += "[";
          for (std::size_t component = 0; component < item.size(); ++component) {
            if (component != 0)
              output.push_back(',');
            appendNumber(output, item[component]);
          }
          output += "]";
        }
      },
      value);
}

void appendSelection(std::string &output,
                     const EditorSelectionSnapshot &selection) {
  output += "{\"authority\":";
  appendUInt(output, selection.authority.value);
  output += ",\"kind\":";
  appendJsonString(output, selectionKindName(selection.kind()));
  output += ",\"value\":";
  std::visit(
      [&output](const auto &value) {
        using Selection = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Selection, std::monostate>) {
          output += "null";
        } else if constexpr (std::is_same_v<Selection, NodeSelection>) {
          output += "{\"scene\":";
          appendTypedId(output, value.scene);
          output += ",\"node\":";
          appendTypedId(output, value.node);
          output += ",\"parent\":";
          if (value.parent)
            appendTypedId(output, *value.parent);
          else
            output += "null";
          output += "}";
        } else if constexpr (std::is_same_v<Selection, PrimitiveSelection>) {
          output += "{\"scene\":";
          appendTypedId(output, value.scene);
          output += ",\"node\":";
          appendTypedId(output, value.node);
          output += ",\"primitive\":";
          appendTypedId(output, value.primitive);
          output += ",\"mesh\":";
          appendTypedId(output, value.mesh);
          output += ",\"instanceOrdinal\":";
          output += std::to_string(value.instanceOrdinal);
          output += "}";
        } else {
          appendTypedId(output, value);
        }
      },
      selection.value);
  output += "}";
}

void appendPrimitive(std::string &output,
                     const std::optional<ui::PrimitiveContext> &primitive) {
  if (!primitive) {
    output += "null";
    return;
  }
  output += "{\"scene\":";
  appendTypedId(output, primitive->scene);
  output += ",\"node\":";
  appendTypedId(output, primitive->node);
  output += ",\"mesh\":";
  appendTypedId(output, primitive->mesh);
  output += ",\"primitive\":";
  appendTypedId(output, primitive->primitive);
  output += ",\"material\":";
  if (primitive->material)
    appendTypedId(output, *primitive->material);
  else
    output += "null";
  output += "}";
}

void appendActionPayload(std::string &output, const ActionPayload &payload) {
  std::visit(
      [&output](const auto &value) {
        using Payload = std::decay_t<decltype(value)>;
        output += "{";
        bool first = true;
        if constexpr (std::is_same_v<Payload, ui::InvokeCommand>) {
          appendKey(output, "command", first);
          appendJsonString(output, commandIdName(value.id));
          appendKey(output, "path", first);
          if (value.path)
            appendJsonString(output, value.path->generic_string());
          else
            output += "null";
        } else if constexpr (std::is_same_v<Payload, ui::SelectObject>) {
          appendKey(output, "selection", first);
          appendSelection(output, value.selection);
        } else if constexpr (std::is_same_v<Payload, ui::ActivateScene>) {
          appendKey(output, "scene", first);
          appendTypedId(output, value.scene);
        } else if constexpr (std::is_same_v<Payload, ui::HierarchyAction>) {
          appendKey(output, "operation", first);
          output += std::to_string(static_cast<unsigned>(value.operation));
          appendKey(output, "scene", first);
          appendTypedId(output, value.scene);
          appendKey(output, "node", first);
          if (value.node)
            appendTypedId(output, *value.node);
          else
            output += "null";
          appendKey(output, "directParent", first);
          if (value.directParent)
            appendTypedId(output, *value.directParent);
          else
            output += "null";
        } else if constexpr (std::is_same_v<Payload, ui::RenameObject>) {
          appendKey(output, "object", first);
          appendObjectId(output, value.object);
          appendKey(output, "name", first);
          appendJsonString(output, value.name);
        } else if constexpr (std::is_same_v<Payload, ui::ReparentNode>) {
          appendKey(output, "sourceScene", first);
          appendTypedId(output, value.source.scene);
          appendKey(output, "sourceNode", first);
          appendTypedId(output, value.source.node);
          appendKey(output, "sourceRevision", first);
          appendUInt(output, value.source.revision);
          appendKey(output, "targetScene", first);
          appendTypedId(output, value.targetScene);
          appendKey(output, "targetParent", first);
          if (value.targetParent)
            appendTypedId(output, *value.targetParent);
          else
            output += "null";
        } else if constexpr (std::is_same_v<Payload, ui::AnimationAction>) {
          appendKey(output, "operation", first);
          output += std::to_string(static_cast<unsigned>(value.operation));
          appendKey(output, "animation", first);
          if (value.animation)
            appendTypedId(output, *value.animation);
          else
            output += "null";
          appendKey(output, "value", first);
          appendNumber(output, value.value);
        } else if constexpr (std::is_same_v<Payload, ui::SelectVariant>) {
          appendKey(output, "variant", first);
          output += std::to_string(value.variant);
        } else if constexpr (std::is_same_v<Payload, ui::PropertyEdit>) {
          appendKey(output, "target", first);
          appendObjectId(output, value.target);
          appendKey(output, "field", first);
          appendJsonString(output, ui::fieldDescriptor(value.field).label);
          appendKey(output, "phase", first);
          output += std::to_string(static_cast<unsigned>(value.phase));
          appendKey(output, "value", first);
          appendValue(output, value.value);
        } else if constexpr (std::is_same_v<Payload, ui::MaterialAction>) {
          appendKey(output, "operation", first);
          output += std::to_string(static_cast<unsigned>(value.operation));
          appendKey(output, "primitive", first);
          appendPrimitive(output, value.primitive);
          appendKey(output, "material", first);
          if (value.material)
            appendTypedId(output, *value.material);
          else
            output += "null";
          appendKey(output, "extension", first);
          appendJsonString(output, value.extension);
        } else if constexpr (std::is_same_v<Payload, ui::CameraAction>) {
          appendKey(output, "operation", first);
          output += std::to_string(static_cast<unsigned>(value.operation));
          appendKey(output, "camera", first);
          appendTypedId(output, value.camera);
          appendKey(output, "node", first);
          if (value.node)
            appendTypedId(output, *value.node);
          else
            output += "null";
        } else if constexpr (std::is_same_v<Payload, ui::SceneTransformAction>) {
          appendKey(output, "scene", first);
          appendTypedId(output, value.scene);
          appendKey(output, "transform", first);
          appendSceneTransform(output, value.transform);
          appendKey(output, "reset", first);
          appendBool(output, value.reset);
        } else if constexpr (std::is_same_v<Payload, ui::RebuildRenderNodes>) {
          appendKey(output, "request", first);
          appendJsonString(output, "fullScene");
        } else if constexpr (std::is_same_v<Payload, ControllerAction>) {
          appendKey(output, "operation", first);
          output += std::to_string(static_cast<unsigned>(value.kind));
          appendKey(output, "node", first);
          if (value.node)
            appendTypedId(output, *value.node);
          else
            output += "null";
          appendKey(output, "transform", first);
          if (value.transform)
            appendNodeTransform(output, *value.transform);
          else
            output += "null";
          appendKey(output, "hovered", first);
          appendBool(output, value.hovered);
          appendKey(output, "physicalRelease", first);
          appendBool(output, value.physicalRelease);
          appendKey(output, "escapePressed", first);
          appendBool(output, value.escapePressed);
          appendKey(output, "focusLost", first);
          appendBool(output, value.focusLost);
          appendKey(output, "sceneIndex", first);
          if (value.sceneIndex)
            output += std::to_string(*value.sceneIndex);
          else
            output += "null";
        } else {
          appendKey(output, "label", first);
          appendJsonString(output, value.label);
        }
        output += "}";
      },
      payload);
}

} // namespace

std::string_view uiStateName(UiState state) noexcept {
  switch (state) {
  case UiState::noDocument:
    return "NoDocument";
  case UiState::loadingCandidate:
    return "LoadingCandidate";
  case UiState::readyClean:
    return "ReadyClean";
  case UiState::readyModified:
    return "ReadyModified";
  case UiState::gizmoHover:
    return "GizmoHover";
  case UiState::gizmoDragging:
    return "GizmoDragging";
  case UiState::modal:
    return "Modal";
  case UiState::savingMergingCompacting:
    return "Saving/Merging/Compacting";
  case UiState::error:
    return "Error";
  }
  return "Invalid";
}

std::optional<UiState> parseUiState(std::string_view name) noexcept {
  for (std::size_t value = 0; value < uiStateCount; ++value) {
    const UiState state = static_cast<UiState>(value);
    if (uiStateName(state) == name)
      return state;
  }
  return std::nullopt;
}

std::string_view actionKindName(ActionKind kind) noexcept {
  switch (kind) {
  case ActionKind::invokeCommand:
    return "invokeCommand";
  case ActionKind::selectObject:
    return "selectObject";
  case ActionKind::activateScene:
    return "activateScene";
  case ActionKind::hierarchy:
    return "hierarchy";
  case ActionKind::rename:
    return "rename";
  case ActionKind::reparent:
    return "reparent";
  case ActionKind::animation:
    return "animation";
  case ActionKind::selectVariant:
    return "selectVariant";
  case ActionKind::property:
    return "property";
  case ActionKind::material:
    return "material";
  case ActionKind::camera:
    return "camera";
  case ActionKind::sceneTransform:
    return "sceneTransform";
  case ActionKind::rebuildRenderNodes:
    return "rebuildRenderNodes";
  case ActionKind::controllerAction:
    return "controllerAction";
  case ActionKind::captureState:
    return "captureState";
  case ActionKind::screenshotMarker:
    return "screenshotMarker";
  }
  return "invalid";
}

std::string_view actionStatusName(ActionStatus status) noexcept {
  switch (status) {
  case ActionStatus::accepted:
    return "accepted";
  case ActionStatus::rejected:
    return "rejected";
  case ActionStatus::blocked:
    return "blocked";
  case ActionStatus::invalid:
    return "invalid";
  case ActionStatus::disabled:
    return "disabled";
  }
  return "invalid";
}

std::string_view replayStatusName(ReplayStatus status) noexcept {
  switch (status) {
  case ReplayStatus::passed:
    return "passed";
  case ReplayStatus::failed:
    return "failed";
  case ReplayStatus::invalid:
    return "invalid";
  case ReplayStatus::blocked:
    return "blocked";
  }
  return "invalid";
}

std::string TestAction::toJson() const {
  std::string output = "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"step\":";
  appendUInt(output, step);
  output += ",\"id\":";
  appendJsonString(output, id);
  output += ",\"kind\":";
  appendJsonString(output, actionKindName(EditorTestDriver::kindOf(payload)));
  output += ",\"source\":";
  appendJsonString(output, commandSourceName(source));
  output += ",\"payload\":";
  appendActionPayload(output, payload);
  output += "}";
  return output;
}

std::string ScriptStep::toJson() const {
  std::string output = "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"id\":";
  appendJsonString(output, id);
  output += ",\"action\":";
  output += action.toJson();
  output += ",\"expectedState\":";
  if (expectedState)
    appendJsonString(output, uiStateName(*expectedState));
  else
    output += "null";
  output += ",\"expectedAccepted\":";
  if (expectedAccepted)
    appendBool(output, *expectedAccepted);
  else
    output += "null";
  output += "}";
  return output;
}

std::string TestScript::toJson() const {
  std::string output = "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"id\":";
  appendJsonString(output, id);
  output += ",\"description\":";
  appendJsonString(output, description);
  output += ",\"coverageComplete\":";
  appendBool(output, coverageComplete());
  output += ",\"coverageGaps\":[";
  for (std::size_t gap = 0; gap < coverageGaps.size(); ++gap) {
    if (gap != 0)
      output.push_back(',');
    appendJsonString(output, coverageGaps[gap]);
  }
  output += "],\"steps\":[";
  for (std::size_t step = 0; step < steps.size(); ++step) {
    if (step != 0)
      output.push_back(',');
    output += steps[step].toJson();
  }
  output += "]}";
  return output;
}

std::string AuthoritySummary::toJson() const {
  std::string output = "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"hasDocument\":";
  appendBool(output, hasDocument);
  output += ",\"hasProjection\":";
  appendBool(output, hasProjection);
  output += ",\"documentRevision\":";
  appendUInt(output, documentRevision);
  output += ",\"cleanRevision\":";
  appendUInt(output, cleanRevision);
  output += ",\"lifecycleRevision\":";
  appendUInt(output, lifecycleRevision);
  output += ",\"mappingEpoch\":";
  appendUInt(output, mappingEpoch);
  output += ",\"sceneActivationEpoch\":";
  appendUInt(output, sceneActivationEpoch);
  output += ",\"selectionRevision\":";
  appendUInt(output, selectionRevision);
  output += ",\"undoCount\":";
  appendUInt(output, undoCount);
  output += ",\"redoCount\":";
  appendUInt(output, redoCount);
  output += ",\"sourcePath\":";
  appendJsonString(output, sourcePath);
  output += ",\"semanticHash\":";
  appendJsonString(output, semanticHash);
  output += ",\"documentDigest\":";
  appendJsonString(output, documentDigest);
  output += ",\"projectionDigest\":";
  appendJsonString(output, projectionDigest);
  output += ",\"selectionKind\":";
  appendJsonString(output, selectionKind);
  output += "}";
  return output;
}

std::string StateSnapshot::toJson() const {
  std::string output;
  output.reserve(2048);
  output += "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"state\":";
  appendJsonString(output, uiStateName(state));
  output += ",\"captureLabel\":";
  appendJsonString(output, captureLabel);
  output += ",\"frame\":";
  appendFrame(output, *this);
  output += ",\"persistent\":";
  appendPersistent(output, persistent);
  output += ",\"authority\":";
  output += authority ? authority->toJson() : "null";
  output += ",\"commands\":[";
  for (std::size_t commandIndex = 0; commandIndex < commands.size();
       ++commandIndex) {
    if (commandIndex != 0)
      output.push_back(',');
    const CommandManifestEntry &command = commands[commandIndex];
    output += "{";
    bool first = true;
    appendKey(output, "id", first);
    output += std::to_string(static_cast<unsigned>(command.id));
    appendKey(output, "name", first);
    appendJsonString(output, commandIdName(command.id));
    appendKey(output, "menu", first);
    appendJsonString(output, command.menu);
    appendKey(output, "label", first);
    appendJsonString(output, command.label);
    appendKey(output, "shortcut", first);
    appendJsonString(output, command.shortcut);
    appendKey(output, "enabled", first);
    appendBool(output, command.enabled);
    appendKey(output, "checked", first);
    appendBool(output, command.checked);
    output += "}";
  }
  output += "]}";
  return output;
}

std::string ActionResult::toJson() const {
  std::string output;
  output += "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"sequence\":";
  appendUInt(output, sequence);
  output += ",\"actionId\":";
  appendJsonString(output, actionId);
  output += ",\"kind\":";
  appendJsonString(output, actionKindName(kind));
  output += ",\"status\":";
  appendJsonString(output, actionStatusName(status));
  output += ",\"executed\":";
  appendBool(output, executed);
  output += ",\"accepted\":";
  appendBool(output, accepted);
  output += ",\"error\":";
  appendJsonString(output, error);
  output += ",\"runtime\":";
  output += runtimeJson(runtime);
  output += ",\"beforeState\":";
  output += beforeState.toJson();
  output += ",\"afterState\":";
  output += state.toJson();
  output += "}";
  return output;
}

std::string ReplayResult::toJson() const {
  std::string output;
  output += "{\"schemaVersion\":";
  output += std::to_string(schemaVersion);
  output += ",\"scriptId\":";
  appendJsonString(output, scriptId);
  output += ",\"status\":";
  appendJsonString(output, replayStatusName(status));
  output += ",\"passed\":";
  appendBool(output, passed);
  output += ",\"error\":";
  appendJsonString(output, error);
  output += ",\"steps\":[";
  for (std::size_t stepIndex = 0; stepIndex < steps.size(); ++stepIndex) {
    if (stepIndex != 0)
      output.push_back(',');
    output += steps[stepIndex].toJson();
  }
  output += "]}";
  return output;
}

EditorTestDriver::EditorTestDriver(Callbacks callbacks, Options options)
    : m_callbacks(std::move(callbacks)), m_options(options) {}

EditorTestDriver::EditorTestDriver(bool enabled) noexcept {
  m_options.enabled = enabled;
}

void EditorTestDriver::setFrameModel(ui::FrameModel frame) {
  m_frame = std::move(frame);
}

void EditorTestDriver::setPersistentState(ui::PersistentState state) noexcept {
  m_persistent = std::move(state);
}

StateSnapshot EditorTestDriver::readSnapshot(std::string_view label) const {
  ui::FrameModel frame = m_frame;
  ui::PersistentState persistent = m_persistent;
  if (m_callbacks.frame) {
    try {
      frame = m_callbacks.frame();
    } catch (...) {
      frame.operation.error = "frame provider threw";
      frame.operation.dismissibleError = true;
    }
  }
  if (m_callbacks.persistent) {
    try {
      persistent = m_callbacks.persistent();
    } catch (...) {
      frame.operation.error = "persistent-state provider threw";
      frame.operation.dismissibleError = true;
    }
  }

  StateSnapshot snapshot;
  snapshot.state = mapLifecycle(frame.lifecycle, frame);
  snapshot.frame = std::move(frame);
  snapshot.persistent = std::move(persistent);
  snapshot.captureLabel = std::string(label);
  if (m_callbacks.authority) {
    try {
      snapshot.authority = m_callbacks.authority();
    } catch (...) {
      AuthoritySummary failed;
      failed.selectionKind = "authority provider threw";
      snapshot.authority = std::move(failed);
    }
  }
  snapshot.commands.reserve(ui::commandDescriptors().size());
  for (const ui::CommandDescriptor &descriptor : ui::commandDescriptors()) {
    const ui::CommandEnablement enablement = ui::evaluateCommand(
        descriptor.id, snapshot.frame, snapshot.persistent.windows,
        snapshot.persistent.viewport);
    snapshot.commands.push_back(CommandManifestEntry{
        descriptor.id,
        std::string(descriptor.menu),
        std::string(descriptor.label),
        std::string(descriptor.shortcut),
        enablement.enabled,
        enablement.checked,
    });
  }
  return snapshot;
}

StateSnapshot EditorTestDriver::snapshot(std::string_view label) const {
  return readSnapshot(label);
}

StateSnapshot EditorTestDriver::captureState(std::string_view label) {
  StateSnapshot captured = readSnapshot(label);
  if (!m_options.enabled)
    return captured;
  m_capturedStates.push_back(captured);
  if (m_callbacks.stateCaptured) {
    try {
      m_callbacks.stateCaptured(m_capturedStates.back());
    } catch (...) {
    }
  }
  return captured;
}

std::span<const StateSnapshot>
EditorTestDriver::capturedStates() const noexcept {
  return m_capturedStates;
}

std::span<const ScreenshotMarker>
EditorTestDriver::screenshotMarkers() const noexcept {
  return m_screenshotMarkers;
}

std::span<const CommandManifestEntry>
EditorTestDriver::commandManifest() const {
  m_manifest.clear();
  const StateSnapshot current = readSnapshot();
  m_manifest = current.commands;
  return m_manifest;
}

std::string EditorTestDriver::commandManifestJson() const {
  const auto manifest = commandManifest();
  std::string output = "{\"schemaVersion\":1,\"commands\":[";
  for (std::size_t commandIndex = 0; commandIndex < manifest.size();
       ++commandIndex) {
    if (commandIndex != 0)
      output.push_back(',');
    const CommandManifestEntry &command = manifest[commandIndex];
    output += "{";
    bool first = true;
    appendKey(output, "id", first);
    output += std::to_string(static_cast<unsigned>(command.id));
    appendKey(output, "name", first);
    appendJsonString(output, commandIdName(command.id));
    appendKey(output, "menu", first);
    appendJsonString(output, command.menu);
    appendKey(output, "label", first);
    appendJsonString(output, command.label);
    appendKey(output, "shortcut", first);
    appendJsonString(output, command.shortcut);
    appendKey(output, "enabled", first);
    appendBool(output, command.enabled);
    appendKey(output, "checked", first);
    appendBool(output, command.checked);
    output += "}";
  }
  output += "]}";
  return output;
}

std::string EditorTestDriver::scriptManifestJson() const {
  std::string output =
      "{\"schemaVersion\":1,\"kind\":\"mgif.editor-ui-script-manifest\",";
  output += "\"scriptCount\":" + std::to_string(m_scripts.size()) +
            ",\"scripts\":[";
  for (std::size_t script = 0; script < m_scripts.size(); ++script) {
    if (script != 0)
      output.push_back(',');
    output += m_scripts[script].toJson();
  }
  output += "]}";
  return output;
}

std::string
EditorTestDriver::surfaceManifestJson(const StateSnapshot &snapshot) {
  std::string output =
      "{\"schemaVersion\":1,\"kind\":\"mgif.editor-ui-surface-manifest\",";
  output +=
      "\"commandDescriptorSource\":\"commandDescriptors/evaluateCommand\",";
  output += "\"menuOrder\":[\"File\",\"Edit\",\"View\",\"Windows\",\"Tools\"],";
  output += "\"commands\":[";
  for (std::size_t index = 0; index < snapshot.commands.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const CommandManifestEntry &command = snapshot.commands[index];
    output += "{\"order\":" + std::to_string(index) + ",\"id\":";
    appendJsonString(output, commandIdName(command.id));
    output += ",\"menu\":";
    appendJsonString(output, command.menu);
    output += ",\"label\":";
    appendJsonString(output, command.label);
    output += ",\"shortcut\":";
    appendJsonString(output, command.shortcut);
    output += ",\"enabled\":";
    appendBool(output, command.enabled);
    output += ",\"checked\":";
    appendBool(output, command.checked);
    output += "}";
  }
  output += "],\"toolbar\":[";
  struct ToolbarEntry final {
    ui::CommandId command;
    std::string_view icon;
    std::string_view tooltip;
  };
  const ui::IconSet icons;
  const std::array<ToolbarEntry, 5> toolbar = {{
      {ui::CommandId::toggleSceneBrowser, icons.sceneBrowser,
       "Toggle Scene Browser Window (F2)"},
      {ui::CommandId::toggleInspector, icons.inspector,
       "Toggle Inspector Window (F4)"},
      {ui::CommandId::toggleGrid, icons.grid, "Toggle Grid (G in viewport)"},
      {ui::CommandId::toggleGizmo, icons.gizmo,
       "Toggle Gizmo (T in viewport)"},
      {ui::CommandId::toggleSnap, icons.snap, "Toggle Snap"},
  }};
  for (std::size_t index = 0; index < toolbar.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const ToolbarEntry &entry = toolbar[index];
    const auto command = std::find_if(
        snapshot.commands.begin(), snapshot.commands.end(),
        [&entry](const CommandManifestEntry &candidate) {
          return candidate.id == entry.command;
        });
    output += "{\"order\":" + std::to_string(index) + ",\"command\":";
    appendJsonString(output, commandIdName(entry.command));
    output += ",\"label\":";
    appendJsonString(output, ui::commandDescriptor(entry.command).label);
    output += ",\"icon\":";
    appendJsonString(output, entry.icon);
    output += ",\"tooltip\":";
    appendJsonString(output, entry.tooltip);
    output += ",\"source\":\"toolbar\",\"enabled\":";
    appendBool(output, command != snapshot.commands.end() && command->enabled);
    output += ",\"checked\":";
    appendBool(output, command != snapshot.commands.end() && command->checked);
    output += "}";
  }
  output += "],\"contexts\":{";
  const bool contextEnabled =
      snapshot.frame.hasDocument && snapshot.frame.mutationEnabled &&
      !snapshot.frame.rendererBusy && !snapshot.frame.modalPending &&
      (snapshot.state == UiState::readyClean ||
       snapshot.state == UiState::readyModified ||
       snapshot.state == UiState::gizmoHover ||
       snapshot.state == UiState::gizmoDragging);
  const auto appendContext =
      [&output](
          std::string_view owner,
          std::initializer_list<std::pair<std::string_view, std::string_view>>
              entries,
          bool ownerEnabled,
          bool &firstContext) {
        if (!firstContext)
          output.push_back(',');
        firstContext = false;
        appendJsonString(output, owner);
        output += ":[";
        std::size_t order = 0;
        for (const auto &[label, result] : entries) {
          if (order != 0)
            output.push_back(',');
          output += "{\"order\":" + std::to_string(order++) + ",\"label\":";
          appendJsonString(output, label);
          output += ",\"enabled\":";
          appendBool(output, label == "separator" ? false : ownerEnabled);
          output += ",\"result\":";
          appendJsonString(output, result);
          output += "}";
        }
        output += "]";
      };
  const auto hasGraphKind = [&snapshot](ui::GraphRowKind kind) {
    return std::any_of(snapshot.frame.browser.graphRows.begin(),
                       snapshot.frame.browser.graphRows.end(),
                       [kind](const ui::GraphRow &row) {
                         return row.kind == kind && row.enabled;
                       });
  };
  bool firstContext = true;
  appendContext("Scene",
                {{"Add Node", "HierarchyAction.addRootNode"},
                 {"separator", "none"},
                 {"Add Point Light", "HierarchyAction.addPointLight"},
                 {"Add Directional Light",
                  "HierarchyAction.addDirectionalLight"},
                 {"Add Spot Light", "HierarchyAction.addSpotLight"}},
                contextEnabled && !snapshot.frame.scenes.empty(),
                firstContext);
  appendContext("Node",
                {{"Duplicate", "HierarchyAction.duplicateNode"},
                 {"Delete", "HierarchyAction.requestDeleteNode"},
                 {"separator", "none"},
                 {"Add Child/Empty Node", "HierarchyAction.addChildNode"},
                 {"Add Child/separator", "none"},
                 {"Add Child/Point Light", "HierarchyAction.addPointLight"},
                 {"Add Child/Directional Light",
                  "HierarchyAction.addDirectionalLight"},
                 {"Add Child/Spot Light", "HierarchyAction.addSpotLight"},
                 {"separator", "none"},
                 {"Rename", "RenameObject"}},
                contextEnabled && hasGraphKind(ui::GraphRowKind::node),
                firstContext);
  appendContext("Mesh", {{"Rename", "RenameObject"}},
                contextEnabled && hasGraphKind(ui::GraphRowKind::mesh),
                firstContext);
  appendContext("Primitive",
                {{"Assign Material...", "MaterialAction.focusAssignment"}},
                contextEnabled &&
                    hasGraphKind(ui::GraphRowKind::primitive) &&
                    !snapshot.frame.materials.empty(),
                firstContext);
  appendContext("Material",
                {{"Rename", "RenameObject"},
                 {"separator", "none"},
                 {"Copy Material ID", "MaterialAction.copyReference"}},
                contextEnabled && !snapshot.frame.materials.empty(),
                firstContext);
  output += "},\"screenshotEvidence\":false}";
  return output;
}

ActionKind EditorTestDriver::kindOf(const ActionPayload &payload) noexcept {
  return std::visit(
      [](const auto &value) noexcept {
        using Payload = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Payload, ui::InvokeCommand>)
          return ActionKind::invokeCommand;
        else if constexpr (std::is_same_v<Payload, ui::SelectObject>)
          return ActionKind::selectObject;
        else if constexpr (std::is_same_v<Payload, ui::ActivateScene>)
          return ActionKind::activateScene;
        else if constexpr (std::is_same_v<Payload, ui::HierarchyAction>)
          return ActionKind::hierarchy;
        else if constexpr (std::is_same_v<Payload, ui::RenameObject>)
          return ActionKind::rename;
        else if constexpr (std::is_same_v<Payload, ui::ReparentNode>)
          return ActionKind::reparent;
        else if constexpr (std::is_same_v<Payload, ui::AnimationAction>)
          return ActionKind::animation;
        else if constexpr (std::is_same_v<Payload, ui::SelectVariant>)
          return ActionKind::selectVariant;
        else if constexpr (std::is_same_v<Payload, ui::PropertyEdit>)
          return ActionKind::property;
        else if constexpr (std::is_same_v<Payload, ui::MaterialAction>)
          return ActionKind::material;
        else if constexpr (std::is_same_v<Payload, ui::CameraAction>)
          return ActionKind::camera;
        else if constexpr (std::is_same_v<Payload, ui::SceneTransformAction>)
          return ActionKind::sceneTransform;
        else if constexpr (std::is_same_v<Payload, ui::RebuildRenderNodes>)
          return ActionKind::rebuildRenderNodes;
        else if constexpr (std::is_same_v<Payload, ControllerAction>)
          return ActionKind::controllerAction;
        else if constexpr (std::is_same_v<Payload, CaptureStateAction>)
          return ActionKind::captureState;
        else
          return ActionKind::screenshotMarker;
      },
      payload);
}

bool EditorTestDriver::isDomainPayload(const ActionPayload &payload) noexcept {
  return kindOf(payload) <= ActionKind::rebuildRenderNodes;
}

ActionResult EditorTestDriver::invalidAction(const TestAction &action,
                                             ActionStatus status,
                                             std::string message) const {
  ActionResult result;
  result.sequence = action.step;
  result.actionId = action.id;
  result.kind = kindOf(action.payload);
  result.status = status;
  result.error = std::move(message);
  result.beforeState = readSnapshot();
  result.state = result.beforeState;
  return result;
}

ActionResult EditorTestDriver::dispatchDomainAction(const TestAction &action,
                                                    StateSnapshot before) {
  ActionResult result;
  result.schemaVersion = ActionResult::kSchemaVersion;
  result.sequence = action.step;
  result.actionId = action.id;
  result.kind = kindOf(action.payload);
  result.beforeState = before;
  result.state = std::move(before);

  if (!m_callbacks.dispatch) {
    result.status = ActionStatus::blocked;
    result.error = "typed dispatch callback is not installed";
    return result;
  }

  ui::UiCommand command;
  command.sequence = action.step;
  command.source = action.source;
  std::visit(
      [&command](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (!std::is_same_v<Payload, CaptureStateAction> &&
                      !std::is_same_v<Payload, ScreenshotMarkerAction> &&
                      !std::is_same_v<Payload, ControllerAction>) {
          command.payload = payload;
        }
      },
      action.payload);

  if (const auto *invoke = std::get_if<ui::InvokeCommand>(&action.payload)) {
    const auto found = std::find_if(
        result.state.commands.begin(), result.state.commands.end(),
        [invoke](const CommandManifestEntry &entry) {
          return entry.id == invoke->id;
        });
    if (found == result.state.commands.end()) {
      result.status = ActionStatus::invalid;
      result.error = "command is outside the closed manifest";
      return result;
    }
    if (!found->enabled) {
      result.status = ActionStatus::disabled;
      result.error = "command is disabled in the captured UI state";
      return result;
    }
  }

  try {
    result.runtime = m_callbacks.dispatch(command);
  } catch (...) {
    result.status = ActionStatus::rejected;
    result.error = "typed dispatch callback threw";
    result.executed = true;
    result.state = readSnapshot();
    return result;
  }
  result.executed = true;
  result.accepted = result.runtime->accepted;
  result.status = result.accepted ? ActionStatus::accepted
                                  : ActionStatus::rejected;
  result.error = result.runtime->error;
  result.state = readSnapshot();
  return result;
}

ActionResult EditorTestDriver::dispatch(const TestAction &action) {
  const ActionKind kind = kindOf(action.payload);
  if (!m_options.enabled)
    return invalidAction(action, ActionStatus::blocked, "driver is disabled");
  if (action.schemaVersion != TestAction::kSchemaVersion)
    return invalidAction(action, ActionStatus::invalid,
                         "unsupported action schema version");
  if (!validText(action.id))
    return invalidAction(action, ActionStatus::invalid,
                         "action id must be non-empty and control-free");
  if (kind == ActionKind::captureState) {
    const auto *capture = std::get_if<CaptureStateAction>(&action.payload);
    if (capture == nullptr || !validText(capture->label))
      return invalidAction(action, ActionStatus::invalid,
                           "capture label is invalid");
  }
  if (kind == ActionKind::screenshotMarker) {
    const auto *marker = std::get_if<ScreenshotMarkerAction>(&action.payload);
    if (marker == nullptr || !validText(marker->label))
      return invalidAction(action, ActionStatus::invalid,
                           "screenshot marker label is invalid");
  }
  if (const auto *control = std::get_if<ControllerAction>(&action.payload)) {
    const bool needsNode =
        control->kind == ControllerActionKind::beginGizmoTransform ||
        control->kind == ControllerActionKind::previewGizmoTransform ||
        control->kind == ControllerActionKind::finishGizmoTransform;
    if ((needsNode && (!control->node || !control->node->valid())) ||
        (control->kind == ControllerActionKind::previewGizmoTransform &&
         !control->transform)) {
      return invalidAction(action, ActionStatus::invalid,
                           "controller action has incomplete typed arguments");
    }
    if (control->kind == ControllerActionKind::activateSceneByIndex &&
        !control->sceneIndex) {
      return invalidAction(action, ActionStatus::invalid,
                           "scene activation action has no scene index");
    }
  }
  if (m_options.strictSequence && action.step != m_nextSequence)
    return invalidAction(action, ActionStatus::invalid,
                         "action sequence is not contiguous");

  const StateSnapshot before = readSnapshot();
  ActionResult result;
  if (const auto *control = std::get_if<ControllerAction>(&action.payload)) {
    result.schemaVersion = ActionResult::kSchemaVersion;
    result.sequence = action.step;
    result.actionId = action.id;
    result.kind = kind;
    result.beforeState = before;
    result.state = before;
    if (!m_callbacks.control) {
      result.status = ActionStatus::blocked;
      result.error = "typed controller callback is not installed";
    } else {
      try {
        result.runtime = m_callbacks.control(*control);
        result.executed = true;
        result.accepted = result.runtime->accepted;
        result.status = result.accepted ? ActionStatus::accepted
                                        : ActionStatus::rejected;
        result.error = result.runtime->error;
      } catch (...) {
        result.executed = true;
        result.status = ActionStatus::rejected;
        result.error = "typed controller callback threw";
      }
      result.state = readSnapshot();
    }
  } else if (const auto *capture =
                 std::get_if<CaptureStateAction>(&action.payload)) {
    result.schemaVersion = ActionResult::kSchemaVersion;
    result.sequence = action.step;
    result.actionId = action.id;
    result.kind = kind;
    result.beforeState = before;
    result.status = ActionStatus::accepted;
    result.executed = true;
    result.accepted = true;
    result.state = captureState(capture->label);
  } else if (const auto *marker =
                 std::get_if<ScreenshotMarkerAction>(&action.payload)) {
    result.schemaVersion = ActionResult::kSchemaVersion;
    result.sequence = action.step;
    result.actionId = action.id;
    result.kind = kind;
    result.beforeState = before;
    ScreenshotMarker typedMarker;
    typedMarker.sequence = action.step;
    typedMarker.label = marker->label;
    typedMarker.state = before.state;
    m_screenshotMarkers.push_back(typedMarker);
    if (m_callbacks.screenshot) {
      try {
        m_callbacks.screenshot(m_screenshotMarkers.back());
      } catch (...) {
        result.status = ActionStatus::rejected;
        result.error = "screenshot callback threw";
        result.state = readSnapshot();
        ++m_nextSequence;
        return result;
      }
    }
    result.status = ActionStatus::accepted;
    result.executed = true;
    result.accepted = true;
    result.state = readSnapshot(marker->label);
  } else {
    result = dispatchDomainAction(action, before);
  }
  ++m_nextSequence;
  return result;
}

ActionResult EditorTestDriver::dispatch(const ScriptStep &step) {
  ActionResult result = dispatch(step.action);
  if (step.expectedState && result.state.state != *step.expectedState) {
    result.status = ActionStatus::invalid;
    result.accepted = false;
    result.error = "post-action state did not match the script expectation";
  }
  if (step.expectedAccepted && result.accepted != *step.expectedAccepted) {
    result.status = ActionStatus::invalid;
    result.accepted = false;
    result.error = "post-action acceptance did not match the script expectation";
  }
  return result;
}

bool EditorTestDriver::markScreenshot(std::string_view label) {
  if (!m_options.enabled || !validText(label))
    return false;
  ScreenshotMarker marker;
  marker.sequence = m_nextSequence++;
  marker.label = std::string(label);
  marker.state = readSnapshot().state;
  m_screenshotMarkers.push_back(marker);
  if (m_callbacks.screenshot) {
    try {
      m_callbacks.screenshot(m_screenshotMarkers.back());
    } catch (...) {
      return false;
    }
  }
  return true;
}

bool EditorTestDriver::knownScenario(std::string_view id) noexcept {
  static constexpr std::array<std::string_view, 8> ids = {
      "U01-empty-new-open", "U02-hierarchy", "U03-transform-gizmo",
      "U04-material", "U05-camera-light-vis", "U06-merge-compact-save",
      "U07-modified-failures", "U08-large-unicode-dpi"};
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

bool EditorTestDriver::validateScript(const TestScript &script,
                                      std::string *error) const {
  const auto reject = [error](std::string message) {
    if (error)
      *error = std::move(message);
    return false;
  };
  if (script.schemaVersion != TestScript::kSchemaVersion)
    return reject("unsupported script schema version");
  if (!validText(script.id))
    return reject("script id must be non-empty and control-free");
  if (!validText(script.description))
    return reject("script description must be non-empty and control-free");
  if (!m_options.allowCustomScripts && !knownScenario(script.id))
    return reject("script id is outside the closed U01-U08 registry");
  if (script.steps.empty() || script.steps.size() > 1024)
    return reject("script must contain between 1 and 1024 steps");
  std::vector<std::string_view> ids;
  std::vector<std::string_view> actionIds;
  ids.reserve(script.steps.size());
  actionIds.reserve(script.steps.size());
  for (const std::string &gap : script.coverageGaps) {
    if (!validText(gap))
      return reject("coverage gaps must be non-empty and control-free");
  }
  for (std::size_t indexStep = 0; indexStep < script.steps.size();
       ++indexStep) {
    const ScriptStep &step = script.steps[indexStep];
    if (step.schemaVersion != ScriptStep::kSchemaVersion)
      return reject("unsupported step schema version");
    if (!validText(step.id))
      return reject("step id must be non-empty and control-free");
    if (std::find(ids.begin(), ids.end(), step.id) != ids.end())
      return reject("step ids must be unique");
    ids.push_back(step.id);
    if (step.action.schemaVersion != TestAction::kSchemaVersion)
      return reject("unsupported action schema version");
    if (!validText(step.action.id))
      return reject("action id must be non-empty and control-free");
    if (std::find(actionIds.begin(), actionIds.end(), step.action.id) !=
        actionIds.end())
      return reject("action ids must be unique");
    actionIds.push_back(step.action.id);
    if (step.action.step != indexStep)
      return reject("step action sequence must be contiguous");
    if (static_cast<unsigned>(step.action.source) >
        static_cast<unsigned>(ui::CommandSource::settings))
      return reject("command source is invalid");
    if (!step.expectedState ||
        static_cast<std::size_t>(*step.expectedState) >= uiStateCount)
      return reject("expected state is invalid");
    if (!step.expectedAccepted)
      return reject("every step requires an explicit acceptance expectation");
    if (const auto *capture =
            std::get_if<CaptureStateAction>(&step.action.payload);
        capture && !validText(capture->label))
      return reject("capture label is invalid");
    if (const auto *marker =
            std::get_if<ScreenshotMarkerAction>(&step.action.payload);
        marker && !validText(marker->label))
      return reject("screenshot marker label is invalid");
    if (const auto *control =
            std::get_if<ControllerAction>(&step.action.payload)) {
      if (static_cast<unsigned>(control->kind) >
          static_cast<unsigned>(ControllerActionKind::activateSceneByIndex))
        return reject("controller action kind is invalid");
      const bool needsNode =
          control->kind == ControllerActionKind::beginGizmoTransform ||
          control->kind == ControllerActionKind::previewGizmoTransform ||
          control->kind == ControllerActionKind::finishGizmoTransform;
      if ((needsNode && (!control->node || !control->node->valid())) ||
          (control->kind == ControllerActionKind::previewGizmoTransform &&
           !control->transform))
        return reject("controller action has incomplete typed arguments");
      if (control->kind == ControllerActionKind::activateSceneByIndex &&
          !control->sceneIndex)
        return reject("scene activation action has no scene index");
    }
    if (const auto *invoke =
            std::get_if<ui::InvokeCommand>(&step.action.payload);
        invoke && static_cast<unsigned>(invoke->id) >=
                      static_cast<unsigned>(ui::CommandId::count))
      return reject("count is not a dispatchable command");
  }
  return true;
}

ScriptRegistrationResult EditorTestDriver::registerScript(TestScript script) {
  std::string error;
  if (!validateScript(script, &error))
    return {false, std::move(error)};
  const auto found = std::find_if(
      m_scripts.begin(), m_scripts.end(),
      [&script](const TestScript &existing) { return existing.id == script.id; });
  if (found != m_scripts.end())
    return {false, "script id is already registered"};
  m_scripts.push_back(std::move(script));
  return {true, {}};
}

ScriptRegistrationResult EditorTestDriver::registerDefaultScripts(
    const ScenarioBindings &bindings) {
  std::vector<std::string> errors;
  std::vector<TestScript> defaults = defaultScripts(bindings, &errors);
  if (!errors.empty())
    return {false, errors.front()};
  for (TestScript script : std::move(defaults)) {
    if (hasScript(script.id))
      continue;
    const ScriptRegistrationResult result = registerScript(std::move(script));
    if (!result.accepted)
      return result;
  }
  return {true, {}};
}

bool EditorTestDriver::hasScript(std::string_view id) const noexcept {
  return std::any_of(m_scripts.begin(), m_scripts.end(),
                     [id](const TestScript &script) { return script.id == id; });
}

std::span<const TestScript> EditorTestDriver::scripts() const noexcept {
  return m_scripts;
}

ReplayResult EditorTestDriver::replay(std::string_view scriptId) {
  const auto found = std::find_if(
      m_scripts.begin(), m_scripts.end(),
      [scriptId](const TestScript &script) { return script.id == scriptId; });
  if (found == m_scripts.end()) {
    ReplayResult result;
    result.scriptId = std::string(scriptId);
    result.status = ReplayStatus::invalid;
    result.error = "script is not registered";
    return result;
  }
  return replay(*found);
}

ReplayResult EditorTestDriver::replay(const TestScript &script) {
  ReplayResult replayResult;
  replayResult.scriptId = script.id;
  std::string validationError;
  if (!validateScript(script, &validationError)) {
    replayResult.status = ReplayStatus::invalid;
    replayResult.error = std::move(validationError);
    return replayResult;
  }
  if (!m_options.enabled) {
    replayResult.status = ReplayStatus::blocked;
    replayResult.error = "driver is disabled";
    return replayResult;
  }

  for (const ScriptStep &declaredStep : script.steps) {
    TestAction action = declaredStep.action;
    action.step = m_nextSequence;
    ActionResult result = dispatch(action);
    if (declaredStep.expectedState &&
        result.state.state != *declaredStep.expectedState) {
      result.status = ActionStatus::invalid;
      result.accepted = false;
      result.error = "post-action state did not match the script expectation";
    }
    if (declaredStep.expectedAccepted &&
        result.accepted != *declaredStep.expectedAccepted) {
      result.status = ActionStatus::invalid;
      result.accepted = false;
      result.error = "post-action acceptance did not match the script expectation";
    }
    replayResult.steps.push_back(std::move(result));
    const ActionResult &last = replayResult.steps.back();
    const bool expectedRejection = declaredStep.expectedAccepted &&
                                   !*declaredStep.expectedAccepted;
    if (last.status == ActionStatus::invalid ||
        last.status == ActionStatus::blocked ||
        last.status == ActionStatus::disabled ||
        (last.status == ActionStatus::rejected && !expectedRejection)) {
      replayResult.status = last.status == ActionStatus::blocked
                                ? ReplayStatus::blocked
                                : ReplayStatus::failed;
      replayResult.error = last.error.empty() ? "script step failed closed"
                                              : last.error;
      return replayResult;
    }
  }
  replayResult.status = ReplayStatus::passed;
  replayResult.passed = true;
  return replayResult;
}

std::optional<TestScript>
EditorTestDriver::defaultScript(std::string_view id,
                                const ScenarioBindings &bindings,
                                std::string *error) {
  const auto fail = [error](std::string message)
      -> std::optional<TestScript> {
    if (error)
      *error = std::move(message);
    return std::nullopt;
  };
  if (!knownScenario(id))
    return fail("scenario id is outside U01-U08");

  TestScript script;
  script.id = std::string(id);
  const auto append = [&script](std::string stepId, ui::CommandSource source,
                                ActionPayload payload, UiState expectedState,
                                bool expectedAccepted) {
    const std::uint64_t ordinal = script.steps.size();
    TestAction action;
    action.step = ordinal;
    action.id = stepId;
    action.source = source;
    action.payload = std::move(payload);
    ScriptStep step;
    step.id = std::move(stepId);
    step.action = std::move(action);
    step.expectedState = expectedState;
    step.expectedAccepted = expectedAccepted;
    script.steps.push_back(std::move(step));
  };
  const auto capture = [&append](UiState state) {
    append("capture-start", ui::CommandSource::menu,
           CaptureStateAction{"start"}, state, true);
  };
  const auto marker = [&append, &script](UiState state) {
    append("boundary-marker", ui::CommandSource::menu,
           ScreenshotMarkerAction{script.id + "-boundary-only"}, state, true);
  };

  if (id == "U01-empty-new-open") {
    if (bindings.primaryDocument.empty() || bindings.invalidDocument.empty())
      return fail("U01 requires valid and invalid document paths");
    script.description = "real controller New and candidate Open workflow";
    script.coverageGaps = {
        "boundary marker is not screenshot evidence"};
    capture(UiState::noDocument);
    append("new-scene", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::newScene, std::nullopt},
           UiState::noDocument, true);
    append("open-invalid", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::open, bindings.invalidDocument},
           UiState::error, false);
    append("open-valid", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::open, bindings.primaryDocument},
           UiState::readyClean, true);
    append("activate-scene-one", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::activateSceneByIndex,
                            std::nullopt, std::nullopt, false, false, false,
                            false, std::size_t{1}},
           UiState::readyClean, true);
    append("activate-scene-zero", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::activateSceneByIndex,
                            std::nullopt, std::nullopt, false, false, false,
                            false, std::size_t{0}},
           UiState::readyClean, true);
    marker(UiState::readyClean);
    return script;
  }

  if (!bindings.primaryScene.valid() || !bindings.primaryNode.valid())
    return fail(std::string(id) + " requires live scene and node IDs");

  if (id == "U02-hierarchy") {
    if (!bindings.reparentSource || !bindings.reparentTarget ||
        bindings.nodeSelection.emptyValue())
      return fail("U02 requires a live selection and reparent occurrence");
    script.description =
        "real hierarchy select/reparent/rename/duplicate/undo/redo workflow";
    script.coverageGaps = {
        "modal focus and pointer drag visuals require live ImGui replay",
        "boundary marker is not screenshot evidence"};
    capture(UiState::readyClean);
    append("select-source", ui::CommandSource::sceneBrowser,
           ui::SelectObject{bindings.nodeSelection}, UiState::readyClean, true);
    append("cross-parent-drop", ui::CommandSource::sceneBrowser,
           ui::ReparentNode{*bindings.reparentSource, bindings.primaryScene,
                            bindings.reparentTarget},
           UiState::readyModified, true);
    append("rename-node", ui::CommandSource::dialog,
           ui::RenameObject{bindings.primaryNode.untyped(),
                            "Driver Hierarchy Node"},
           UiState::readyModified, true);
    append("duplicate-node", ui::CommandSource::sceneBrowser,
           ui::HierarchyAction{ui::HierarchyOperation::duplicateNode,
                               bindings.primaryScene, bindings.primaryNode,
                               std::nullopt},
           UiState::readyModified, true);
    append("undo-duplicate", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::undo, std::nullopt},
           UiState::readyModified, true);
    append("redo-duplicate", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::redo, std::nullopt},
           UiState::readyModified, true);
    marker(UiState::readyModified);
    return script;
  }

  if (id == "U03-transform-gizmo") {
    if (bindings.nodeSelection.emptyValue() || !bindings.nodeTransform)
      return fail("U03 requires a live node selection and transform");
    NodeTransform preview = *bindings.nodeTransform;
    preview.translation[0] += 0.25;
    script.description =
        "real controller viewport toggles and Gizmo transaction workflow";
    script.coverageGaps = {
        "pointer hit classification and focus-loss OS events require live app input",
        "boundary marker is not screenshot evidence"};
    capture(UiState::readyClean);
    append("select-node", ui::CommandSource::sceneBrowser,
           ui::SelectObject{bindings.nodeSelection}, UiState::readyClean, true);
    append("toggle-grid", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::toggleGrid, std::nullopt},
           UiState::readyClean, true);
    append("toggle-gizmo", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::toggleGizmo, std::nullopt},
           UiState::readyClean, true);
    append("toggle-snap", ui::CommandSource::toolbar,
           ui::InvokeCommand{ui::CommandId::toggleSnap, std::nullopt},
           UiState::readyClean, true);
    append("gizmo-hover", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::setGizmoHover, std::nullopt,
                            std::nullopt, true},
           UiState::gizmoHover, true);
    append("gizmo-begin", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::beginGizmoTransform,
                            bindings.primaryNode},
           UiState::gizmoDragging, true);
    append("gizmo-preview", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::previewGizmoTransform,
                            bindings.primaryNode, preview},
           UiState::gizmoDragging, true);
    append("gizmo-release", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::finishGizmoTransform,
                            bindings.primaryNode, std::nullopt, false, true,
                            false, false},
           UiState::gizmoHover, true);
    append("gizmo-hover-clear", ui::CommandSource::sceneBrowser,
           ControllerAction{ControllerActionKind::setGizmoHover, std::nullopt,
                            std::nullopt, false},
           UiState::readyModified, true);
    append("undo-transform", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::undo, std::nullopt},
           UiState::readyModified, true);
    append("redo-transform", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::redo, std::nullopt},
           UiState::readyModified, true);
    if (bindings.animation) {
      append("pause-animation", ui::CommandSource::sceneBrowser,
             ui::AnimationAction{ui::AnimationOperation::setPlaying,
                                 bindings.animation, 0.0},
             UiState::readyModified, true);
    }
    marker(UiState::readyModified);
    return script;
  }

  if (id == "U04-material") {
    if (bindings.primitiveSelection.emptyValue() || !bindings.primitive ||
        !bindings.primaryMaterial || !bindings.alternateMaterial)
      return fail("U04 requires live primitive and two material IDs");
    script.description =
        "real material selection/copy/assign/paste/extension/topology workflow";
    script.coverageGaps = {
        "popup focus and tooltip pixels require live ImGui replay",
        "boundary marker is not screenshot evidence"};
    ui::PrimitiveContext alternateContext = *bindings.primitive;
    alternateContext.material = bindings.alternateMaterial;
    capture(UiState::readyClean);
    append("select-primitive", ui::CommandSource::sceneBrowser,
           ui::SelectObject{bindings.primitiveSelection}, UiState::readyClean,
           true);
    append("copy-material", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::copyReference,
                              std::nullopt, bindings.primaryMaterial, {}},
           UiState::readyClean, true);
    append("assign-alternate", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::assign,
                              bindings.primitive, bindings.alternateMaterial,
                              {}},
           UiState::readyModified, true);
    append("paste-canonical", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::pasteReference,
                              alternateContext, std::nullopt, {}},
           UiState::readyModified, true);
    append("add-clearcoat", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::addExtension,
                              std::nullopt, bindings.primaryMaterial,
                              "KHR_materials_clearcoat"},
           UiState::readyModified, true);
    append("merge-node-meshes", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::merge,
                              bindings.primitive, bindings.primaryMaterial, {}},
           UiState::readyModified, true);
    append("split-primitive", ui::CommandSource::inspector,
           ui::MaterialAction{ui::MaterialOperation::split,
                              bindings.primitive, bindings.primaryMaterial, {}},
           UiState::readyModified, true);
    marker(UiState::readyModified);
    return script;
  }

  if (id == "U05-camera-light-vis") {
    if (!bindings.camera || !bindings.light ||
        bindings.cameraSelection.emptyValue() ||
        bindings.lightSelection.emptyValue())
      return fail("U05 requires live camera and light selections");
    script.description =
        "real camera, light, creation, and visibility command workflow";
    script.coverageGaps = {
        "numeric camera view proof requires the application camera bridge",
        "boundary marker is not screenshot evidence"};
    capture(UiState::readyClean);
    append("select-camera", ui::CommandSource::sceneBrowser,
           ui::SelectObject{bindings.cameraSelection}, UiState::readyClean,
           true);
    append("apply-camera", ui::CommandSource::inspector,
           ui::CameraAction{ui::CameraOperation::applyToCurrentView,
                            *bindings.camera, bindings.cameraNode},
           UiState::readyClean, true);
    append("edit-camera-yfov", ui::CommandSource::inspector,
           ui::PropertyEdit{bindings.camera->untyped(),
                            ui::FieldId::cameraYFov, ui::EditPhase::commit,
                            ui::Value{0.8}},
           UiState::readyModified, true);
    append("select-light", ui::CommandSource::sceneBrowser,
           ui::SelectObject{bindings.lightSelection}, UiState::readyModified,
           true);
    append("edit-light-intensity", ui::CommandSource::inspector,
           ui::PropertyEdit{bindings.light->untyped(),
                            ui::FieldId::lightIntensity, ui::EditPhase::commit,
                            ui::Value{3.0}},
           UiState::readyModified, true);
    for (const auto [operation, label] :
         {std::pair{ui::HierarchyOperation::addPointLight, "add-point-light"},
          std::pair{ui::HierarchyOperation::addDirectionalLight,
                    "add-directional-light"},
          std::pair{ui::HierarchyOperation::addSpotLight, "add-spot-light"}}) {
      append(label, ui::CommandSource::sceneBrowser,
             ui::HierarchyAction{operation, bindings.primaryScene,
                                 std::nullopt, std::nullopt},
             UiState::readyModified, true);
    }
    append("hide-node", ui::CommandSource::inspector,
           ui::PropertyEdit{bindings.primaryNode.untyped(),
                            ui::FieldId::visibility, ui::EditPhase::commit,
                            ui::Value{false}},
           UiState::readyModified, true);
    marker(UiState::readyModified);
    return script;
  }

  if (id == "U06-merge-compact-save") {
    if (bindings.mergeDocument.empty() || bindings.saveTarget.empty())
      return fail("U06 requires merge and Save As paths");
    script.description =
        "real Merge, Compact, Save As, and reload controller workflow";
    script.coverageGaps = {
        "GPU frame-safe publication requires the live renderer acceptance run",
        "boundary marker is not screenshot evidence"};
    capture(UiState::readyClean);
    append("merge-document", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::importMerge,
                             bindings.mergeDocument},
           UiState::readyModified, true);
    append("compact-document", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::compactScene, std::nullopt},
           UiState::readyModified, true);
    append("save-as", ui::CommandSource::dialog,
           ui::InvokeCommand{ui::CommandId::saveAs, bindings.saveTarget},
           UiState::readyClean, true);
    append("reload-saved", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::open, bindings.saveTarget},
           UiState::readyClean, true);
    marker(UiState::readyClean);
    return script;
  }

  if (id == "U07-modified-failures") {
    if (bindings.invalidDocument.empty() || bindings.saveTarget.empty())
      return fail("U07 requires invalid load/merge and Save As paths");
    script.description =
        "real modified-document Save/Merge/Open failure preservation workflow";
    script.coverageGaps = {
        "native dialog focus requires live ImGui/OS replay",
        "boundary marker is not screenshot evidence"};
    capture(UiState::readyClean);
    append("make-modified", ui::CommandSource::dialog,
           ui::RenameObject{bindings.primaryNode.untyped(),
                            "Modified Before Failures"},
           UiState::readyModified, true);
    append("merge-invalid", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::importMerge,
                             bindings.invalidDocument},
           UiState::error, false);
    append("dismiss-merge-error", ui::CommandSource::dialog,
           ControllerAction{ControllerActionKind::dismissError},
           UiState::readyModified, true);
    append("save-invalid", ui::CommandSource::dialog,
           ui::InvokeCommand{ui::CommandId::saveAs, bindings.saveTarget},
           UiState::error, false);
    append("dismiss-save-error", ui::CommandSource::dialog,
           ControllerAction{ControllerActionKind::dismissError},
           UiState::readyModified, true);
    append("open-invalid", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::open, bindings.invalidDocument},
           UiState::error, false);
    append("new-after-error", ui::CommandSource::menu,
           ui::InvokeCommand{ui::CommandId::newScene, std::nullopt},
           UiState::noDocument, true);
    append("exit", ui::CommandSource::shortcut,
           ui::InvokeCommand{ui::CommandId::exit, std::nullopt},
           UiState::noDocument, true);
    marker(UiState::noDocument);
    return script;
  }

  if (bindings.nodeSelection.emptyValue() || bindings.unicodeName.empty())
    return fail("U08 requires a live node and Unicode name");
  script.description =
      "real Unicode rename, clear, and restore-selection workflow";
  script.coverageGaps = {
      "10k-row clipping and four-DPI pixel layout require live ImGui capture",
      "boundary marker is not screenshot evidence"};
  capture(UiState::readyClean);
  append("select-unicode-node", ui::CommandSource::sceneBrowser,
         ui::SelectObject{bindings.nodeSelection}, UiState::readyClean, true);
  append("rename-long-unicode", ui::CommandSource::dialog,
         ui::RenameObject{bindings.primaryNode.untyped(),
                          bindings.unicodeName},
         UiState::readyModified, true);
  append("clear-selection", ui::CommandSource::shortcut,
         ui::InvokeCommand{ui::CommandId::clearSelection, std::nullopt},
         UiState::readyModified, true);
  append("restore-selection", ui::CommandSource::sceneBrowser,
         ui::SelectObject{bindings.nodeSelection}, UiState::readyModified, true);
  marker(UiState::readyModified);
  return script;
}

std::vector<TestScript>
EditorTestDriver::defaultScripts(const ScenarioBindings &bindings,
                                 std::vector<std::string> *errors) {
  static constexpr std::array<std::string_view, 8> ids = {
      "U01-empty-new-open", "U02-hierarchy", "U03-transform-gizmo",
      "U04-material", "U05-camera-light-vis", "U06-merge-compact-save",
      "U07-modified-failures", "U08-large-unicode-dpi"};
  std::vector<TestScript> scripts;
  scripts.reserve(ids.size());
  for (const std::string_view id : ids) {
    std::string error;
    auto script = defaultScript(id, bindings, &error);
    if (script)
      scripts.push_back(std::move(*script));
    else if (errors)
      errors->push_back(std::string(id) + ": " + error);
  }
  return scripts;
}

std::string EditorTestDriver::actionSchemaJson() {
  return R"({"schemaVersion":1,"type":"EditorTestAction","required":["schemaVersion","step","id","kind","source","payload"],"kind":["invokeCommand","selectObject","activateScene","hierarchy","rename","reparent","animation","selectVariant","property","material","camera","sceneTransform","rebuildRenderNodes","controllerAction","captureState","screenshotMarker"],"resultRequired":["beforeState","afterState","accepted","status"],"failClosed":true})";
}

std::string EditorTestDriver::stateSchemaJson() {
  return R"({"schemaVersion":1,"type":"EditorUiStateSnapshot","required":["schemaVersion","state","frame","persistent","authority","commands"],"state":["NoDocument","LoadingCandidate","ReadyClean","ReadyModified","GizmoHover","GizmoDragging","Modal","Saving/Merging/Compacting","Error"],"authorityRequired":["documentRevision","cleanRevision","documentDigest","projectionDigest","selectionRevision","undoCount","redoCount"],"failClosed":true})";
}

std::string EditorTestDriver::scriptSchemaJson() {
  return R"({"schemaVersion":1,"type":"EditorTestScript","required":["schemaVersion","id","description","coverageComplete","coverageGaps","steps"],"scenarioIds":["U01-empty-new-open","U02-hierarchy","U03-transform-gizmo","U04-material","U05-camera-light-vis","U06-merge-compact-save","U07-modified-failures","U08-large-unicode-dpi"],"stepRequired":["schemaVersion","id","action","expectedState","expectedAccepted"],"resultType":"ReplayResult","screenshotMarkerMeaning":"boundary-only-not-image-evidence","failClosed":true})";
}

} // namespace demo::editor::testing
