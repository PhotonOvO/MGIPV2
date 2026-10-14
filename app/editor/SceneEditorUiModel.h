#pragma once

#include <mgif/scene_editor/EditorSelection.h>
#include <mgif/scene_editor/SceneCommands.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor::ui {

enum class LifecycleState : std::uint8_t {
  noDocument = 0,
  loadingCandidate,
  readyClean,
  readyModified,
  gizmoHover,
  gizmoDragging,
  modal,
  // Save, merge, and compact are distinct operations, but deliberately share
  // one observable lifecycle state.  The operation field below carries the
  // specific verb without creating additional UI states.
  savingMergingCompacting,
  error,
};

inline constexpr std::size_t lifecycleStateCount = 9u;

inline constexpr std::array<LifecycleState, lifecycleStateCount>
    lifecycleStates{{LifecycleState::noDocument,
                     LifecycleState::loadingCandidate,
                     LifecycleState::readyClean,
                     LifecycleState::readyModified,
                     LifecycleState::gizmoHover,
                     LifecycleState::gizmoDragging,
                     LifecycleState::modal,
                     LifecycleState::savingMergingCompacting,
                     LifecycleState::error}};

static_assert(static_cast<std::size_t>(LifecycleState::error) + 1u ==
              lifecycleStateCount);

[[nodiscard]] constexpr std::string_view
lifecycleStateName(LifecycleState state) noexcept {
  switch (state) {
  case LifecycleState::noDocument:
    return "NoDocument";
  case LifecycleState::loadingCandidate:
    return "LoadingCandidate";
  case LifecycleState::readyClean:
    return "ReadyClean";
  case LifecycleState::readyModified:
    return "ReadyModified";
  case LifecycleState::gizmoHover:
    return "GizmoHover";
  case LifecycleState::gizmoDragging:
    return "GizmoDragging";
  case LifecycleState::modal:
    return "Modal";
  case LifecycleState::savingMergingCompacting:
    return "Saving/Merging/Compacting";
  case LifecycleState::error:
    return "Error";
  }
  return "Error";
}

[[nodiscard]] constexpr bool
isKnownLifecycleState(LifecycleState state) noexcept {
  return static_cast<std::size_t>(state) < lifecycleStateCount;
}

