#include "SceneEditorUi.h"
#include "MaterialClipboardCodec.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace demo::editor::ui {
namespace {

template <typename Enum>
[[nodiscard]] constexpr std::size_t enumIndex(Enum value) noexcept {
  return static_cast<std::size_t>(value);
}

constexpr std::array<CommandDescriptor, enumIndex(CommandId::count)>
    kCommandDescriptors{{
        {CommandId::newScene, "File", "New Scene", "Ctrl+N"},
        {CommandId::open, "File", "Open", "Ctrl+O"},
        {CommandId::importMerge, "File", "Import/Merge Scene...", ""},
        {CommandId::openRecent, "File", "Open Recent", ""},
        {CommandId::saveAs, "File", "Save As", "Ctrl+S"},
        {CommandId::exit, "File", "Exit", "Ctrl+Q"},
        {CommandId::undo, "Edit", "Undo", "Ctrl+Z"},
        {CommandId::redo, "Edit", "Redo", "Ctrl+Y"},
        {CommandId::duplicateSelection, "Edit", "Duplicate", "Ctrl+D"},
        {CommandId::deleteSelection, "Edit", "Delete", "Delete"},
        {CommandId::fitScene, "View", "Fit Scene", "Ctrl+Shift+F"},
        {CommandId::fitObject, "View", "Fit Object", "Ctrl+F"},
        {CommandId::toggleGrid, "View", "Grid", "G"},
        {CommandId::toggleGizmo, "View", "Gizmo", "T"},
        {CommandId::toggleSnap, "View", "Snap", ""},
        {CommandId::showGridSnapSettings, "View", "Grid & Snap Settings...",
         ""},
        {CommandId::toggleSceneBrowser, "Windows", "Scene Browser", "F2"},
        {CommandId::toggleInspector, "Windows", "Inspector", "F4"},
        {CommandId::compactScene, "Tools", "Compact Scene", "Ctrl+K"},
        {CommandId::clearSelection, "", "Clear selection", "Escape"},
    }};

constexpr std::array<MaterialExtensionDescriptor, 14>
    kMaterialExtensionDescriptors{{
        {"Specular-Glossiness", "KHR_materials_pbrSpecularGlossiness", true},
        {"Anisotropy", "KHR_materials_anisotropy", false},
        {"Clearcoat", "KHR_materials_clearcoat", false},
        {"Diffuse Transmission", "KHR_materials_diffuse_transmission", false},
        {"Dispersion", "KHR_materials_dispersion", false},
        {"Emissive Strength", "KHR_materials_emissive_strength", false},
        {"IOR", "KHR_materials_ior", false},
        {"Iridescence", "KHR_materials_iridescence", false},
        {"Sheen", "KHR_materials_sheen", false},
        {"Specular", "KHR_materials_specular", false},
        {"Transmission", "KHR_materials_transmission", false},
        {"Unlit", "KHR_materials_unlit", false},
        {"Volume", "KHR_materials_volume", false},
        {"Volume Scatter", "KHR_materials_volume_scatter", false},
    }};

[[nodiscard]] std::array<FieldDescriptor, enumIndex(FieldId::count)>
makeFieldDescriptors() {
  std::array<FieldDescriptor, enumIndex(FieldId::count)> result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index].id = static_cast<FieldId>(index);
    result[index].label = "Unknown";
  }

  const auto set = [&result](FieldId id, std::string_view label, FieldKind kind,
                             double speed = 0.01,
                             std::optional<double> minimum = std::nullopt,
                             std::optional<double> maximum = std::nullopt,
                             std::string_view format = "%.3f",
                             bool continuous = false) {
    result[enumIndex(id)] = FieldDescriptor{
        id, label, kind, speed, minimum, maximum, format, continuous};
  };

  set(FieldId::name, "Name", FieldKind::text);
  set(FieldId::parent, "Parent", FieldKind::objectReference);
  set(FieldId::children, "Children", FieldKind::readOnlyText);
  set(FieldId::meshAttachment, "Mesh", FieldKind::objectReference);
  set(FieldId::cameraAttachment, "Camera", FieldKind::objectReference);
  set(FieldId::skinAttachment, "Skin", FieldKind::objectReference);
  set(FieldId::lightAttachment, "Light", FieldKind::objectReference);
  set(FieldId::translation, "Translation", FieldKind::vector3, 0.01,
      std::nullopt, std::nullopt, "%.3f", true);
  set(FieldId::rotationDegrees, "Rotation", FieldKind::vector3, 0.1,
      std::nullopt, std::nullopt, "%.3f deg", true);
  set(FieldId::scale, "Scale", FieldKind::vector3, 0.01, std::nullopt,
      std::nullopt, "%.3f", true);
  set(FieldId::visibility, "Visible", FieldKind::boolean);
  set(FieldId::primitiveIndex, "Primitive", FieldKind::integer);
  set(FieldId::meshIndex, "Mesh", FieldKind::integer);
  set(FieldId::vertexCount, "Vertices", FieldKind::integer);
  set(FieldId::triangleCount, "Triangles", FieldKind::integer);
  set(FieldId::materialReference, "Material", FieldKind::objectReference);
  set(FieldId::primitiveCount, "Primitives", FieldKind::integer);
  set(FieldId::referencedBy, "Referenced by", FieldKind::readOnlyText);

  set(FieldId::baseColor, "Base Color", FieldKind::color4, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::baseColorTexture, "Base Color", FieldKind::choice);
  set(FieldId::metallic, "Metallic", FieldKind::scalar, 0.01, 0.0, 1.0, "%.3f",
      true);
  set(FieldId::roughness, "Roughness", FieldKind::scalar, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::metallicRoughnessTexture, "Metallic-Roughness",
      FieldKind::choice);
  set(FieldId::diffuseColor, "Diffuse Color", FieldKind::color4, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::diffuseTexture, "Diffuse", FieldKind::choice);
  set(FieldId::specularGlossinessColor, "Specular", FieldKind::color3, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::glossiness, "Glossiness", FieldKind::scalar, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::specularGlossinessTexture, "Specular-Glossiness",
      FieldKind::choice);
  set(FieldId::emissiveColor, "Emissive", FieldKind::color3, 0.01, 0.0,
      std::nullopt, "%.3f", true);
  set(FieldId::emissiveTexture, "Emissive", FieldKind::choice);
  set(FieldId::normalTexture, "Normal", FieldKind::choice);
  set(FieldId::normalScale, "Normal Scale", FieldKind::scalar, 0.01,
      std::nullopt, std::nullopt, "%.3f", true);
  set(FieldId::occlusionTexture, "Occlusion", FieldKind::choice);
  set(FieldId::occlusionStrength, "Occlusion Strength", FieldKind::scalar, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::alphaMode, "Alpha Mode", FieldKind::choice);
  set(FieldId::alphaCutoff, "Alpha Cutoff", FieldKind::scalar, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::doubleSided, "Double Sided", FieldKind::boolean);

  set(FieldId::anisotropyStrength, "Strength", FieldKind::scalar, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::anisotropyRotation, "Rotation", FieldKind::scalar, 0.01,
      -3.141592653589793, 3.141592653589793, "%.3f", true);
  set(FieldId::anisotropyTexture, "Anisotropy", FieldKind::choice);
  set(FieldId::clearcoatFactor, "Factor", FieldKind::scalar, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::clearcoatRoughness, "Roughness", FieldKind::scalar, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::clearcoatTexture, "Clearcoat", FieldKind::choice);
  set(FieldId::clearcoatRoughnessTexture, "Clearcoat Roughness",
      FieldKind::choice);
  set(FieldId::clearcoatNormalTexture, "Clearcoat Normal", FieldKind::choice);
  set(FieldId::diffuseTransmissionFactor, "Factor", FieldKind::scalar, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::diffuseTransmissionColor, "Color", FieldKind::color3, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::diffuseTransmissionTexture, "Diffuse Transmission",
      FieldKind::choice);
  set(FieldId::diffuseTransmissionColorTexture, "Diffuse Transmission Color",
      FieldKind::choice);
  set(FieldId::dispersion, "Dispersion Factor", FieldKind::scalar, 0.01, 0.0,
      10.0, "%.3f", true);
  set(FieldId::emissiveStrength, "Emissive Strength", FieldKind::scalar, 0.01,
      0.0, std::nullopt, "%.3f", true);
  set(FieldId::ior, "IOR", FieldKind::scalar, 0.01, 0.0, 10.0, "%.3f", true);
  set(FieldId::iridescenceFactor, "Iridescence Factor", FieldKind::scalar, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::iridescenceIor, "Iridescence IOR", FieldKind::scalar, 0.01, 0.0,
      10.0, "%.3f", true);
  set(FieldId::iridescenceThicknessMinimum, "Thickness Min", FieldKind::scalar,
      0.01, 0.0, 1000.0, "%.3f nm", true);
  set(FieldId::iridescenceThicknessMaximum, "Thickness Max", FieldKind::scalar,
      0.01, 0.0, 1000.0, "%.3f nm", true);
  set(FieldId::iridescenceTexture, "Iridescence", FieldKind::choice);
  set(FieldId::iridescenceThicknessTexture, "Iridescence Thickness",
      FieldKind::choice);
  set(FieldId::sheenColor, "Sheen Color", FieldKind::color3, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::sheenRoughness, "Sheen Roughness", FieldKind::scalar, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::sheenColorTexture, "Sheen Color", FieldKind::choice);
  set(FieldId::sheenRoughnessTexture, "Sheen Roughness", FieldKind::choice);
  set(FieldId::specularFactor, "Specular Factor", FieldKind::scalar, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::specularColor, "Specular Color", FieldKind::color3, 0.01, 0.0,
      1.0, "%.3f", true);
  set(FieldId::specularTexture, "Specular", FieldKind::choice);
  set(FieldId::specularColorTexture, "Specular Color", FieldKind::choice);
  set(FieldId::transmissionFactor, "Factor", FieldKind::scalar, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::transmissionTexture, "Transmission", FieldKind::choice);
  set(FieldId::unlit, "Unlit", FieldKind::boolean);
  set(FieldId::volumeThickness, "Thickness", FieldKind::scalar, 0.01, 0.0,
      std::nullopt, "%.3f", true);
  set(FieldId::volumeThicknessTexture, "Thickness", FieldKind::choice);
  set(FieldId::attenuationColor, "Attenuation Color", FieldKind::color3, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::attenuationDistance, "Attenuation Distance", FieldKind::scalar,
      0.01, 0.0, std::nullopt, "%.3f", true);
  set(FieldId::infiniteAttenuation, "Infinite Attenuation", FieldKind::boolean);
  set(FieldId::multiscatterColor, "Multiscatter Color", FieldKind::color3, 0.01,
      0.0, 1.0, "%.3f", true);
  set(FieldId::scatterAnisotropy, "Scatter Anisotropy", FieldKind::scalar, 0.01,
      -1.0, 1.0, "%.3f", true);

  set(FieldId::cameraType, "Type", FieldKind::choice);
  set(FieldId::cameraYFov, "Y FOV (rad)", FieldKind::scalar, 0.01, 0.001,
      3.124139361, "%.3f", true);
  set(FieldId::cameraAspectRatio, "Aspect Ratio", FieldKind::scalar, 0.01, 0.1,
      10.0, "%.3f", true);
  set(FieldId::cameraZNear, "Z Near", FieldKind::scalar, 0.01, 0.001, 1000.0,
      "%.3f", true);
  set(FieldId::cameraZFar, "Z Far", FieldKind::scalar, 0.01, 0.001, 10000.0,
      "%.3f", true);
  set(FieldId::cameraXMag, "X Mag", FieldKind::scalar, 0.01, -1000.0, 1000.0,
      "%.3f", true);
  set(FieldId::cameraYMag, "Y Mag", FieldKind::scalar, 0.01, -1000.0, 1000.0,
      "%.3f", true);

  set(FieldId::lightType, "Type", FieldKind::choice);
  set(FieldId::lightColorSrgb, "Color", FieldKind::color3, 0.01, 0.0, 1.0,
      "%.3f", true);
  set(FieldId::lightIntensity, "Intensity", FieldKind::scalar, 0.1, 0.0,
      100000.0, "%.3f", true);
  set(FieldId::lightRange, "Range", FieldKind::scalar, 0.1, 0.0, 10000.0,
      "%.3f", true);
  set(FieldId::lightInnerConeAngle, "Inner Cone Angle", FieldKind::scalar, 0.01,
      0.0, 1.570796327, "%.3f", true);
  set(FieldId::lightOuterConeAngle, "Outer Cone Angle", FieldKind::scalar, 0.01,
      0.0, 1.570796327, "%.3f", true);

  return result;
}

const auto kFieldDescriptors = makeFieldDescriptors();

[[nodiscard]] bool lifecycleBlocksCommands(LifecycleState state) noexcept {
  switch (state) {
  case LifecycleState::loadingCandidate:
  case LifecycleState::modal:
  case LifecycleState::savingMergingCompacting:
  case LifecycleState::error:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] std::string fallbackName(std::string_view name, const char *kind,
                                       std::uint32_t index) {
  if (!name.empty())
    return std::string(name);
  return std::string(kind) + " " + std::to_string(index);
}

[[nodiscard]] constexpr std::string_view graphGlyph(GraphRowKind kind) noexcept {
  switch (kind) {
  case GraphRowKind::scene: return "[S]";
  case GraphRowKind::node: return "[N]";
  case GraphRowKind::mesh: return "[M]";
  case GraphRowKind::primitive: return "[P]";
  case GraphRowKind::camera: return "[C]";
  case GraphRowKind::light: return "[L]";
  case GraphRowKind::skin: return "[K]";
  }
  return "[?]";
}

[[nodiscard]] bool inputTextString(const char *label, std::string &value,
                                   ImGuiInputTextFlags flags = 0) {
  flags |= ImGuiInputTextFlags_CallbackResize;
  const auto callback = [](ImGuiInputTextCallbackData *data) -> int {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
      auto &text = *static_cast<std::string *>(data->UserData);
      text.resize(static_cast<std::size_t>(data->BufTextLen));
      data->Buf = text.data();
    }
    return 0;
  };

  if (value.capacity() < 255)
    value.reserve(255);
  return ImGui::InputText(label, value.data(), value.capacity() + 1, flags,
                          callback, &value);
}

void pushObjectId(EditorObjectId id) {
  ImGui::PushID(static_cast<int>(id.kind));
  ImGui::PushID(static_cast<int>(id.slot));
  ImGui::PushID(static_cast<int>(id.generation));
  ImGui::PushID(static_cast<int>(id.authority.value & 0xffffffffULL));
  ImGui::PushID(static_cast<int>(id.authority.value >> 32U));
}

void popObjectId() {
  for (int index = 0; index < 5; ++index)
    ImGui::PopID();
}

void pushGraphKey(const GraphRowKey &key) {
  pushObjectId(key.scene.untyped());
  pushObjectId(key.object);
  if (key.directParent)
    pushObjectId(key.directParent->untyped());
  else
    ImGui::PushID("root");
  ImGui::PushID(static_cast<int>(key.occurrence));
}

void popGraphKey(const GraphRowKey &key) {
  ImGui::PopID();
  if (key.directParent)
    popObjectId();
  else
    ImGui::PopID();
  popObjectId();
  popObjectId();
}

[[nodiscard]] std::optional<NodeSelection>
selectedNode(const EditorSelectionSnapshot &selection) {
  if (const auto *node = std::get_if<NodeSelection>(&selection.value))
    return *node;
  if (const auto *primitive =
          std::get_if<PrimitiveSelection>(&selection.value)) {
    return NodeSelection{primitive->scene, primitive->node, std::nullopt};
  }
  return std::nullopt;
}

[[nodiscard]] bool sameObject(EditorObjectId lhs, EditorObjectId rhs) noexcept {
  return lhs == rhs;
}

[[nodiscard]] const ResourceGroup *
findResourceGroup(const SceneBrowserModel &browser,
                  ResourceGroupKind kind) noexcept {
  const auto found = std::find_if(
      browser.resourceGroups.begin(), browser.resourceGroups.end(),
      [kind](const ResourceGroup &group) { return group.kind == kind; });
  return found == browser.resourceGroups.end() ? nullptr : &*found;
}

constexpr std::array<ResourceGroupKind, 8> kResourceGroupOrder{{
    ResourceGroupKind::nodes,
    ResourceGroupKind::meshes,
    ResourceGroupKind::materials,
    ResourceGroupKind::cameras,
    ResourceGroupKind::lights,
    ResourceGroupKind::textures,
    ResourceGroupKind::images,
    ResourceGroupKind::animations,
}};

[[nodiscard]] std::string_view resourceGroupName(ResourceGroupKind kind) {
  switch (kind) {
  case ResourceGroupKind::nodes:
    return "Nodes";
  case ResourceGroupKind::meshes:
    return "Meshes";
  case ResourceGroupKind::materials:
    return "Materials";
  case ResourceGroupKind::cameras:
    return "Cameras";
  case ResourceGroupKind::lights:
    return "Lights";
  case ResourceGroupKind::textures:
    return "Textures";
  case ResourceGroupKind::images:
    return "Images";
  case ResourceGroupKind::animations:
    return "Animations";
  }
  return "Resources";
}

[[nodiscard]] const char *resourceItemName(ResourceGroupKind kind) noexcept {
  switch (kind) {
  case ResourceGroupKind::nodes:
    return "Node";
  case ResourceGroupKind::meshes:
    return "Mesh";
  case ResourceGroupKind::materials:
    return "Material";
  case ResourceGroupKind::cameras:
    return "Camera";
  case ResourceGroupKind::lights:
    return "Light";
  case ResourceGroupKind::textures:
    return "Texture";
  case ResourceGroupKind::images:
    return "Image";
  case ResourceGroupKind::animations:
    return "Animation";
  }
  return "Resource";
}