[[nodiscard]] constexpr std::optional<LifecycleState>
parseLifecycleState(std::string_view name) noexcept {
  for (const LifecycleState state : lifecycleStates) {
    if (lifecycleStateName(state) == name)
      return state;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr bool
isLifecycleOperationState(LifecycleState state) noexcept {
  return state == LifecycleState::loadingCandidate ||
         state == LifecycleState::savingMergingCompacting;
}

[[nodiscard]] constexpr bool
isLifecycleDocumentReady(LifecycleState state) noexcept {
  return state == LifecycleState::readyClean ||
         state == LifecycleState::readyModified ||
         state == LifecycleState::gizmoHover ||
         state == LifecycleState::gizmoDragging;
}

// Operation kind is intentionally orthogonal to LifecycleState.  In
// particular, save/merge/compact all project to the one busy state above.
enum class LifecycleOperation : std::uint8_t {
  none = 0,
  loadCandidate,
  save,
  merge,
  compact,
};

using OperationKind = LifecycleOperation;

[[nodiscard]] constexpr std::string_view
lifecycleOperationName(LifecycleOperation operation) noexcept {
  switch (operation) {
  case LifecycleOperation::none:
    return "";
  case LifecycleOperation::loadCandidate:
    return "Loading candidate";
  case LifecycleOperation::save:
    return "Saving scene";
  case LifecycleOperation::merge:
    return "Merging scene";
  case LifecycleOperation::compact:
    return "Compacting scene";
  }
  return "";
}

enum class CommandId : std::uint8_t {
  newScene = 0,
  open,
  importMerge,
  openRecent,
  saveAs,
  exit,
  undo,
  redo,
  duplicateSelection,
  deleteSelection,
  fitScene,
  fitObject,
  toggleGrid,
  toggleGizmo,
  toggleSnap,
  showGridSnapSettings,
  toggleSceneBrowser,
  toggleInspector,
  compactScene,
  clearSelection,
  count,
};

enum class CommandSource : std::uint8_t {
  menu = 0,
  toolbar,
  shortcut,
  sceneBrowser,
  inspector,
  dialog,
  settings,
};

struct CommandDescriptor final {
  CommandId id{CommandId::newScene};
  std::string_view menu;
  std::string_view label;
  std::string_view shortcut;
};

[[nodiscard]] std::span<const CommandDescriptor> commandDescriptors() noexcept;
[[nodiscard]] const CommandDescriptor &commandDescriptor(CommandId id) noexcept;

struct WindowState final {
  bool sceneBrowser{true};
  bool inspector{true};
  bool gridSnapSettings{false};

  auto operator<=>(const WindowState &) const = default;
};

struct ViewportSettings final {
  bool showGrid{true};
  bool showGizmo{true};
  bool snapEnabled{false};
  double gridUnit{1.0};
  double snapRotationDegrees{45.0};
  double snapScale{0.1};

  auto operator<=>(const ViewportSettings &) const = default;
};

struct LayoutState final {
  // The app owns dock creation. These public ImGui dock IDs only provide
  // FirstUseEver placement without private docking APIs.
  std::uint32_t sceneBrowserDockId{0};
  std::uint32_t inspectorDockId{0};
  bool createDockspace{false};
  float minimumPanelWidth{260.0F};
  float minimumPanelHeight{220.0F};
};

struct OperationState final {
  std::string label;
  std::optional<float> progress;
  std::string error;
  bool dismissibleError{false};
  LifecycleOperation kind{LifecycleOperation::none};

  auto operator<=>(const OperationState &) const = default;
};

// Lightweight value snapshot for hosts that need lifecycle state without
// materializing the complete Scene Browser/Inspector frame model.
struct LifecycleSnapshot final {
  LifecycleState state{LifecycleState::noDocument};
  OperationState operation;
  std::uint64_t revision{0};
  bool overridden{false};

  auto operator<=>(const LifecycleSnapshot &) const = default;
};

struct RecentFile final {
  std::filesystem::path path;
  bool exists{true};
};

struct AssetField final {
  std::string label;
  std::string value;
};

struct AssetInfo final {
  std::string version;
  std::string generator;
  std::string copyright;
  std::string minimumVersion;
  std::vector<std::string> extensions;
  std::vector<AssetField> metadata;
};

struct SceneChoice final {
  SceneId id;
  std::uint32_t documentIndex{0};
  std::string name;
  bool current{false};
  SceneRootTransform transform;

  SceneChoice() = default;
  SceneChoice(SceneId sceneId, std::uint32_t index, std::string sceneName,
              bool isCurrent, SceneRootTransform sceneTransform = {})
      : id(sceneId), documentIndex(index), name(std::move(sceneName)),
        current(isCurrent), transform(sceneTransform) {}
};

struct AnimationItem final {
  AnimationId id;
  std::uint32_t documentIndex{0};
  std::string name;
  double startTime{0.0};
  double endTime{0.0};
  double currentTime{0.0};
};

struct AnimationState final {
  std::vector<AnimationItem> items;
  std::optional<AnimationId> current;
  bool playing{true};
  double speed{1.0};
  // The controller marks this true only when the selected clip passes the
  // runtime sampler/analysis checks. Unsupported or malformed clips keep the
  // controls visible but disabled while their list entry remains inspectable.
  bool runtimeControlsAvailable{false};
};

struct VariantItem final {
  std::uint32_t index{0};
  std::string name;
  bool current{false};
  bool enabled{false};
};

enum class BrowserTab : std::uint8_t {
  sceneGraph = 0,
  sceneList,
  debug,
};

enum class GraphRowKind : std::uint8_t {
  scene = 0,
  node,
  mesh,
  primitive,
  camera,
  light,
  skin,
};

struct GraphRowKey final {
  SceneId scene;
  EditorObjectId object;
  std::optional<NodeId> directParent;
  std::uint32_t occurrence{0};

  auto operator<=>(const GraphRowKey &) const = default;
};

struct GraphRow final {
  GraphRowKey key;
  GraphRowKind kind{GraphRowKind::node};
  std::uint32_t documentIndex{0};
  std::uint32_t depth{0};
  std::string label;
  std::string secondary;
  bool hasChildren{false};
  bool defaultExpanded{false};
  bool selected{false};
  bool selectable{false};
  bool enabled{true};
  bool effectiveVisible{true};
  bool acceptsNodeDrop{false};
  std::string nodeDropHint;
  EditorSelectionSnapshot selection;
};

enum class ResourceGroupKind : std::uint8_t {
  nodes = 0,
  meshes,
  materials,
  cameras,
  lights,
  textures,
  images,
  animations,
};

struct ResourceEntry final {
  EditorObjectId object;
  std::uint32_t documentIndex{0};
  std::string name;
  std::string detail;
  std::string tooltip;
  bool selected{false};
  bool selectable{false};
  EditorSelectionSnapshot selection;
  std::optional<ImageId> linkedImage;
};

struct ResourceGroup final {
  ResourceGroupKind kind{ResourceGroupKind::nodes};
  std::vector<ResourceEntry> entries;
};

struct DebugRenderRow final {
  std::uint32_t renderNode{0};
  std::optional<std::uint32_t> documentNode;
  std::string nodeName;
  std::optional<std::uint32_t> primitive;
  std::optional<std::uint32_t> material;
  bool valid{true};
};

struct BrowserDebugState final {
  std::uint64_t renderNodeCount{0};
  std::uint64_t renderPrimitiveCount{0};
  std::uint64_t documentNodeCount{0};
  std::uint64_t meshCount{0};
  std::uint64_t nodesWithMeshes{0};
  std::uint64_t expectedRenderNodeCount{0};
  bool gpuSyncValidationActive{false};
  std::vector<DebugRenderRow> rows;
};

struct SceneBrowserModel final {
  AssetInfo asset;
  AnimationState animation;
  std::vector<VariantItem> variants;
  std::vector<GraphRow> graphRows;
  std::vector<ResourceGroup> resourceGroups;
  BrowserDebugState debug;
};

using Value =
    std::variant<std::monostate, bool, std::int32_t, std::uint32_t, double,
                 std::string, std::array<double, 2>, std::array<double, 3>,
                 std::array<double, 4>, EditorObjectId>;

enum class FieldKind : std::uint8_t {
  readOnlyText = 0,
  boolean,
  scalar,
  integer,
  text,
  vector2,
  vector3,
  color3,
  color4,
  choice,
  objectReference,
};

// Field IDs are stable command tokens. Controllers translate them to the
// corresponding SceneCommands query/mutation without parsing visible labels.
enum class FieldId : std::uint16_t {
  name = 0,
  parent,
  children,
  meshAttachment,
  cameraAttachment,
  skinAttachment,
  lightAttachment,
  translation,
  rotationDegrees,
  scale,
  visibility,
  primitiveIndex,
  meshIndex,
  vertexCount,
  triangleCount,
  materialReference,
  primitiveCount,
  referencedBy,
  baseColor,
  baseColorTexture,
  metallic,
  roughness,
  metallicRoughnessTexture,
  diffuseColor,
  diffuseTexture,
  specularGlossinessColor,
  glossiness,
  specularGlossinessTexture,
  emissiveColor,
  emissiveTexture,
  normalTexture,
  normalScale,
  occlusionTexture,
  occlusionStrength,
  alphaMode,
  alphaCutoff,
  doubleSided,
  anisotropyStrength,
  anisotropyRotation,
  anisotropyTexture,
  clearcoatFactor,
  clearcoatRoughness,
  clearcoatTexture,
  clearcoatRoughnessTexture,
  clearcoatNormalTexture,
  diffuseTransmissionFactor,
  diffuseTransmissionColor,
  diffuseTransmissionTexture,
  diffuseTransmissionColorTexture,
  dispersion,
  emissiveStrength,
  ior,
  iridescenceFactor,
  iridescenceIor,
  iridescenceThicknessMinimum,
  iridescenceThicknessMaximum,
  iridescenceTexture,
  iridescenceThicknessTexture,
  sheenColor,
  sheenRoughness,
  sheenColorTexture,
  sheenRoughnessTexture,
  specularFactor,
  specularColor,
  specularTexture,
  specularColorTexture,
  transmissionFactor,
  transmissionTexture,
  unlit,
  volumeThickness,
  volumeThicknessTexture,
  attenuationColor,
  attenuationDistance,
  infiniteAttenuation,
  multiscatterColor,
  scatterAnisotropy,
  cameraType,
  cameraYFov,
  cameraAspectRatio,
  cameraZNear,
  cameraZFar,
  cameraXMag,
  cameraYMag,
  lightType,
  lightColorSrgb,
  lightIntensity,
  lightRange,
  lightInnerConeAngle,
  lightOuterConeAngle,
  count,
};

struct FieldDescriptor final {
  FieldId id{FieldId::name};
  std::string_view label;
  FieldKind kind{FieldKind::readOnlyText};
  double speed{0.01};
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::string_view format;
  bool continuous{false};
};

[[nodiscard]] const FieldDescriptor &fieldDescriptor(FieldId id) noexcept;

struct MaterialExtensionDescriptor final {
  std::string_view label;
  std::string_view extension;
  bool baseWorkflow{false};
};

[[nodiscard]] std::span<const MaterialExtensionDescriptor>
materialExtensionDescriptors() noexcept;

struct Choice final {
  std::int32_t value{0};
  std::string label;
  std::optional<EditorObjectId> object;
};

struct InspectorField final {
  FieldId id{FieldId::name};
  std::optional<EditorObjectId> target;
  Value value;
  std::string labelOverride;
  std::string tooltip;
  std::vector<Choice> choices;
  std::optional<double> minimumOverride;
  std::optional<double> maximumOverride;
  std::optional<double> speedOverride;
  // Increment when the backing value changes outside the current UI edit.
  // Rotation widgets use this to retain a stable Euler representation.
  std::uint64_t externalValueRevision{0};
  bool visible{true};
  bool enabled{true};
  bool readOnly{false};
};

enum class InspectorSectionKind : std::uint8_t {
  identity = 0,
  transform,
  attachments,
  primitive,
  mesh,
  material,
  camera,
  light,
  materialExtension,
};

struct InspectorSection final {
  InspectorSectionKind kind{InspectorSectionKind::identity};
  std::string label;
  bool defaultOpen{false};
  std::string materialExtension;
  bool extensionPresent{false};
  bool extensionMutable{false};
  std::vector<InspectorField> fields;

  InspectorSection() = default;
  InspectorSection(InspectorSectionKind sectionKind, std::string sectionLabel,
                   bool open)
      : kind(sectionKind), label(std::move(sectionLabel)), defaultOpen(open) {}
};

struct PrimitiveContext final {
  SceneId scene;
  NodeId node;
  MeshId mesh;
  PrimitiveId primitive;
  std::optional<MaterialId> material;
  bool canSplit{true};
  bool canMerge{true};
  bool canAssign{true};
};

struct CameraContext final {
  CameraId camera;
  std::optional<NodeId> node;
  bool canApplyToView{true};
  bool canSetFromView{true};
};

struct InspectorModel final {
  EditorSelectionSnapshot selection;
  SelectionRevision selectionRevision{0};
  std::string title;
  std::vector<InspectorSection> sections;
  std::optional<PrimitiveContext> primitive;
  std::optional<MaterialId> material;
  std::optional<CameraContext> camera;
};

struct MaterialChoice final {
  MaterialId id;
  std::uint32_t documentIndex{0};
  std::string name;
};

struct FrameModel final {
  LifecycleState lifecycle{LifecycleState::noDocument};
  bool hasDocument{false};
  bool mutationEnabled{false};
  // True when a document-scoped modal is open or scheduled for the next
  // frame. Commands use this value so programmatic dispatch and the visible
  // menu share the same modal lockout.
  bool modalPending{false};
  bool rendererBusy{false};
  bool canUndo{false};
  bool canRedo{false};
  bool canFitSelection{false};
  bool debugUiAvailable{false};
  bool viewportHovered{false};
  bool viewportFocused{false};
  // Native Gizmo ownership is distinct from an Inspector continuous edit.
  bool viewportOwnsEscape{false};
  std::string undoDescription;
  std::string redoDescription;
  std::uint64_t revision{0};
  std::uint64_t cleanRevision{0};
  // Monotonic lifecycle publication identity.  It changes whenever an
  // explicit override, operation, gizmo interaction, or error transition is
  // published, allowing headless consumers to capture transient states.
  std::uint64_t lifecycleRevision{0};
  bool lifecycleOverridden{false};
  SelectionRevision selectionRevision{0};
  OperationState operation;
  LayoutState layout;
  std::vector<RecentFile> recentFiles;
  std::vector<SceneChoice> scenes;
  EditorSelectionSnapshot selection;
  SceneBrowserModel browser;
  InspectorModel inspector;
  std::vector<MaterialChoice> materials;
};

struct CommandEnablement final {
  bool enabled{false};
  bool checked{false};
};

[[nodiscard]] CommandEnablement
evaluateCommand(CommandId id, const FrameModel &model,
                const WindowState &windows,
                const ViewportSettings &viewport) noexcept;

struct InvokeCommand final {
  CommandId id{CommandId::newScene};
  std::optional<std::filesystem::path> path;
};

struct SelectObject final {
  EditorSelectionSnapshot selection;
};

struct ActivateScene final {
  SceneId scene;
};

enum class HierarchyOperation : std::uint8_t {
  addRootNode = 0,
  addChildNode,
  addPointLight,
  addDirectionalLight,
  addSpotLight,
  duplicateNode,
  requestDeleteNode,
  deleteNode,
};

struct HierarchyAction final {
  HierarchyOperation operation{HierarchyOperation::addRootNode};
  SceneId scene;
  std::optional<NodeId> node;
  std::optional<NodeId> directParent;
};

struct RenameObject final {
  EditorObjectId object;
  std::string name;
};

struct ReparentNode final {
  SceneNodeOccurrence source;
  SceneId targetScene;
  std::optional<NodeId> targetParent;
};

enum class AnimationOperation : std::uint8_t {
  select = 0,
  setPlaying,
  step,
  reset,
  setSpeed,
  setTime,
};

struct AnimationAction final {
  AnimationOperation operation{AnimationOperation::select};
  std::optional<AnimationId> animation;
  double value{0.0};
};

struct SelectVariant final {
  std::uint32_t variant{0};
};

enum class EditPhase : std::uint8_t {
  begin = 0,
  preview,
  commit,
};

struct PropertyEdit final {
  EditorObjectId target;
  FieldId field{FieldId::name};
  EditPhase phase{EditPhase::commit};
  Value value;
};

enum class MaterialOperation : std::uint8_t {
  focusAssignment = 0,
  assign,
  copyReference,
  pasteReference,
  duplicate,
  split,
  merge,
  addExtension,
  removeExtension,
};

struct MaterialAction final {
  MaterialOperation operation{MaterialOperation::focusAssignment};
  std::optional<PrimitiveContext> primitive;
  std::optional<MaterialId> material;
  std::string extension;
};

enum class CameraOperation : std::uint8_t {
  applyToCurrentView = 0,
  setFromCurrentView,
};

struct CameraAction final {
  CameraOperation operation{CameraOperation::applyToCurrentView};
  CameraId camera;
  std::optional<NodeId> node;
};

struct SceneTransformAction final {
  SceneId scene;
  SceneRootTransform transform;
  bool reset{false};
};

struct RebuildRenderNodes final {};

using CommandPayload =
    std::variant<InvokeCommand, SelectObject, ActivateScene, HierarchyAction,
                 RenameObject, ReparentNode, AnimationAction, SelectVariant,
                 PropertyEdit, MaterialAction, CameraAction,
                 SceneTransformAction, RebuildRenderNodes>;

struct UiCommand final {
  std::uint64_t sequence{0};
  CommandSource source{CommandSource::menu};
  CommandPayload payload;
};

enum class ProjectionRefresh : std::uint8_t {
  none = 0,
  selectionOnly,
  properties,
  hierarchy,
  topology,
  fullScene,
};

struct RuntimeActionResult final {
  bool accepted{false};
  ProjectionRefresh refresh{ProjectionRefresh::none};
  std::string error;
};

enum class ClipboardMaterialState : std::uint8_t {
  none = 0,
  malformed,
  validIndex,
  outOfRange,
};

struct ClipboardMaterial final {
  ClipboardMaterialState state{ClipboardMaterialState::none};
  std::optional<std::uint32_t> index;
  std::optional<MaterialId> material;
  std::string materialName;
};

struct PersistentState final {
  WindowState windows;
  ViewportSettings viewport;
  BrowserTab browserTab{BrowserTab::sceneGraph};
};

} // namespace demo::editor::ui