[[nodiscard]] bool resourceGroupSelectable(ResourceGroupKind kind) noexcept {
  return kind != ResourceGroupKind::textures &&
         kind != ResourceGroupKind::images &&
         kind != ResourceGroupKind::animations;
}

[[nodiscard]] std::array<double, 3>
quaternionToEulerDegrees(const std::array<double, 4> &quaternion) noexcept {
  const double x = quaternion[0];
  const double y = quaternion[1];
  const double z = quaternion[2];
  const double w = quaternion[3];

  const double sinRollCosPitch = 2.0 * (w * x + y * z);
  const double cosRollCosPitch = 1.0 - 2.0 * (x * x + y * y);
  const double roll = std::atan2(sinRollCosPitch, cosRollCosPitch);

  const double sinPitch = 2.0 * (w * y - z * x);
  const double pitch = std::abs(sinPitch) >= 1.0
                           ? std::copysign(1.5707963267948966, sinPitch)
                           : std::asin(sinPitch);

  const double sinYawCosPitch = 2.0 * (w * z + x * y);
  const double cosYawCosPitch = 1.0 - 2.0 * (y * y + z * z);
  const double yaw = std::atan2(sinYawCosPitch, cosYawCosPitch);
  constexpr double radiansToDegrees = 57.295779513082320876;
  return {roll * radiansToDegrees, pitch * radiansToDegrees,
          yaw * radiansToDegrees};
}

[[nodiscard]] std::array<double, 4>
eulerDegreesToQuaternion(const std::array<double, 3> &euler) noexcept {
  constexpr double degreesToHalfRadians = 0.0087266462599716478846;
  const double cr = std::cos(euler[0] * degreesToHalfRadians);
  const double sr = std::sin(euler[0] * degreesToHalfRadians);
  const double cp = std::cos(euler[1] * degreesToHalfRadians);
  const double sp = std::sin(euler[1] * degreesToHalfRadians);
  const double cy = std::cos(euler[2] * degreesToHalfRadians);
  const double sy = std::sin(euler[2] * degreesToHalfRadians);
  return {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
          cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}

} // namespace

std::span<const CommandDescriptor> commandDescriptors() noexcept {
  return kCommandDescriptors;
}

const CommandDescriptor &commandDescriptor(CommandId id) noexcept {
  const std::size_t index = enumIndex(id);
  return kCommandDescriptors[index < kCommandDescriptors.size() ? index : 0];
}

const FieldDescriptor &fieldDescriptor(FieldId id) noexcept {
  const std::size_t index = enumIndex(id);
  return kFieldDescriptors[index < kFieldDescriptors.size() ? index : 0];
}

std::span<const MaterialExtensionDescriptor>
materialExtensionDescriptors() noexcept {
  return kMaterialExtensionDescriptors;
}

CommandEnablement evaluateCommand(CommandId id, const FrameModel &model,
                                  const WindowState &windows,
                                  const ViewportSettings &viewport) noexcept {
  const bool blocked = lifecycleBlocksCommands(model.lifecycle) ||
                       model.rendererBusy || model.modalPending;
  // A failed candidate leaves the current document and its error visible, but
  // Open/New must remain available so the user can recover without restarting.
  const bool fileCommandsBlocked =
      model.rendererBusy || model.modalPending ||
      model.lifecycle == LifecycleState::loadingCandidate ||
      model.lifecycle == LifecycleState::modal ||
      model.lifecycle == LifecycleState::savingMergingCompacting;
  const bool hasSelection = !model.selection.emptyValue();
  const bool transformable = selectedNode(model.selection).has_value();
  const bool mutableDocument = model.hasDocument && model.mutationEnabled &&
                               !blocked;
  CommandEnablement result{};

  switch (id) {
  case CommandId::newScene:
  case CommandId::open:
  case CommandId::openRecent:
    result.enabled = !fileCommandsBlocked;
    break;
  case CommandId::exit:
    result.enabled = model.lifecycle != LifecycleState::modal;
    break;
  case CommandId::importMerge:
  case CommandId::saveAs:
  case CommandId::compactScene:
    result.enabled = mutableDocument;
    break;
  case CommandId::undo:
    result.enabled = mutableDocument && model.canUndo;
    break;
  case CommandId::redo:
    result.enabled = mutableDocument && model.canRedo;
    break;
  case CommandId::duplicateSelection:
  case CommandId::deleteSelection:
    result.enabled =
        mutableDocument && transformable;
    break;
  case CommandId::fitScene:
    result.enabled = model.hasDocument && !blocked;
    break;
  case CommandId::fitObject:
    result.enabled = model.hasDocument && model.canFitSelection && !blocked;
    break;
  case CommandId::toggleGrid:
    result.enabled = model.hasDocument && !blocked;
    result.checked = viewport.showGrid;
    break;
  case CommandId::toggleGizmo:
    result.enabled = model.hasDocument && !blocked;
    result.checked = viewport.showGizmo;
    break;
  case CommandId::toggleSnap:
    result.enabled = model.hasDocument && !blocked;
    result.checked = viewport.snapEnabled;
    break;
  case CommandId::showGridSnapSettings:
    result.enabled = model.hasDocument && !blocked;
    result.checked = windows.gridSnapSettings;
    break;
  case CommandId::toggleSceneBrowser:
    result.enabled = true;
    result.checked = windows.sceneBrowser;
    break;
  case CommandId::toggleInspector:
    result.enabled = true;
    result.checked = windows.inspector;
    break;
  case CommandId::clearSelection:
    result.enabled = hasSelection && model.lifecycle != LifecycleState::modal;
    break;
  case CommandId::count:
    break;
  }
  return result;
}

struct SceneEditorUi::Impl final {
  struct ExpansionOverride final {
    GraphRowKey key;
    bool expanded{false};
  };

  struct PendingDelete final {
    SceneId scene;
    NodeId node;
    std::optional<NodeId> parent;
    std::string name;
    std::optional<GraphRowKey> row;
  };

  struct FieldEditState final {
    EditorObjectId target;
    FieldId field{FieldId::name};
    Value original;
    Value value;
    bool active{false};
    bool deferred{false};
  };

  struct MaterialDraftState final {
    EditorObjectId target;
    std::vector<PropertyEdit> edits;
  };

  struct EulerCache final {
    EditorObjectId target;
    std::uint64_t externalRevision{0};
    std::array<double, 3> value{};
    bool valid{false};
  };

  explicit Impl(IconSet values) : icons(std::move(values)) {}

  IconSet icons;
  PersistentState persistent;
  std::array<bool, enumIndex(CommandId::count)> invokedThisFrame{};
  std::uint64_t nextSequence{1};
  std::uint64_t pendingViewportEscapes{0};
  RenderResult result;

  std::vector<ExpansionOverride> expansion;
  std::vector<std::size_t> visibleGraphRows;
  std::uint64_t cachedGraphRevision{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t expansionRevision{0};
  std::uint64_t cachedExpansionRevision{
      std::numeric_limits<std::uint64_t>::max()};
  std::vector<EditorObjectId> focusPath;
  std::optional<SceneId> focusScene;
  EditorObjectId focusTarget;
  std::optional<GraphRowKey> focusRow;
  bool scrollToFocus{false};
  std::optional<GraphRowKey> dragSource;
  std::uint64_t dragSourceRevision{0};

  EditorObjectId renameTarget;
  std::string renameBuffer;
  std::optional<GraphRowKey> renameRow;
  bool openRenameNextFrame{false};
  bool focusRenameInput{false};

  std::optional<PendingDelete> pendingDelete;
  bool openDeleteNextFrame{false};

  std::optional<PrimitiveContext> assignPrimitive;
  std::optional<MaterialId> assignMaterial;
  bool openAssignNextFrame{false};

  struct TexturePicker final {
    EditorObjectId target;
    FieldId field{FieldId::baseColorTexture};
    std::vector<Choice> choices;
    Value original;
    std::int32_t selected{-1};
  };
  std::optional<TexturePicker> texturePicker;
  bool openTextureNextFrame{false};

  std::optional<SceneId> sceneTransformScene;
  SceneRootTransform sceneTransform;
  bool openSceneTransformNextFrame{false};
  std::optional<ImageId> pendingImageScroll;
  bool forceImagesOpen{false};

  FieldEditState fieldEdit;
  std::optional<MaterialDraftState> materialDraft;
  bool materialDraftCommitPending{false};
  bool materialDraftParkedThisFrame{false};
  EulerCache rotationCache;
  bool focusInspectorNextFrame{false};
  bool focusSceneBrowserNextFrame{false};
  std::string transientError;
  std::string presentedError;
  bool errorWindowOpen{false};
  EditorSelectionSnapshot previousSelection;
  bool hasPreviousSelection{false};
  SelectionRevision renderedSelectionRevision{0};
  SelectionRevision focusRevision{0};
  EditorUiProbe *probe{nullptr};
  std::uint32_t probeWindowToken{0};
  std::uint64_t probeItemOrder{0};

  [[nodiscard]] EditorUiProbeItem captureLastItem(
      EditorUiProbeSemantic semantic, const GraphRowKey &row,
      EditorObjectId object, bool enabled, std::string_view label,
      std::uint32_t popupToken = 0u, std::uint32_t modalToken = 0u,
      std::uint32_t windowToken = 0u) const {
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    return EditorUiProbeItem{
        .key = {semantic, row, object},
        .label = label,
        .itemId = static_cast<std::uint32_t>(ImGui::GetItemID()),
        .windowToken = windowToken != 0u ? windowToken : probeWindowToken,
        .popupToken = popupToken,
        .modalToken = modalToken,
        .rectCoordinateSpace =
            EditorUiProbeCoordinateSpace::imguiScreenLogical,
        .rect = {minimum.x, minimum.y, maximum.x, maximum.y},
        .visible = ImGui::IsItemVisible(),
        .hovered = ImGui::IsItemHovered(),
        .active = ImGui::IsItemActive(),
        .focused = ImGui::IsItemFocused(),
        .enabled = enabled,
    };
  }

  void observeItem(EditorUiProbeItem item) {
    if (probe == nullptr)
      return;
    item.order = ++probeItemOrder;
    probe->observeItem(item);
  }

  void observeLastItem(EditorUiProbeSemantic semantic,
                       const GraphRowKey &row, EditorObjectId object,
                       bool enabled, std::string_view label,
                       std::uint32_t popupToken = 0u,
                       std::uint32_t modalToken = 0u,
                       std::uint32_t windowToken = 0u) {
    if (probe == nullptr)
      return;
    observeItem(captureLastItem(semantic, row, object, enabled, label,
                                popupToken, modalToken, windowToken));
  }

  [[nodiscard]] bool modalPending() const noexcept {
    return renameTarget.valid() || pendingDelete.has_value() ||
           assignPrimitive.has_value() || texturePicker.has_value();
  }

  void clearSelectionScopedState() {
    fieldEdit = {};
    materialDraft.reset();
    materialDraftCommitPending = false;
    materialDraftParkedThisFrame = false;
    rotationCache = {};
    renameTarget = {};
    renameBuffer.clear();
    renameRow.reset();
    openRenameNextFrame = false;
    focusRenameInput = false;
    pendingDelete.reset();
    openDeleteNextFrame = false;
    assignPrimitive.reset();
    assignMaterial.reset();
    openAssignNextFrame = false;
    texturePicker.reset();
    openTextureNextFrame = false;
  }

  void resetDocumentState() noexcept {
    clearSelectionScopedState();
    pendingViewportEscapes = 0u;
    // Graph rows and expansion overrides are keyed by the previous document's
    // stable IDs.  Discard both the materialized view and its cache revisions
    // so the next document cannot reuse stale rows or collapse state.
    expansion.clear();
    visibleGraphRows.clear();
    cachedGraphRevision = std::numeric_limits<std::uint64_t>::max();
    expansionRevision = 0;
    cachedExpansionRevision = std::numeric_limits<std::uint64_t>::max();
    focusPath.clear();
    focusScene.reset();
    focusTarget = {};
    focusRow.reset();
    scrollToFocus = false;
    dragSource.reset();
    dragSourceRevision = 0;
    sceneTransformScene.reset();
    sceneTransform = {};
    openSceneTransformNextFrame = false;
    pendingImageScroll.reset();
    forceImagesOpen = false;
    focusInspectorNextFrame = false;
    focusSceneBrowserNextFrame = false;
    previousSelection = EditorSelectionSnapshot::empty();
    hasPreviousSelection = false;
    renderedSelectionRevision = 0;
    focusRevision = 0;
    transientError.clear();
    presentedError.clear();
    errorWindowOpen = false;
  }

  void consume(const RuntimeActionResult &actionResult,
               SceneEditorUiHost &host) {
    // EditorController publishes document projections synchronously. The
    // refresh value is an outcome classification, not a request to rebuild a
    // second projection from the same document revision.
    (void)host;
    if (!actionResult.error.empty()) {
      transientError = actionResult.error;
      result.lastError = actionResult.error;
    }
  }

  template <typename Payload>
  bool submit(CommandSource source, Payload payload, SceneEditorUiHost &host) {
    UiCommand command{nextSequence++, source,
                      CommandPayload{std::move(payload)}};
    ++result.submittedCommandCount;
    const RuntimeActionResult actionResult = host.dispatch(command);
    if (probe != nullptr)
      probe->observeDispatch(command, actionResult);
    consume(actionResult, host);
    return actionResult.accepted;
  }

  void camera(const CameraRequest &request, SceneEditorUiHost &host) {
    ++result.submittedCommandCount;
    consume(host.cameraRequest(request), host);
  }

  [[nodiscard]] bool expanded(const GraphRow &row) const {
    const bool rowIsFocusedScene = !focusScene || row.key.scene == *focusScene;
    if (!rowIsFocusedScene) {
      const auto found = std::find_if(
          expansion.begin(), expansion.end(), [&row](const ExpansionOverride &entry) {
            return entry.key == row.key;
          });
      return found == expansion.end() ? row.defaultExpanded : found->expanded;
    }
    if ((!focusScene || row.key.scene == *focusScene) &&
        std::find(focusPath.begin(), focusPath.end(), row.key.object) !=
            focusPath.end())
      return true;
    const auto found = std::find_if(expansion.begin(), expansion.end(),
                                    [&row](const ExpansionOverride &entry) {
                                      return entry.key == row.key;
                                    });
    return found == expansion.end() ? row.defaultExpanded : found->expanded;
  }

  void setExpanded(const GraphRow &row, bool value) {
    auto found = std::find_if(expansion.begin(), expansion.end(),
                              [&row](const ExpansionOverride &entry) {
                                return entry.key == row.key;
                              });
    if (found == expansion.end()) {
      expansion.push_back(ExpansionOverride{row.key, value});
      ++expansionRevision;
    } else if (found->expanded != value) {
      found->expanded = value;
      ++expansionRevision;
    }
  }

  [[nodiscard]] std::string nodeName(const FrameModel &model,
                                     NodeId node) const {
    const auto found = std::find_if(model.browser.graphRows.begin(),
                                    model.browser.graphRows.end(),
                                    [node](const GraphRow &row) {
                                      return row.kind == GraphRowKind::node &&
                                             row.key.object == node.untyped();
                                    });
    return found == model.browser.graphRows.end()
               ? std::string("Node ") + std::to_string(node.slot())
               : fallbackName(found->label, "Node", found->documentIndex);
  }

  void requestDelete(const FrameModel &model, NodeSelection selected,
                     std::optional<GraphRowKey> exactRow = std::nullopt) {
    if (!exactRow) {
      const auto found = std::find_if(
          model.browser.graphRows.begin(), model.browser.graphRows.end(),
          [&selected](const GraphRow &row) {
            return row.kind == GraphRowKind::node &&
                   row.key.scene == selected.scene &&
                   row.key.object == selected.node.untyped() &&
                   (!selected.parent ||
                    row.key.directParent == selected.parent);
          });
      if (found != model.browser.graphRows.end()) {
        exactRow = found->key;
        if (!selected.parent)
        selected.parent = found->key.directParent;
      }
    }
    pendingDelete =
        PendingDelete{selected.scene, selected.node, selected.parent,
                      nodeName(model, selected.node), std::move(exactRow)};
    openDeleteNextFrame = true;
  }

  void openRename(EditorObjectId object, std::string_view name,
                  std::optional<GraphRowKey> exactRow = std::nullopt) {
    renameTarget = object;
    renameBuffer = name;
    renameRow = std::move(exactRow);
    openRenameNextFrame = true;
    focusRenameInput = true;
  }

  void openAssignment(const PrimitiveContext &primitive) {
    assignPrimitive = primitive;
    assignMaterial = primitive.material;
    openAssignNextFrame = true;
  }

  void updateViewport(const ViewportSettings &settings,
                      SceneEditorUiHost &host) {
    persistent.viewport = settings;
    host.setViewportSettings(settings);
  }

  void invoke(CommandId id, CommandSource source, const FrameModel &model,
              SceneEditorUiHost &host,
              std::optional<std::filesystem::path> path = std::nullopt) {
    const std::size_t index = enumIndex(id);
    if (index >= invokedThisFrame.size() || invokedThisFrame[index])
      return;
    invokedThisFrame[index] = true;

    if (!evaluateCommand(id, model, persistent.windows, persistent.viewport)
             .enabled)
      return;
    if (!SceneEditorUi::viewportCommandAvailable(
            id, host.viewportControlsAvailable()))
      return;

    switch (id) {
    case CommandId::newScene:
    case CommandId::exit:
    case CommandId::undo:
    case CommandId::redo:
    case CommandId::duplicateSelection:
    case CommandId::compactScene:
      submit(source, InvokeCommand{id, std::move(path)}, host);
      break;

    case CommandId::open: {
      auto selected = host.nativeFileDialog(
          {NativeDialogKind::openScene, "Load 3D Scene", {}, {}});
      if (selected)
        submit(source, InvokeCommand{id, std::move(selected)}, host);
      break;
    }

    case CommandId::importMerge: {
      auto selected = host.nativeFileDialog(
          {NativeDialogKind::importMergeScene, "Import/Merge Scene", {}, {}});
      if (selected)
        submit(source, InvokeCommand{id, std::move(selected)}, host);
      break;
    }

    case CommandId::saveAs: {
      auto selected = host.nativeFileDialog(
          {NativeDialogKind::saveScene, "Save glTF", {}, {}});
      if (selected)
        submit(source, InvokeCommand{id, std::move(selected)}, host);
      break;
    }

    case CommandId::openRecent:
      if (path)
        submit(source, InvokeCommand{id, std::move(path)}, host);
      break;

    case CommandId::deleteSelection:
      if (const auto selected = selectedNode(model.selection))
        requestDelete(model, *selected);
      break;

    case CommandId::fitScene:
      camera(CameraRequest{CameraRequestKind::fitScene, std::nullopt,
                           std::nullopt},
             host);
      break;

    case CommandId::fitObject:
      camera(CameraRequest{CameraRequestKind::fitSelection, std::nullopt,
                           std::nullopt},
             host);
      break;

    case CommandId::toggleGrid: {
      auto settings = persistent.viewport;
      settings.showGrid = !settings.showGrid;
      updateViewport(settings, host);
      break;
    }

    case CommandId::toggleGizmo: {
      auto settings = persistent.viewport;
      settings.showGizmo = !settings.showGizmo;
      updateViewport(settings, host);
      break;
    }

    case CommandId::toggleSnap: {
      auto settings = persistent.viewport;
      settings.snapEnabled = !settings.snapEnabled;
      updateViewport(settings, host);
      break;
    }

    case CommandId::showGridSnapSettings:
      persistent.windows.gridSnapSettings = true;
      ImGui::SetWindowFocus("Grid & Snap");
      break;

    case CommandId::toggleSceneBrowser:
      persistent.windows.sceneBrowser = !persistent.windows.sceneBrowser;
      if (persistent.windows.sceneBrowser) {
        focusSceneBrowserNextFrame = true;
        result.requestedSceneBrowserFocus = true;
      }
      break;

    case CommandId::toggleInspector:
      persistent.windows.inspector = !persistent.windows.inspector;
      if (persistent.windows.inspector) {
        focusInspectorNextFrame = true;
        result.requestedInspectorFocus = true;
      }
      break;

    case CommandId::clearSelection:
      submit(source, SelectObject{EditorSelectionSnapshot::empty()}, host);
      break;

    case CommandId::count:
      break;
    }
  }

  [[nodiscard]] std::string historyLabel(CommandId id,
                                         const FrameModel &model) const {
    const std::string_view description =
        id == CommandId::undo ? std::string_view(model.undoDescription)
                              : std::string_view(model.redoDescription);
    std::string label(commandDescriptor(id).label);
    if (!description.empty()) {
      label.push_back(' ');
      label.append(description);
    }
    return label;
  }

  bool menuItem(CommandId id, const FrameModel &model, bool selected = false,
                const char *labelOverride = nullptr) const {
    const auto state =
        evaluateCommand(id, model, persistent.windows, persistent.viewport);
    const auto &descriptor = commandDescriptor(id);
    return ImGui::MenuItem(
        labelOverride ? labelOverride : descriptor.label.data(),
        descriptor.shortcut.empty() ? nullptr : descriptor.shortcut.data(),
        selected, state.enabled);
  }

  void drawFileMenu(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMenu("File"))
      return;
    if (menuItem(CommandId::newScene, model))
      invoke(CommandId::newScene, CommandSource::menu, model, host);
    if (menuItem(CommandId::open, model))
      invoke(CommandId::open, CommandSource::menu, model, host);
    if (menuItem(CommandId::importMerge, model))
      invoke(CommandId::importMerge, CommandSource::menu, model, host);

    const auto recentState = evaluateCommand(
        CommandId::openRecent, model, persistent.windows, persistent.viewport);
    if (ImGui::BeginMenu("Open Recent", recentState.enabled)) {
      if (model.recentFiles.empty()) {
        ImGui::MenuItem("No recent files", nullptr, false, false);
      }
      for (std::size_t index = 0; index < model.recentFiles.size(); ++index) {
        const auto &recent = model.recentFiles[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string label = recent.path.string();
        if (ImGui::MenuItem(label.c_str())) {
          invokedThisFrame[enumIndex(CommandId::openRecent)] = false;
          invoke(CommandId::openRecent, CommandSource::menu, model, host,
                 recent.path);
        }
        if (!recent.exists)
          ImGui::SetItemTooltip(
              "File is unavailable; selecting it reports the error without "
              "replacing the current document.");
        ImGui::PopID();
      }
      ImGui::EndMenu();
    }

    if (menuItem(CommandId::saveAs, model))
      invoke(CommandId::saveAs, CommandSource::menu, model, host);
    ImGui::Separator();
    if (menuItem(CommandId::exit, model))
      invoke(CommandId::exit, CommandSource::menu, model, host);
    ImGui::EndMenu();
  }

  void drawEditMenu(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMenu("Edit"))
      return;
    const std::string undo = historyLabel(CommandId::undo, model);
    if (menuItem(CommandId::undo, model, false, undo.c_str()))
      invoke(CommandId::undo, CommandSource::menu, model, host);
    const std::string redo = historyLabel(CommandId::redo, model);
    if (menuItem(CommandId::redo, model, false, redo.c_str()))
      invoke(CommandId::redo, CommandSource::menu, model, host);
    ImGui::Separator();
    if (menuItem(CommandId::duplicateSelection, model))
      invoke(CommandId::duplicateSelection, CommandSource::menu, model, host);
    if (menuItem(CommandId::deleteSelection, model))
      invoke(CommandId::deleteSelection, CommandSource::menu, model, host);
    ImGui::EndMenu();
  }

  void drawViewMenu(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMenu("View"))
      return;
    if (menuItem(CommandId::fitScene, model))
      invoke(CommandId::fitScene, CommandSource::menu, model, host);
    if (menuItem(CommandId::fitObject, model))
      invoke(CommandId::fitObject, CommandSource::menu, model, host);
    ImGui::Separator();
    for (const CommandId id : {CommandId::toggleGrid, CommandId::toggleGizmo,
                               CommandId::toggleSnap}) {
      const auto enabled =
          evaluateCommand(id, model, persistent.windows, persistent.viewport);
      const bool viewportAvailable = SceneEditorUi::viewportCommandAvailable(
          id, host.viewportControlsAvailable());
      const std::string label(commandDescriptor(id).label);
      const std::string shortcut(commandDescriptor(id).shortcut);
      if (ImGui::MenuItem(label.c_str(),
                          shortcut.empty() ? nullptr : shortcut.c_str(),
                          enabled.checked && viewportAvailable,
                          enabled.enabled && viewportAvailable))
        invoke(id, CommandSource::menu, model, host);
      if (!viewportAvailable &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Central viewport controls are not connected");
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!host.viewportControlsAvailable());
    if (menuItem(CommandId::showGridSnapSettings, model,
                 persistent.windows.gridSnapSettings))
      invoke(CommandId::showGridSnapSettings, CommandSource::menu, model, host);
    ImGui::EndDisabled();
    ImGui::EndMenu();
  }

  void drawWindowsMenu(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMenu("Windows"))
      return;
    if (menuItem(CommandId::toggleSceneBrowser, model,
                 persistent.windows.sceneBrowser))
      invoke(CommandId::toggleSceneBrowser, CommandSource::menu, model, host);
    if (menuItem(CommandId::toggleInspector, model,
                 persistent.windows.inspector))
      invoke(CommandId::toggleInspector, CommandSource::menu, model, host);
    ImGui::BeginDisabled(!host.viewportControlsAvailable());
    if (menuItem(CommandId::showGridSnapSettings, model,
                 persistent.windows.gridSnapSettings, "Grid & Snap"))
      invoke(CommandId::showGridSnapSettings, CommandSource::menu, model, host);
    ImGui::EndDisabled();
    ImGui::EndMenu();
  }

  void drawToolsMenu(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMenu("Tools"))
      return;
    if (menuItem(CommandId::compactScene, model))
      invoke(CommandId::compactScene, CommandSource::menu, model, host);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Remove orphaned resources and clear stale history "
                        "after a successful compact operation.");
    ImGui::EndMenu();
  }

  bool toolbarButton(CommandId id, std::string_view icon, const char *tooltip,
                     const FrameModel &model, bool available = true) {
    const auto command =
        evaluateCommand(id, model, persistent.windows, persistent.viewport);
    const float side = ImGui::GetFrameHeight();
    ImGui::PushID(static_cast<int>(id));
    ImGui::BeginDisabled(!command.enabled || !available);
    if (command.checked && available)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const std::string label(icon);
    const bool clicked = ImGui::Button(label.c_str(), ImVec2(side, side));
    if (command.checked && available)
      ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", available
                                  ? tooltip
                                  : "Central viewport controls are not connected");
    }
    ImGui::PopID();
    return clicked;
  }

  void drawToolbar(const FrameModel &model, SceneEditorUiHost &host) {
    const ImGuiStyle &style = ImGui::GetStyle();
    const float buttonSide = ImGui::GetFrameHeight();
    const float toolbarWidth =
        5.0F * buttonSide + 4.0F * style.ItemSpacing.x + 32.0F;
    const float centered =
        std::max(ImGui::GetCursorPosX() + style.ItemSpacing.x,
                 ImGui::GetWindowWidth() * 0.5F - toolbarWidth * 0.5F);
    ImGui::SameLine(centered);
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (toolbarButton(CommandId::toggleSceneBrowser, icons.sceneBrowser,
                      "Toggle Scene Browser Window (F2)", model))
      invoke(CommandId::toggleSceneBrowser, CommandSource::toolbar, model,
             host);
    ImGui::SameLine(0.0F, 0.0F);
    if (toolbarButton(CommandId::toggleInspector, icons.inspector,
                      "Toggle Inspector Window (F4)", model))
      invoke(CommandId::toggleInspector, CommandSource::toolbar, model, host);

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const bool viewportAvailable = host.viewportControlsAvailable();
    if (toolbarButton(CommandId::toggleGrid, icons.grid,
                      "Toggle Grid (G in viewport)", model,
                      viewportAvailable))
      invoke(CommandId::toggleGrid, CommandSource::toolbar, model, host);
    ImGui::SameLine(0.0F, 0.0F);
    if (toolbarButton(CommandId::toggleGizmo, icons.gizmo,
                      "Toggle Gizmo (T in viewport)", model,
                      viewportAvailable))
      invoke(CommandId::toggleGizmo, CommandSource::toolbar, model, host);
    ImGui::SameLine(0.0F, 0.0F);
    if (toolbarButton(CommandId::toggleSnap, icons.snap, "Toggle Snap", model,
                      viewportAvailable))
      invoke(CommandId::toggleSnap, CommandSource::toolbar, model, host);

    if (!model.operation.label.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", model.operation.label.c_str());
      if (model.operation.progress) {
        ImGui::SameLine();
        ImGui::ProgressBar(std::clamp(*model.operation.progress, 0.0F, 1.0F),
                           ImVec2(80.0F, 0.0F));
      }
    }
  }

  void drawMenuBar(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginMainMenuBar())
      return;
    drawFileMenu(model, host);
    drawEditMenu(model, host);
    drawViewMenu(model, host);
    drawWindowsMenu(model, host);
    drawToolsMenu(model, host);
    drawToolbar(model, host);
    ImGui::EndMainMenuBar();
  }

  void handleShortcuts(const FrameModel &model, SceneEditorUiHost &host) {
    const ImGuiIO &io = ImGui::GetIO();
    if (io.AppFocusLost)
      pendingViewportEscapes = 0u;
    const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const bool viewportHandledEscape = escapePressed && pendingViewportEscapes != 0u;
    if (viewportHandledEscape)
      --pendingViewportEscapes;
    const bool textInput = io.WantTextInput;
    const bool popupActive =
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (modalPending() || model.lifecycle == LifecycleState::modal ||
        popupActive)
      return;

    if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
      invoke(CommandId::toggleSceneBrowser, CommandSource::shortcut, model,
             host);
    if (ImGui::IsKeyPressed(ImGuiKey_F4, false))
      invoke(CommandId::toggleInspector, CommandSource::shortcut, model, host);

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
      invoke(CommandId::fitScene, CommandSource::shortcut, model, host);
    else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
      invoke(CommandId::fitObject, CommandSource::shortcut, model, host);

    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N))
      invoke(CommandId::newScene, CommandSource::shortcut, model, host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
      invoke(CommandId::open, CommandSource::shortcut, model, host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
      invoke(CommandId::saveAs, CommandSource::shortcut, model, host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q))
      invoke(CommandId::exit, CommandSource::shortcut, model, host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
      invoke(CommandId::undo, CommandSource::shortcut, model, host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
      invoke(CommandId::redo, CommandSource::shortcut, model, host);
    if (!textInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D))
      invoke(CommandId::duplicateSelection, CommandSource::shortcut, model,
             host);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K))
      invoke(CommandId::compactScene, CommandSource::shortcut, model, host);

    if (!textInput && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
      invoke(CommandId::deleteSelection, CommandSource::shortcut, model, host);

    const bool noModifiers =
        !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
    if (SceneEditorUi::viewportShortcutEligible(
            model, modalPending(), textInput, noModifiers,
            host.viewportControlsAvailable())) {
      if (ImGui::IsKeyPressed(ImGuiKey_G, false))
        invoke(CommandId::toggleGrid, CommandSource::shortcut, model, host);
      if (ImGui::IsKeyPressed(ImGuiKey_T, false))
        invoke(CommandId::toggleGizmo, CommandSource::shortcut, model, host);
    }

    if (escapePressed && !viewportHandledEscape && !model.viewportOwnsEscape)
      invoke(CommandId::clearSelection, CommandSource::shortcut, model, host);
  }

  void drawAssetInfo(const AssetInfo &asset) {
    if (!ImGui::CollapsingHeader("Asset Info"))
      return;

    ImGui::Text("glTF Version: %s", asset.version.c_str());
    if (!asset.generator.empty())
      ImGui::TextWrapped("Generator: %s", asset.generator.c_str());
    if (!asset.copyright.empty())
      ImGui::TextWrapped("Copyright: %s", asset.copyright.c_str());
    if (!asset.minimumVersion.empty())
      ImGui::Text("Min Version: %s", asset.minimumVersion.c_str());

    if (!asset.extensions.empty()) {
      ImGui::SeparatorText("Extensions");
      for (const std::string &extension : asset.extensions)
        ImGui::BulletText("%s", extension.c_str());
    }

    if (!asset.metadata.empty()) {
      ImGui::SeparatorText("Metadata");
      if (ImGui::BeginTable(
              "AssetMetadata", 2,
              ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Value");
        for (const AssetField &field : asset.metadata) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(field.label.c_str());
          ImGui::TableNextColumn();
          ImGui::TextWrapped("%s", field.value.c_str());
        }
        ImGui::EndTable();
      }
    }
  }

  void drawAnimation(const AnimationState &animation, SceneEditorUiHost &host) {
    if (animation.items.empty() || !ImGui::CollapsingHeader("Animation"))
      return;

    int selectedIndex = 0;
    for (std::size_t index = 0; index < animation.items.size(); ++index) {
      if (animation.current &&
          animation.items[index].id == *animation.current) {
        selectedIndex = static_cast<int>(index);
        break;
      }
    }

    const std::string currentName = fallbackName(
        animation.items[static_cast<std::size_t>(selectedIndex)].name,
        "Animation",
        animation.items[static_cast<std::size_t>(selectedIndex)].documentIndex);
    if (ImGui::BeginCombo("Animations", currentName.c_str())) {
      for (std::size_t index = 0; index < animation.items.size(); ++index) {
        const AnimationItem &item = animation.items[index];
        const bool selected = static_cast<int>(index) == selectedIndex;
        const std::string name =
            fallbackName(item.name, "Animation", item.documentIndex);
        if (ImGui::Selectable(name.c_str(), selected))
          submit(CommandSource::sceneBrowser,
                 AnimationAction{AnimationOperation::select, item.id, 0.0},
                 host);
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }

    ImGui::SeparatorText("Animation Controls");
    ImGui::BeginDisabled(!animation.runtimeControlsAvailable);
    if (ImGui::Button(
            std::string(animation.playing ? icons.pause : icons.play).c_str()))
      submit(CommandSource::sceneBrowser,
             AnimationAction{AnimationOperation::setPlaying, animation.current,
                             animation.playing ? 0.0 : 1.0},
             host);
    ImGui::SetItemTooltip("Play/Pause the animation");
    ImGui::SameLine();
    if (ImGui::Button(std::string(icons.step).c_str()))
      submit(CommandSource::sceneBrowser,
             AnimationAction{AnimationOperation::step, animation.current, 0.0},
             host);
    ImGui::SetItemTooltip("Advance one frame");
    ImGui::SameLine();
    if (ImGui::Button(std::string(icons.reset).c_str()))
      submit(CommandSource::sceneBrowser,
             AnimationAction{AnimationOperation::reset, animation.current, 0.0},
             host);
    ImGui::SetItemTooltip("Reset animation to start");

    ImGui::SameLine(0.0F, 10.0F);
    float speed = static_cast<float>(animation.speed);
    ImGui::SetNextItemWidth(70.0F);
    if (ImGui::DragFloat("##animation_speed", &speed, 0.01F, 0.0F, 100.0F,
                         "%.2f"))
      submit(CommandSource::sceneBrowser,
             AnimationAction{AnimationOperation::setSpeed, animation.current,
                             static_cast<double>(speed)},
             host);
    ImGui::SetItemTooltip("Playback speed multiplier");
    ImGui::SameLine(0.0F, 2.0F);
    ImGui::TextUnformatted("x");

    const AnimationItem &current =
        animation.items[static_cast<std::size_t>(selectedIndex)];
    float time = static_cast<float>(current.currentTime);
    ImGui::TextDisabled("Timeline");
    ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
    if (ImGui::SliderFloat("##animation_timeline", &time,
                           static_cast<float>(current.startTime),
                           static_cast<float>(current.endTime), "Time: %.2f"))
      submit(CommandSource::sceneBrowser,
              AnimationAction{AnimationOperation::setTime, current.id,
                              static_cast<double>(time)},
              host);
    ImGui::EndDisabled();
    if (!animation.runtimeControlsAvailable &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "Animation sampling is unavailable in the current runtime");
  }

  void drawVariants(const std::vector<VariantItem> &variants,
                    SceneEditorUiHost &host) {
    if (variants.empty() || !ImGui::CollapsingHeader("Material Variants"))
      return;
    ImGui::PushID("Variants");
    for (const VariantItem &variant : variants) {
      ImGui::PushID(static_cast<int>(variant.index));
      const std::string name =
          fallbackName(variant.name, "Variant", variant.index);
      ImGui::BeginDisabled(!variant.enabled);
      if (ImGui::Selectable(name.c_str(), variant.current) && variant.enabled)
        submit(CommandSource::sceneBrowser, SelectVariant{variant.index}, host);
      ImGui::EndDisabled();
      if (!variant.enabled &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "Variant application is unavailable in the current runtime");
      ImGui::PopID();
    }
    ImGui::PopID();
  }

  void beginRenameForRow(const GraphRow &row) {
    openRename(row.key.object,
               fallbackName(row.label,
                            editorObjectKindName(row.key.object.kind),
                            row.documentIndex),
               row.key);
  }

  [[nodiscard]] static std::optional<PrimitiveContext>
  primitiveContextForRow(const FrameModel &model, const GraphRow &row) {
    return SceneEditorUi::primitiveAssignmentContext(model, row);
  }

  void drawGraphContextMenu(const FrameModel &model, const GraphRow &row,
                            SceneEditorUiHost &host) {
    const std::uint32_t contextPopupToken =
        static_cast<std::uint32_t>(ImGui::GetID("GraphContext"));
    if (!ImGui::BeginPopupContextItem("GraphContext"))
      return;
    const std::uint32_t contextWindowToken = static_cast<std::uint32_t>(
        ImGui::GetID("##GraphContextProbeWindow"));

    const bool enabled = model.mutationEnabled && !model.rendererBusy &&
                         !model.modalPending && row.enabled &&
                         !lifecycleBlocksCommands(model.lifecycle);
    ImGui::BeginDisabled(!enabled);
    if (row.kind == GraphRowKind::scene) {
      const bool addNode = ImGui::MenuItem("Add Node");
      observeLastItem(EditorUiProbeSemantic::sceneAddNode, row.key,
                      row.key.object, enabled, "Add Node", contextPopupToken, 0u,
                      contextWindowToken);
      if (addNode)
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addRootNode, row.key.scene,
                               std::nullopt, std::nullopt},
               host);
      ImGui::Separator();
      const bool addPointLight = ImGui::MenuItem("Add Point Light");
      observeLastItem(EditorUiProbeSemantic::sceneAddPointLight, row.key,
                      row.key.object, enabled, "Add Point Light",
                      contextPopupToken, 0u,
                      contextWindowToken);
      if (addPointLight)
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addPointLight, row.key.scene,
                               std::nullopt, std::nullopt},
               host);
      const bool addDirectionalLight =
          ImGui::MenuItem("Add Directional Light");
      observeLastItem(EditorUiProbeSemantic::sceneAddDirectionalLight, row.key,
                      row.key.object, enabled, "Add Directional Light",
                      contextPopupToken, 0u,
                      contextWindowToken);
      if (addDirectionalLight)
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addDirectionalLight,
                               row.key.scene, std::nullopt, std::nullopt},
               host);
      const bool addSpotLight = ImGui::MenuItem("Add Spot Light");
      observeLastItem(EditorUiProbeSemantic::sceneAddSpotLight, row.key,
                      row.key.object, enabled, "Add Spot Light",
                      contextPopupToken, 0u,
                      contextWindowToken);
      if (addSpotLight)
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addSpotLight, row.key.scene,
                               std::nullopt, std::nullopt},
               host);
    } else if (row.kind == GraphRowKind::node) {
      const auto node = NodeId::from(row.key.object);
      if (node) {
        const bool duplicate = ImGui::MenuItem("Duplicate", "Ctrl+D");
        observeLastItem(EditorUiProbeSemantic::nodeDuplicate, row.key,
                        row.key.object, enabled, "Duplicate", contextPopupToken, 0u,
                        contextWindowToken);
        if (duplicate)
          submit(CommandSource::sceneBrowser,
                 HierarchyAction{HierarchyOperation::duplicateNode,
                                 row.key.scene, *node, row.key.directParent},
                 host);
        const bool deleteNode = ImGui::MenuItem("Delete", "Delete");
        observeLastItem(EditorUiProbeSemantic::nodeDelete, row.key,
                        row.key.object, enabled, "Delete", contextPopupToken, 0u,
                        contextWindowToken);
        if (deleteNode)
          requestDelete(
              model, NodeSelection{row.key.scene, *node, row.key.directParent},
              row.key);
        ImGui::Separator();
        const bool addChildOpen = ImGui::BeginMenu("Add Child");
        const std::uint32_t addChildPopupToken =
            static_cast<std::uint32_t>(ImGui::GetItemID());
        observeLastItem(EditorUiProbeSemantic::nodeAddChildMenu, row.key,
                        row.key.object, enabled, "Add Child", contextPopupToken,
                        0u,
                        contextWindowToken);
        if (addChildOpen) {
          const std::uint32_t addChildWindowToken =
              static_cast<std::uint32_t>(
                  ImGui::GetID("##AddChildProbeWindow"));
          const bool addChildNode = ImGui::MenuItem("Empty Node");
          observeLastItem(EditorUiProbeSemantic::nodeAddChildNode, row.key,
                          row.key.object, enabled, "Empty Node",
                          addChildPopupToken, 0u,
                          addChildWindowToken);
          if (addChildNode)
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addChildNode,
                                   row.key.scene, *node, row.key.directParent},
                   host);
          ImGui::Separator();
          const bool addPointLight = ImGui::MenuItem("Point Light");
          observeLastItem(EditorUiProbeSemantic::nodeAddChildPointLight,
                          row.key, row.key.object, enabled, "Point Light",
                          addChildPopupToken, 0u,
                          addChildWindowToken);
          if (addPointLight)
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addPointLight,
                                   row.key.scene, *node, row.key.directParent},
                   host);
          const bool addDirectionalLight =
              ImGui::MenuItem("Directional Light");
          observeLastItem(EditorUiProbeSemantic::nodeAddChildDirectionalLight,
                          row.key, row.key.object, enabled,
                          "Directional Light", addChildPopupToken, 0u,
                          addChildWindowToken);
          if (addDirectionalLight)
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addDirectionalLight,
                                   row.key.scene, *node, row.key.directParent},
                   host);
          const bool addSpotLight = ImGui::MenuItem("Spot Light");
          observeLastItem(EditorUiProbeSemantic::nodeAddChildSpotLight,
                          row.key, row.key.object, enabled, "Spot Light",
                          addChildPopupToken, 0u,
                          addChildWindowToken);
          if (addSpotLight)
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addSpotLight,
                                   row.key.scene, *node, row.key.directParent},
                   host);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        const bool rename = ImGui::MenuItem("Rename");
        observeLastItem(EditorUiProbeSemantic::nodeRename, row.key,
                        row.key.object, enabled, "Rename", contextPopupToken, 0u,
                        contextWindowToken);
        if (rename)
          beginRenameForRow(row);
      }
    } else if (row.kind == GraphRowKind::mesh) {
      if (ImGui::MenuItem("Rename"))
        beginRenameForRow(row);
    } else if (row.kind == GraphRowKind::primitive) {
      ImGui::TextDisabled("Primitive %u operations", row.documentIndex);
      ImGui::Separator();
      const std::optional<PrimitiveContext> clickedPrimitive =
          primitiveContextForRow(model, row);
      ImGui::BeginDisabled(!clickedPrimitive || model.materials.empty());
      if (ImGui::MenuItem("Assign Material...")) {
        persistent.windows.inspector = true;
        focusInspectorNextFrame = true;
        result.requestedInspectorFocus = true;
        openAssignment(*clickedPrimitive);
      }
      ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
  }

  struct DragPayload final {
    EditorObjectId node;
  };

  static_assert(std::is_trivially_copyable_v<DragPayload>);

  void drawDragSource(const FrameModel &model, const GraphRow &row) {
    if (row.kind != GraphRowKind::node || !model.mutationEnabled ||
        model.rendererBusy || model.modalPending ||
        lifecycleBlocksCommands(model.lifecycle))
      return;
    const auto node = NodeId::from(row.key.object);
    if (!node || !ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
      return;
    DragPayload payload{node->untyped()};
    ImGui::SetDragDropPayload("MGIF_SCENE_NODE_MOVE", &payload,
                              sizeof(payload));
    ImGui::Text("Move: %s", row.label.c_str());
    dragSource = row.key;
    dragSourceRevision = model.revision;
    ImGui::EndDragDropSource();
  }

  [[nodiscard]] bool drawDropTarget(const FrameModel &model,
                                    const GraphRow &row,
                                    SceneEditorUiHost &host) {
    if ((row.kind != GraphRowKind::scene && row.kind != GraphRowKind::node) ||
        !model.mutationEnabled || model.rendererBusy || model.modalPending ||
        lifecycleBlocksCommands(model.lifecycle) ||
        !ImGui::BeginDragDropTarget())
      return false;

    bool dropAccepted = false;
    const ImGuiDragDropFlags flags =
        ImGuiDragDropFlags_AcceptBeforeDelivery |
        (row.acceptsNodeDrop ? 0 : ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    const ImGuiPayload *raw =
        ImGui::AcceptDragDropPayload("MGIF_SCENE_NODE_MOVE", flags);
    if (raw && raw->DataSize == static_cast<int>(sizeof(DragPayload))) {
      const auto payload = *static_cast<const DragPayload *>(raw->Data);
      const auto node = NodeId::from(payload.node);
      const auto targetNode = row.kind == GraphRowKind::node
                                  ? NodeId::from(row.key.object)
                                  : std::optional<NodeId>{};
      if (dragSource && node && dragSource->object == node->untyped() &&
          (row.kind == GraphRowKind::scene || targetNode)) {
        ReparentNode request{SceneNodeOccurrence{dragSource->scene, *node,
                                                 dragSource->directParent,
                                                 dragSourceRevision},
                             row.key.scene, targetNode};
        const NodeDropValidation validation =
            row.acceptsNodeDrop ? host.validateNodeDrop(request)
                                : NodeDropValidation{false, row.nodeDropHint};
        dropAccepted = validation.accepted;
        if (!validation.accepted) {
          if (ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(validation.reason.empty()
                                       ? "Cannot move node here"
                                       : validation.reason.c_str());
            ImGui::EndTooltip();
          }
        } else if (raw->IsDelivery()) {
          submit(CommandSource::sceneBrowser, request, host);
        }
      }
    }
    ImGui::EndDragDropTarget();
    return dropAccepted;
  }

  void rebuildVisibleGraphRows(const FrameModel &model) {
    if (cachedGraphRevision == model.revision &&
        cachedExpansionRevision == expansionRevision)
      return;
    for (const GraphRow &row : model.browser.graphRows) {
      if (row.hasChildren && (!focusScene || row.key.scene == *focusScene) &&
          std::find(focusPath.begin(), focusPath.end(), row.key.object) !=
              focusPath.end())
        setExpanded(row, true);
    }
    cachedGraphRevision = model.revision;
    cachedExpansionRevision = expansionRevision;
    visibleGraphRows.clear();
    visibleGraphRows.reserve(model.browser.graphRows.size());

    std::optional<std::uint32_t> collapsedDepth;
    for (std::size_t index = 0; index < model.browser.graphRows.size();
         ++index) {
      const GraphRow &row = model.browser.graphRows[index];
      if (collapsedDepth && row.depth > *collapsedDepth)
        continue;
      collapsedDepth.reset();
      visibleGraphRows.push_back(index);
      if (row.hasChildren && !expanded(row))
        collapsedDepth = row.depth;
    }
  }

  void drawGraphRow(const FrameModel &model, const GraphRow &row,
                    SceneEditorUiHost &host) {
    pushGraphKey(row.key);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();

    const float indent =
        static_cast<float>(row.depth) * ImGui::GetTreeNodeToLabelSpacing();
    if (indent > 0.0F)
      ImGui::Indent(indent);

    if ((!focusScene || row.key.scene == *focusScene) &&
        std::find(focusPath.begin(), focusPath.end(), row.key.object) !=
            focusPath.end())
      ImGui::SetNextItemOpen(true);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_NoTreePushOnOpen;
    // Keep the flattened row cache and ImGui's first frame in the same state.
    // ImGui storage still wins after the user explicitly collapses a row.
    if (row.defaultExpanded)
      flags |= ImGuiTreeNodeFlags_DefaultOpen;
    if (!row.hasChildren)
      flags |= ImGuiTreeNodeFlags_Leaf;
    if (row.selected)
      flags |= ImGuiTreeNodeFlags_Selected;

    const std::string label =
        std::string(graphGlyph(row.kind)) + " " +
        fallbackName(row.label, editorObjectKindName(row.key.object.kind),
                     row.documentIndex);
    const std::string renderedLabel =
        std::string(row.effectiveVisible ? "" : "Hidden") + " [" +
        std::to_string(row.documentIndex) + "] " + label;
    ImGui::BeginDisabled(!row.enabled);
    const bool hierarchyInteractionEnabled =
        row.enabled && model.mutationEnabled && !model.rendererBusy &&
        !model.modalPending && !lifecycleBlocksCommands(model.lifecycle);
    const bool open = ImGui::TreeNodeEx("##row", flags, "%s",
                                        renderedLabel.c_str());
    std::optional<EditorUiProbeItem> dropTargetObservation;
    if (probe != nullptr && (row.kind == GraphRowKind::scene ||
                             row.kind == GraphRowKind::node)) {
      dropTargetObservation = captureLastItem(
          EditorUiProbeSemantic::hierarchyDropTarget, row.key, row.key.object,
          hierarchyInteractionEnabled, renderedLabel);
      dropTargetObservation->hovered =
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    observeLastItem(EditorUiProbeSemantic::graphRow, row.key, row.key.object,
                    hierarchyInteractionEnabled, renderedLabel);
    if (row.kind == GraphRowKind::node)
      observeLastItem(EditorUiProbeSemantic::nodeDragSource, row.key,
                      row.key.object, hierarchyInteractionEnabled,
                      renderedLabel);
    if (row.hasChildren && open != expanded(row))
      setExpanded(row, open);

    if (row.selectable && row.enabled && ImGui::IsItemClicked() &&
        !ImGui::IsItemToggledOpen())
      submit(CommandSource::sceneBrowser, SelectObject{row.selection}, host);

    if (ImGui::IsItemHovered() &&
        ImGui::CalcTextSize(label.c_str()).x > ImGui::GetContentRegionAvail().x)
      ImGui::SetTooltip("%s", label.c_str());

    drawDragSource(model, row);
    const bool dropAccepted = drawDropTarget(model, row, host);
    if (dropTargetObservation) {
      dropTargetObservation->dropAccepted = dropAccepted;
      observeItem(std::move(*dropTargetObservation));
    }
    drawGraphContextMenu(model, row, host);
    ImGui::EndDisabled();

    if (indent > 0.0F)
      ImGui::Unindent(indent);

    ImGui::TableNextColumn();
    if (row.kind == GraphRowKind::scene) {
      ImGui::BeginDisabled(!model.mutationEnabled ||
                           lifecycleBlocksCommands(model.lifecycle));
      if (ImGui::SmallButton("Transform")) {
        sceneTransformScene = row.key.scene;
        const auto found =
            std::find_if(model.scenes.begin(), model.scenes.end(),
                         [&row](const SceneChoice &scene) {
                           return scene.id == row.key.scene;
                         });
        sceneTransform = found == model.scenes.end() ? SceneRootTransform{}
                                                     : found->transform;
        openSceneTransformNextFrame = true;
      }
      ImGui::EndDisabled();
    } else if (!row.secondary.empty()) {
      ImGui::TextUnformatted(row.secondary.c_str());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", row.secondary.c_str());
    }

    const bool exactFocus = focusRow
                                ? row.key == *focusRow
                                : ((!focusScene || row.key.scene == *focusScene) &&
                                   sameObject(row.key.object, focusTarget));
    if (scrollToFocus && exactFocus) {
      ImGui::SetScrollHereY(0.5F);
      scrollToFocus = false;
      focusPath.clear();
      focusScene.reset();
      focusTarget = {};
      focusRow.reset();
    }
    popGraphKey(row.key);
  }

  void drawSceneGraph(const FrameModel &model, SceneEditorUiHost &host) {
    rebuildVisibleGraphRows(model);
    if (!ImGui::BeginTable("SceneGraphTable", 2,
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_BordersV))
      return;
    const std::uint32_t previousProbeWindowToken = probeWindowToken;
    probeWindowToken =
        static_cast<std::uint32_t>(ImGui::GetID("##SceneGraphProbeWindow"));
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFrameHeight() * 3.0F);
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visibleGraphRows.size()),
                  ImGui::GetTextLineHeightWithSpacing());
    if (scrollToFocus) {
      const auto found = std::find_if(
          visibleGraphRows.begin(), visibleGraphRows.end(),
          [this, &model](std::size_t index) {
            const GraphRowKey &key = model.browser.graphRows[index].key;
            return focusRow
                       ? key == *focusRow
                       : ((!focusScene || key.scene == *focusScene) &&
                          key.object == focusTarget);
          });
      if (found != visibleGraphRows.end())
        clipper.IncludeItemByIndex(
            static_cast<int>(std::distance(visibleGraphRows.begin(), found)));
    }
    while (clipper.Step()) {
      for (int display = clipper.DisplayStart; display < clipper.DisplayEnd;
           ++display) {
        const std::size_t rowIndex =
            visibleGraphRows[static_cast<std::size_t>(display)];
        drawGraphRow(model, model.browser.graphRows[rowIndex], host);
      }
    }
    ImGui::EndTable();
    probeWindowToken = previousProbeWindowToken;
  }

  void copyMaterial(const FrameModel &model, MaterialId material,
                    SceneEditorUiHost &host, CommandSource source) {
    const auto found =
        std::find_if(model.materials.begin(), model.materials.end(),
                     [material](const MaterialChoice &choice) {
                       return choice.id == material;
                     });
    if (found == model.materials.end())
      return;
    submit(source,
           MaterialAction{
               MaterialOperation::copyReference, std::nullopt, material, {}},
           host);
  }

  void drawResourceContextMenu(const FrameModel &model, ResourceGroupKind group,
                               const ResourceEntry &entry,
                               SceneEditorUiHost &host) {
    if (!ImGui::BeginPopupContextItem("ResourceContext"))
      return;
    const bool canMutate = model.mutationEnabled && !model.rendererBusy &&
                           !model.modalPending &&
                           !lifecycleBlocksCommands(model.lifecycle);
    ImGui::BeginDisabled(!canMutate);
    if (group == ResourceGroupKind::nodes) {
      if (const auto *node =
              std::get_if<NodeSelection>(&entry.selection.value)) {
        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
          submit(CommandSource::sceneBrowser,
                 HierarchyAction{HierarchyOperation::duplicateNode, node->scene,
                                 node->node, node->parent},
                 host);
        if (ImGui::MenuItem("Delete", "Delete"))
          requestDelete(model, *node);
        ImGui::Separator();
        if (ImGui::BeginMenu("Add Child")) {
          if (ImGui::MenuItem("Empty Node"))
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addChildNode,
                                   node->scene, node->node, node->parent},
                   host);
          ImGui::Separator();
          if (ImGui::MenuItem("Point Light"))
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addPointLight,
                                   node->scene, node->node, node->parent},
                   host);
          if (ImGui::MenuItem("Directional Light"))
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addDirectionalLight,
                                   node->scene, node->node, node->parent},
                   host);
          if (ImGui::MenuItem("Spot Light"))
            submit(CommandSource::sceneBrowser,
                   HierarchyAction{HierarchyOperation::addSpotLight,
                                   node->scene, node->node, node->parent},
                   host);
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename"))
          openRename(entry.object,
                     fallbackName(entry.name, "Node", entry.documentIndex));
      }
    } else if (group == ResourceGroupKind::meshes) {
      if (ImGui::MenuItem("Rename"))
        openRename(entry.object,
                   fallbackName(entry.name, "Mesh", entry.documentIndex));
    } else if (group == ResourceGroupKind::materials) {
      if (ImGui::MenuItem("Rename"))
        openRename(entry.object,
                   fallbackName(entry.name, "Material", entry.documentIndex));
      ImGui::Separator();
      if (ImGui::MenuItem("Copy Material ID")) {
        if (const auto material = MaterialId::from(entry.object))
          copyMaterial(model, *material, host, CommandSource::sceneBrowser);
      }
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
  }

  void drawResourceEntry(const FrameModel &model, ResourceGroupKind group,
                         const ResourceEntry &entry, SceneEditorUiHost &host) {
    pushObjectId(entry.object);
    ImGui::PushID(static_cast<int>(entry.documentIndex));
    const std::string name =
        fallbackName(entry.name, resourceItemName(group), entry.documentIndex);
    const std::string label =
        "[" + std::to_string(entry.documentIndex) + "] " + name;

    if (entry.selectable && resourceGroupSelectable(group)) {
      if (ImGui::Selectable(label.c_str(), entry.selected))
        submit(CommandSource::sceneBrowser, SelectObject{entry.selection},
               host);
      drawResourceContextMenu(model, group, entry, host);
    } else if (entry.linkedImage) {
      if (ImGui::Selectable(label.c_str(), false)) {
        pendingImageScroll = entry.linkedImage;
        forceImagesOpen = true;
      }
    } else {
      ImGui::TextUnformatted(label.c_str());
    }

    if (!entry.detail.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", entry.detail.c_str());
    }
    if (ImGui::IsItemHovered() && !entry.tooltip.empty())
      ImGui::SetTooltip("%s", entry.tooltip.c_str());

    if (pendingImageScroll && entry.object == pendingImageScroll->untyped()) {
      ImGui::SetScrollHereY(0.5F);
      pendingImageScroll.reset();
    }
    ImGui::PopID();
    popObjectId();
  }

  void drawLightCreation(const FrameModel &model, SceneEditorUiHost &host) {
    if (!model.mutationEnabled || model.rendererBusy || model.modalPending ||
        lifecycleBlocksCommands(model.lifecycle))
      return;
    const auto current =
        std::find_if(model.scenes.begin(), model.scenes.end(),
                     [](const SceneChoice &scene) { return scene.current; });
    if (current == model.scenes.end())
      return;

    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
    if (ImGui::SmallButton("+"))
      ImGui::OpenPopup("AddLightPopup");
    ImGui::SetItemTooltip("Add light");
    if (ImGui::BeginPopup("AddLightPopup")) {
      if (ImGui::MenuItem("Point Light"))
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addPointLight, current->id,
                               std::nullopt, std::nullopt},
               host);
      if (ImGui::MenuItem("Directional Light"))
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addDirectionalLight,
                               current->id, std::nullopt, std::nullopt},
               host);
      if (ImGui::MenuItem("Spot Light"))
        submit(CommandSource::sceneBrowser,
               HierarchyAction{HierarchyOperation::addSpotLight, current->id,
                               std::nullopt, std::nullopt},
               host);
      ImGui::EndPopup();
    }
  }

  void drawResourceGroup(const FrameModel &model, ResourceGroupKind kind,
                         SceneEditorUiHost &host) {
    const ResourceGroup *group = findResourceGroup(model.browser, kind);
    const std::size_t count = group ? group->entries.size() : 0;
    if (kind == ResourceGroupKind::images && forceImagesOpen) {
      ImGui::SetNextItemOpen(true);
      forceImagesOpen = false;
    }
    const std::string label = std::string(resourceGroupName(kind)) + " (" +
                              std::to_string(count) + ")";
    const bool open = ImGui::CollapsingHeader(label.c_str());
    if (kind == ResourceGroupKind::lights)
      drawLightCreation(model, host);
    if (!open)
      return;

    if (!resourceGroupSelectable(kind))
      ImGui::TextDisabled("(Display only - not selectable)");
    if (!group || group->entries.empty()) {
      ImGui::TextDisabled("No items");
      return;
    }

    const std::string childName =
        std::string(resourceGroupName(kind)) + "ScrollRegion";
    if (ImGui::BeginChild(childName.c_str(), ImVec2(0.0F, 200.0F), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(group->entries.size()),
                    ImGui::GetTextLineHeightWithSpacing());
      if (pendingImageScroll && kind == ResourceGroupKind::images) {
        const auto found =
            std::find_if(group->entries.begin(), group->entries.end(),
                         [this](const ResourceEntry &entry) {
                           return entry.object == pendingImageScroll->untyped();
                         });
        if (found != group->entries.end())
          clipper.IncludeItemByIndex(
              static_cast<int>(std::distance(group->entries.begin(), found)));
      }
      while (clipper.Step()) {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd;
             ++index)
          drawResourceEntry(model, kind,
                            group->entries[static_cast<std::size_t>(index)],
                            host);
      }
    }
    ImGui::EndChild();
  }

  void drawSceneList(const FrameModel &model, SceneEditorUiHost &host) {
    for (ResourceGroupKind kind : kResourceGroupOrder)
      drawResourceGroup(model, kind, host);
  }

  void drawDebug(const FrameModel &model, SceneEditorUiHost &host) {
    const BrowserDebugState &debug = model.browser.debug;
    ImGui::TextUnformatted("Render Node Statistics");
    ImGui::Separator();
    ImGui::Text("Render Nodes: %llu",
                static_cast<unsigned long long>(debug.renderNodeCount));
    ImGui::Text("Render Primitives: %llu",
                static_cast<unsigned long long>(debug.renderPrimitiveCount));
    ImGui::Text("Scene Nodes: %llu",
                static_cast<unsigned long long>(debug.documentNodeCount));
    ImGui::Text("Meshes: %llu",
                static_cast<unsigned long long>(debug.meshCount));
    ImGui::Text("Nodes with Meshes: %llu",
                static_cast<unsigned long long>(debug.nodesWithMeshes));
    ImGui::Text("Expected Render Nodes: %llu",
                static_cast<unsigned long long>(debug.expectedRenderNodeCount));

    if (debug.renderNodeCount != debug.expectedRenderNodeCount) {
      ImGui::TextUnformatted("MISMATCH DETECTED!");
      ImGui::Text("Actual: %llu render nodes",
                  static_cast<unsigned long long>(debug.renderNodeCount));
      ImGui::Text(
          "Expected: %llu render nodes",
          static_cast<unsigned long long>(debug.expectedRenderNodeCount));
    } else {
      ImGui::TextUnformatted("Render nodes match expected count");
    }

    ImGui::SeparatorText("Repair Tools");
    if (debug.gpuSyncValidationActive)
      ImGui::TextDisabled(
          "GPU sync validation active (auto-checked after every scene change)");
    ImGui::BeginDisabled(model.rendererBusy || model.modalPending ||
                         lifecycleBlocksCommands(model.lifecycle));
    if (ImGui::Button("Rebuild All Render Nodes", ImVec2(-1.0F, 0.0F)))
      submit(CommandSource::sceneBrowser, RebuildRenderNodes{}, host);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Force complete regeneration of render nodes from "
                          "the current document projection.");

    ImGui::SeparatorText("Render Node Details");
    if (ImGui::BeginChild("RenderNodeDetails", ImVec2(0.0F, 300.0F), true)) {
      if (ImGui::BeginTable("RenderNodesTable", 5,
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersOuter |
                                ImGuiTableFlags_BordersV |
                                ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("RN ID", ImGuiTableColumnFlags_WidthFixed,
                                50.0F);
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthFixed,
                                50.0F);
        ImGui::TableSetupColumn("Node Name",
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Prim", ImGuiTableColumnFlags_WidthFixed,
                                50.0F);
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed,
                                70.0F);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(debug.rows.size()),
                      ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step()) {
          for (int index = clipper.DisplayStart; index < clipper.DisplayEnd;
               ++index) {
            const DebugRenderRow &row =
                debug.rows[static_cast<std::size_t>(index)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", row.renderNode);
            ImGui::TableNextColumn();
            if (row.documentNode)
              ImGui::Text("%u", *row.documentNode);
            else
              ImGui::TextUnformatted("ERR");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.valid ? row.nodeName.c_str()
                                             : "INVALID");
            ImGui::TableNextColumn();
            if (row.primitive)
              ImGui::Text("%u", *row.primitive);
            else
              ImGui::TextUnformatted("ERR");
            ImGui::TableNextColumn();
            if (row.material)
              ImGui::Text("%u", *row.material);
            else
              ImGui::TextUnformatted("-");
          }
        }
        ImGui::EndTable();
      }
    }
    ImGui::EndChild();
  }

  void drawBrowserTabs(const FrameModel &model, SceneEditorUiHost &host) {
    if (!ImGui::BeginTabBar("SceneBrowserTabs"))
      return;
    if (ImGui::BeginTabItem("Scene Graph")) {
      persistent.browserTab = BrowserTab::sceneGraph;
      drawSceneGraph(model, host);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Scene List")) {
      persistent.browserTab = BrowserTab::sceneList;
      drawSceneList(model, host);
      ImGui::EndTabItem();
    }
#ifndef NDEBUG
    if (model.debugUiAvailable) {
      std::string debugLabel = std::string(icons.debug);
      if (debugLabel != "Debug")
        debugLabel += " Debug";
      if (ImGui::BeginTabItem(debugLabel.c_str())) {
        persistent.browserTab = BrowserTab::debug;
        drawDebug(model, host);
        ImGui::EndTabItem();
      }
    }
#endif
    ImGui::EndTabBar();
  }

  void drawSceneBrowser(const FrameModel &model, SceneEditorUiHost &host) {
    if (!persistent.windows.sceneBrowser)
      return;
    const bool focusRequested = focusSceneBrowserNextFrame;
    focusSceneBrowserNextFrame = false;
    if (model.layout.sceneBrowserDockId != 0)
      ImGui::SetNextWindowDockID(model.layout.sceneBrowserDockId,
                                 ImGuiCond_FirstUseEver);
    else {
      const ImGuiViewport *viewport = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSize(ImVec2(std::max(model.layout.minimumPanelWidth,
                                               viewport->WorkSize.x * 0.22F),
                                      viewport->WorkSize.y),
                               ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(model.layout.minimumPanelWidth, model.layout.minimumPanelHeight),
        ImVec2(std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()));
    const bool sceneBrowserVisible =
        ImGui::Begin("Scene Browser", &persistent.windows.sceneBrowser);
    if (focusRequested)
      ImGui::SetWindowFocus();
    if (!sceneBrowserVisible) {
      ImGui::End();
      return;
    }
    if (!model.hasDocument) {
      ImGui::TextDisabled("No scene loaded");
      fieldEdit = {};
      rotationCache = {};
      ImGui::End();
      return;
    }

    const bool readOnlyBusy = lifecycleBlocksCommands(model.lifecycle) ||
                              model.rendererBusy || model.modalPending ||
                              !model.mutationEnabled;
    if (readOnlyBusy) {
      ImGui::TextDisabled("Scene operation in progress...");
      if (!model.operation.label.empty())
        ImGui::TextWrapped("%s", model.operation.label.c_str());
      ImGui::Separator();
    }

    drawAssetInfo(model.browser.asset);
    ImGui::BeginDisabled(readOnlyBusy);
    drawAnimation(model.browser.animation, host);
    drawVariants(model.browser.variants, host);
    drawBrowserTabs(model, host);
    ImGui::EndDisabled();
    drawSceneTransformPopup(model, host);
    ImGui::End();
  }

  [[nodiscard]] EditorObjectId
  inspectorTarget(const InspectorModel &inspector) const noexcept {
    const EditorSelectionValue &selection = inspector.selection.value;
    if (const auto *node = std::get_if<NodeSelection>(&selection))
      return node->node.untyped();
    if (const auto *primitive = std::get_if<PrimitiveSelection>(&selection))
      return primitive->primitive.untyped();
    if (const auto *mesh = std::get_if<MeshId>(&selection))
      return mesh->untyped();
    if (const auto *material = std::get_if<MaterialId>(&selection))
      return material->untyped();
    if (const auto *cameraValue = std::get_if<CameraId>(&selection))
      return cameraValue->untyped();
    if (const auto *light = std::get_if<LightId>(&selection))
      return light->untyped();
    return {};
  }

  [[nodiscard]] static bool isTextureField(FieldId id) noexcept {
    switch (id) {
    case FieldId::baseColorTexture:
    case FieldId::metallicRoughnessTexture:
    case FieldId::diffuseTexture:
    case FieldId::specularGlossinessTexture:
    case FieldId::emissiveTexture:
    case FieldId::normalTexture:
    case FieldId::occlusionTexture:
    case FieldId::anisotropyTexture:
    case FieldId::clearcoatTexture:
    case FieldId::clearcoatRoughnessTexture:
    case FieldId::clearcoatNormalTexture:
    case FieldId::diffuseTransmissionTexture:
    case FieldId::diffuseTransmissionColorTexture:
    case FieldId::iridescenceTexture:
    case FieldId::iridescenceThicknessTexture:
    case FieldId::sheenColorTexture:
    case FieldId::sheenRoughnessTexture:
    case FieldId::specularTexture:
    case FieldId::specularColorTexture:
    case FieldId::transmissionTexture:
    case FieldId::volumeThicknessTexture:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static std::string valueText(const Value &value) {
    if (const auto *text = std::get_if<std::string>(&value))
      return *text;
    if (const auto *flag = std::get_if<bool>(&value))
      return *flag ? "true" : "false";
    if (const auto *signedValue = std::get_if<std::int32_t>(&value))
      return std::to_string(*signedValue);
    if (const auto *unsignedValue = std::get_if<std::uint32_t>(&value))
      return std::to_string(*unsignedValue);
    if (const auto *number = std::get_if<double>(&value))
      return std::to_string(*number);
    if (const auto *object = std::get_if<EditorObjectId>(&value)) {
      if (!object->valid())
        return "None";
      return std::string(editorObjectKindName(object->kind)) + " [" +
             std::to_string(object->slot) + "]";
    }
    return "-";
  }

  [[nodiscard]] static std::optional<std::int32_t>
  choiceValue(const Value &value) noexcept {
    if (const auto *signedValue = std::get_if<std::int32_t>(&value))
      return *signedValue;
    if (const auto *unsignedValue = std::get_if<std::uint32_t>(&value)) {
      if (*unsignedValue <=
          static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return static_cast<std::int32_t>(*unsignedValue);
    }
    return std::nullopt;
  }

  bool property(EditorObjectId target, FieldId field, EditPhase phase,
                Value value, SceneEditorUiHost &host) {
    if (!target.valid())
      return false;
    return submit(CommandSource::inspector,
           PropertyEdit{target, field, phase, std::move(value)}, host);
  }

  bool finishFieldEdit(SceneEditorUiHost &host) {
    if (fieldEdit.active && fieldEdit.deferred) {
      if (!materialDraft || materialDraft->target != fieldEdit.target)
        materialDraft = MaterialDraftState{fieldEdit.target, {}};
      auto found = std::find_if(
          materialDraft->edits.begin(), materialDraft->edits.end(),
          [&](const PropertyEdit &edit) { return edit.field == fieldEdit.field; });
      PropertyEdit parked{fieldEdit.target, fieldEdit.field, EditPhase::commit,
                          fieldEdit.value};
      if (found == materialDraft->edits.end())
        materialDraft->edits.push_back(std::move(parked));
      else
        *found = std::move(parked);
      fieldEdit = {};
    } else if (fieldEdit.active) {
      const FieldEditState previous = std::exchange(fieldEdit, {});
      if (!property(previous.target, previous.field, EditPhase::commit,
                    previous.value, host))
        return false;
    }
    if (!materialDraft || materialDraft->edits.empty())
      return true;
    const MaterialEditDraft draft{materialDraft->target, materialDraft->edits};
    const RuntimeActionResult result = host.commitMaterialDraft(draft);
    if (result.accepted) {
      materialDraft.reset();
      materialDraftCommitPending = false;
    } else
      consume(result, host);
    return result.accepted;
  }

  void parkMaterialField() {
    if (!fieldEdit.active || !fieldEdit.deferred) return;
    if (!materialDraft || materialDraft->target != fieldEdit.target)
      materialDraft = MaterialDraftState{fieldEdit.target, {}};
    auto found = std::find_if(
        materialDraft->edits.begin(), materialDraft->edits.end(),
        [&](const PropertyEdit &edit) { return edit.field == fieldEdit.field; });
    PropertyEdit parked{fieldEdit.target, fieldEdit.field, EditPhase::commit,
                        fieldEdit.value};
    if (found == materialDraft->edits.end())
      materialDraft->edits.push_back(std::move(parked));
    else
      *found = std::move(parked);
    fieldEdit = {};
    materialDraftCommitPending = true;
    materialDraftParkedThisFrame = true;
  }

  void emitFieldEdit(EditorObjectId target, const InspectorField &field,
                     const FieldDescriptor &descriptor, bool activated,
                     bool changed, bool deactivated, Value edited,
                     SceneEditorUiHost &host, bool colorPickerOpen = false) {
    if (!target.valid() || field.readOnly)
      return;

    const bool continuous = descriptor.continuous ||
                            descriptor.kind == FieldKind::text ||
                            descriptor.kind == FieldKind::color3 ||
                            descriptor.kind == FieldKind::color4;
    // Renderer-visible continuous material inputs share one draft. The
    // viewport consumes supported values directly; document/history work
    // happens once at the end of the drag or when the color popup closes.
    const bool materialTarget = MaterialId::from(target).has_value();
    const bool colorField = descriptor.kind == FieldKind::color3 ||
                            descriptor.kind == FieldKind::color4;
    const auto rendererPreviewField = [](FieldId id) {
      switch (id) {
      case FieldId::baseColor:
      case FieldId::metallic:
      case FieldId::roughness:
      case FieldId::emissiveColor:
      case FieldId::alphaCutoff:
      case FieldId::normalScale:
      case FieldId::occlusionStrength:
        return true;
      default:
        return false;
      }
    };
    const bool deferMaterial = materialTarget && rendererPreviewField(field.id);
    if (continuous) {
      const bool ownsEdit = fieldEdit.active && fieldEdit.target == target &&
                            fieldEdit.field == field.id;
      if (!ownsEdit && (activated || changed)) {
        // Components/fields may deactivate and activate in the same frame.
        // Finish the previous owner before beginning the next gesture.
        bool ready = true;
        if (fieldEdit.active) {
          if (fieldEdit.deferred)
            parkMaterialField();
          else
            ready = finishFieldEdit(host);
        } else if (!deferMaterial) {
          ready = finishFieldEdit(host);
        }
        if (!ready ||
            (!deferMaterial &&
             !property(target, field.id, EditPhase::begin, field.value, host)))
          return;
        fieldEdit = FieldEditState{target, field.id, field.value, field.value,
                                   true, deferMaterial};
      }
      if (!fieldEdit.active || fieldEdit.target != target ||
          fieldEdit.field != field.id)
        return;
      if (changed) {
        if (deferMaterial || property(target, field.id, EditPhase::preview,
                                       edited, host)) {
          fieldEdit.value = edited;
          if (deferMaterial) {
            if (!materialDraft || materialDraft->target != target)
              materialDraft = MaterialDraftState{target, {}};
            auto found = std::find_if(
                materialDraft->edits.begin(), materialDraft->edits.end(),
                [&](const PropertyEdit &edit) { return edit.field == field.id; });
            PropertyEdit draft{target, field.id, EditPhase::commit, edited};
            if (found == materialDraft->edits.end())
              materialDraft->edits.push_back(std::move(draft));
            else
              *found = std::move(draft);
          }
        }
      }
      const bool colorMouseReleased =
          deferMaterial && colorField && !activated && !ImGui::IsItemActive() &&
          !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !colorPickerOpen;
      if ((deactivated || colorMouseReleased) && !activated &&
          (!deferMaterial || !colorPickerOpen)) {
        if (deferMaterial)
          parkMaterialField();
        else
          (void)finishFieldEdit(host);
      }
    } else if (changed) {
      if (!finishFieldEdit(host))
        return;
      if (!materialTarget) {
        property(target, field.id, EditPhase::begin, field.value, host);
        property(target, field.id, EditPhase::preview, edited, host);
      }
      property(target, field.id, EditPhase::commit, std::move(edited), host);
    }
  }

  bool drawTextureField(EditorObjectId target, const InspectorField &field,
                        const char *label, SceneEditorUiHost &host) {
    const auto selected = choiceValue(field.value).value_or(-1);
    const auto current = std::find_if(
        field.choices.begin(), field.choices.end(),
        [selected](const Choice &choice) { return choice.value == selected; });
    const bool hasTexture = selected >= 0 && current != field.choices.end();

    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    if (hasTexture) {
      ImGui::TextDisabled("%s", current->label.c_str());
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", current->label.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Switch")) {
        texturePicker = TexturePicker{target, field.id, field.choices,
                                      field.value, selected};
        openTextureNextFrame = true;
      }
      ImGui::SetItemTooltip("Switch texture");
      ImGui::SameLine(0.0F, 2.0F);
      if (ImGui::SmallButton("Remove"))
        emitFieldEdit(target, field, fieldDescriptor(field.id), true, true,
                      true, Value{std::int32_t{-1}}, host);
      ImGui::SetItemTooltip("Remove texture");
    } else {
      ImGui::TextDisabled("None");
      ImGui::SameLine();
      ImGui::BeginDisabled(field.choices.empty());
      if (ImGui::SmallButton("Add")) {
        texturePicker = TexturePicker{
            target, field.id, field.choices, field.value,
            field.choices.empty() ? -1 : field.choices.front().value};
        openTextureNextFrame = true;
      }
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Add texture");
    }
    return false;
  }

  [[nodiscard]] static std::optional<EditorUiProbeSemantic>
  vectorProbeSemantic(FieldId field, int component) {
    if (component < 0 || component > 2)
      return std::nullopt;
    const int offset = component;
    switch (field) {
    case FieldId::translation:
      return static_cast<EditorUiProbeSemantic>(
          enumIndex(EditorUiProbeSemantic::inspectorTranslationX) + offset);
    case FieldId::rotationDegrees:
      return static_cast<EditorUiProbeSemantic>(
          enumIndex(EditorUiProbeSemantic::inspectorRotationX) + offset);
    case FieldId::scale:
      return static_cast<EditorUiProbeSemantic>(
          enumIndex(EditorUiProbeSemantic::inspectorScaleX) + offset);
    default:
      return std::nullopt;
    }
  }

  bool drawInspectorVector3(EditorObjectId target,
                            const InspectorField &field,
                            const FieldDescriptor &descriptor,
                            std::array<double, 3> &value,
                            const std::string &label, bool &activated,
                            bool &deactivated, bool &tooltipHovered) {
    const float speed = static_cast<float>(
        field.speedOverride.value_or(descriptor.speed));
    const char *format = descriptor.format.empty() ? "%.3f"
                                                    : descriptor.format.data();
    bool changed = false;

    // Match the pinned DragScalarN IDs and rounded PushMultiItemsWidths splits
    // while observing each actual component before the group replaces it.
    ImGui::BeginGroup();
    ImGui::PushID(label.c_str());
    const float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float totalWidth = ImGui::CalcItemWidth() - 2.0F * innerSpacing;
    const float firstSplit = std::trunc(totalWidth / 3.0F);
    const float secondSplit = std::trunc(totalWidth * 2.0F / 3.0F);
    const std::array<float, 3> widths{
        std::max(firstSplit, 1.0F),
        std::max(secondSplit - firstSplit, 1.0F),
        std::max(totalWidth - secondSplit, 1.0F)};
    for (int component = 0; component < 3; ++component) {
      ImGui::PushID(component);
      if (component > 0)
        ImGui::SameLine(0.0F, innerSpacing);
      ImGui::PushItemWidth(widths[component]);
      const bool componentChanged = ImGui::DragScalar(
          "", ImGuiDataType_Double, &value[component], speed,
          nullptr, nullptr, format);
      changed |= componentChanged;
      activated |= ImGui::IsItemActivated();
      deactivated |= ImGui::IsItemDeactivated();
      tooltipHovered |=
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
      if (const auto semantic = vectorProbeSemantic(field.id, component)) {
        const bool enabled =
            (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) == 0;
        observeLastItem(*semantic, {}, target, enabled, label);
      }
      ImGui::PopID();
      ImGui::PopItemWidth();
    }
    ImGui::PopID();
    const std::size_t renderedLength = label.find("##");
    const char *labelEnd = label.c_str() +
                          (renderedLength == std::string::npos
                               ? label.size()
                               : renderedLength);
    if (label.c_str() != labelEnd) {
      ImGui::SameLine(0.0F, innerSpacing);
      ImGui::TextUnformatted(label.c_str(), labelEnd);
      tooltipHovered |=
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    }
    ImGui::EndGroup();
    return changed;
  }

  void drawInspectorField(const InspectorModel &inspector,
                          const InspectorField &field,
                          SceneEditorUiHost &host) {
    if (!field.visible)
      return;
    const FieldDescriptor &descriptor = fieldDescriptor(field.id);
    const std::string label = field.labelOverride.empty()
                                  ? std::string(descriptor.label)
                                  : field.labelOverride;
    const EditorObjectId target =
        field.target.value_or(inspectorTarget(inspector));
    bool colorPickerOpen = false;

    pushObjectId(target);
    ImGui::PushID(static_cast<int>(field.id));
    const bool disabled = !field.enabled || field.readOnly || !target.valid();
    ImGui::BeginDisabled(disabled);

    if (descriptor.kind == FieldKind::readOnlyText ||
        descriptor.kind == FieldKind::objectReference || field.readOnly) {
      ImGui::Text("%s: %s", label.c_str(), valueText(field.value).c_str());
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
          !field.tooltip.empty())
        ImGui::SetTooltip("%s", field.tooltip.c_str());
      ImGui::PopID();
      popObjectId();
      return;
    }

    if (descriptor.kind == FieldKind::choice && isTextureField(field.id)) {
      drawTextureField(target, field, label.c_str(), host);
      ImGui::EndDisabled();
      ImGui::PopID();
      popObjectId();
      return;
    }

    const bool fieldEditActive = fieldEdit.active &&
                                 fieldEdit.target == target &&
                                 fieldEdit.field == field.id;
    Value edited = fieldEditActive ? fieldEdit.value : field.value;
    if (!fieldEditActive && materialDraft && materialDraft->target == target) {
      const auto found = std::find_if(
          materialDraft->edits.begin(), materialDraft->edits.end(),
          [&](const PropertyEdit &edit) { return edit.field == field.id; });
      if (found != materialDraft->edits.end())
        edited = found->value;
    }
    if (field.id == FieldId::rotationDegrees) {
      if (const auto *rotation =
              std::get_if<std::array<double, 3>>(&field.value)) {
        if (!rotationCache.valid || rotationCache.target != target ||
            rotationCache.externalRevision != field.externalValueRevision) {
          rotationCache =
              EulerCache{target, field.externalValueRevision, *rotation, true};
        }
        if (!fieldEditActive)
          edited = rotationCache.value;
      }
    }
    bool changed = false;
    bool activated = false;
    const ImVec2 fieldOrigin = ImGui::GetCursorScreenPos();
    const float fieldWidth = ImGui::CalcItemWidth();
    bool deactivated = false;
    bool tooltipHovered = false;
    bool vectorProbeControl = false;
    switch (descriptor.kind) {
    case FieldKind::boolean:
      if (auto *value = std::get_if<bool>(&edited))
        changed = ImGui::Checkbox(label.c_str(), value);
      break;

    case FieldKind::scalar:
      if (auto *value = std::get_if<double>(&edited)) {
        const double speed = field.speedOverride.value_or(descriptor.speed);
        const auto minimum =
            field.minimumOverride ? field.minimumOverride : descriptor.minimum;
        const auto maximum =
            field.maximumOverride ? field.maximumOverride : descriptor.maximum;
        const double *minimumPointer = minimum ? &*minimum : nullptr;
        const double *maximumPointer = maximum ? &*maximum : nullptr;
        changed = ImGui::DragScalar(
            label.c_str(), ImGuiDataType_Double, value,
            static_cast<float>(speed), minimumPointer, maximumPointer,
            descriptor.format.empty() ? "%.3f" : descriptor.format.data());
      }
      break;

    case FieldKind::integer:
      if (auto *value = std::get_if<std::int32_t>(&edited))
        changed = ImGui::DragInt(label.c_str(), value, 1.0F);
      else if (auto *unsignedValue = std::get_if<std::uint32_t>(&edited))
        changed = ImGui::DragScalar(label.c_str(), ImGuiDataType_U32,
                                    unsignedValue, 1.0F);
      break;

    case FieldKind::text:
      if (auto *value = std::get_if<std::string>(&edited))
        changed = inputTextString(label.c_str(), *value);
      break;

    case FieldKind::vector2:
      if (auto *value = std::get_if<std::array<double, 2>>(&edited)) {
        const double speed = field.speedOverride.value_or(descriptor.speed);
        changed = ImGui::DragScalarN(
            label.c_str(), ImGuiDataType_Double, value->data(), 2,
            static_cast<float>(speed), nullptr, nullptr,
            descriptor.format.empty() ? "%.3f" : descriptor.format.data());
      }
      break;

    case FieldKind::vector3:
      if (auto *value = std::get_if<std::array<double, 3>>(&edited)) {
        vectorProbeControl = vectorProbeSemantic(field.id, 0).has_value();
        if (vectorProbeControl)
          changed = drawInspectorVector3(target, field, descriptor, *value,
                                         label, activated, deactivated,
                                         tooltipHovered);
        else {
          const double speed = field.speedOverride.value_or(descriptor.speed);
          changed = ImGui::DragScalarN(
              label.c_str(), ImGuiDataType_Double, value->data(), 3,
              static_cast<float>(speed), nullptr, nullptr,
              descriptor.format.empty() ? "%.3f" : descriptor.format.data());
        }
      }
      break;

    case FieldKind::color3:
      if (auto *value = std::get_if<std::array<double, 3>>(&edited)) {
        std::array<float, 3> converted{static_cast<float>((*value)[0]),
                                       static_cast<float>((*value)[1]),
                                       static_cast<float>((*value)[2])};
        if (ImGui::ColorEdit3(label.c_str(), converted.data())) {
          *value = {converted[0], converted[1], converted[2]};
          changed = true;
        }
        // ColorEdit scopes its popup under the label; querying from the
        // parent scope misses it and commits on every mouse release.
        ImGui::PushID(label.c_str());
        colorPickerOpen = ImGui::IsPopupOpen("picker");
        ImGui::PopID();
      }
      break;

    case FieldKind::color4:
      if (auto *value = std::get_if<std::array<double, 4>>(&edited)) {
        std::array<float, 4> converted{
            static_cast<float>((*value)[0]), static_cast<float>((*value)[1]),
            static_cast<float>((*value)[2]), static_cast<float>((*value)[3])};
        if (ImGui::ColorEdit4(label.c_str(), converted.data())) {
          *value = {converted[0], converted[1], converted[2], converted[3]};
          changed = true;
        }
        ImGui::PushID(label.c_str());
        colorPickerOpen = ImGui::IsPopupOpen("picker");
        ImGui::PopID();
      }
      break;

    case FieldKind::choice: {
      const auto selected = choiceValue(edited).value_or(0);
      const auto current =
          std::find_if(field.choices.begin(), field.choices.end(),
                       [selected](const Choice &choice) {
                         return choice.value == selected;
                       });
      const char *preview =
          current == field.choices.end() ? "-" : current->label.c_str();
      if (ImGui::BeginCombo(label.c_str(), preview)) {
        for (const Choice &choice : field.choices) {
          const bool selectedChoice = choice.value == selected;
          if (ImGui::Selectable(choice.label.c_str(), selectedChoice)) {
            edited = choice.value;
            changed = true;
          }
          if (selectedChoice)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      break;
    }

    case FieldKind::readOnlyText:
    case FieldKind::objectReference:
      break;
    }

    if (!vectorProbeControl) {
      activated = ImGui::IsItemActivated();
      deactivated = ImGui::IsItemDeactivated();
      tooltipHovered =
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    }
    if (probe != nullptr && field.id == FieldId::baseColor) {
      auto item = captureLastItem(EditorUiProbeSemantic::inspectorBaseColorSwatch,
                                 {}, target, !disabled, label);
      item.itemId = ImGui::GetID(label.c_str());
      const float size = ImGui::GetFrameHeight();
      const float left = ImGui::GetStyle().ColorButtonPosition == ImGuiDir_Left
                             ? fieldOrigin.x : fieldOrigin.x + fieldWidth - size;
      item.rect = {left, fieldOrigin.y, left + size, fieldOrigin.y + size};
      observeItem(item);
    }
    if (probe != nullptr && field.id == FieldId::metallic) {
      observeLastItem(EditorUiProbeSemantic::inspectorMetallic, {}, target,
                      !disabled, label);
    }
    if (probe != nullptr && field.id == FieldId::roughness) {
      observeLastItem(EditorUiProbeSemantic::inspectorRoughness, {}, target,
                      !disabled, label);
    }
    if (changed && field.id == FieldId::rotationDegrees) {
      if (const auto *rotation = std::get_if<std::array<double, 3>>(&edited)) {
        rotationCache =
            EulerCache{target, field.externalValueRevision, *rotation, true};
      }
    }
    ImGui::EndDisabled();
    if (tooltipHovered && !field.tooltip.empty())
      ImGui::SetTooltip("%s", field.tooltip.c_str());
    emitFieldEdit(target, field, descriptor, activated, changed, deactivated,
                  std::move(edited), host, colorPickerOpen);
    ImGui::PopID();
    popObjectId();
  }

  [[nodiscard]] std::string sectionLabel(const InspectorSection &section,
                                         EditorSelectionKind kind) const {
    if (!section.label.empty())
      return section.label;
    switch (section.kind) {
    case InspectorSectionKind::identity:
      return {};
    case InspectorSectionKind::transform:
      return kind == EditorSelectionKind::primitive ? "TRANSFORM (Node)"
                                                    : "TRANSFORM";
    case InspectorSectionKind::attachments:
      return "ATTACHMENTS";
    case InspectorSectionKind::primitive:
      return "PRIMITIVE";
    case InspectorSectionKind::mesh:
      return "MESH";
    case InspectorSectionKind::material:
      return "MATERIAL";
    case InspectorSectionKind::camera:
      return "CAMERA";
    case InspectorSectionKind::light:
      return "LIGHT";
    case InspectorSectionKind::materialExtension:
      return section.materialExtension;
    }
    return {};
  }

  void drawMaterialToolbar(const FrameModel &model,
                           const PrimitiveContext &primitive,
                           SceneEditorUiHost &host) {
    ImGui::TextUnformatted("Primitive/Material Operations");
    ImGui::SameLine();
    ImGui::PushID("material_assignment_toolbar");

    ImGui::BeginDisabled(!primitive.canSplit);
    if (ImGui::SmallButton(std::string(icons.split).c_str()))
      submit(CommandSource::inspector,
             MaterialAction{
                 MaterialOperation::split, primitive, primitive.material, {}},
             host);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Split\nDuplicate mesh and material for independent editing");

    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!primitive.canMerge);
    if (ImGui::SmallButton(std::string(icons.merge).c_str()))
      submit(CommandSource::inspector,
             MaterialAction{
                 MaterialOperation::merge, primitive, primitive.material, {}},
             host);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Merge\nUse an equivalent shared mesh if one exists");

    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!primitive.canAssign || model.materials.empty());
    if (ImGui::SmallButton(std::string(icons.assign).c_str()))
      openAssignment(primitive);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Assign a different material");

    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!primitive.material);
    if (ImGui::SmallButton(std::string(icons.copy).c_str()) &&
        primitive.material)
      copyMaterial(model, *primitive.material, host, CommandSource::inspector);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Copy material reference");

    const ClipboardMaterial clipboard = SceneEditorUi::parseMaterialClipboard(
        host.clipboardText(), model.materials);
    const bool pasteUnavailable =
        clipboard.state == ClipboardMaterialState::none ||
        clipboard.state == ClipboardMaterialState::malformed;
    ImGui::SameLine(0.0F, 2.0F);
    ImGui::BeginDisabled(!primitive.canAssign || !model.mutationEnabled ||
                         pasteUnavailable);
    if (ImGui::SmallButton(std::string(icons.paste).c_str()))
      submit(CommandSource::inspector,
             MaterialAction{MaterialOperation::pasteReference, primitive,
                            std::nullopt, {}},
             host);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      switch (clipboard.state) {
      case ClipboardMaterialState::validIndex:
        ImGui::SetTooltip("Paste: %s (mat %u)", clipboard.materialName.c_str(),
                          *clipboard.index);
        break;
      case ClipboardMaterialState::outOfRange:
        ImGui::SetTooltip("Paste material\n(invalid clipboard)");
        break;
      case ClipboardMaterialState::none:
      case ClipboardMaterialState::malformed:
        ImGui::SetTooltip("Paste material\n(no material copied)");
        break;
      }
    }
    ImGui::PopID();
  }

  void drawMaterialOperations(const FrameModel &model, MaterialId material,
                              SceneEditorUiHost &host) {
    ImGui::Separator();
    ImGui::TextUnformatted("Material Operations:");
    if (ImGui::SmallButton("Duplicate"))
      submit(CommandSource::inspector,
             MaterialAction{
                 MaterialOperation::duplicate, std::nullopt, material, {}},
             host);
    ImGui::SetItemTooltip("Create a copy of this material");
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
      copyMaterial(model, material, host, CommandSource::inspector);
    ImGui::SetItemTooltip("Copy material reference to clipboard");
  }

  void drawInspectorSection(const FrameModel &model,
                            const InspectorSection &section,
                            SceneEditorUiHost &host) {
    const InspectorModel &inspector = model.inspector;
    const EditorSelectionKind selectionKind = inspector.selection.kind();
    const std::string label = sectionLabel(section, selectionKind);
    bool open = true;

    if (section.kind == InspectorSectionKind::materialExtension) {
      ImGuiTreeNodeFlags flags = section.extensionPresent
                                     ? ImGuiTreeNodeFlags_DefaultOpen
                                     : ImGuiTreeNodeFlags_None;
      open = ImGui::TreeNodeEx(label.c_str(), flags);
      if (open) {
        if (inspector.material && section.extensionMutable) {
          if (section.extensionPresent) {
            if (ImGui::SmallButton("Remove"))
              submit(CommandSource::inspector,
                     MaterialAction{MaterialOperation::removeExtension,
                                    std::nullopt, inspector.material,
                                    section.materialExtension},
                     host);
          } else if (ImGui::SmallButton("Add")) {
            submit(CommandSource::inspector,
                   MaterialAction{MaterialOperation::addExtension, std::nullopt,
                                  inspector.material,
                                  section.materialExtension},
                   host);
          }
        }
        for (const InspectorField &field : section.fields)
          drawInspectorField(inspector, field, host);
        ImGui::TreePop();
      }
      return;
    }

    if (section.kind != InspectorSectionKind::identity) {
      ImGuiTreeNodeFlags flags = section.defaultOpen
                                     ? ImGuiTreeNodeFlags_DefaultOpen
                                     : ImGuiTreeNodeFlags_None;
      open = ImGui::CollapsingHeader(label.c_str(), flags);
    }
    if (!open)
      return;

    if (section.kind == InspectorSectionKind::material && inspector.primitive)
      drawMaterialToolbar(model, *inspector.primitive, host);
    for (const InspectorField &field : section.fields)
      drawInspectorField(inspector, field, host);
  }

  void drawCameraSync(const CameraContext &cameraContext,
                      SceneEditorUiHost &host) {
    ImGui::Separator();
    ImGui::TextUnformatted("Camera Sync:");
    ImGui::BeginDisabled(!cameraContext.canApplyToView);
    if (ImGui::Button("Apply to Current View"))
      camera(CameraRequest{CameraRequestKind::applyDocumentCamera,
                           cameraContext.camera, cameraContext.node},
             host);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!cameraContext.canSetFromView);
    if (ImGui::Button("Set from Current View"))
      camera(CameraRequest{CameraRequestKind::setDocumentCameraFromView,
                           cameraContext.camera, cameraContext.node},
             host);
    ImGui::EndDisabled();
  }

  void drawInspector(const FrameModel &model, SceneEditorUiHost &host) {
    if (!persistent.windows.inspector) {
      (void)finishFieldEdit(host);
      return;
    }
    if (focusInspectorNextFrame) {
      ImGui::SetNextWindowFocus();
      focusInspectorNextFrame = false;
    }
    if (model.layout.inspectorDockId != 0)
      ImGui::SetNextWindowDockID(model.layout.inspectorDockId,
                                 ImGuiCond_FirstUseEver);
    else {
      const ImGuiViewport *viewport = ImGui::GetMainViewport();
      const float width = std::max(model.layout.minimumPanelWidth,
                                   viewport->WorkSize.x * 0.22F);
      ImGui::SetNextWindowPos(
          ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width,
                 viewport->WorkPos.y),
          ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y),
                               ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(model.layout.minimumPanelWidth, model.layout.minimumPanelHeight),
        ImVec2(std::numeric_limits<float>::max(),
               std::numeric_limits<float>::max()));
    if (!ImGui::Begin("Inspector", &persistent.windows.inspector)) {
      (void)finishFieldEdit(host);
      ImGui::End();
      return;
    }
    // Inspector component probes need a stable non-zero window token just
    // like Scene Graph probes. The frame resets this token before rendering.
    probeWindowToken =
        static_cast<std::uint32_t>(ImGui::GetID("##InspectorProbeWindow"));
    if (!model.hasDocument) {
      ImGui::TextDisabled("No scene loaded");
      ImGui::End();
      return;
    }
    if (model.inspector.selection.emptyValue()) {
      ImGui::TextDisabled("No selection");
      ImGui::Separator();
      ImGui::TextWrapped("Select an element in the Scene Browser or 3D view to "
                         "view its properties.");
      fieldEdit = {};
      rotationCache = {};
      ImGui::End();
      return;
    }

    const bool readOnlyBusy = lifecycleBlocksCommands(model.lifecycle) ||
                              model.rendererBusy || model.modalPending ||
                              !model.mutationEnabled;
    if (readOnlyBusy) {
      ImGui::TextDisabled("Scene operation in progress...");
      ImGui::TextWrapped(
          "Inspector properties are read-only until the operation completes.");
      ImGui::Separator();
    }

    ImGui::TextUnformatted(
        model.inspector.title.empty()
            ? editorObjectKindName(inspectorTarget(model.inspector).kind)
            : model.inspector.title.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(readOnlyBusy);
    for (const InspectorSection &section : model.inspector.sections) {
      if (section.kind != InspectorSectionKind::materialExtension) {
        drawInspectorSection(model, section, host);
        continue;
      }
      const auto descriptor = std::find_if(
          materialExtensionDescriptors().begin(),
          materialExtensionDescriptors().end(),
          [&section](const MaterialExtensionDescriptor &candidate) {
            return candidate.baseWorkflow &&
                   candidate.extension == section.materialExtension;
          });
      if (descriptor != materialExtensionDescriptors().end())
        drawInspectorSection(model, section, host);
    }
    if (model.inspector.material) {
      ImGui::SeparatorText("Material Extensions");
      for (const MaterialExtensionDescriptor &descriptor :
           materialExtensionDescriptors()) {
        if (descriptor.baseWorkflow)
          continue;
        const auto found = std::find_if(
            model.inspector.sections.begin(), model.inspector.sections.end(),
            [&descriptor](const InspectorSection &section) {
              return section.kind == InspectorSectionKind::materialExtension &&
                     section.materialExtension == descriptor.extension;
            });
        if (found != model.inspector.sections.end()) {
          drawInspectorSection(model, *found, host);
        } else {
          InspectorSection missing;
          missing.kind = InspectorSectionKind::materialExtension;
          missing.label = descriptor.label;
          missing.materialExtension = descriptor.extension;
          missing.extensionMutable = true;
          drawInspectorSection(model, missing, host);
        }
      }
    }
    if (model.inspector.material &&
        model.inspector.selection.kind() == EditorSelectionKind::material)
      drawMaterialOperations(model, *model.inspector.material, host);
    if (model.inspector.camera)
      drawCameraSync(*model.inspector.camera, host);
    ImGui::EndDisabled();
    ImGui::End();
  }

  void drawSceneTransformPopup(const FrameModel &model,
                               SceneEditorUiHost &host) {
    if (openSceneTransformNextFrame && sceneTransformScene) {
      ImGui::OpenPopup("scene_transform_popup");
      openSceneTransformNextFrame = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.0F, 0.0F),
                                        ImVec2(600.0F, 600.0F));
    if (!ImGui::BeginPopup("scene_transform_popup"))
      return;

    ImGui::TextUnformatted("Scene Transform");
    ImGui::Separator();
    const bool blocked = model.rendererBusy || model.modalPending ||
                         lifecycleBlocksCommands(model.lifecycle) ||
                         !model.mutationEnabled;
    ImGui::BeginDisabled(blocked);
    bool changed = false;
    changed |= ImGui::DragScalarN("Translation", ImGuiDataType_Double,
                                  sceneTransform.translation.data(), 3, 0.01F);
    std::array<double, 3> rotationEuler =
        quaternionToEulerDegrees(sceneTransform.rotation);
    if (ImGui::DragScalarN("Rotation", ImGuiDataType_Double,
                           rotationEuler.data(), 3, 0.1F, nullptr, nullptr,
                           "%.3f deg")) {
      sceneTransform.rotation = eulerDegreesToQuaternion(rotationEuler);
      changed = true;
    }
    changed |= ImGui::DragScalarN("Scale", ImGuiDataType_Double,
                                  sceneTransform.scale.data(), 3, 0.01F);
    if (changed && sceneTransformScene)
      submit(CommandSource::sceneBrowser,
             SceneTransformAction{*sceneTransformScene, sceneTransform, false},
             host);
    if (ImGui::Button("Reset") && sceneTransformScene) {
      sceneTransform = {};
      submit(CommandSource::sceneBrowser,
             SceneTransformAction{*sceneTransformScene, sceneTransform, true},
             host);
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
  }

  void drawRenameDialog(SceneEditorUiHost &host) {
    const std::uint32_t modalToken =
        static_cast<std::uint32_t>(ImGui::GetID("RenameDialog"));
    if (openRenameNextFrame && renameTarget.valid()) {
      ImGui::OpenPopup("RenameDialog");
      openRenameNextFrame = false;
    }
    bool open = true;
    if (!ImGui::BeginPopupModal("RenameDialog", &open,
                                ImGuiWindowFlags_AlwaysAutoResize))
      return;
    if (!open) {
      renameTarget = {};
      renameBuffer.clear();
      renameRow.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::EndPopup();
      return;
    }
    if (!renameTarget.valid()) {
      renameRow.reset();
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }

    const std::uint32_t modalWindowToken =
        static_cast<std::uint32_t>(ImGui::GetID("##RenameProbeWindow"));
    ImGui::TextUnformatted("Rename:");
    if (focusRenameInput) {
      ImGui::SetKeyboardFocusHere();
      focusRenameInput = false;
    }
    const bool enter = inputTextString("##name", renameBuffer,
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    observeLastItem(EditorUiProbeSemantic::renameInput,
                    renameRow.value_or(GraphRowKey{}), renameTarget, true,
                    "##name", 0u, modalToken, modalWindowToken);
    const bool ok = ImGui::Button("OK");
    observeLastItem(EditorUiProbeSemantic::renameAccept,
                    renameRow.value_or(GraphRowKey{}), renameTarget, true, "OK",
                    0u, modalToken, modalWindowToken);
    ImGui::SameLine();
    const bool cancelButton = ImGui::Button("Cancel");
    observeLastItem(EditorUiProbeSemantic::renameCancel,
                    renameRow.value_or(GraphRowKey{}), renameTarget, true,
                    "Cancel", 0u, modalToken, modalWindowToken);
    const bool cancel =
        cancelButton || ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    if ((enter || ok) && !renameBuffer.empty() && renameTarget.valid()) {
      submit(CommandSource::dialog, RenameObject{renameTarget, renameBuffer},
             host);
      renameTarget = {};
      renameBuffer.clear();
      renameRow.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::CloseCurrentPopup();
    } else if (cancel) {
      renameTarget = {};
      renameBuffer.clear();
      renameRow.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void drawDeleteDialog(SceneEditorUiHost &host) {
    const std::uint32_t modalToken =
        static_cast<std::uint32_t>(ImGui::GetID("DeleteConfirmation"));
    if (openDeleteNextFrame && pendingDelete) {
      ImGui::OpenPopup("DeleteConfirmation");
      openDeleteNextFrame = false;
    }
    bool open = true;
    if (!ImGui::BeginPopupModal("DeleteConfirmation", &open,
                                ImGuiWindowFlags_AlwaysAutoResize))
      return;
    if (!open) {
      pendingDelete.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::EndPopup();
      return;
    }
    if (!pendingDelete) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }

    const std::uint32_t modalWindowToken =
        static_cast<std::uint32_t>(ImGui::GetID("##DeleteProbeWindow"));
    const GraphRowKey deleteRow =
        pendingDelete->row.value_or(GraphRowKey{});
    const EditorObjectId deleteObject = pendingDelete->node.untyped();
    ImGui::Text("Delete node '%s'?", pendingDelete->name.c_str());
    ImGui::TextUnformatted("This will delete the node and all its children.");
    ImGui::TextUnformatted("You can undo this with Ctrl+Z.");
    ImGui::Separator();
    const bool deleteNode = ImGui::Button("Delete", ImVec2(120.0F, 0.0F));
    observeLastItem(EditorUiProbeSemantic::deleteAccept, deleteRow,
                    deleteObject, true, "Delete", 0u, modalToken,
                    modalWindowToken);
    if (deleteNode) {
      submit(CommandSource::dialog,
             HierarchyAction{HierarchyOperation::deleteNode,
                             pendingDelete->scene, pendingDelete->node,
                             pendingDelete->parent},
             host);
      pendingDelete.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemDefaultFocus();
    ImGui::SameLine();
    const bool cancelButton =
        ImGui::Button("Cancel", ImVec2(120.0F, 0.0F));
    observeLastItem(EditorUiProbeSemantic::deleteCancel, deleteRow,
                    deleteObject, true, "Cancel", 0u, modalToken,
                    modalWindowToken);
    if (cancelButton || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      pendingDelete.reset();
      focusSceneBrowserNextFrame = persistent.windows.sceneBrowser;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void drawAssignDialog(const FrameModel &model, SceneEditorUiHost &host) {
    if (openAssignNextFrame && assignPrimitive) {
      ImGui::OpenPopup("AssignMaterial");
      openAssignNextFrame = false;
    }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520.0F, 0.0F), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal("AssignMaterial", &open,
                                ImGuiWindowFlags_None)) {
      if (!open) {
        assignPrimitive.reset();
        assignMaterial.reset();
        focusInspectorNextFrame = persistent.windows.inspector;
      }
      return;
    }
    if (!open) {
      assignPrimitive.reset();
      assignMaterial.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::EndPopup();
      return;
    }
    if (!assignPrimitive) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::TextUnformatted("Select material:");
    if (ImGui::BeginListBox(
            "##MaterialList",
            ImVec2(-std::numeric_limits<float>::min(),
                   8.0F * ImGui::GetTextLineHeightWithSpacing()))) {
      for (const MaterialChoice &material : model.materials) {
        pushObjectId(material.id.untyped());
        const bool selected = assignMaterial && *assignMaterial == material.id;
        const std::string label =
            fallbackName(material.name, "Material", material.documentIndex) +
            " (mat " + std::to_string(material.documentIndex) + ")";
        if (ImGui::Selectable(label.c_str(), selected))
          assignMaterial = material.id;
        if (selected)
          ImGui::SetItemDefaultFocus();
        popObjectId();
      }
      ImGui::EndListBox();
    }
    if (ImGui::Button("OK")) {
      if (assignPrimitive && assignMaterial)
        submit(
            CommandSource::dialog,
            MaterialAction{
                MaterialOperation::assign, assignPrimitive, assignMaterial, {}},
            host);
      assignPrimitive.reset();
      assignMaterial.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      assignPrimitive.reset();
      assignMaterial.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void drawTextureDialog(SceneEditorUiHost &host) {
    if (openTextureNextFrame && texturePicker) {
      ImGui::OpenPopup("SwitchTexture");
      openTextureNextFrame = false;
    }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(520.0F, 0.0F), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal("SwitchTexture", &open,
                                ImGuiWindowFlags_None)) {
      if (!open) {
        texturePicker.reset();
        focusInspectorNextFrame = persistent.windows.inspector;
      }
      return;
    }
    if (!open) {
      texturePicker.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::EndPopup();
      return;
    }
    if (!texturePicker) {
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::TextUnformatted("Select texture:");
    if (ImGui::BeginListBox(
            "##TextureList",
            ImVec2(-std::numeric_limits<float>::min(),
                   8.0F * ImGui::GetTextLineHeightWithSpacing()))) {
      for (const Choice &choice : texturePicker->choices) {
        ImGui::PushID(choice.value);
        const bool selected = choice.value == texturePicker->selected;
        if (ImGui::Selectable(choice.label.c_str(), selected))
          texturePicker->selected = choice.value;
        if (selected)
          ImGui::SetItemDefaultFocus();
        ImGui::PopID();
      }
      ImGui::EndListBox();
    }
    if (ImGui::Button("OK")) {
      property(texturePicker->target, texturePicker->field, EditPhase::begin,
               texturePicker->original, host);
      property(texturePicker->target, texturePicker->field, EditPhase::preview,
               Value{texturePicker->selected}, host);
      property(texturePicker->target, texturePicker->field, EditPhase::commit,
               Value{texturePicker->selected}, host);
      texturePicker.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
      texturePicker.reset();
      focusInspectorNextFrame = persistent.windows.inspector;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  void drawDialogHost(const FrameModel &model, SceneEditorUiHost &host) {
    if (!modalPending())
      return;
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1.0F, 1.0F), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0F);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##SceneEditorDialogHost", nullptr, flags);
    // BeginPopupModal clears p_open for unopened popups. Only an active
    // dialog may interpret that transition as a close/focus-restoration event.
    if (renameTarget.valid())
      drawRenameDialog(host);
    if (pendingDelete)
      drawDeleteDialog(host);
    if (assignPrimitive)
      drawAssignDialog(model, host);
    if (texturePicker)
      drawTextureDialog(host);
    ImGui::End();
  }

  void drawGridSnapSettings(const FrameModel &model, SceneEditorUiHost &host) {
    if (!persistent.windows.gridSnapSettings)
      return;
    if (!ImGui::Begin("Grid & Snap", &persistent.windows.gridSnapSettings)) {
      ImGui::End();
      return;
    }
    auto settings = persistent.viewport;
    bool changed = false;
    const bool viewportAvailable = host.viewportControlsAvailable();
    ImGui::BeginDisabled(model.rendererBusy || model.modalPending ||
                         lifecycleBlocksCommands(model.lifecycle) ||
                         !viewportAvailable);
    if (!viewportAvailable)
      ImGui::TextDisabled("Central viewport controls are not connected");
    const double gridMinimum = 0.001;
    const double gridMaximum = 1000.0;
    changed |=
        ImGui::DragScalar("Grid Unit", ImGuiDataType_Double, &settings.gridUnit,
                          0.01F, &gridMinimum, &gridMaximum, "%.3f");
    ImGui::Separator();
    changed |= ImGui::Checkbox("Enable Snapping", &settings.snapEnabled);
    ImGui::BeginDisabled(!settings.snapEnabled);
    ImGui::Text("Translation: %.3f (= Grid Unit)", settings.gridUnit);
    const double rotationMinimum = 1.0;
    const double rotationMaximum = 180.0;
    changed |= ImGui::DragScalar("Rotation (deg)", ImGuiDataType_Double,
                                 &settings.snapRotationDegrees, 1.0F,
                                 &rotationMinimum, &rotationMaximum, "%.1f");
    const double scaleMinimum = 0.001;
    const double scaleMaximum = 10.0;
    changed |=
        ImGui::DragScalar("Scale", ImGuiDataType_Double, &settings.snapScale,
                          0.01F, &scaleMinimum, &scaleMaximum, "%.3f");
    ImGui::EndDisabled();
    if (changed)
      updateViewport(settings, host);
    ImGui::EndDisabled();
    ImGui::End();
  }

  void drawError(const FrameModel &model) {
    std::string currentError = model.operation.error;
    if (currentError.empty())
      currentError = transientError;
    if (!currentError.empty() && currentError != presentedError) {
      presentedError = currentError;
      errorWindowOpen = true;
    }
    if (!errorWindowOpen || presentedError.empty())
      return;
    if (ImGui::Begin("Editor Error", &errorWindowOpen,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s", presentedError.c_str());
    }
    ImGui::End();
  }

  void renderMultipleScenesSettings(const FrameModel &model,
                                    SceneEditorUiHost &host) {
    if (!model.hasDocument || model.scenes.size() <= 1)
      return;
    if (!ImGui::CollapsingHeader("Multiple Scenes"))
      return;
    ImGui::PushID("Scenes");
    for (const SceneChoice &scene : model.scenes) {
      pushObjectId(scene.id.untyped());
      ImGui::PushID(static_cast<int>(scene.documentIndex));
      const std::string label =
          fallbackName(scene.name, "Scene", scene.documentIndex);
      ImGui::BeginDisabled(model.rendererBusy || model.modalPending ||
                           lifecycleBlocksCommands(model.lifecycle));
      if (ImGui::RadioButton(label.c_str(), scene.current))
        submit(CommandSource::settings, ActivateScene{scene.id}, host);
      ImGui::EndDisabled();
      ImGui::PopID();
      popObjectId();
    }
    ImGui::PopID();
  }

  RenderResult render(const FrameModel &model, SceneEditorUiHost &host) {
    result = {};
    materialDraftParkedThisFrame = false;
    invokedThisFrame.fill(false);
    persistent.viewport = host.viewportSettings();
    renderedSelectionRevision = model.selectionRevision;

    if (!hasPreviousSelection || previousSelection != model.selection) {
      if (model.hasDocument)
        (void)finishFieldEdit(host);
      if (!model.hasDocument || model.selection.emptyValue())
        clearSelectionScopedState();
      else {
        fieldEdit = {};
        rotationCache = {};
      }
      previousSelection = model.selection;
      hasPreviousSelection = true;
    } else if (!model.hasDocument) {
      clearSelectionScopedState();
    }

    if (model.layout.createDockspace &&
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0)
      ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                   ImGuiDockNodeFlags_PassthruCentralNode);

    drawMenuBar(model, host);
    handleShortcuts(model, host);
    drawSceneBrowser(model, host);
    drawInspector(model, host);
    if (materialDraft && materialDraftCommitPending &&
        !materialDraftParkedThisFrame && !fieldEdit.active &&
        !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
      (void)finishFieldEdit(host);
    }
    drawGridSnapSettings(model, host);
    drawDialogHost(model, host);
    drawError(model);
    return result;
  }
};

SceneEditorUi::SceneEditorUi(IconSet icons)
    : m_impl(std::make_unique<Impl>(std::move(icons))) {}

SceneEditorUi::~SceneEditorUi() = default;
SceneEditorUi::SceneEditorUi(SceneEditorUi &&) noexcept = default;
SceneEditorUi &SceneEditorUi::operator=(SceneEditorUi &&) noexcept = default;

bool SceneEditorUi::hasMaterialEditPreview() const noexcept {
  return m_impl->materialDraft.has_value() ||
         (m_impl->fieldEdit.active && m_impl->fieldEdit.deferred);
}

std::optional<MaterialEditDraft> SceneEditorUi::materialEditDraft() const {
  if (!m_impl->materialDraft && !m_impl->fieldEdit.active)
    return std::nullopt;
  MaterialEditDraft result;
  if (m_impl->materialDraft) {
    result.target = m_impl->materialDraft->target;
    result.edits = m_impl->materialDraft->edits;
  }
  if (m_impl->fieldEdit.active && m_impl->fieldEdit.deferred) {
    result.target = m_impl->fieldEdit.target;
    auto found = std::find_if(
        result.edits.begin(), result.edits.end(),
        [&](const PropertyEdit &edit) {
          return edit.field == m_impl->fieldEdit.field;
        });
    PropertyEdit current{m_impl->fieldEdit.target, m_impl->fieldEdit.field,
                         EditPhase::preview, m_impl->fieldEdit.value};
    if (found == result.edits.end())
      result.edits.push_back(std::move(current));
    else
      *found = std::move(current);
  }
  return result.edits.empty() ? std::nullopt
                              : std::optional<MaterialEditDraft>{std::move(result)};
}

RenderResult SceneEditorUi::render(const FrameModel &model,
                                   SceneEditorUiHost &host,
                                   EditorUiProbe *probe) {
  if (probe != nullptr) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImGuiIO &io = ImGui::GetIO();
    probe->beginFrame(EditorUiProbeFrameBinding{
        .lifecycle = model.lifecycle,
        .lifecycleRevision = model.lifecycleRevision,
        .documentRevision = model.revision,
        .selectionRevision = model.selectionRevision,
        .frameSerial = static_cast<std::uint64_t>(ImGui::GetFrameCount()),
        .mainViewportId = static_cast<std::uint32_t>(viewport->ID),
        .viewportPosition = {viewport->Pos.x, viewport->Pos.y},
        .viewportSize = {viewport->Size.x, viewport->Size.y},
        .displaySize = {io.DisplaySize.x, io.DisplaySize.y},
        .framebufferScale = {io.DisplayFramebufferScale.x,
                             io.DisplayFramebufferScale.y},
    });
  }
  m_impl->probe = probe;
  m_impl->probeWindowToken = 0u;
  m_impl->probeItemOrder = 0u;
  RenderResult result;
  try {
    result = m_impl->render(model, host);
  } catch (...) {
    m_impl->probe = nullptr;
    m_impl->probeWindowToken = 0u;
    if (probe != nullptr)
      probe->endFrame();
    throw;
  }
  m_impl->probe = nullptr;
  m_impl->probeWindowToken = 0u;
  if (probe != nullptr)
    probe->endFrame();
  return result;
}

void SceneEditorUi::renderMultipleScenesSettings(const FrameModel &model,
                                                 SceneEditorUiHost &host) {
  m_impl->renderMultipleScenesSettings(model, host);
}

void SceneEditorUi::requestFocusOnSelection(
    std::span<const EditorObjectId> parentPath, EditorObjectId selected,
    std::optional<GraphRowKey> exactRow) {
  m_impl->focusPath.assign(parentPath.begin(), parentPath.end());
  m_impl->focusScene.reset();
  if (exactRow) {
    m_impl->focusScene = exactRow->scene;
  } else if (!parentPath.empty() &&
             parentPath.front().kind == EditorObjectKind::scene) {
    m_impl->focusScene = SceneId::from(parentPath.front());
  }
  if (selected.valid() &&
      std::find(m_impl->focusPath.begin(), m_impl->focusPath.end(), selected) ==
          m_impl->focusPath.end())
    m_impl->focusPath.push_back(selected);
  m_impl->focusTarget = selected;
  m_impl->focusRow = std::move(exactRow);
  m_impl->scrollToFocus = selected.valid();
  ++m_impl->expansionRevision;
}

void SceneEditorUi::requestFocus(
    const SceneBrowserFocusRequest &request,
    std::span<const EditorObjectId> parentPath, EditorObjectId selected,
    std::optional<GraphRowKey> exactRow) {
  // P6 focus is reveal/scroll only. A malformed request cannot force a tab
  // change because this adapter never mutates persistent.browserTab.
  const SelectionRevision revisionFloor =
      std::max(m_impl->renderedSelectionRevision, m_impl->focusRevision);
  if (request.selectionRevision == 0 ||
      (revisionFloor != 0 && request.selectionRevision < revisionFloor) ||
      request.selectTab ||
      (!request.revealParents && !request.scrollToTarget)) {
    return;
  }
  m_impl->focusRevision = request.selectionRevision;
  requestFocusOnSelection(request.revealParents ? parentPath
                                                : std::span<const EditorObjectId>{},
                          selected, std::move(exactRow));
  if (!request.scrollToTarget) {
    m_impl->scrollToFocus = false;
  }
}

void SceneEditorUi::requestInspectorFocus() noexcept {
  m_impl->persistent.windows.inspector = true;
  m_impl->focusInspectorNextFrame = true;
}

void SceneEditorUi::requestSceneBrowserFocus() noexcept {
  m_impl->persistent.windows.sceneBrowser = true;
  m_impl->focusSceneBrowserNextFrame = true;
}

void SceneEditorUi::noteViewportEscapeHandled() noexcept {
  if (m_impl->pendingViewportEscapes !=
      (std::numeric_limits<std::uint64_t>::max)())
    ++m_impl->pendingViewportEscapes;
}

void SceneEditorUi::discardViewportEscapeHandling() noexcept {
  m_impl->pendingViewportEscapes = 0u;
}

const PersistentState &SceneEditorUi::state() const noexcept {
  return m_impl->persistent;
}

BrowserTab SceneEditorUi::browserTab() const noexcept {
  return m_impl->persistent.browserTab;
}

bool SceneEditorUi::modalPending() const noexcept {
  return m_impl->modalPending();
}

void SceneEditorUi::setState(PersistentState state) noexcept {
  m_impl->persistent = state;
}

void SceneEditorUi::resetDocumentState() noexcept {
  m_impl->resetDocumentState();
}

std::optional<PrimitiveContext>
SceneEditorUi::primitiveAssignmentContext(const FrameModel &model,
                                          const GraphRow &row) noexcept {
  if (row.kind != GraphRowKind::primitive)
    return std::nullopt;
  const auto *selection =
      std::get_if<PrimitiveSelection>(&row.selection.value);
  if (selection == nullptr || !selection->scene.valid() ||
      !selection->node.valid() || !selection->mesh.valid() ||
      !selection->primitive.valid())
    return std::nullopt;

  if (model.inspector.primitive) {
    const PrimitiveContext &inspector = *model.inspector.primitive;
    if (inspector.scene == selection->scene &&
        inspector.node == selection->node &&
        inspector.mesh == selection->mesh &&
        inspector.primitive == selection->primitive)
      return inspector;
  }

  // The clicked row is authoritative even when the Inspector is focused on a
  // different primitive. Material is intentionally unknown in that case; the
  // assignment dialog still targets the row and requires an explicit choice.
  return PrimitiveContext{selection->scene, selection->node, selection->mesh,
                          selection->primitive, std::nullopt, false, false,
                          !model.materials.empty()};
}

bool SceneEditorUi::viewportShortcutEligible(
    const FrameModel &model, bool modalPending, bool textInput,
    bool noModifiers, bool controlsAvailable) noexcept {
  return controlsAvailable && !modalPending &&
         model.lifecycle != LifecycleState::modal && !textInput &&
         noModifiers && (model.viewportHovered || model.viewportFocused);
}

bool SceneEditorUi::viewportCommandAvailable(
    CommandId id, bool controlsAvailable) noexcept {
  switch (id) {
  case CommandId::toggleGrid:
  case CommandId::toggleGizmo:
  case CommandId::toggleSnap:
  case CommandId::showGridSnapSettings:
    return controlsAvailable;
  default:
    return true;
  }
}

ClipboardMaterial SceneEditorUi::parseMaterialClipboard(
    std::string_view text, std::span<const MaterialChoice> materials) {
  const ParsedMaterialClipboard parsed = MaterialClipboardCodec::parse(text);
  if (parsed.syntax == MaterialClipboardSyntax::absent)
    return {};
  if (parsed.syntax != MaterialClipboardSyntax::parsed ||
      !parsed.documentIndex)
    return {ClipboardMaterialState::malformed, std::nullopt, std::nullopt, {}};
  if (*parsed.documentIndex >
      std::numeric_limits<std::uint32_t>::max()) {
    return {ClipboardMaterialState::outOfRange, std::nullopt, std::nullopt, {}};
  }
  const auto index = static_cast<std::uint32_t>(*parsed.documentIndex);

  const auto found = std::find_if(materials.begin(), materials.end(),
                                  [index](const MaterialChoice &choice) {
                                    return choice.documentIndex == index;
                                  });
  if (found == materials.end())
    return {ClipboardMaterialState::outOfRange, index, std::nullopt, {}};
  return {ClipboardMaterialState::validIndex, index, found->id,
          fallbackName(found->name, "Material", found->documentIndex)};
}

} // namespace demo::editor::ui
