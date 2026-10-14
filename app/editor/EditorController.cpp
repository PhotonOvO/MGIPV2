#include "EditorController.h"
#include "MaterialClipboardCodec.h"

#include <mgif/scene_editor/commands/HierarchyCommands.h>
#include <mgif/scene_editor/GltfSemanticCompare.h>
#include <mgif/scene_editor/SceneCompactor.h>
#include <mgif/scene_editor/SceneMerger.h>
#include <mgif/scene_editor/commands/HistoryLifecycle.h>
#include <mgif/scene_editor/commands/PropertyCommands.h>
#include <mgif/scene_editor/commands/TransformCommands.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <utility>

namespace demo::editor::ui {
namespace {

using namespace demo::editor;

[[nodiscard]] RuntimeActionResult accepted(ProjectionRefresh refresh = ProjectionRefresh::none) {
  return RuntimeActionResult{true, refresh, {}};
}

[[nodiscard]] RuntimeActionResult rejected(MutationErrorCode error,
                                           std::string message) {
  RuntimeActionResult result;
  result.error = message.empty() ? "Editor operation rejected" : std::move(message);
  if (error != MutationErrorCode::none && result.error.empty()) {
    result.error = "Editor operation rejected";
  }
  return result;
}

[[nodiscard]] bool seedNodeNameProjectionCache(
    detail::NodeNameProjectionDigestCache &cache,
    const SceneMutationEvidence &evidence, const GltfDocument &document,
    const SceneProjectionResult &before) {
  const SourceMutationFence &source = evidence.sourceMutation;
  if (!evidence.structuralEvidenceComplete ||
      !evidence.exactChanges.empty() || !evidence.stableIdRemap.empty() ||
      evidence.fullSnapshot || !source.mutationToken.valid() ||
      source.mutationToken.authority != document.documentAuthority() ||
      before.documentRevision == std::numeric_limits<std::uint64_t>::max() ||
      source.baseDocumentRevision != before.documentRevision ||
      source.targetDocumentRevision != before.documentRevision + 1u ||
      source.targetDocumentRevision != document.revision() ||
      source.mutationToken.revision != source.targetDocumentRevision ||
      source.baseDocumentDigest != before.documentDigest ||
      !sceneDigestPresent(source.targetDocumentDigest)) {
    return false;
  }

  // mutateNodeNameOnly computes and caches both values before its atomic
  // publication. Re-read the cache to bind the retained command evidence to
  // the live authority without serializing the document again.
  if (document.documentDigest() != source.targetDocumentDigest) {
    return false;
  }
  const std::string semanticHash = GltfSemanticCompare::digest(document);
  if (semanticHash.empty()) {
    return false;
  }
  cache.documentDigest = source.targetDocumentDigest;
  cache.semanticHash = semanticHash;
  return true;
}

[[nodiscard]] std::optional<NodeId>
nodeFromSelection(const EditorSelectionSnapshot &selection) {
  if (const auto *node = std::get_if<NodeSelection>(&selection.value)) {
    return node->node;
  }
  if (const auto *primitive = std::get_if<PrimitiveSelection>(&selection.value)) {
    return primitive->node;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<SceneId>
sceneFromSelection(const EditorSelectionSnapshot &selection) {
  if (const auto *node = std::get_if<NodeSelection>(&selection.value)) {
    return node->scene;
  }
  if (const auto *primitive = std::get_if<PrimitiveSelection>(&selection.value)) {
    return primitive->scene;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::array<double, 3>>
valueVec3(const Value &value) {
  if (const auto *v = std::get_if<std::array<double, 3>>(&value)) {
    return *v;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::array<double, 4>>
valueVec4(const Value &value) {
  if (const auto *v = std::get_if<std::array<double, 4>>(&value)) {
    return *v;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<double> valueNumber(const Value &value) {
  if (const auto *v = std::get_if<double>(&value)) {
    return *v;
  }
  if (const auto *v = std::get_if<std::int32_t>(&value)) {
    return static_cast<double>(*v);
  }
  if (const auto *v = std::get_if<std::uint32_t>(&value)) {
    return static_cast<double>(*v);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<bool> valueBool(const Value &value) {
  if (const auto *v = std::get_if<bool>(&value)) {
    return *v;
  }
  return std::nullopt;
}

[[nodiscard]] std::array<double, 3> quaternionEulerDegrees(
    const std::array<double, 4> &quaternion) noexcept {
  const double x = quaternion[0];
  const double y = quaternion[1];
  const double z = quaternion[2];
  const double w = quaternion[3];
  const double roll = std::atan2(2.0 * (w * x + y * z),
                                 1.0 - 2.0 * (x * x + y * y));
  const double pitchTerm = 2.0 * (w * y - z * x);
  const double pitch = std::abs(pitchTerm) >= 1.0
                           ? std::copysign(1.5707963267948966, pitchTerm)
                           : std::asin(pitchTerm);
  const double yaw = std::atan2(2.0 * (w * z + x * y),
                                1.0 - 2.0 * (y * y + z * z));
  constexpr double radiansToDegrees = 57.295779513082320876;
  return {roll * radiansToDegrees, pitch * radiansToDegrees,
          yaw * radiansToDegrees};
}

[[nodiscard]] NodeTransform nodeTransformFor(const SceneCommands &commands,
                                             NodeId node) {
  return commands.nodeTransform(node).value_or(NodeTransform{});
}

[[nodiscard]] CameraProperties cameraPropertiesFor(const SceneCommands &commands,
                                                   CameraId camera) {
  return commands.cameraProperties(camera).value_or(CameraProperties{});
}

[[nodiscard]] LightProperties lightPropertiesFor(const SceneCommands &commands,
                                                 LightId light) {
  return commands.lightProperties(light).value_or(LightProperties{});
}

struct ExtensionFieldSpec final {
  FieldId field;
  std::string_view extension;
  std::string_view member;
  FieldKind kind;
  bool texture{false};
};

constexpr std::array<ExtensionFieldSpec, 28> kExtensionFields{{
    {FieldId::diffuseColor, "KHR_materials_pbrSpecularGlossiness",
     "diffuseFactor", FieldKind::color4, false},
    {FieldId::diffuseTexture, "KHR_materials_pbrSpecularGlossiness",
     "diffuseTexture", FieldKind::choice, true},
    {FieldId::specularGlossinessColor,
     "KHR_materials_pbrSpecularGlossiness", "specularFactor",
     FieldKind::color3, false},
    {FieldId::glossiness, "KHR_materials_pbrSpecularGlossiness",
     "glossinessFactor", FieldKind::scalar, false},
    {FieldId::specularGlossinessTexture,
     "KHR_materials_pbrSpecularGlossiness", "specularGlossinessTexture",
     FieldKind::choice, true},
    {FieldId::anisotropyStrength, "KHR_materials_anisotropy",
     "anisotropyStrength", FieldKind::scalar, false},
    {FieldId::anisotropyRotation, "KHR_materials_anisotropy",
     "anisotropyRotation", FieldKind::scalar, false},
    {FieldId::anisotropyTexture, "KHR_materials_anisotropy",
     "anisotropyTexture", FieldKind::choice, true},
    {FieldId::clearcoatFactor, "KHR_materials_clearcoat", "clearcoatFactor",
     FieldKind::scalar, false},
    {FieldId::clearcoatRoughness, "KHR_materials_clearcoat",
     "clearcoatRoughnessFactor", FieldKind::scalar, false},
    {FieldId::clearcoatTexture, "KHR_materials_clearcoat", "clearcoatTexture",
     FieldKind::choice, true},
    {FieldId::clearcoatRoughnessTexture, "KHR_materials_clearcoat",
     "clearcoatRoughnessTexture", FieldKind::choice, true},
    {FieldId::clearcoatNormalTexture, "KHR_materials_clearcoat",
     "clearcoatNormalTexture", FieldKind::choice, true},
    {FieldId::diffuseTransmissionFactor, "KHR_materials_diffuse_transmission",
     "diffuseTransmissionFactor", FieldKind::scalar, false},
    {FieldId::diffuseTransmissionColor, "KHR_materials_diffuse_transmission",
     "diffuseTransmissionColorFactor", FieldKind::color3, false},
    {FieldId::diffuseTransmissionTexture,
     "KHR_materials_diffuse_transmission", "diffuseTransmissionTexture",
     FieldKind::choice, true},
    {FieldId::diffuseTransmissionColorTexture,
     "KHR_materials_diffuse_transmission", "diffuseTransmissionColorTexture",
     FieldKind::choice, true},
    {FieldId::dispersion, "KHR_materials_dispersion", "dispersion",
     FieldKind::scalar, false},
    {FieldId::emissiveStrength, "KHR_materials_emissive_strength",
     "emissiveStrength", FieldKind::scalar, false},
    {FieldId::ior, "KHR_materials_ior", "ior", FieldKind::scalar, false},
    {FieldId::iridescenceFactor, "KHR_materials_iridescence",
     "iridescenceFactor", FieldKind::scalar, false},
    {FieldId::iridescenceIor, "KHR_materials_iridescence", "iridescenceIor",
     FieldKind::scalar, false},
    {FieldId::iridescenceThicknessMinimum, "KHR_materials_iridescence",
     "iridescenceThicknessMinimum", FieldKind::scalar, false},
    {FieldId::iridescenceThicknessMaximum, "KHR_materials_iridescence",
     "iridescenceThicknessMaximum", FieldKind::scalar, false},
    {FieldId::iridescenceTexture, "KHR_materials_iridescence",
     "iridescenceTexture", FieldKind::choice, true},
    {FieldId::iridescenceThicknessTexture, "KHR_materials_iridescence",
     "iridescenceThicknessTexture", FieldKind::choice, true},
    {FieldId::sheenColor, "KHR_materials_sheen", "sheenColorFactor",
     FieldKind::color3, false},
    {FieldId::sheenRoughness, "KHR_materials_sheen", "sheenRoughnessFactor",
     FieldKind::scalar, false},
}};

constexpr std::array<ExtensionFieldSpec, 14> kExtensionFieldsTail{{
    {FieldId::sheenColorTexture, "KHR_materials_sheen", "sheenColorTexture",
     FieldKind::choice, true},
    {FieldId::sheenRoughnessTexture, "KHR_materials_sheen",
     "sheenRoughnessTexture", FieldKind::choice, true},
    {FieldId::specularFactor, "KHR_materials_specular", "specularFactor",
     FieldKind::scalar, false},
    {FieldId::specularColor, "KHR_materials_specular", "specularColorFactor",
     FieldKind::color3, false},
    {FieldId::specularTexture, "KHR_materials_specular", "specularTexture",
     FieldKind::choice, true},
    {FieldId::specularColorTexture, "KHR_materials_specular",
     "specularColorTexture", FieldKind::choice, true},
    {FieldId::transmissionFactor, "KHR_materials_transmission",
     "transmissionFactor", FieldKind::scalar, false},
    {FieldId::transmissionTexture, "KHR_materials_transmission",
     "transmissionTexture", FieldKind::choice, true},
    {FieldId::unlit, "KHR_materials_unlit", "", FieldKind::boolean, false},
    {FieldId::volumeThickness, "KHR_materials_volume", "thicknessFactor",
     FieldKind::scalar, false},
    {FieldId::volumeThicknessTexture, "KHR_materials_volume", "thicknessTexture",
     FieldKind::choice, true},
    {FieldId::attenuationColor, "KHR_materials_volume", "attenuationColor",
     FieldKind::color3, false},
    {FieldId::attenuationDistance, "KHR_materials_volume",
     "attenuationDistance", FieldKind::scalar, false},
    {FieldId::multiscatterColor, "KHR_materials_volume_scatter",
     "multiscatterColor", FieldKind::color3, false},
}};

constexpr std::array<ExtensionFieldSpec, 1> kExtensionFieldsLast{{
    {FieldId::scatterAnisotropy, "KHR_materials_volume_scatter",
     "scatterAnisotropy", FieldKind::scalar, false},
}};

template <typename Function>
void forExtensionFieldSpec(Function &&function) {
  for (const ExtensionFieldSpec &spec : kExtensionFields)
    function(spec);
  for (const ExtensionFieldSpec &spec : kExtensionFieldsTail)
    function(spec);
  for (const ExtensionFieldSpec &spec : kExtensionFieldsLast)
    function(spec);
}

[[nodiscard]] const ExtensionFieldSpec *extensionFieldSpec(FieldId field) {
  const ExtensionFieldSpec *result = nullptr;
  forExtensionFieldSpec([&](const ExtensionFieldSpec &spec) {
    if (result == nullptr && spec.field == field)
      result = &spec;
  });
  return result;
}

[[nodiscard]] const MaterialExtensionValue *extensionMember(
    const MaterialExtensionValue::Object &object, std::string_view name) {
  const auto found = std::find_if(
      object.begin(), object.end(), [name](const auto &entry) {
        return entry.first == name;
      });
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] MaterialExtensionValue *extensionMember(
    MaterialExtensionValue::Object &object, std::string_view name) {
  const auto found = std::find_if(
      object.begin(), object.end(), [name](const auto &entry) {
        return entry.first == name;
      });
  return found == object.end() ? nullptr : &found->second;
}

void eraseExtensionMember(MaterialExtensionValue::Object &object,
                          std::string_view name) {
  object.erase(std::remove_if(object.begin(), object.end(),
                              [name](const auto &entry) {
                                return entry.first == name;
                              }),
                object.end());
}

[[nodiscard]] std::optional<double> extensionNumber(
    const MaterialExtensionValue *value) {
  if (value == nullptr)
    return std::nullopt;
  if (const auto *number = std::get_if<double>(&value->value))
    return *number;
  if (const auto *integer = std::get_if<std::int32_t>(&value->value))
    return static_cast<double>(*integer);
  return std::nullopt;
}

template <std::size_t Count>
[[nodiscard]] std::optional<std::array<double, Count>> extensionVector(
    const MaterialExtensionValue *value) {
  if (value == nullptr)
    return std::nullopt;
  const auto *array = std::get_if<MaterialExtensionValue::Array>(&value->value);
  if (array == nullptr || array->size() != Count)
    return std::nullopt;
  std::array<double, Count> result{};
  for (std::size_t index = 0; index < Count; ++index) {
    const auto number = extensionNumber(&(*array)[index]);
    if (!number)
      return std::nullopt;
    result[index] = *number;
  }
  return result;
}

[[nodiscard]] std::optional<std::int32_t> valueIndex(const Value &value) {
  if (const auto *signedValue = std::get_if<std::int32_t>(&value))
    return *signedValue;
  if (const auto *unsignedValue = std::get_if<std::uint32_t>(&value)) {
    if (*unsignedValue <= static_cast<std::uint32_t>(
                             std::numeric_limits<std::int32_t>::max()))
      return static_cast<std::int32_t>(*unsignedValue);
  }
  return std::nullopt;
}

[[nodiscard]] Value extensionValueFor(const MaterialExtensionValue *root,
                                      const ExtensionFieldSpec &spec) {
  if (spec.field == FieldId::unlit)
    return Value{root != nullptr};
  const auto *object = root == nullptr
                           ? nullptr
                           : std::get_if<MaterialExtensionValue::Object>(
                                 &root->value);
  const MaterialExtensionValue *member =
      object == nullptr ? nullptr : extensionMember(*object, spec.member);
  if (spec.texture) {
    if (member != nullptr) {
      if (const auto *textureObject =
              std::get_if<MaterialExtensionValue::Object>(&member->value)) {
        if (const auto *index = extensionMember(*textureObject, "index")) {
          if (const auto number = extensionNumber(index))
            return Value{static_cast<std::int32_t>(*number)};
        }
      }
    }
    return Value{std::int32_t{-1}};
  }
  switch (spec.kind) {
  case FieldKind::scalar:
    if (const auto number = extensionNumber(member))
      return Value{*number};
    switch (spec.field) {
    case FieldId::glossiness:
    case FieldId::emissiveStrength:
    case FieldId::specularFactor:
      return Value{1.0};
    case FieldId::ior:
      return Value{1.5};
    case FieldId::iridescenceIor:
      return Value{1.3};
    case FieldId::iridescenceThicknessMinimum:
      return Value{100.0};
    case FieldId::iridescenceThicknessMaximum:
      return Value{400.0};
    default:
      return Value{0.0};
    }
  case FieldKind::color3:
    if (const auto vector = extensionVector<3>(member))
      return Value{*vector};
    if (spec.field == FieldId::diffuseTransmissionColor ||
        spec.field == FieldId::specularGlossinessColor ||
        spec.field == FieldId::specularColor ||
        spec.field == FieldId::attenuationColor)
      return Value{std::array<double, 3>{1.0, 1.0, 1.0}};
    return Value{std::array<double, 3>{0.0, 0.0, 0.0}};
  case FieldKind::color4:
    return Value{extensionVector<4>(member).value_or(
        std::array<double, 4>{1.0, 1.0, 1.0, 1.0})};
  case FieldKind::boolean:
    if (member != nullptr)
      if (const auto *flag = std::get_if<bool>(&member->value))
        return Value{*flag};
    return Value{false};
  default:
    return Value{};
  }
}

[[nodiscard]] std::optional<MaterialExtensionValue> editExtensionValue(
    const std::optional<MaterialExtensionValue> &before,
    const ExtensionFieldSpec &spec, const Value &value) {
  if (spec.field == FieldId::unlit) {
    const auto flag = valueBool(value);
    if (!flag)
      return std::nullopt;
    if (!*flag)
      return std::nullopt;
    MaterialExtensionValue result;
    result.value = MaterialExtensionValue::Object{};
    return result;
  }
  MaterialExtensionValue result = before.value_or(MaterialExtensionValue{});
  if (!std::holds_alternative<MaterialExtensionValue::Object>(result.value))
    result.value = MaterialExtensionValue::Object{};
  auto &object = std::get<MaterialExtensionValue::Object>(result.value);
  const auto setMember = [&](MaterialExtensionValue member) {
    if (auto *existing = extensionMember(object, spec.member))
      *existing = std::move(member);
    else
      object.emplace_back(std::string(spec.member), std::move(member));
  };
  if (spec.texture) {
    const auto index = valueIndex(value);
    if (!index)
      return std::nullopt;
    if (*index < 0) {
      eraseExtensionMember(object, spec.member);
      return result;
    }
    MaterialExtensionValue texture;
    MaterialExtensionValue::Object textureObject;
    if (const auto *existing = extensionMember(object, spec.member))
      if (const auto *existingObject =
              std::get_if<MaterialExtensionValue::Object>(&existing->value))
        textureObject = *existingObject;
    MaterialExtensionValue indexValue;
    indexValue.value = *index;
    if (auto *existingIndex = extensionMember(textureObject, "index"))
      *existingIndex = std::move(indexValue);
    else
      textureObject.emplace_back("index", std::move(indexValue));
    texture.value = std::move(textureObject);
    setMember(std::move(texture));
    return result;
  }
  MaterialExtensionValue member;
  switch (spec.kind) {
  case FieldKind::scalar: {
    const auto number = valueNumber(value);
    if (!number)
      return std::nullopt;
    member.value = *number;
    break;
  }
  case FieldKind::color3: {
    const auto vector = valueVec3(value);
    if (!vector)
      return std::nullopt;
    MaterialExtensionValue::Array array;
    for (double item : *vector) {
      MaterialExtensionValue number;
      number.value = item;
      array.push_back(std::move(number));
    }
    member.value = std::move(array);
    break;
  }
  case FieldKind::color4: {
    const auto vector = valueVec4(value);
    if (!vector)
      return std::nullopt;
    MaterialExtensionValue::Array array;
    for (double item : *vector) {
      MaterialExtensionValue number;
      number.value = item;
      array.push_back(std::move(number));
    }
    member.value = std::move(array);
    break;
  }
  case FieldKind::boolean: {
    const auto flag = valueBool(value);
    if (!flag)
      return std::nullopt;
    member.value = *flag;
    break;
  }
  default:
    return std::nullopt;
  }
  setMember(std::move(member));
  return result;
}

[[nodiscard]] std::optional<MaterialTextureSlot>
materialTextureSlotFor(FieldId field) {
  switch (field) {
  case FieldId::baseColorTexture:
    return MaterialTextureSlot::baseColor;
  case FieldId::metallicRoughnessTexture:
    return MaterialTextureSlot::metallicRoughness;
  case FieldId::normalTexture:
    return MaterialTextureSlot::normal;
  case FieldId::occlusionTexture:
    return MaterialTextureSlot::occlusion;
  case FieldId::emissiveTexture:
    return MaterialTextureSlot::emissive;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] CommandPrepareResult prepareHierarchyCommand(
    SceneEditor &editor, const HierarchyAction &action) {
  EditorCommandContext &context = editor.commandContext();
  switch (action.operation) {
  case HierarchyOperation::addChildNode:
    if (!action.node) {
      return {};
    }
    return prepareAddChildNodeCommand(
        context, AddChildNodeRequest{action.scene, *action.node, "Node"});
  case HierarchyOperation::addPointLight:
  case HierarchyOperation::addDirectionalLight:
  case HierarchyOperation::addSpotLight: {
    LightProperties properties;
    properties.type = action.operation == HierarchyOperation::addPointLight
                          ? "point"
                          : action.operation == HierarchyOperation::addSpotLight
                                ? "spot"
                                : "directional";
    return prepareAddLightCommand(
        context,
        AddLightRequest{action.scene, action.node, "Light", "Light",
                        properties});
  }
  case HierarchyOperation::duplicateNode: {
    if (!action.node) return {};
    const auto occurrence = editor.commands().captureNodeOccurrence(
        action.scene, *action.node);
    if (!occurrence) return {};
    return prepareDuplicateNodeCommand(
        context, DuplicateNodeRequest{*occurrence, std::nullopt,
                                      SceneOccurrencePolicy::clickedScene});
  }
  case HierarchyOperation::requestDeleteNode:
  case HierarchyOperation::deleteNode: {
    if (!action.node) return {};
    const auto occurrence = editor.commands().captureNodeOccurrence(
        action.scene, *action.node);
    if (!occurrence) return {};
    return prepareDeleteNodeCommand(
        context, DeleteNodeRequest{*occurrence,
                                   SceneOccurrencePolicy::clickedScene});
  }
  case HierarchyOperation::addRootNode:
    break;
  }
  return {};
}

} // namespace

// UI rows can refer to primitive occurrences in any document scene, while the
// renderer lookup intentionally contains only the currently published scene.
// Keep a small document-backed lookup for non-viewport selection paths so the
// UI never treats a missing renderer occurrence as a stale document ID.
class DocumentOccurrenceLookup final : public ISceneSelectionOccurrenceLookup {
public:
  explicit DocumentOccurrenceLookup(const GltfDocument &document) noexcept
      : m_document(&document) {}

  [[nodiscard]] EditorDocumentAuthority authority() const noexcept override {
    return m_document != nullptr ? m_document->documentAuthority()
                                  : EditorDocumentAuthority{};
  }

  [[nodiscard]] std::uint64_t documentRevision() const noexcept override {
    return m_document != nullptr ? m_document->revision() : 0u;
  }

  [[nodiscard]] std::uint64_t sceneActivationEpoch() const noexcept override {
    return m_document != nullptr ? m_document->sceneActivationEpoch() : 0u;
  }

  [[nodiscard]] std::uint64_t generation() const noexcept override {
    if (m_document == nullptr)
      return 0u;
    const std::uint64_t mixed =
        (m_document->revision() * 0x9e3779b97f4a7c15ULL) ^
        (m_document->sceneActivationEpoch() + 0x517cc1b727220a95ULL);
    return mixed == 0u ? 1u : mixed;
  }

  [[nodiscard]] bool contains(PrimitiveOccurrenceKey occurrence) const noexcept override {
    if (m_document == nullptr || occurrence.instanceOrdinal != 0u ||
        occurrence.scene.authority() != m_document->documentAuthority() ||
        occurrence.node.authority() != m_document->documentAuthority() ||
        occurrence.primitive.authority() != m_document->documentAuthority()) {
      return false;
    }
    const auto address = m_document->primitiveAddress(occurrence.primitive);
    if (!address)
      return false;
    const auto mesh = MeshId::from(
        m_document->idAt(EditorObjectKind::mesh, address->meshIndex));
    return mesh && m_document->ownsPrimitiveOccurrence(
                       occurrence.scene, occurrence.node, *mesh,
                       occurrence.primitive);
  }

  [[nodiscard]] bool visible(PrimitiveOccurrenceKey occurrence) const noexcept override {
    if (!contains(occurrence))
      return false;
    try {
      if (!m_document->objectVisible(occurrence.node.untyped()))
        return false;
      for (const NodeId ancestor : m_document->sceneNodeParentPath(
               occurrence.scene, occurrence.node)) {
        if (!m_document->objectVisible(ancestor.untyped()))
          return false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] bool nodeHighlightEligible(NodeId node) const noexcept override {
    if (m_document == nullptr || !node.valid() ||
        node.authority() != m_document->documentAuthority())
      return false;
    try {
      for (std::size_t index = 0;
           index < m_document->objectCount(EditorObjectKind::scene); ++index) {
        const auto scene = SceneId::from(
            m_document->idAt(EditorObjectKind::scene, index));
        if (scene && m_document->hasNodeOccurrence(*scene, node, std::nullopt))
          return true;
      }
    } catch (...) {
      return false;
    }
    return false;
  }

private:
  const GltfDocument *m_document{nullptr};
};

class EditorController::SelectionAuthorityBridge final
    : public ILegacySelectionAuthority {
public:
  explicit SelectionAuthorityBridge(GltfDocument &document) noexcept
      : m_document(&document), m_selection(document.documentAuthority()),
        m_documentOccurrences(document) {
    refreshLegacy();
  }

  void setOccurrenceLookup(
      const ISceneSelectionOccurrenceLookup *occurrences) noexcept {
    m_occurrences = occurrences;
  }

  void setTransitionCause(SelectionCause cause) noexcept {
    m_transitionCause = cause;
  }

  [[nodiscard]] const SceneSelection &selection() const noexcept {
    return m_selection;
  }

  [[nodiscard]] std::vector<SelectionChanged> drainChanges() {
    return m_selection.drainChanges();
  }

  [[nodiscard]] bool applyEffect(const EditorSelectionEffect &effect) noexcept {
    if (m_document == nullptr ||
        effect.expectedRevision != m_selection.revision()) {
      return false;
    }
    const EditorSelectionSnapshot before = m_legacy;
    bool changed = false;
    try {
      switch (effect.kind) {
      case EditorSelectionEffectKind::clear:
        changed = m_selection.clear(effect.cause);
        m_nodeSceneContext.reset();
        break;
      case EditorSelectionEffectKind::setPrimitive:
        changed = m_occurrences != nullptr &&
                  m_selection.setPrimitive(*m_document, *m_occurrences,
                                           effect.occurrence, effect.cause,
                                           true);
        break;
      case EditorSelectionEffectKind::togglePrimitive:
        changed = m_occurrences != nullptr &&
                  m_selection.toggleViewportPrimitive(
                      *m_document, *m_occurrences, effect.occurrence,
                      effect.soleHighlightedOccurrenceMatches);
        break;
      }
    } catch (...) {
      return false;
    }
    refreshLegacy();
    publishLegacyEvent(before, changed);
    return changed || m_selection.lastSetStatus() ==
                          SceneSelectionSetStatus::unchanged;
  }

  [[nodiscard]] const EditorSelectionSnapshot &snapshot() const noexcept override {
    refreshLegacy();
    return m_legacy;
  }

  [[nodiscard]] std::uint64_t revision() const noexcept override {
    return m_selection.revision();
  }

  [[nodiscard]] const std::optional<EditorSelectionEvent> &
  lastEvent() const noexcept override {
    return m_lastEvent;
  }

  [[nodiscard]] EditorSelectionTransition clearTransition() noexcept override {
    const EditorSelectionSnapshot before = snapshot();
    bool changed = false;
    try {
      changed = m_selection.clear(m_transitionCause);
    } catch (...) {
      return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
    }
    if (changed)
      m_nodeSceneContext.reset();
    refreshLegacy();
    return transitionResult(before, changed);
  }

  [[nodiscard]] EditorSelectionTransition
  transition(const GltfDocument &document,
             EditorSelectionSnapshot value) noexcept override {
    const SelectionCause cause = m_transitionCause;
    if (&document != m_document) {
      return {EditorSelectionTransitionStatus::rejectedForeignAuthority,
              std::nullopt};
    }
    const EditorSelectionSnapshot before = snapshot();
    const std::optional<SceneSelectionValue> converted = toSceneValue(value);
    if (!converted) {
      return {value.authority.valid() &&
                      value.authority != document.documentAuthority()
                  ? EditorSelectionTransitionStatus::rejectedForeignAuthority
                  : EditorSelectionTransitionStatus::rejectedStale,
              std::nullopt};
    }
    bool changed = false;
    try {
      if (std::holds_alternative<std::monostate>(*converted)) {
        changed = m_selection.clear(cause);
        m_nodeSceneContext.reset();
      } else if (const auto *occurrence =
                     std::get_if<PrimitiveOccurrenceKey>(&*converted)) {
        const bool viewportCause =
            cause == SelectionCause::viewportSingle ||
            cause == SelectionCause::viewportSamePrimitiveToggle;
        const ISceneSelectionOccurrenceLookup *lookup = m_occurrences;
        if (!viewportCause &&
            (lookup == nullptr || !lookup->contains(*occurrence))) {
          lookup = &m_documentOccurrences;
        }
        changed = lookup != nullptr &&
                  m_selection.setPrimitive(document, *lookup, *occurrence,
                                           cause, false);
      } else {
        changed = m_selection.set(document, *converted, cause, false);
      }
    } catch (...) {
      return {EditorSelectionTransitionStatus::rejectedStale, std::nullopt};
    }
    const SceneSelectionSetStatus status = m_selection.lastSetStatus();
    if (!changed && status != SceneSelectionSetStatus::unchanged) {
      return {status == SceneSelectionSetStatus::rejectedForeignAuthority
                  ? EditorSelectionTransitionStatus::rejectedForeignAuthority
                  : status == SceneSelectionSetStatus::rejectedRevisionExhausted
                        ? EditorSelectionTransitionStatus::rejectedRevisionExhausted
                        : EditorSelectionTransitionStatus::rejectedStale,
              std::nullopt};
    }
    const bool nodeContextChanged = [&]() noexcept {
      const auto *requested = std::get_if<NodeSelection>(&value.value);
      const auto *current = std::get_if<NodeSelection>(&m_legacy.value);
      return requested != nullptr &&
             (current == nullptr || requested->scene != current->scene ||
              requested->parent != current->parent);
    }();
    if (const auto *requested = std::get_if<NodeSelection>(&value.value)) {
      m_nodeSceneContext = requested->scene;
    } else if (std::holds_alternative<std::monostate>(value.value)) {
      m_nodeSceneContext.reset();
    }
    if (changed || nodeContextChanged) {
      m_legacy = std::move(value);
    } else {
      refreshLegacy();
    }
    return transitionResult(before, changed);
  }

  [[nodiscard]] SelectionRestoreStatus
  restore(const GltfDocument &document,
          const EditorSelectionSnapshot &value) noexcept override {
    m_transitionCause = SelectionCause::undoRedoRestore;
    const EditorSelectionTransition result = transition(document, value);
    if (result.accepted()) {
      m_transitionCause = SelectionCause::programmatic;
      return value.emptyValue() ? SelectionRestoreStatus::clearedEmpty
                                : SelectionRestoreStatus::restored;
    }
    (void)clearTransition();
    m_transitionCause = SelectionCause::programmatic;
    return result.status ==
                   EditorSelectionTransitionStatus::rejectedForeignAuthority
               ? SelectionRestoreStatus::clearedForeignAuthority
               : SelectionRestoreStatus::clearedStale;
  }

  [[nodiscard]] SelectionRestoreStatus
  revalidate(const GltfDocument &document) noexcept override {
    if (&document != m_document) {
      (void)clearTransition();
      return SelectionRestoreStatus::clearedForeignAuthority;
    }
    const EditorSelectionSnapshot before = snapshot();
    bool changed = false;
    try {
      changed = m_selection.reconcile(document, nullptr, &m_documentOccurrences,
                                      SelectionCause::documentMutation);
    } catch (...) {
      return SelectionRestoreStatus::clearedStale;
    }
    refreshLegacy();
    publishLegacyEvent(before, changed);
    return m_legacy.emptyValue() ? SelectionRestoreStatus::clearedStale
                                 : SelectionRestoreStatus::restored;
  }

private:
  [[nodiscard]] std::optional<SceneSelectionValue>
  toSceneValue(const EditorSelectionSnapshot &legacy) const noexcept {
    if (legacy.emptyValue()) {
      return legacy.authority.valid() ? std::nullopt
                                      : std::optional<SceneSelectionValue>{
                                            SceneSelectionValue{}};
    }
    if (m_document == nullptr ||
        legacy.authority != m_document->documentAuthority()) {
      return std::nullopt;
    }
    if (const auto *node = std::get_if<NodeSelection>(&legacy.value)) {
      return SceneSelectionValue{node->node};
    }
    if (const auto *primitive =
            std::get_if<PrimitiveSelection>(&legacy.value)) {
      return SceneSelectionValue{PrimitiveOccurrenceKey{
          primitive->scene, primitive->node, primitive->primitive,
          primitive->instanceOrdinal}};
    }
    if (const auto *mesh = std::get_if<MeshId>(&legacy.value)) {
      return SceneSelectionValue{*mesh};
    }
    if (const auto *material = std::get_if<MaterialId>(&legacy.value)) {
      return SceneSelectionValue{*material};
    }
    if (const auto *camera = std::get_if<CameraId>(&legacy.value)) {
      return SceneSelectionValue{*camera};
    }
    if (const auto *light = std::get_if<LightId>(&legacy.value)) {
      return SceneSelectionValue{*light};
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<MeshId>
  meshForOccurrence(PrimitiveOccurrenceKey occurrence) const noexcept {
    if (m_document == nullptr) {
      return std::nullopt;
    }
    for (std::size_t index = 0;
         index < m_document->objectCount(EditorObjectKind::mesh); ++index) {
      const std::optional<MeshId> mesh = MeshId::from(
          m_document->idAt(EditorObjectKind::mesh, index));
      if (mesh && m_document->ownsPrimitiveOccurrence(
                      occurrence.scene, occurrence.node, *mesh,
                      occurrence.primitive)) {
        return mesh;
      }
    }
    return std::nullopt;
  }

  void refreshLegacy() const noexcept {
    if (m_document == nullptr) {
      m_legacy = EditorSelectionSnapshot::empty();
      return;
    }
    const SceneSelectionValue &value = m_selection.value();
    if (std::holds_alternative<std::monostate>(value)) {
      m_legacy = EditorSelectionSnapshot::empty();
      return;
    }
    m_legacy.authority = m_document->documentAuthority();
    if (const auto *node = std::get_if<NodeId>(&value)) {
      SceneId scene = m_nodeSceneContext.value_or(
          m_document->activeSceneId().value_or(SceneId{}));
      if (!scene.valid() ||
          !m_document->hasNodeOccurrence(scene, *node, std::nullopt)) {
        scene = m_document->activeSceneId().value_or(SceneId{});
      }
      std::vector<NodeId> parents;
      try {
        parents = m_document->sceneNodeParentPath(scene, *node);
      } catch (...) {
        parents.clear();
      }
      const std::optional<NodeId> parent =
          parents.empty() ? std::nullopt
                          : std::optional<NodeId>{parents.back()};
      m_legacy.value = NodeSelection{scene, *node, parent};
    } else if (const auto *occurrence =
                   std::get_if<PrimitiveOccurrenceKey>(&value)) {
      const std::optional<MeshId> mesh = meshForOccurrence(*occurrence);
      if (!mesh) {
        m_legacy = EditorSelectionSnapshot::empty();
        return;
      }
      m_legacy.value = PrimitiveSelection{
          occurrence->scene, occurrence->node, occurrence->primitive, *mesh,
          occurrence->instanceOrdinal};
    } else if (const auto *mesh = std::get_if<MeshId>(&value)) {
      m_legacy.value = *mesh;
    } else if (const auto *material = std::get_if<MaterialId>(&value)) {
      m_legacy.value = *material;
    } else if (const auto *camera = std::get_if<CameraId>(&value)) {
      m_legacy.value = *camera;
    } else if (const auto *light = std::get_if<LightId>(&value)) {
      m_legacy.value = *light;
    }
  }

  [[nodiscard]] EditorSelectionTransition transitionResult(
      const EditorSelectionSnapshot &before, bool changed) noexcept {
    refreshLegacy();
    if (!changed) {
      return {EditorSelectionTransitionStatus::unchanged, std::nullopt};
    }
    EditorSelectionEvent event{m_selection.revision(), before, m_legacy};
    m_lastEvent = event;
    return {EditorSelectionTransitionStatus::changed, event};
  }

  void publishLegacyEvent(const EditorSelectionSnapshot &before,
                          bool changed) noexcept {
    if (changed) {
      m_lastEvent =
          EditorSelectionEvent{m_selection.revision(), before, m_legacy};
    }
  }

  GltfDocument *m_document{nullptr};
  SceneSelection m_selection;
  const ISceneSelectionOccurrenceLookup *m_occurrences{nullptr};
  DocumentOccurrenceLookup m_documentOccurrences;
  std::optional<SceneId> m_nodeSceneContext;
  mutable EditorSelectionSnapshot m_legacy;
  std::optional<EditorSelectionEvent> m_lastEvent;
  SelectionCause m_transitionCause{SelectionCause::programmatic};
};

EditorController::EditorController(Callbacks callbacks)
    : m_callbacks(std::move(callbacks)) {}

EditorController::~EditorController() { unload(); }

std::optional<SceneMaterialColorPreview>
EditorController::materialColorPreview() const noexcept {
  const auto draft = m_ui.materialEditDraft();
  if (!draft) return std::nullopt;
  auto result = SceneMaterialColorPreview{};
  const auto material = MaterialId::from(draft->target);
  if (!material) return std::nullopt;
  result.material = *material;
  for (const PropertyEdit &edit : draft->edits) {
    if (const auto *value = std::get_if<std::array<double, 4>>(&edit.value)) {
      if (edit.field != FieldId::baseColor) continue;
      result.baseColorOverridden = true;
      for (std::size_t i = 0; i < 4u; ++i)
        result.baseColor[i] = static_cast<float>((*value)[i]);
    } else if (const auto *value = std::get_if<double>(&edit.value)) {
      const float converted = static_cast<float>(*value);
      switch (edit.field) {
      case FieldId::metallic: result.metallic = converted; break;
      case FieldId::roughness: result.roughness = converted; break;
      case FieldId::alphaCutoff: result.alphaCutoff = converted; break;
      case FieldId::normalScale: result.normalScale = converted; break;
      case FieldId::occlusionStrength: result.occlusionStrength = converted; break;
      default: break;
      }
    } else if (const auto *value =
                   std::get_if<std::array<double, 3>>(&edit.value)) {
      if (edit.field != FieldId::emissiveColor) continue;
      std::array<float, 3> converted{};
      for (std::size_t i = 0; i < 3u; ++i)
        converted[i] = static_cast<float>((*value)[i]);
      result.emissiveColor = converted;
    }
  }
  if (!m_projection || !result.valid()) return std::nullopt;
  const auto index = m_projection->mapping.sceneAssetIndex(
      result.material.untyped());
  if (!index) return std::nullopt;
  result.materialIndex = *index;
  result.documentRevision = m_projection->documentRevision;
  return result;
}

LifecycleState EditorController::computedLifecycleState(
    bool rendererPublicationFailed, bool rendererBusy) const noexcept {
  // An explicit producer state always wins while an operation is in flight.
  // This is what makes the short synchronous load/save windows observable to
  // a callback that samples buildFrameModel() before the operation returns.
  if (m_lifecycleOverride)
    return isKnownLifecycleState(m_lifecycleOverride->state)
               ? m_lifecycleOverride->state
               : LifecycleState::error;
  if (rendererPublicationFailed || !m_lastError.empty())
    return LifecycleState::error;
  if (m_ui.modalPending())
    return LifecycleState::modal;
  if (!hasDocument())
    return LifecycleState::noDocument;
  if (rendererBusy)
    return LifecycleState::savingMergingCompacting;
  if (m_gizmoDragging ||
      (m_activeSession.get() != nullptr &&
       m_activeSession.get()->editor().continuousEdit().active()))
    return LifecycleState::gizmoDragging;
  if (m_gizmoHovered)
    return LifecycleState::gizmoHover;
  return session()->document().dirty() ? LifecycleState::readyModified
                                       : LifecycleState::readyClean;
}

LifecycleState EditorController::lifecycleState() const noexcept {
  bool rendererPublicationFailed = false;
  bool rendererBusy = false;
  if (m_callbacks.rendererPublicationFailed) {
    try {
      rendererPublicationFailed = m_callbacks.rendererPublicationFailed();
    } catch (...) {
      rendererPublicationFailed = true;
    }
  }
  if (m_callbacks.rendererBusy) {
    try {
      rendererBusy = m_callbacks.rendererBusy();
    } catch (...) {
      rendererBusy = true;
    }
  }
  return computedLifecycleState(rendererPublicationFailed, rendererBusy);
}

OperationState EditorController::operationState() const {
  OperationState result = m_lifecycleOverride
                              ? m_lifecycleOverride->operation
                              : m_operationState;
  if (!m_lastError.empty()) {
    result.error = m_lastError;
    result.dismissibleError = true;
  } else if (result.error.empty() &&
             lifecycleState() == LifecycleState::error) {
    result.error = "Renderer publication failed";
    result.dismissibleError = true;
  }
  if (result.label.empty() &&
      lifecycleState() == LifecycleState::savingMergingCompacting) {
    result.label = "Scene operation in progress...";
  }
  return result;
}

LifecycleSnapshot EditorController::lifecycleSnapshot() const {
  return LifecycleSnapshot{lifecycleState(), operationState(),
                           m_lifecycleRevision,
                           m_lifecycleOverride.has_value()};
}

std::uint64_t EditorController::lifecycleRevision() const noexcept {
  return m_lifecycleRevision;
}

void EditorController::setLifecycleOverride(LifecycleState state,
                                             OperationState operation) {
  if (!isKnownLifecycleState(state)) {
    setLifecycleError("Invalid lifecycle state override");
    return;
  }
  if (operation.progress) {
    if (!std::isfinite(*operation.progress)) {
      operation.progress.reset();
    } else {
      *operation.progress = std::clamp(*operation.progress, 0.0F, 1.0F);
    }
  }
  m_lifecycleOverride =
      LifecycleOverride{state, std::move(operation)};
  m_operationState = m_lifecycleOverride->operation;
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::setLifecycleOverride(
    std::optional<LifecycleState> state, OperationState operation) {
  if (!state) {
    clearLifecycleOverride();
    return;
  }
  setLifecycleOverride(*state, std::move(operation));
}

void EditorController::clearLifecycleOverride() noexcept {
  m_lifecycleOverride.reset();
  m_operationState = {};
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::setOperationProgress(std::optional<float> progress) {
  if (progress) {
    if (!std::isfinite(*progress))
      progress.reset();
    else
      *progress = std::clamp(*progress, 0.0F, 1.0F);
  }
  if (m_lifecycleOverride) {
    m_lifecycleOverride->operation.progress = progress;
    m_operationState = m_lifecycleOverride->operation;
  } else {
    m_operationState.progress = progress;
  }
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::dismissError() noexcept {
  m_lastError.clear();
  if (m_lifecycleOverride &&
      m_lifecycleOverride->state == LifecycleState::error)
    m_lifecycleOverride.reset();
  m_operationState.error.clear();
  m_operationState.dismissibleError = false;
  if (m_lifecycleOverride) {
    m_operationState = m_lifecycleOverride->operation;
  }
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::setGizmoInteraction(bool hovered,
                                            bool dragging) noexcept {
  m_gizmoHovered = hovered || dragging;
  m_gizmoDragging = dragging;
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::setGizmoHover(bool hovered) noexcept {
  setGizmoInteraction(hovered, m_gizmoDragging);
}

void EditorController::noteViewportEscapeHandled() noexcept {
  m_ui.noteViewportEscapeHandled();
}

void EditorController::discardViewportEscapeHandling() noexcept {
  m_ui.discardViewportEscapeHandling();
}

void EditorController::notifyLifecycle() noexcept {
  if (!m_callbacks.onLifecycle)
    return;
  try {
    m_callbacks.onLifecycle(lifecycleState(), operationState());
  } catch (...) {
    // Lifecycle observation is diagnostic/acceptance plumbing and must never
    // turn a document operation into a failed publication.
  }
}

void EditorController::setLifecycleError(std::string message) {
  if (message.empty())
    message = "Editor operation failed";
  if (!m_lifecycleOverride && m_lastError == message &&
      m_operationState.error == message)
    return;
  m_lifecycleOverride.reset();
  m_savedLifecycleOverride.reset();
  m_operationState = {};
  m_lastError = std::move(message);
  m_operationState.error = m_lastError;
  m_operationState.dismissibleError = true;
  ++m_lifecycleRevision;
  notifyLifecycle();
  if (m_callbacks.onError) {
    try {
      m_callbacks.onError(m_lastError);
    } catch (...) {
    }
  }
}

void EditorController::clearLifecycleError() noexcept {
  if (m_lastError.empty() && m_operationState.error.empty())
    return;
  m_lastError.clear();
  m_operationState.error.clear();
  m_operationState.dismissibleError = false;
}

EditorController::LifecycleOperationFrame
EditorController::beginLifecycleOperation(LifecycleState state,
                                           LifecycleOperation operation,
                                           std::string label) {
  LifecycleOperationFrame frame;
  if (m_lifecycleOperationDepth == 0u) {
    frame.owner = true;
    m_savedLifecycleOverride = m_lifecycleOverride;
    clearLifecycleError();
    OperationState operationState;
    operationState.kind = operation;
    operationState.label = label.empty()
                               ? std::string(lifecycleOperationName(operation))
                               : std::move(label);
    m_lifecycleOverride =
        LifecycleOverride{state, std::move(operationState)};
    m_operationState = m_lifecycleOverride->operation;
    ++m_lifecycleRevision;
    notifyLifecycle();
  }
  ++m_lifecycleOperationDepth;
  return frame;
}

void EditorController::finishLifecycleOperation(LifecycleOperationFrame &frame,
                                                 bool succeeded,
                                                 std::string error) {
  if (frame.finished)
    return;
  frame.finished = true;
  if (m_lifecycleOperationDepth != 0u)
    --m_lifecycleOperationDepth;
  if (!frame.owner || m_lifecycleOperationDepth != 0u)
    return;

  if (!succeeded) {
    setLifecycleError(std::move(error));
    return;
  }

  m_lifecycleOverride = std::move(m_savedLifecycleOverride);
  m_savedLifecycleOverride.reset();
  m_operationState = m_lifecycleOverride
                         ? m_lifecycleOverride->operation
                         : OperationState{};
  clearLifecycleError();
  ++m_lifecycleRevision;
  notifyLifecycle();
}

void EditorController::captureBeforeMutation() {
  // Editing an animation-driven document transform while playback is live
  // would immediately overwrite the authored value in the next runtime pose.
  // Pause first; the authored document remains the sole mutation authority.
  if (!m_beforeMutationAnimation) {
    m_beforeMutationAnimation = AnimationRuntimeState{
        m_currentAnimation, m_animationPlaying, m_animationSpeed,
        m_animationTime, m_animationLoop, m_animationPose};
  }
  if (m_currentAnimation)
    m_animationPlaying = false;
  m_animationPose.reset();
  if (!m_beforeMutationProjection && m_projection) {
    m_beforeMutationProjection = *m_projection;
  }
}

void EditorController::restoreBeforeMutationAnimation() noexcept {
  if (!m_beforeMutationAnimation)
    return;
  const AnimationRuntimeState state = std::move(*m_beforeMutationAnimation);
  m_beforeMutationAnimation.reset();
  m_currentAnimation = state.current;
  m_animationPlaying = state.playing;
  m_animationSpeed = state.speed;
  m_animationTime = state.time;
  m_animationLoop = state.loop;
  m_animationPose = state.pose;
}

SceneCaptureSnapshot EditorController::captureSnapshot(
    const SceneProjectionResult &projection, bool includePayload) const {
  SceneCaptureSnapshot snapshot;
  snapshot.authority = projection.mapping.authority();
  snapshot.documentRevision = projection.documentRevision;
  snapshot.sceneActivationEpoch = projection.sceneActivationEpoch;
  snapshot.rootExternalStateSerial = projection.mapping.rootExternalStateSerial();
  snapshot.activeScene = projection.activeScene;
  snapshot.documentDigest = projection.documentDigest;
  snapshot.projectionDigest = projection.projectionDigest;
  if (includePayload) snapshot.immutableTarget = projection.immutableSnapshot;
  return snapshot;
}

std::optional<SceneCanonicalizationResult> EditorController::canonicalizeMutation(
    const SceneProjectionResult &before, const SceneProjectionResult &after,
    const SceneMutationEvidence &evidence,
    std::optional<FullRebuildReason> reason) const {
  return canonicalizeMutation(captureSnapshot(before, false), after, evidence, reason);
}

std::optional<SceneCanonicalizationResult> EditorController::canonicalizeMutation(
    const SceneCaptureSnapshot &beforeSnapshot, const SceneProjectionResult &after,
    const SceneMutationEvidence &evidence,
    std::optional<FullRebuildReason> reason) const {
  SceneCaptureSnapshot afterSnapshot = captureSnapshot(after, !evidence.structuralEvidenceComplete);
  SceneCanonicalizationResult canonical = SceneChangeCapture::finalize(
      beforeSnapshot, afterSnapshot, {evidence}, reason);
  if (canonical.succeeded()) {
    return canonical;
  }

  // A command that changed the document must still have a renderer-safe
  // publication if a legacy command omitted exact field evidence. Rebuild a
  // conservative full-snapshot envelope from the immutable projection. This
  // path is deliberately fail-closed: it requires a valid source token and
  // keeps the original error visible when neither is available.
  if (!evidence.sourceMutation.mutationToken.valid()) {
    return std::nullopt;
  }
  SceneMutationEvidence conservative = evidence;
  conservative.structuralEvidenceComplete = false;
  const FullRebuildReason fallbackReason =
      reason.value_or(FullRebuildReason::ExplicitCallerRequest);
  if (!afterSnapshot.immutableTarget) afterSnapshot.immutableTarget = after.immutableSnapshot;
  canonical = SceneChangeCapture::finalize(
      beforeSnapshot, afterSnapshot, {std::move(conservative)}, fallbackReason);
  return canonical.succeeded() ? std::optional<SceneCanonicalizationResult>{
                                    std::move(canonical)}
                              : std::nullopt;
}

void EditorController::emitProjection(
    ProjectionRefresh refresh,
    std::optional<SceneCanonicalizationResult> canonical) {
  if (!m_projection) {
    return;
  }
  if (m_callbacks.onProjection) {
    m_callbacks.onProjection(*m_projection, refresh);
  }
  if (m_callbacks.onPublication) {
    ProjectionPublication publication;
    publication.projection = &*m_projection;
    publication.refresh = refresh;
    publication.canonical = canonical ? &*canonical : nullptr;
    m_callbacks.onPublication(publication);
  }
}

bool EditorController::open(const std::filesystem::path &path,
                            std::string *error) {
  LifecycleOperationFrame lifecycle = beginLifecycleOperation(
      LifecycleState::loadingCandidate, LifecycleOperation::loadCandidate,
      "Loading candidate");
  const auto fail = [&](std::string message) {
    if (message.empty()) message = "Failed to load glTF candidate";
    if (error) *error = message;
    finishLifecycleOperation(lifecycle, false, std::move(message));
    return false;
  };
  try {
    GltfLoadResult loaded = GltfDocumentIO::loadCandidate(path);
    if (!loaded.succeeded()) {
      return fail(loaded.message().empty() ? "Failed to load glTF candidate"
                                           : loaded.message());
    }
    if (!replaceDocument(std::move(loaded).takeCandidate(), error)) {
      return fail(error != nullptr ? *error
                                   : "Failed to activate glTF candidate");
    }
    rememberRecentFile(path);
    finishLifecycleOperation(lifecycle, true);
    return true;
  } catch (...) {
    return fail("Unexpected exception while loading glTF candidate");
  }
}

bool EditorController::replaceDocument(std::unique_ptr<GltfDocument> document,
                                        std::string *error) {
  LifecycleOperationFrame lifecycle = beginLifecycleOperation(
      LifecycleState::loadingCandidate, LifecycleOperation::loadCandidate,
      "Loading candidate");
  const std::optional<std::uint32_t> previousVariant = m_currentVariant;
  bool published = false;
  const auto fail = [&](std::string message) {
    if (message.empty()) message = "Failed to activate glTF candidate";
    // Candidate preparation is isolated from the active runtime.  Restore
    // controller-only selection of the active variant when preparation fails.
    if (!published)
      m_currentVariant = previousVariant;
    if (error) *error = message;
    finishLifecycleOperation(lifecycle, false, std::move(message));
    return false;
  };
  // A retained rename hint is scoped to one document authority and must never
  // survive a replacement attempt, including a failed candidate projection.
  m_nodeNameTransaction.reset();
  m_nodeNameFastPathUsed = false;
  try {
    if (!document) return fail("Cannot activate an empty glTF document");

    SessionPrepareResult prepared =
        EditorSessionBundle::prepare(std::move(document));
    if (!prepared.succeeded()) return fail("Failed to prepare editor session");

    // A new document starts with the reference editor's default variant state.
    // Keep the active controller's variant untouched until the candidate is
    // atomically published; loading snapshots must remain coherent with the
    // old projection.
    std::optional<std::uint32_t> candidateVariant;
    SceneProjectionOptions options;
    options.baseMappingEpoch = m_projection ? m_projection->mappingEpoch : 0u;
    options.activeVariant = candidateVariant;
    SceneProjectionResult projected =
        SceneProjection::project(prepared.session->document(), options);
    if (!projected.succeeded()) return fail(projected.error);
    if (!projected.asset.materialVariantNames.empty()) {
      candidateVariant = 0u;
      options.activeVariant = candidateVariant;
      projected = SceneProjection::project(prepared.session->document(), options);
      if (!projected.succeeded()) return fail(projected.error);
    }

    auto candidateSelectionAuthority =
        std::make_unique<SelectionAuthorityBridge>(prepared.session->document());
    prepared.session->editor().bindSelectionAuthority(
        *candidateSelectionAuthority);
    if (!m_activeSession.replace(std::move(prepared.session)))
      return fail("Failed to publish editor session");
    m_selectionAuthority = std::move(candidateSelectionAuthority);
    m_currentVariant = candidateVariant;
    m_projection = std::move(projected);
    m_frameModelCache.reset();
    m_frameModelCacheVariant.reset();
    m_frameModelCacheProjection = nullptr;
    m_frameModelCacheMappingEpoch = 0u;
    published = true;
    m_beforeMutationProjection.reset();
    // A successful replacement starts a new document-scoped UI session.  Keep
    // persistent window/viewport preferences, but drop graph, popup, focus,
    // error, and selection-adjacent state bound to the old document.
    m_ui.resetDocumentState();
    resetRuntimeControls();
    m_gizmoHovered = false;
    m_gizmoDragging = false;
    // Match the reference browser's first-animation selection immediately on
    // open, while keeping the pose runtime-only and publishing one snapshot.
    if (const std::size_t animationCount =
            m_activeSession.get()->document().objectCount(
                EditorObjectKind::animation);
        animationCount != 0u) {
      const auto firstAnimation = AnimationId::from(
          m_activeSession.get()->document().idAt(EditorObjectKind::animation, 0u));
      if (firstAnimation) {
        const AnimationClipAnalysis analysis = analyzeAnimation(
            m_activeSession.get()->document(), 0u);
        if (analysis.succeeded()) {
          const AnimationSampleResult sample = sampleAnimation(
              m_activeSession.get()->document(), 0u, analysis.info.startTime,
              true);
          if (sample.succeeded()) {
            m_currentAnimation = firstAnimation;
            m_animationTime = sample.normalizedTime;
            m_animationPose = SceneAnimationPose{
                0u, sample.normalizedTime, true, sample.transforms};
            if (!rebuildProjection(ProjectionRefresh::fullScene, nullptr,
                                   false)) {
              resetRuntimeControls();
            }
          }
        }
      }
    }
    emitProjection(ProjectionRefresh::fullScene);
    finishLifecycleOperation(lifecycle, true);
    return true;
  } catch (...) {
    return fail("Unexpected exception while activating glTF candidate");
  }
}

void EditorController::unload() noexcept {
  const bool hadDocument = m_projection.has_value() ||
                           m_activeSession.get() != nullptr;
  m_beforeMutationProjection.reset();
  m_nodeNameTransaction.reset();
  m_nodeNameFastPathUsed = false;
  m_projection.reset();
  m_frameModelCache.reset();
  m_frameModelCacheVariant.reset();
  m_frameModelCacheProjection = nullptr;
  m_frameModelCacheMappingEpoch = 0u;
  m_activeSession.unload();
  m_selectionAuthority.reset();
  resetRuntimeControls();
  m_currentVariant.reset();
  m_lastError.clear();
  m_lifecycleOverride.reset();
  m_savedLifecycleOverride.reset();
  m_lifecycleOperationDepth = 0u;
  m_operationState = {};
  m_gizmoHovered = false;
  m_gizmoDragging = false;
  m_viewportHovered = false;
  m_viewportFocused = false;
  m_ui.resetDocumentState();
  ++m_lifecycleRevision;
  notifyLifecycle();
  if (hadDocument && m_callbacks.onDocumentReset) {
    try {
      m_callbacks.onDocumentReset();
    } catch (...) {
      // unload() is noexcept; a host reset callback must not tear down the
      // editor process if a renderer-side cleanup path reports an exception.
    }
  }
}

bool EditorController::hasDocument() const noexcept {
  return m_activeSession.get() != nullptr && m_projection.has_value();
}

EditorSessionBundle *EditorController::session() noexcept {
  return m_activeSession.get();
}

const EditorSessionBundle *EditorController::session() const noexcept {
  return m_activeSession.get();
}

const SceneProjectionResult *EditorController::projection() const noexcept {
  return m_projection ? &*m_projection : nullptr;
}

SceneProjectionResult *EditorController::projection() noexcept {
  return m_projection ? &*m_projection : nullptr;
}

RuntimeActionResult EditorController::dispatch(const UiCommand &command) {
  if (command.sequence != 0u && command.sequence <= m_commandSequence) {
    return rejected(MutationErrorCode::staleTransaction,
                    "stale editor command sequence");
  }
  m_commandSequence = std::max(m_commandSequence, command.sequence);
  bool rendererBusy = false;
  if (m_callbacks.rendererBusy) {
    try {
      rendererBusy = m_callbacks.rendererBusy();
    } catch (...) {
      rendererBusy = true;
    }
  }
  const bool allowedWhileBusy = std::visit(
      [](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, InvokeCommand>)
          return payload.id == CommandId::exit;
        if constexpr (std::is_same_v<T, SelectObject>)
          return true;
        if constexpr (std::is_same_v<T, MaterialAction>)
          return payload.operation == MaterialOperation::copyReference ||
                 payload.operation == MaterialOperation::pasteReference;
        return false;
      },
      command.payload);
  if (rendererBusy && !allowedWhileBusy) {
    return rejected(MutationErrorCode::staleTransaction,
                    "Editor command is unavailable while the renderer is busy");
  }
  const LifecycleState lifecycle = lifecycleState();
  if (lifecycle == LifecycleState::loadingCandidate ||
      lifecycle == LifecycleState::savingMergingCompacting) {
    const bool allowExit = std::visit(
        [](const auto &payload) {
          using T = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<T, InvokeCommand>)
            return payload.id == CommandId::exit;
          return false;
        },
        command.payload);
    if (!allowExit && !allowedWhileBusy) {
      return rejected(MutationErrorCode::staleTransaction,
                      "Editor command is unavailable during a scene operation");
    }
  }
  if (lifecycle == LifecycleState::modal &&
      command.source != CommandSource::dialog) {
    const bool allowExit = std::visit(
        [](const auto &payload) {
          using T = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<T, InvokeCommand>)
            return payload.id == CommandId::exit;
          return false;
        },
        command.payload);
    if (!allowExit) {
      return rejected(MutationErrorCode::staleTransaction,
                      "Editor command is unavailable while a modal is open");
    }
  }
  const bool preservesNodeNameTransaction = std::visit(
      [](const auto &payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, RenameObject>) {
          return true;
        } else if constexpr (std::is_same_v<T, InvokeCommand>) {
          return payload.id == CommandId::undo || payload.id == CommandId::redo;
        }
        return false;
      },
      command.payload);
  if (!preservesNodeNameTransaction) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
  }
  SelectionCause transitionCause = SelectionCause::programmatic;
  if (command.source == CommandSource::inspector) {
    transitionCause = SelectionCause::inspector;
  } else if (command.source == CommandSource::sceneBrowser) {
    transitionCause = SelectionCause::hierarchy;
  }
  if (m_selectionAuthority != nullptr) {
    m_selectionAuthority->setTransitionCause(transitionCause);
  }
  struct TransitionCauseReset final {
    SelectionAuthorityBridge *authority{nullptr};
    ~TransitionCauseReset() {
      if (authority != nullptr) {
        authority->setTransitionCause(SelectionCause::programmatic);
      }
    }
  } transitionCauseReset{m_selectionAuthority.get()};
  const RuntimeActionResult dispatched = std::visit(
      [this](const auto &payload) -> RuntimeActionResult {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, InvokeCommand>) return invoke(payload);
        if constexpr (std::is_same_v<T, SelectObject>) return select(payload);
        if constexpr (std::is_same_v<T, ActivateScene>) return activate(payload);
        if constexpr (std::is_same_v<T, HierarchyAction>) return hierarchy(payload);
        if constexpr (std::is_same_v<T, RenameObject>) return rename(payload);
        if constexpr (std::is_same_v<T, ReparentNode>) return reparent(payload);
        if constexpr (std::is_same_v<T, AnimationAction>) return animation(payload);
        if constexpr (std::is_same_v<T, SelectVariant>) {
          if (!hasDocument()) return unsupported("No active document");
          if (!m_projection || payload.variant >=
                                  m_projection->asset.materialVariantNames.size()) {
            return rejected(MutationErrorCode::invalidReference,
                            "Material variant selection is stale");
          }
          if (m_currentVariant && *m_currentVariant == payload.variant)
            return accepted();
          const std::optional<std::uint32_t> previous = m_currentVariant;
          m_currentVariant = payload.variant;
          std::string error;
          if (!rebuildProjection(ProjectionRefresh::fullScene, &error, false)) {
            m_currentVariant = previous;
            (void)rebuildProjection(ProjectionRefresh::fullScene, nullptr, false);
            return rejected(MutationErrorCode::internalError,
                            error.empty() ? "Failed to apply material variant"
                                          : std::move(error));
          }
          emitProjection(ProjectionRefresh::fullScene);
          return accepted(ProjectionRefresh::fullScene);
        }
        if constexpr (std::is_same_v<T, PropertyEdit>) return property(payload);
        if constexpr (std::is_same_v<T, MaterialAction>) return material(payload);
        if constexpr (std::is_same_v<T, CameraAction>) return camera(payload);
        if constexpr (std::is_same_v<T, SceneTransformAction>) return sceneTransform(payload);
        if constexpr (std::is_same_v<T, RebuildRenderNodes>) {
          refreshProjection(ProjectionRefresh::fullScene);
          return accepted(ProjectionRefresh::fullScene);
        }
        return unsupported("Unknown editor command payload");
      },
      command.payload);
  return dispatched;
}

RuntimeActionResult EditorController::commitMaterialDraft(
    const MaterialEditDraft &draft) {
  if (!hasDocument()) return unsupported("No active document");
  const auto material = MaterialId::from(draft.target);
  if (!material || draft.edits.empty())
    return accepted();
  if (draft.target.authority != session()->document().documentAuthority())
    return rejected(MutationErrorCode::crossDocument,
                    "Material draft belongs to another document");

  SceneCommands &commands = session()->editor().commands();
  MaterialDraftProperties value;
  value.properties = commands.materialProperties(*material)
                         .value_or(MaterialProperties{});
  value.normal = commands.materialTexture(*material, MaterialTextureSlot::normal)
                     .value_or(MaterialTextureProperties{});
  value.occlusion = commands.materialTexture(
      *material, MaterialTextureSlot::occlusion)
                        .value_or(MaterialTextureProperties{});
  for (const PropertyEdit &edit : draft.edits) {
    if (edit.target != draft.target || edit.phase != EditPhase::commit)
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Material draft contains an invalid field");
    switch (edit.field) {
    case FieldId::baseColor: {
      const auto *v = std::get_if<std::array<double, 4>>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Base Color draft value is invalid");
      value.properties.baseColorFactor = *v;
      break;
    }
    case FieldId::metallic: {
      const auto *v = std::get_if<double>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Metallic draft value is invalid");
      value.properties.metallicFactor = *v;
      break;
    }
    case FieldId::roughness: {
      const auto *v = std::get_if<double>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Roughness draft value is invalid");
      value.properties.roughnessFactor = *v;
      break;
    }
    case FieldId::emissiveColor: {
      const auto *v = std::get_if<std::array<double, 3>>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Emissive draft value is invalid");
      value.properties.emissiveFactor = *v;
      break;
    }
    case FieldId::alphaCutoff: {
      const auto *v = std::get_if<double>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Alpha Cutoff draft value is invalid");
      value.properties.alphaCutoff = *v;
      break;
    }
    case FieldId::normalScale: {
      const auto *v = std::get_if<double>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Normal Scale draft value is invalid");
      value.normal.scalar = *v;
      break;
    }
    case FieldId::occlusionStrength: {
      const auto *v = std::get_if<double>(&edit.value);
      if (!v) return rejected(MutationErrorCode::invalidReference,
                               "Occlusion Strength draft value is invalid");
      value.occlusion.scalar = *v;
      break;
    }
    default:
      return rejected(MutationErrorCode::unsupportedKind,
                      "Material field is not supported by draft commit");
    }
  }
  const auto before = commands.materialSnapshot(*material);
  const auto after = commands.prepareMaterialSnapshot(*material, value);
  if (!before || !after)
    return rejected(MutationErrorCode::invalidSnapshot,
                    "Material draft snapshot could not be prepared");
  if (*before == *after) return accepted();
  CommandPrepareResult prepared = prepareEditMaterialCommand(
      session()->editor().commandContext(), *before, *after);
  if (!prepared.succeeded())
    return rejected(prepared.error, std::move(prepared.message));
  captureBeforeMutation();
  const HistoryResult history = session()->editor().history().execute(
      std::move(prepared.command));
  return publishCommand(ProjectionRefresh::properties, history.command);
}

RuntimeActionResult EditorController::invoke(const InvokeCommand &command) {
  switch (command.id) {
  case CommandId::undo:
    if (!hasDocument()) return unsupported("No active document");
    captureBeforeMutation();
    return publishCommand(ProjectionRefresh::properties,
                          session()->editor().history().undo().command);
  case CommandId::redo:
    if (!hasDocument()) return unsupported("No active document");
    captureBeforeMutation();
    return publishCommand(ProjectionRefresh::properties,
                          session()->editor().history().redo().command);
  case CommandId::open:
  case CommandId::openRecent:
  case CommandId::importMerge:
  case CommandId::saveAs: {
    std::filesystem::path path;
    if (command.path) {
      path = *command.path;
    } else {
      NativeDialogRequest request;
      request.kind = command.id == CommandId::saveAs
                         ? NativeDialogKind::saveScene
                         : command.id == CommandId::importMerge
                               ? NativeDialogKind::importMergeScene
                               : NativeDialogKind::openScene;
      request.title = command.id == CommandId::saveAs ? "Save As" : "Open glTF";
      const auto selected = nativeFileDialog(request);
      if (!selected) return accepted();
      path = *selected;
    }
    if (command.id == CommandId::saveAs) {
      std::string error;
      return saveAs(path, &error) ? accepted() : rejected(
          MutationErrorCode::internalError, std::move(error));
    }
    if (command.id == CommandId::importMerge) {
      if (!hasDocument())
        return unsupported("No active document");
      LifecycleOperationFrame lifecycle = beginLifecycleOperation(
          LifecycleState::loadingCandidate, LifecycleOperation::loadCandidate,
          "Loading merge candidate");
      const auto fail = [&](std::string message,
                            MutationErrorCode code =
                                MutationErrorCode::invalidReference) {
        if (message.empty()) message = "Scene merge failed";
        finishLifecycleOperation(lifecycle, false, message);
        return rejected(code, std::move(message));
      };
      try {
        GltfLoadResult incoming = GltfDocumentIO::loadCandidate(path);
        if (!incoming.succeeded() || incoming.candidate() == nullptr) {
          const std::string message = incoming.message().empty()
                                          ? "Failed to load merge candidate"
                                          : incoming.message();
          applyHistoryLifecycle(session()->editor().history(),
                                HistoryLifecycleEvent::merge,
                                LifecycleCommitState::failed);
          return fail(message);
        }

        OperationState mergeOperation;
        mergeOperation.kind = LifecycleOperation::merge;
        mergeOperation.label = "Merging scene";
        setLifecycleOverride(LifecycleState::savingMergingCompacting,
                             std::move(mergeOperation));
        const std::string wrapperName = path.stem().string();
        const SceneMergeResult merged = SceneMerger::merge(
            session()->document(), *incoming.candidate(), wrapperName);
        const LifecycleCommitState state =
            merged.applied() ? LifecycleCommitState::changed
                             : merged.unchanged() ? LifecycleCommitState::noOp
                                                  : LifecycleCommitState::failed;
        applyHistoryLifecycle(session()->editor().history(),
                              HistoryLifecycleEvent::merge, state);
        if (!merged.succeeded()) {
          return fail(merged.message.empty() ? "Scene merge failed"
                                             : merged.message,
                      MutationErrorCode::validationFailed);
        }
        if (merged.unchanged()) {
          finishLifecycleOperation(lifecycle, true);
          return accepted();
        }
        (void)session()->editor().selection().revalidate(session()->document());
        m_beforeMutationProjection.reset();
        std::string error;
        if (!rebuildProjection(ProjectionRefresh::fullScene, &error)) {
          return fail(error.empty() ? "Merged document projection failed"
                                    : std::move(error),
                      MutationErrorCode::internalError);
        }
        finishLifecycleOperation(lifecycle, true);
        return accepted(ProjectionRefresh::fullScene);
      } catch (...) {
        applyHistoryLifecycle(session()->editor().history(),
                              HistoryLifecycleEvent::merge,
                              LifecycleCommitState::failed);
        return fail("Unexpected exception while merging scene",
                    MutationErrorCode::internalError);
      }
    }
    std::string error;
    return open(path, &error) ? accepted(ProjectionRefresh::fullScene)
                              : rejected(MutationErrorCode::invalidReference,
                                         std::move(error));
  }
  case CommandId::deleteSelection: {
    if (!hasDocument()) return unsupported("No active document");
    const auto node = nodeFromSelection(session()->editor().selection().snapshot());
    const auto scene = sceneFromSelection(session()->editor().selection().snapshot());
    if (!node || !scene) return accepted();
    return hierarchy(HierarchyAction{HierarchyOperation::deleteNode, *scene, node,
                                    std::nullopt});
  }
  case CommandId::duplicateSelection: {
    if (!hasDocument()) return unsupported("No active document");
    const auto node = nodeFromSelection(session()->editor().selection().snapshot());
    const auto scene = sceneFromSelection(session()->editor().selection().snapshot());
    if (!node || !scene) return accepted();
    return hierarchy(HierarchyAction{HierarchyOperation::duplicateNode, *scene, node,
                                    std::nullopt});
  }
  case CommandId::clearSelection:
    if (hasDocument()) session()->editor().selection().clear();
    return accepted(ProjectionRefresh::selectionOnly);
  case CommandId::toggleGrid:
    m_persistent.viewport.showGrid = !m_persistent.viewport.showGrid;
    return accepted();
  case CommandId::toggleGizmo:
    m_persistent.viewport.showGizmo = !m_persistent.viewport.showGizmo;
    return accepted();
  case CommandId::toggleSnap:
    m_persistent.viewport.snapEnabled = !m_persistent.viewport.snapEnabled;
    return accepted();
  case CommandId::toggleSceneBrowser:
    m_persistent.windows.sceneBrowser = !m_persistent.windows.sceneBrowser;
    return accepted();
  case CommandId::toggleInspector:
    m_persistent.windows.inspector = !m_persistent.windows.inspector;
    return accepted();
  case CommandId::showGridSnapSettings:
    m_persistent.windows.gridSnapSettings = !m_persistent.windows.gridSnapSettings;
    return accepted();
  case CommandId::compactScene:
    if (!hasDocument())
      return unsupported("No active document");
    {
      LifecycleOperationFrame lifecycle = beginLifecycleOperation(
          LifecycleState::savingMergingCompacting, LifecycleOperation::compact,
          "Compacting scene");
      const auto fail = [&](std::string message,
                            MutationErrorCode code =
                                MutationErrorCode::validationFailed) {
        if (message.empty()) message = "Scene compact failed";
        finishLifecycleOperation(lifecycle, false, message);
        return rejected(code, std::move(message));
      };
      try {
        const SceneCompactionResult compacted =
            SceneCompactor::compact(session()->document());
        const LifecycleCommitState state =
            compacted.applied()
                ? LifecycleCommitState::changed
                : compacted.unchanged() ? LifecycleCommitState::noOp
                                        : LifecycleCommitState::failed;
        applyHistoryLifecycle(session()->editor().history(),
                              HistoryLifecycleEvent::compact, state);
        if (!compacted.succeeded()) {
          return fail(compacted.message.empty() ? "Scene compact failed"
                                                : compacted.message);
        }
        if (compacted.unchanged()) {
          finishLifecycleOperation(lifecycle, true);
          return accepted();
        }
        (void)session()->editor().selection().revalidate(session()->document());
        m_beforeMutationProjection.reset();
        std::string error;
        if (!rebuildProjection(ProjectionRefresh::fullScene, &error)) {
          return fail(error.empty() ? "Compacted document projection failed"
                                    : std::move(error),
                      MutationErrorCode::internalError);
        }
        finishLifecycleOperation(lifecycle, true);
        return accepted(ProjectionRefresh::fullScene);
      } catch (...) {
        applyHistoryLifecycle(session()->editor().history(),
                              HistoryLifecycleEvent::compact,
                              LifecycleCommitState::failed);
        return fail("Unexpected exception while compacting scene",
                    MutationErrorCode::internalError);
      }
    }
  case CommandId::fitScene:
  case CommandId::fitObject:
    return cameraRequest(CameraRequest{command.id == CommandId::fitScene
                                           ? CameraRequestKind::fitScene
                                           : CameraRequestKind::fitSelection,
                                       std::nullopt, std::nullopt});
  case CommandId::newScene:
    unload();
    return accepted();
  case CommandId::exit:
    if (m_callbacks.requestExit)
      m_callbacks.requestExit();
    return accepted();
  case CommandId::count:
    break;
  }
  return unsupported("Unsupported editor command");
}

RuntimeActionResult EditorController::select(const SelectObject &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (!session()->editor().selection().select(session()->document(),
                                               command.selection)) {
    return rejected(MutationErrorCode::staleId, "Selection is stale");
  }
  return accepted(ProjectionRefresh::selectionOnly);
}

RuntimeActionResult EditorController::activate(const ActivateScene &command) {
  if (!hasDocument()) return unsupported("No active document");
  captureBeforeMutation();
  const SceneActivationResult result =
      session()->document().activateCurrentScene(command.scene);
  if (result.status == SceneActivationStatus::InvalidScene) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::invalidReference, "Invalid scene");
  }
  if (result.status == SceneActivationStatus::EpochExhausted) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::sceneActivationEpochExhausted,
                    "Scene activation epoch is exhausted");
  }
  if (result.changed()) {
    // Animation pose and material variant are scene-local runtime state. Do
    // not carry either selection across an active-scene switch.
    resetRuntimeControls();
    m_currentVariant.reset();
  }
  MutationResult mutation;
  mutation.status = result.changed() ? MutationStatus::applied
                                     : MutationStatus::domainNoop;
  mutation.error = MutationErrorCode::none;
  mutation.sceneActivationTransition = result;
  return publishMutation(ProjectionRefresh::fullScene, mutation);
}

RuntimeActionResult EditorController::hierarchy(const HierarchyAction &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (command.operation == HierarchyOperation::addRootNode) {
    captureBeforeMutation();
    const EditorMutationResult result = session()->editor().addRootNode(command.scene, "Node");
    return publishMutation(ProjectionRefresh::hierarchy, result.mutation);
  }
  CommandPrepareResult prepared = prepareHierarchyCommand(session()->editor(), command);
  if (!prepared.succeeded()) {
    return rejected(MutationErrorCode::invalidReference,
                    "Hierarchy operation could not be prepared");
  }
  captureBeforeMutation();
  const HistoryResult history = session()->editor().history().execute(
      std::move(prepared.command));
  return publishCommand(ProjectionRefresh::hierarchy, history.command);
}

RuntimeActionResult EditorController::rename(const RenameObject &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (const auto node = NodeId::from(command.object)) {
    CommandPrepareResult prepared = prepareRenameNodeCommand(
        session()->editor().commandContext(), *node, command.name);
    if (!prepared.succeeded()) {
      return rejected(prepared.error, std::move(prepared.message));
    }
    captureBeforeMutation();
    if (!m_beforeMutationProjection) {
      return rejected(MutationErrorCode::staleTransaction,
                      "rename has no before-projection capture");
    }
    NodeNameTransactionHint hint;
    hint.node = *node;
    hint.beforeName = std::string(
        session()->document().objectName(node->untyped()));
    hint.afterName = command.name;
    hint.before.documentDigest = m_beforeMutationProjection->documentDigest;
    hint.before.semanticHash = m_beforeMutationProjection->semanticHash;
    m_nodeNameTransaction = std::move(hint);
    const HistoryResult history = session()->editor().history().execute(
        std::move(prepared.command));
    if (!history.succeeded() || history.command.status != CommandStatus::applied) {
      m_nodeNameTransaction.reset();
    } else {
      NodeNameTransactionHint &retained = *m_nodeNameTransaction;
      if (!seedNodeNameProjectionCache(retained.after,
                                       history.command.changes,
                                       session()->document(),
                                       *m_beforeMutationProjection)) {
        m_nodeNameTransaction.reset();
      }
    }
    return publishCommand(ProjectionRefresh::hierarchy, history.command);
  }
  captureBeforeMutation();
  const MutationResult result = session()->editor().commands().rename(
      command.object, command.name);
  return publishMutation(ProjectionRefresh::properties, result);
}

RuntimeActionResult EditorController::reparent(const ReparentNode &command) {
  if (!hasDocument()) return unsupported("No active document");
  const SceneId targetScene = command.targetScene.valid()
                                  ? command.targetScene
                                  : command.source.scene;
  if (targetScene.authority() != session()->document().documentAuthority()) {
    return rejected(MutationErrorCode::crossDocument,
                    "Reparent target scene belongs to another document");
  }
  if (!targetScene.valid() ||
      !session()->document().contains(targetScene.untyped())) {
    return rejected(MutationErrorCode::staleOccurrence,
                    "Reparent target scene is stale");
  }
  const auto parent = command.targetParent
                          ? session()->editor().commands().captureNodeOccurrence(
                                targetScene, *command.targetParent)
                          : std::optional<SceneNodeOccurrence>{};
  if (command.source.revision != session()->document().revision()) {
    return rejected(MutationErrorCode::staleOccurrence,
                    "Reparent source occurrence is stale");
  }
  CommandPrepareResult prepared = prepareReparentNodeCommand(
      session()->editor().commandContext(),
      ReparentNodeRequest{command.source, parent,
                          SceneOccurrencePolicy::clickedScene, targetScene});
  if (!prepared.succeeded()) {
    return rejected(prepared.error, std::move(prepared.message));
  }
  captureBeforeMutation();
  const HistoryResult history = session()->editor().history().execute(
      std::move(prepared.command));
  return publishCommand(ProjectionRefresh::hierarchy, history.command);
}

RuntimeActionResult EditorController::animation(const AnimationAction &command) {
  if (!hasDocument())
    return unsupported("No active document");
  if (!command.animation)
    return rejected(MutationErrorCode::invalidReference,
                    "Animation selection is missing");
  const auto animationIndex = session()->document().indexOf(
      command.animation->untyped());
  if (!animationIndex || command.animation->authority() !=
                             session()->document().documentAuthority()) {
    return rejected(MutationErrorCode::staleId,
                    "Animation selection is stale");
  }
  const AnimationClipAnalysis analysis =
      analyzeAnimation(session()->document(), *animationIndex);
  if (!analysis.succeeded()) {
    return rejected(MutationErrorCode::unsupportedKind,
                    "Animation runtime is unavailable for this clip");
  }
  if (command.operation == AnimationOperation::select) {
    const std::optional<AnimationId> previousAnimation = m_currentAnimation;
    const bool previousPlaying = m_animationPlaying;
    const double previousTime = m_animationTime;
    const std::optional<SceneAnimationPose> previousPose = m_animationPose;
    m_currentAnimation = command.animation;
    m_animationPlaying = true;
    const RuntimeActionResult published =
        publishAnimationProjectionAt(analysis.info.startTime, m_animationLoop);
    if (!published.accepted) {
      m_currentAnimation = previousAnimation;
      m_animationPlaying = previousPlaying;
      m_animationTime = previousTime;
      m_animationPose = previousPose;
    }
    return published;
  }
  switch (command.operation) {
  case AnimationOperation::setPlaying:
    m_animationPlaying = command.value > 0.5;
    return accepted();
  case AnimationOperation::setSpeed:
    if (!std::isfinite(command.value) || command.value < 0.0 ||
        command.value > 100.0)
      return rejected(MutationErrorCode::invalidReference,
                      "Animation speed is outside the supported range");
    m_animationSpeed = command.value;
    return accepted();
  case AnimationOperation::reset:
    return publishAnimationProjectionAt(analysis.info.startTime,
                                        m_animationLoop);
  case AnimationOperation::setTime:
    if (!std::isfinite(command.value))
      return rejected(MutationErrorCode::invalidReference,
                      "Animation time is not finite");
    return publishAnimationProjectionAt(
        std::clamp(command.value, analysis.info.startTime, analysis.info.endTime),
        false);
  case AnimationOperation::step: {
    AnimationPlaybackState state{m_animationTime, m_animationSpeed, true,
                                 m_animationLoop};
    const AnimationAdvanceResult advanced = advanceAnimationPlayback(
        state, 1.0 / 60.0, analysis.info);
    if (!advanced.succeeded())
      return rejected(MutationErrorCode::invalidReference,
                      "Animation step could not advance the clip");
    return publishAnimationProjectionAt(state.time, m_animationLoop);
  }
  case AnimationOperation::select:
    break;
  }
  return unsupported("Unsupported animation operation");
}

RuntimeActionResult EditorController::property(const PropertyEdit &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (command.field == FieldId::name) {
    if (command.phase != EditPhase::commit)
      return accepted();
    if (const auto *name = std::get_if<std::string>(&command.value))
      return rename(RenameObject{command.target, *name});
    return rejected(MutationErrorCode::invalidName,
                    "Name requires a string");
  }
  const auto node = NodeId::from(command.target);
  if (node && command.field == FieldId::visibility) {
    const auto visible = valueBool(command.value);
    if (!visible.has_value())
      return rejected(MutationErrorCode::invalidReference,
                      "Visibility requires a boolean");
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().setVisibility(
        node->untyped(), *visible);
    return publishMutation(ProjectionRefresh::properties, result);
  }
  if (node && (command.field == FieldId::translation ||
               command.field == FieldId::rotationDegrees ||
               command.field == FieldId::scale)) {
    NodeTransform transform = nodeTransformFor(session()->editor().commands(), *node);
    if (command.field == FieldId::translation) {
      const auto value = valueVec3(command.value);
      if (!value) return rejected(MutationErrorCode::invalidTransform, "Translation requires three values");
      transform.translation = *value;
    } else if (command.field == FieldId::scale) {
      const auto value = valueVec3(command.value);
      if (!value) return rejected(MutationErrorCode::invalidTransform, "Scale requires three values");
      transform.scale = *value;
    } else {
      const auto value = valueVec3(command.value);
      if (!value) return rejected(MutationErrorCode::invalidTransform, "Rotation requires three values");
      const glm::vec3 degrees(static_cast<float>((*value)[0]),
                              static_cast<float>((*value)[1]),
                              static_cast<float>((*value)[2]));
      const glm::quat q = glm::quat(glm::radians(degrees));
      transform.rotation = {q.x, q.y, q.z, q.w};
    }
    if (command.phase == EditPhase::begin) {
      captureBeforeMutation();
      const ContinuousResult result = session()->editor().continuousEdit().beginTransform(
          ContinuousEditKind::inspectorTransform, *node);
      if (result.status == ContinuousStatus::rejected) {
        if (!session()->editor().continuousEdit().active()) {
          restoreBeforeMutationAnimation();
          m_beforeMutationProjection.reset();
        }
        return rejected(result.error, result.message);
      }
      return accepted();
    }
    if (command.phase == EditPhase::preview) {
      const ContinuousResult result = session()->editor().continuousEdit().preview(transform);
      return publishContinuousPreview(ProjectionRefresh::properties, result);
    }
    const ContinuousResult previewResult = session()->editor().continuousEdit().preview(transform);
    if (previewResult.status == ContinuousStatus::rejected) {
      return rejected(previewResult.error, previewResult.message);
    }
    const RuntimeActionResult previewPublished = publishContinuousPreview(
        ProjectionRefresh::properties, previewResult);
    if (!previewPublished.accepted) {
      return previewPublished;
    }
    const ContinuousResult finished = session()->editor().continuousEdit().finishFrame(
        ContinuousFrameSignals{.applicationFrame = m_commandSequence,
                                .inspectorDeactivatedAfterEdit = true,
                                .windowFocused = true});
    if (finished.status == ContinuousStatus::rejected) {
      restoreBeforeMutationAnimation();
      m_beforeMutationProjection.reset();
      return rejected(finished.error, finished.message);
    }
    m_beforeMutationAnimation.reset();
    m_beforeMutationProjection.reset();
    return accepted(previewPublished.refresh);
  }

  if (const auto material = MaterialId::from(command.target)) {
    SceneCommands &commands = session()->editor().commands();
    MaterialProperties properties =
        commands.materialProperties(*material).value_or(MaterialProperties{});
    MaterialEdit edit{properties};
    switch (command.field) {
    case FieldId::baseColor: {
      const auto v = valueVec4(command.value);
      if (!v) return rejected(MutationErrorCode::invalidReference, "Base color requires four values");
      properties.baseColorFactor = *v;
      edit = properties;
      break;
    }
    case FieldId::metallic: {
      const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Metallic requires a number"); properties.metallicFactor = *v; edit = properties; break;
    }
    case FieldId::roughness: {
      const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Roughness requires a number"); properties.roughnessFactor = *v; edit = properties; break;
    }
    case FieldId::emissiveColor: {
      const auto v = valueVec3(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Emissive color requires three values"); properties.emissiveFactor = *v; edit = properties; break;
    }
    case FieldId::alphaCutoff: {
      const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Alpha cutoff requires a number"); properties.alphaCutoff = *v; edit = properties; break;
    }
    case FieldId::doubleSided: {
      const auto v = valueBool(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Double sided requires a boolean"); properties.doubleSided = *v; edit = properties; break;
    }
    case FieldId::alphaMode: {
      const auto v = valueIndex(command.value);
      if (!v || *v < 0 || *v > 2)
        return rejected(MutationErrorCode::invalidReference,
                        "Alpha mode requires a valid choice");
      properties.alphaMode = *v == 0 ? "OPAQUE" : *v == 1 ? "MASK" : "BLEND";
      edit = properties;
      break;
    }
    case FieldId::normalScale:
    case FieldId::occlusionStrength: {
      const MaterialTextureSlot slot =
          command.field == FieldId::normalScale
              ? MaterialTextureSlot::normal
              : MaterialTextureSlot::occlusion;
      MaterialTextureProperties texture =
          commands.materialTexture(*material, slot)
              .value_or(MaterialTextureProperties{});
      const auto scalar = valueNumber(command.value);
      if (!scalar)
        return rejected(MutationErrorCode::invalidReference,
                        "Texture scalar requires a number");
      texture.scalar = *scalar;
      edit = MaterialTextureEdit{slot, texture};
      break;
    }
    default: {
      if (const auto slot = materialTextureSlotFor(command.field)) {
        const auto index = valueIndex(command.value);
        if (!index || *index < -1)
          return rejected(MutationErrorCode::invalidReference,
                          "Texture choice is invalid");
        MaterialTextureProperties texture =
            commands.materialTexture(*material, *slot)
                .value_or(MaterialTextureProperties{});
        texture.texture.reset();
        if (*index >= 0) {
          const EditorObjectId id = session()->document().idAt(
              EditorObjectKind::texture, static_cast<std::size_t>(*index));
          const auto textureId = TextureId::from(id);
          if (!textureId)
            return rejected(MutationErrorCode::invalidReference,
                            "Texture choice is stale");
          texture.texture = *textureId;
        }
        edit = MaterialTextureEdit{*slot, texture};
        break;
      }
      if (command.field == FieldId::infiniteAttenuation) {
        const auto infinite = valueBool(command.value);
        if (!infinite)
          return rejected(MutationErrorCode::invalidReference,
                          "Infinite attenuation requires a boolean");
        auto extension = commands.materialExtension(
            *material, "KHR_materials_volume");
        if (!extension)
          return rejected(MutationErrorCode::invalidExtension,
                          "Volume extension is not present");
        auto *object = std::get_if<MaterialExtensionValue::Object>(
            &extension->value);
        if (object == nullptr)
          return rejected(MutationErrorCode::invalidExtension,
                          "Volume extension is malformed");
        if (*infinite) {
          eraseExtensionMember(*object, "attenuationDistance");
        } else if (extensionMember(*object, "attenuationDistance") == nullptr) {
          MaterialExtensionValue distance;
          distance.value = 1.0;
          object->emplace_back("attenuationDistance", std::move(distance));
        }
        edit = MaterialExtensionEdit{"KHR_materials_volume",
                                     std::move(extension)};
        break;
      }
      const ExtensionFieldSpec *spec = extensionFieldSpec(command.field);
      if (spec == nullptr)
        return unsupported("Material field has no public domain mapping");
      const std::optional<MaterialExtensionValue> beforeExtension =
          commands.materialExtension(*material, spec->extension);
      if (!beforeExtension && command.field != FieldId::unlit)
        return rejected(MutationErrorCode::invalidExtension,
                        "Material extension is not present");
      if (spec->texture) {
        const auto index = valueIndex(command.value);
        if (!index || *index < -1 ||
            (*index >= 0 && static_cast<std::size_t>(*index) >=
                                session()->document().objectCount(
                                    EditorObjectKind::texture))) {
          return rejected(MutationErrorCode::invalidReference,
                          "Extension texture choice is invalid");
        }
      }
      if (command.field == FieldId::unlit) {
        const auto enabled = valueBool(command.value);
        if (!enabled)
          return rejected(MutationErrorCode::invalidReference,
                          "Unlit requires a boolean");
        if (*enabled) {
          MaterialExtensionValue value;
          value.value = MaterialExtensionValue::Object{};
          edit = MaterialExtensionEdit{std::string(spec->extension),
                                       std::move(value)};
        } else {
          edit = MaterialExtensionEdit{std::string(spec->extension),
                                       std::nullopt};
        }
        break;
      }
      const auto afterExtension =
          editExtensionValue(beforeExtension, *spec, command.value);
      if (!afterExtension)
        return rejected(MutationErrorCode::invalidReference,
                        "Material extension value has the wrong type");
      edit = MaterialExtensionEdit{std::string(spec->extension),
                                   std::move(afterExtension)};
      break;
    }
    }
    const auto after = commands.prepareMaterialSnapshot(*material, edit);
    if (!after) {
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Material snapshot could not be prepared");
    }
    if (command.phase == EditPhase::begin) {
      captureBeforeMutation();
      const ContinuousResult result =
          session()->editor().continuousEdit().beginMaterial(*material);
      if (result.status == ContinuousStatus::rejected) {
        if (!session()->editor().continuousEdit().active()) {
          restoreBeforeMutationAnimation();
          m_beforeMutationProjection.reset();
        }
        return rejected(result.error, result.message);
      }
      return accepted();
    }
    if (command.phase == EditPhase::preview ||
        session()->editor().continuousEdit().active()) {
      const ContinuousResult preview =
          session()->editor().continuousEdit().preview(*after);
      if (preview.status == ContinuousStatus::rejected) {
        return rejected(preview.error, preview.message);
      }
      if (command.phase == EditPhase::preview) {
        return publishContinuousPreview(ProjectionRefresh::properties, preview);
      }
      const RuntimeActionResult previewPublished = publishContinuousPreview(
          ProjectionRefresh::properties, preview);
      if (!previewPublished.accepted) {
        return previewPublished;
      }
      const ContinuousResult finished =
          session()->editor().continuousEdit().finishFrame(
              ContinuousFrameSignals{.applicationFrame = m_commandSequence,
                                     .inspectorDeactivatedAfterEdit = true,
                                     .windowFocused = true});
      if (finished.status == ContinuousStatus::rejected) {
        restoreBeforeMutationAnimation();
        m_beforeMutationProjection.reset();
        return rejected(finished.error, finished.message);
      }
      m_beforeMutationAnimation.reset();
      m_beforeMutationProjection.reset();
      return accepted(previewPublished.refresh);
    }
    const auto before = commands.materialSnapshot(*material);
    if (!before) {
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Material snapshot is unavailable");
    }
    CommandPrepareResult prepared = prepareEditMaterialCommand(
        session()->editor().commandContext(), *before, *after);
    if (!prepared.succeeded()) {
      return rejected(prepared.error, std::move(prepared.message));
    }
    captureBeforeMutation();
    const HistoryResult history = session()->editor().history().execute(
        std::move(prepared.command));
    return publishCommand(ProjectionRefresh::properties, history.command);
  }

  if (const auto camera = CameraId::from(command.target)) {
    CameraProperties properties =
        cameraPropertiesFor(session()->editor().commands(), *camera);
    const auto number = valueNumber(command.value);
    switch (command.field) {
    case FieldId::cameraType: {
      const auto choice = valueIndex(command.value);
      if (!choice || (*choice != 0 && *choice != 1))
        return rejected(MutationErrorCode::invalidReference,
                        "Camera type requires a valid choice");
      properties.type = *choice == 0 ? "perspective" : "orthographic";
      break;
    }
    case FieldId::cameraYFov: if (number) properties.yfov = *number; break;
    case FieldId::cameraAspectRatio: if (number) properties.aspectRatio = *number; break;
    case FieldId::cameraZNear: if (number) properties.znear = *number; break;
    case FieldId::cameraZFar: if (number) properties.zfar = *number; break;
    case FieldId::cameraXMag: if (number) properties.xmag = *number; break;
    case FieldId::cameraYMag: if (number) properties.ymag = *number; break;
    default: return unsupported("Camera field is not yet mapped");
    }
    if (command.field != FieldId::cameraType && !number) {
      return rejected(MutationErrorCode::invalidReference,
                      "Camera property requires a number");
    }
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().setCamera(
        *camera, properties);
    return publishMutation(ProjectionRefresh::properties, result);
  }

  if (const auto light = LightId::from(command.target)) {
    LightProperties properties = lightPropertiesFor(session()->editor().commands(), *light);
    switch (command.field) {
    case FieldId::lightType: {
      const auto choice = valueIndex(command.value);
      if (!choice || *choice < 0 || *choice > 2)
        return rejected(MutationErrorCode::invalidReference,
                        "Light type requires a valid choice");
      properties.type = *choice == 0
                            ? "directional"
                            : *choice == 1 ? "point" : "spot";
      break;
    }
    case FieldId::lightIntensity: { const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Intensity requires a number"); properties.intensity = *v; break; }
    case FieldId::lightRange: { const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Range requires a number"); properties.range = *v; break; }
    case FieldId::lightColorSrgb: { const auto v = valueVec3(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Light color requires three values"); properties.color = *v; break; }
    case FieldId::lightInnerConeAngle: { const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Inner cone requires a number"); properties.innerConeAngle = *v; break; }
    case FieldId::lightOuterConeAngle: { const auto v = valueNumber(command.value); if (!v) return rejected(MutationErrorCode::invalidReference, "Outer cone requires a number"); properties.outerConeAngle = *v; break; }
    default: return unsupported("Light field is not yet mapped");
    }
    if (command.phase == EditPhase::begin) {
      captureBeforeMutation();
      const ContinuousResult result = session()->editor().continuousEdit().beginLight(*light);
      if (result.status == ContinuousStatus::rejected) {
        if (!session()->editor().continuousEdit().active()) {
          restoreBeforeMutationAnimation();
          m_beforeMutationProjection.reset();
        }
        return rejected(result.error, result.message);
      }
      return accepted();
    }
    if (command.phase == EditPhase::preview) {
      const auto snapshot = session()->editor().commands().prepareLightSnapshot(*light, properties);
      if (!snapshot) return rejected(MutationErrorCode::invalidSnapshot, "Light snapshot could not be prepared");
      const ContinuousResult result = session()->editor().continuousEdit().preview(*snapshot);
      return publishContinuousPreview(ProjectionRefresh::properties, result);
    }
    const auto after = session()->editor().commands().prepareLightSnapshot(
        *light, properties);
    if (!after) {
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Light snapshot could not be prepared");
    }
    if (session()->editor().continuousEdit().active()) {
      const ContinuousResult preview =
          session()->editor().continuousEdit().preview(*after);
      if (preview.status == ContinuousStatus::rejected) {
        return rejected(preview.error, preview.message);
      }
      const RuntimeActionResult previewPublished = publishContinuousPreview(
          ProjectionRefresh::properties, preview);
      if (!previewPublished.accepted) {
        return previewPublished;
      }
      const ContinuousResult finished =
          session()->editor().continuousEdit().finishFrame(
              ContinuousFrameSignals{.applicationFrame = m_commandSequence,
                                     .inspectorDeactivatedAfterEdit = true,
                                     .windowFocused = true});
      if (finished.status == ContinuousStatus::rejected) {
        restoreBeforeMutationAnimation();
        m_beforeMutationProjection.reset();
        return rejected(finished.error, finished.message);
      }
      m_beforeMutationAnimation.reset();
      m_beforeMutationProjection.reset();
      return accepted(previewPublished.refresh);
    }
    const auto before = session()->editor().commands().lightSnapshot(*light);
    if (!before) {
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Light snapshot is unavailable");
    }
    CommandPrepareResult prepared = prepareEditLightCommand(
        session()->editor().commandContext(), *before, *after);
    if (!prepared.succeeded()) {
      return rejected(prepared.error, std::move(prepared.message));
    }
    captureBeforeMutation();
    const HistoryResult history = session()->editor().history().execute(
        std::move(prepared.command));
    return publishCommand(ProjectionRefresh::properties, history.command);
  }
  return unsupported("Property target is not supported");
}

RuntimeActionResult EditorController::material(const MaterialAction &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (command.operation == MaterialOperation::pasteReference) {
    if (!command.primitive) {
      return rejected(MutationErrorCode::invalidReference,
                      "Paste requires a primitive context");
    }
    if (m_callbacks.clipboard == nullptr) {
      return rejected(MutationErrorCode::internalError,
                      "The OS clipboard is unavailable");
    }
    const MaterialPasteAvailability paste =
        MaterialClipboardCodec::resolveOnPasteClick(
            session()->document(), *m_callbacks.clipboard);
    if (!paste.menuEnabled) {
      return rejected(MutationErrorCode::invalidReference,
                      "The clipboard has no material reference");
    }
    // Canonical but out-of-range text remains a click-time no-op. It never
    // carries an ID resolved against a previous document into this command.
    if (!paste.applicable() ||
        !paste.wouldChange(command.primitive->material)) {
      return accepted();
    }
    captureBeforeMutation();
    const MutationResult result =
        session()->editor().commands().assignPrimitiveMaterial(
            command.primitive->primitive, paste.activeDocumentMaterial);
    return publishMutation(ProjectionRefresh::properties, result);
  }
  if (command.operation == MaterialOperation::assign && command.primitive) {
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().assignPrimitiveMaterial(
        command.primitive->primitive, command.material);
    return publishMutation(ProjectionRefresh::properties, result);
  }
  if (command.operation == MaterialOperation::split && command.primitive) {
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().splitPrimitive(
        command.primitive->node, command.primitive->primitive);
    return publishMutation(ProjectionRefresh::topology, result);
  }
  if (command.operation == MaterialOperation::merge && command.primitive) {
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().mergeMeshes(
        command.primitive->node);
    return publishMutation(ProjectionRefresh::topology, result);
  }
  if (command.operation == MaterialOperation::focusAssignment) {
    return rejected(MutationErrorCode::invalidReference,
                    "Material assignment requires a primitive context");
  }
  if (command.operation == MaterialOperation::duplicate && command.material) {
    captureBeforeMutation();
    const MutationResult result = session()->editor().commands().duplicateMaterial(
        *command.material);
    const RuntimeActionResult published =
        publishMutation(ProjectionRefresh::properties, result);
    if (!published.accepted || !result.applied()) return published;
    for (const MutationRemapEntry &entry : result.remap.materials) {
      if (!entry.oldId.valid() && entry.newId.valid()) {
        const MaterialId duplicated = MaterialId::from(entry.newId).value_or(MaterialId{});
        if (duplicated.valid()) {
          (void)session()->editor().selection().select(
              session()->document(),
              EditorSelectionSnapshot{session()->document().documentAuthority(), duplicated});
        }
        break;
      }
    }
    return published;
  }
  if (command.operation == MaterialOperation::copyReference && command.material) {
    return m_callbacks.clipboard != nullptr &&
                   MaterialClipboardCodec::copy(
                       session()->document(), *command.material,
                       *m_callbacks.clipboard)
               ? accepted()
               : rejected(MutationErrorCode::internalError,
                          "The OS clipboard is unavailable");
  }
  if ((command.operation == MaterialOperation::addExtension ||
       command.operation == MaterialOperation::removeExtension) &&
      command.material && !command.extension.empty()) {
    SceneCommands &commands = session()->editor().commands();
    const auto before = commands.materialSnapshot(*command.material);
    if (!before)
      return rejected(MutationErrorCode::invalidSnapshot,
                      "Material snapshot is unavailable");
    std::optional<MaterialExtensionValue> value;
    if (command.operation == MaterialOperation::addExtension) {
      if (commands.materialExtension(*command.material, command.extension))
        return accepted();
      MaterialExtensionValue empty;
      empty.value = MaterialExtensionValue::Object{};
      value = std::move(empty);
    } else if (!commands.materialExtension(*command.material,
                                           command.extension)) {
      return accepted();
    }
    const auto after = commands.prepareMaterialSnapshot(
        *command.material,
        MaterialEdit{MaterialExtensionEdit{command.extension,
                                           std::move(value)}});
    if (!after)
      return rejected(MutationErrorCode::invalidExtension,
                      "Material extension change violates document constraints");
    CommandPrepareResult prepared = prepareEditMaterialCommand(
        session()->editor().commandContext(), *before, *after);
    if (!prepared.succeeded())
      return rejected(prepared.error, std::move(prepared.message));
    captureBeforeMutation();
    const HistoryResult history = session()->editor().history().execute(
        std::move(prepared.command));
    return publishCommand(ProjectionRefresh::properties, history.command);
  }
  return rejected(MutationErrorCode::invalidReference,
                  "Material operation has incomplete context");
}

RuntimeActionResult EditorController::camera(const CameraAction &command) {
  if (!hasDocument()) return unsupported("No active document");
  if (m_callbacks.cameraRequest) {
    return m_callbacks.cameraRequest(
        CameraRequest{command.operation == CameraOperation::applyToCurrentView
                          ? CameraRequestKind::applyDocumentCamera
                          : CameraRequestKind::setDocumentCameraFromView,
                      command.camera, command.node});
  }
  return unsupported("Camera bridge is not connected");
}

RuntimeActionResult EditorController::setCameraFromView(
    CameraId camera, NodeId node, const CameraProperties &properties,
    const NodeTransform &transform) {
  if (!hasDocument()) return unsupported("No active document");
  captureBeforeMutation();
  const MutationResult result = session()->editor().commands().setCameraFromView(
      camera, node, properties, transform);
  return publishMutation(ProjectionRefresh::properties, result);
}

RuntimeActionResult EditorController::applyViewportSelection(
    const EditorSelectionSnapshot &selection, bool toggle) {
  if (!hasDocument()) return unsupported("No active document");

  if (m_selectionAuthority != nullptr) {
    const bool primitiveSelection =
        std::holds_alternative<PrimitiveSelection>(selection.value);
    m_selectionAuthority->setTransitionCause(
        primitiveSelection
            ? (toggle ? SelectionCause::viewportSamePrimitiveToggle
                      : SelectionCause::viewportSingle)
            : SelectionCause::programmatic);
  }
  struct ViewportSelectionCauseReset final {
    SelectionAuthorityBridge *authority{nullptr};
    ~ViewportSelectionCauseReset() {
      if (authority != nullptr)
        authority->setTransitionCause(SelectionCause::programmatic);
    }
  } causeReset{m_selectionAuthority.get()};

  const EditorSelectionSnapshot current =
      session()->editor().selection().snapshot();
  if (toggle && selection == current) {
    const EditorSelectionTransition transition =
        session()->editor().selection().clearTransition();
    if (!transition.accepted()) {
      return rejected(MutationErrorCode::internalError,
                      "Viewport selection could not be cleared");
    }
    return accepted(ProjectionRefresh::selectionOnly);
  }

  const EditorSelectionTransition transition = selection.emptyValue()
      ? session()->editor().selection().clearTransition()
      : session()->editor().selection().transition(session()->document(),
                                                    selection);
  if (!transition.accepted()) {
    const MutationErrorCode error =
        transition.status ==
                EditorSelectionTransitionStatus::rejectedForeignAuthority
            ? MutationErrorCode::crossDocument
            : MutationErrorCode::staleId;
    return rejected(error, "Viewport selection is stale");
  }
  return accepted(ProjectionRefresh::selectionOnly);
}

RuntimeActionResult EditorController::applyPickingSelection(
    const EditorSelectionEffect &effect) {
  if (!hasDocument() || m_selectionAuthority == nullptr) {
    return unsupported("No active selection authority");
  }
  if (!m_selectionAuthority->applyEffect(effect)) {
    return rejected(MutationErrorCode::staleId,
                    "Picking selection effect is stale or invalid");
  }
  return accepted(ProjectionRefresh::selectionOnly);
}

void EditorController::setSelectionOccurrenceLookup(
    const ISceneSelectionOccurrenceLookup *occurrences) noexcept {
  if (m_selectionAuthority != nullptr) {
    m_selectionAuthority->setOccurrenceLookup(occurrences);
  }
}

const SceneSelection &EditorController::sceneSelection() const noexcept {
  static const SceneSelection empty;
  return m_selectionAuthority != nullptr
             ? m_selectionAuthority->selection()
             : empty;
}

SelectionRevision EditorController::selectionRevision() const noexcept {
  return sceneSelection().revision();
}

std::vector<SelectionChanged> EditorController::drainSelectionChanges() {
  return m_selectionAuthority != nullptr
             ? m_selectionAuthority->drainChanges()
             : std::vector<SelectionChanged>{};
}

std::optional<NodeTransform>
EditorController::viewportNodeTransform(NodeId node) const noexcept {
  if (!hasDocument() || !node.valid()) return std::nullopt;
  return session()->editor().commands().nodeTransform(node);
}

std::vector<NodeId> EditorController::sceneNodeParentPath(
    SceneId scene, NodeId node) const {
  if (!hasDocument() || !scene.valid() || !node.valid())
    return {};
  return session()->document().sceneNodeParentPath(scene, node);
}

RuntimeActionResult EditorController::beginGizmoTransform(NodeId node) {
  if (!hasDocument()) return unsupported("No active document");
  if (!node.valid() || node.authority() != session()->document().documentAuthority() ||
      !session()->document().contains(node.untyped())) {
    return rejected(MutationErrorCode::staleId,
                    "Gizmo node selection is stale");
  }
  if (session()->editor().continuousEdit().active()) {
    return rejected(MutationErrorCode::staleTransaction,
                    "Another continuous edit is active");
  }

  captureBeforeMutation();
  const ContinuousResult result =
      session()->editor().continuousEdit().beginTransform(
          ContinuousEditKind::gizmoTransform, node);
  if (result.status == ContinuousStatus::rejected) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    m_gizmoDragging = false;
    ++m_lifecycleRevision;
    notifyLifecycle();
    return rejected(result.error, result.message);
  }
  m_gizmoFinishSequence = 0u;
  m_gizmoHovered = true;
  m_gizmoDragging = true;
  ++m_lifecycleRevision;
  notifyLifecycle();
  return accepted();
}

RuntimeActionResult EditorController::previewGizmoTransform(
    NodeId node, const NodeTransform &transform) {
  if (!hasDocument()) return unsupported("No active document");
  if (!node.valid() || node.authority() != session()->document().documentAuthority()) {
    return rejected(MutationErrorCode::staleId,
                    "Gizmo node selection is stale");
  }
  const ContinuousResult result =
      session()->editor().continuousEdit().preview(transform);
  // A rejected value can leave the native gesture and its last preview alive.
  // Preserve both its publication baseline and Escape ownership until detach.
  if (result.status == ContinuousStatus::rejected && m_gizmoDragging &&
      session()->editor().continuousEdit().active() &&
      m_beforeMutationProjection) {
    return rejected(result.error, result.message);
  }
  if (result.status != ContinuousStatus::rejected) {
    m_gizmoHovered = true;
    m_gizmoDragging = true;
    ++m_lifecycleRevision;
    notifyLifecycle();
  }
  RuntimeActionResult published =
      publishContinuousPreview(ProjectionRefresh::properties, result);
  if (!published.accepted && m_gizmoDragging) {
    m_gizmoDragging = false;
    ++m_lifecycleRevision;
    notifyLifecycle();
  }
  return published;
}

RuntimeActionResult EditorController::finishGizmoTransform(
    NodeId node, bool physicalRelease, bool escapePressed, bool focusLost) {
  if (!hasDocument()) return unsupported("No active document");
  if (!node.valid() || node.authority() != session()->document().documentAuthority()) {
    return rejected(MutationErrorCode::staleId,
                    "Gizmo node selection is stale");
  }

  if (m_gizmoFinishSequence == std::numeric_limits<std::uint64_t>::max()) {
    return rejected(MutationErrorCode::staleTransaction,
                    "Gizmo terminal sequence is exhausted");
  }
  ContinuousFrameSignals signals;
  signals.applicationFrame = ++m_gizmoFinishSequence;
  signals.physicalReleaseDelivered = physicalRelease;
  signals.escapePressed = escapePressed;
  signals.focusLost = focusLost;
  signals.windowFocused = !focusLost;
  signals.selectionDetached = escapePressed;
  const ContinuousResult result =
      session()->editor().continuousEdit().finishFrame(signals);

  if (result.status == ContinuousStatus::rejected) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    m_gizmoDragging = false;
    ++m_lifecycleRevision;
    notifyLifecycle();
    return rejected(result.error, result.message);
  }

  if (result.status == ContinuousStatus::suspended) {
    m_gizmoDragging = true;
    m_gizmoHovered = true;
    ++m_lifecycleRevision;
    notifyLifecycle();
    return accepted();
  }

  // A physical release observed after focus loss is not a delivered commit.
  // The continuous session remains suspended, so keep the native Gizmo owner
  // and its publication baseline until an explicit detach or a real commit.
  if (result.status == ContinuousStatus::noOp &&
      result.state == ContinuousEditState::suspendedUnfocused &&
      session()->editor().continuousEdit().active()) {
    // Reassert ownership from the session state rather than trusting the
    // host-side visual flag.  A polling/adapter edge may have cleared that
    // flag while the suspended command is still the sole active gesture.
    const bool ownershipChanged = !m_gizmoDragging || !m_gizmoHovered;
    m_gizmoDragging = true;
    m_gizmoHovered = true;
    if (ownershipChanged) {
      ++m_lifecycleRevision;
      notifyLifecycle();
    }
    return accepted();
  }

  if (escapePressed) {
    // Escape intentionally keeps the last preview in the document, drops the
    // prepared history command, and clears selection without a transform
    // history entry.
    m_beforeMutationAnimation.reset();
    m_beforeMutationProjection.reset();
    m_gizmoDragging = false;
    ++m_lifecycleRevision;
    notifyLifecycle();
    emitProjection(ProjectionRefresh::selectionOnly);
    return accepted(ProjectionRefresh::selectionOnly);
  }

  if (physicalRelease || result.status == ContinuousStatus::committed ||
      result.status == ContinuousStatus::abandonedRetainingPreview) {
    m_beforeMutationAnimation.reset();
    m_beforeMutationProjection.reset();
    m_gizmoDragging = false;
    ++m_lifecycleRevision;
    notifyLifecycle();
  }
  return accepted(ProjectionRefresh::properties);
}

RuntimeActionResult EditorController::publishAnimationProjection(
    bool animationLoopOverride) {
  return publishAnimationProjectionAt(m_animationTime, animationLoopOverride);
}

RuntimeActionResult EditorController::publishAnimationProjectionAt(
    double requestedTime, bool animationLoopOverride) {
  if (!hasDocument() || !m_currentAnimation)
    return rejected(MutationErrorCode::invalidReference,
                    "No active animation is selected");
  const auto index = session()->document().indexOf(
      m_currentAnimation->untyped());
  if (!index)
    return rejected(MutationErrorCode::staleId,
                    "Animation selection is stale");

  const std::optional<SceneAnimationPose> previousPose = m_animationPose;
  const double previousTime = m_animationTime;
  const bool previousLoop = m_animationLoop;
  const AnimationSampleResult sample = sampleAnimation(
      session()->document(), *index, requestedTime, animationLoopOverride);
  if (!sample.succeeded()) {
    m_animationPose = previousPose;
    m_animationTime = previousTime;
    m_animationLoop = previousLoop;
    return rejected(MutationErrorCode::unsupportedKind,
                    "Animation sampling failed; runtime pose was not published");
  }
  m_animationTime = sample.normalizedTime;
  m_animationPose = SceneAnimationPose{
      static_cast<std::uint32_t>(*index), sample.normalizedTime,
      animationLoopOverride, sample.transforms};

  std::string error;
  if (!rebuildProjection(ProjectionRefresh::fullScene, &error, false)) {
    m_animationPose = previousPose;
    m_animationTime = previousTime;
    m_animationLoop = previousLoop;
    (void)rebuildProjection(ProjectionRefresh::fullScene, nullptr, false);
    return rejected(MutationErrorCode::internalError,
                    error.empty() ? "Failed to publish animation pose"
                                   : std::move(error));
  }
  emitProjection(ProjectionRefresh::fullScene);
  return accepted(ProjectionRefresh::fullScene);
}

RuntimeActionResult EditorController::updateAnimation(double deltaSeconds) {
  if (!hasDocument() || !m_currentAnimation || !m_animationPlaying)
    return accepted();
  const auto index = session()->document().indexOf(
      m_currentAnimation->untyped());
  if (!index)
    return rejected(MutationErrorCode::staleId,
                    "Animation selection is stale");
  const AnimationClipAnalysis analysis =
      analyzeAnimation(session()->document(), *index);
  if (!analysis.succeeded()) {
    m_animationPlaying = false;
    m_animationPose.reset();
    return rejected(MutationErrorCode::unsupportedKind,
                    "Animation runtime is unavailable for this clip");
  }
  AnimationPlaybackState state{m_animationTime, m_animationSpeed, true,
                               m_animationLoop};
  const AnimationAdvanceResult advanced =
      advanceAnimationPlayback(state, deltaSeconds, analysis.info);
  if (!advanced.succeeded()) {
    m_animationPlaying = false;
    return rejected(MutationErrorCode::invalidReference,
                    "Animation playback could not advance");
  }
  if (state.time == m_animationTime && m_animationPose)
    return accepted();
  return publishAnimationProjectionAt(state.time, m_animationLoop);
}

RuntimeActionResult EditorController::sceneTransform(const SceneTransformAction &command) {
  if (!hasDocument()) return unsupported("No active document");
  captureBeforeMutation();
  const MutationResult result = command.reset
                                   ? session()->editor().commands().resetSceneRootTransform(command.scene)
                                   : session()->editor().commands().previewSceneRootTransform(command.scene, command.transform);
  return publishMutation(ProjectionRefresh::properties, result);
}

RuntimeActionResult EditorController::publishMutation(
    ProjectionRefresh refresh, const MutationResult &result) {
  if (result.rejected()) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(result.error, result.message);
  }
  if (result.domainNoop()) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return accepted();
  }
  if (!m_beforeMutationProjection) {
    // This can only happen for a caller that mutated the document outside the
    // controller. Refuse to publish an unbound renderer update.
    return rejected(MutationErrorCode::staleTransaction,
                    "mutation has no before-projection capture");
  }
  const SceneProjectionResult &before = *m_beforeMutationProjection;
  if (refresh == ProjectionRefresh::topology) {
    (void)session()->editor().selection().revalidate(session()->document());
  }
  if (!rebuildProjection(refresh, nullptr, false, std::nullopt, &result.changes)) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to rebuild editor projection");
  }
  std::optional<SceneCanonicalizationResult> canonical;
  if (m_projection) {
    if (result.sceneActivationTransition) {
      canonical = SceneChangeCapture::finalizeActivation(
          captureSnapshot(before), captureSnapshot(*m_projection),
          result.sceneActivationTransition->targetEpoch);
    } else if (result.rootStateTransition) {
      const auto toValue = [](const SceneRootTransform &value) {
        SceneNodeTransformValue result;
        result.value = SceneTrsValue{value.translation, value.rotation, value.scale};
        return result;
      };
      canonical = SceneChangeCapture::finalizeRootState(
          captureSnapshot(before), captureSnapshot(*m_projection),
          toValue(result.rootStateTransition->before),
          toValue(result.rootStateTransition->after),
          result.rootStateTransition->targetExternalStateSerial);
    } else {
      canonical = canonicalizeMutation(
          before, *m_projection, result.changes,
          result.changes.structuralEvidenceComplete
              ? std::nullopt
              : result.fullSnapshotReason);
    }
  }
  if (!canonical || !canonical->succeeded()) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to canonicalize editor mutation");
  }
  emitProjection(refresh, std::move(canonical));
  m_beforeMutationAnimation.reset();
  m_beforeMutationProjection.reset();
  return accepted(refresh);
}

RuntimeActionResult EditorController::publishCommand(ProjectionRefresh refresh,
                                                      const CommandResult &result) {
  if (!result.accepted()) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(result.error, result.message);
  }
  if (result.status == CommandStatus::unchanged ||
      result.status == CommandStatus::historyEnvelopeNoop) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return accepted();
  }
  if (!m_beforeMutationProjection) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    return rejected(MutationErrorCode::staleTransaction,
                    "command has no before-projection capture");
  }
  const SceneProjectionResult &before = *m_beforeMutationProjection;
  if (m_nodeNameTransaction) {
    NodeNameTransactionHint &hint = *m_nodeNameTransaction;
    const std::string_view currentName =
        session()->document().objectName(hint.node.untyped());
    detail::NodeNameProjectionDigestCache *cache = nullptr;
    if (currentName == hint.beforeName) {
      cache = &hint.before;
    } else if (currentName == hint.afterName) {
      cache = &hint.after;
    }
    if (cache == nullptr ||
        !seedNodeNameProjectionCache(*cache, result.changes,
                                     session()->document(), before)) {
      m_nodeNameTransaction.reset();
    }
  }
  if (!rebuildProjection(refresh, nullptr, false, std::nullopt, &result.changes)) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to rebuild editor projection");
  }
  std::optional<SceneCanonicalizationResult> canonical;
  if (m_projection) {
    if (result.sceneActivationTransition) {
      canonical = SceneChangeCapture::finalizeActivation(
          captureSnapshot(before), captureSnapshot(*m_projection),
          result.sceneActivationTransition->targetEpoch);
    } else if (result.rootStateTransition) {
      const auto toValue = [](const SceneRootTransform &value) {
        SceneNodeTransformValue result;
        result.value = SceneTrsValue{value.translation, value.rotation, value.scale};
        return result;
      };
      canonical = SceneChangeCapture::finalizeRootState(
          captureSnapshot(before), captureSnapshot(*m_projection),
          toValue(result.rootStateTransition->before),
          toValue(result.rootStateTransition->after),
          result.rootStateTransition->targetExternalStateSerial);
    } else {
      canonical = canonicalizeMutation(
          before, *m_projection, result.changes,
          result.changes.structuralEvidenceComplete
              ? std::nullopt
              : result.fullSnapshotReason);
    }
  }
  if (!canonical || !canonical->succeeded()) {
    m_nodeNameTransaction.reset();
    m_nodeNameFastPathUsed = false;
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to canonicalize editor command");
  }
  emitProjection(refresh, std::move(canonical));
  m_beforeMutationAnimation.reset();
  m_beforeMutationProjection.reset();
  return accepted(refresh);
}

RuntimeActionResult EditorController::publishContinuousPreview(
    ProjectionRefresh refresh, const ContinuousResult &result) {
  if (result.status == ContinuousStatus::rejected) {
    // A rejected input leaves the continuous session and its last valid
    // preview alive. Keep their capture so a subsequent valid input can publish.
    if (!session() || !session()->editor().continuousEdit().active()) {
      restoreBeforeMutationAnimation();
      m_beforeMutationProjection.reset();
    }
    return rejected(result.error, result.message);
  }
  if (!result.previewCommand) {
    return accepted();
  }
  if (!m_beforeMutationProjection) {
    return rejected(MutationErrorCode::staleTransaction,
                    "continuous preview has no before-projection capture");
  }
  const SceneCaptureSnapshot before = captureSnapshot(
      m_projection ? *m_projection : *m_beforeMutationProjection, false);
  if (!rebuildProjection(refresh, nullptr, false, std::nullopt,
                         &result.previewCommand->changes) || !m_projection) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to rebuild continuous editor projection");
  }
  const std::optional<SceneCanonicalizationResult> canonical =
      canonicalizeMutation(
          before, *m_projection, result.previewCommand->changes,
          result.previewCommand->changes.structuralEvidenceComplete
              ? std::nullopt
              : result.previewCommand->fullSnapshotReason);
  if (!canonical || !canonical->succeeded()) {
    restoreBeforeMutationAnimation();
    m_beforeMutationProjection.reset();
    return rejected(MutationErrorCode::internalError,
                    "failed to canonicalize continuous preview");
  }
  emitProjection(refresh, *canonical);
  return accepted(refresh);
}

bool EditorController::rebuildProjection(ProjectionRefresh refresh,
                                          std::string *error, bool publish,
                                          std::optional<bool> animationLoopOverride,
                                          const SceneMutationEvidence* evidence) {
  if (!session()) {
    const std::string message = "No active editor session";
    if (error) *error = message;
    setLifecycleError(message);
    return false;
  }
  SceneProjectionOptions options;
  options.baseMappingEpoch = m_projection ? m_projection->mappingEpoch : 0u;
  options.activeVariant = m_currentVariant;
  if (m_animationPose) {
    options.activeAnimation = m_animationPose->animationIndex;
    options.animationTime = m_animationPose->time;
    options.animationLoop = animationLoopOverride.value_or(
        m_animationPose->loop);
    options.animationPose = m_animationPose;
    options.animationPose->loop = options.animationLoop;
  }

  m_nodeNameFastPathUsed = false;
  if (m_projection && m_nodeNameTransaction) {
    NodeNameTransactionHint &hint = *m_nodeNameTransaction;
    const std::string_view currentName =
        session()->document().objectName(hint.node.untyped());
    detail::NodeNameProjectionDigestCache *cache = nullptr;
    if (currentName == hint.beforeName) {
      cache = &hint.before;
    } else if (currentName == hint.afterName) {
      cache = &hint.after;
    }
    if (cache != nullptr) {
      std::optional<SceneProjectionResult> optimized =
          detail::reprojectNodeNameOnly(session()->document(), *m_projection,
                                        hint.node, options, *cache);
      if (optimized && optimized->succeeded()) {
        cache->documentDigest = optimized->documentDigest;
        cache->semanticHash = optimized->semanticHash;
        m_projection = std::move(*optimized);
        m_nodeNameFastPathUsed = true;
        if (publish) {
          emitProjection(refresh);
        }
        return true;
      }
    }
  }

  if (m_projection && evidence) {
    if (auto next = detail::reprojectMaterialParameters(session()->document(),
                                                        *m_projection, *evidence, options)) {
      m_projection = std::move(*next);
      if (publish) emitProjection(refresh);
      return true;
    }
  }
  SceneProjectionResult next = SceneProjection::project(session()->document(), options);
  if (!next.succeeded()) {
    if (error) *error = next.error;
    setLifecycleError(next.error);
    return false;
  }
  m_projection = std::move(next);
  // A full projection is authoritative but means the retained rename hint was
  // not proven safe for this transition; require a fresh hint for later edits.
  m_nodeNameTransaction.reset();
  if (publish) {
    emitProjection(refresh);
  }
  return true;
}

void EditorController::refreshProjection(ProjectionRefresh refresh) {
  std::string error;
  (void)rebuildProjection(refresh, &error);
}

bool EditorController::saveAs(const std::filesystem::path &path,
                               std::string *error) {
  if (!session()) {
    if (error) *error = "No active document";
    return false;
  }
  LifecycleOperationFrame lifecycle = beginLifecycleOperation(
      LifecycleState::savingMergingCompacting, LifecycleOperation::save,
      "Saving scene");
  const auto fail = [&](std::string message) {
    if (message.empty()) message = "Scene save failed";
    if (error) *error = message;
    finishLifecycleOperation(lifecycle, false, std::move(message));
    return false;
  };
  try {
    const GltfSaveResult result =
        GltfDocumentIO::saveAs(session()->document(), path);
    if (!result.succeeded())
      return fail(result.message().empty() ? "Scene save failed"
                                            : result.message());
    rememberRecentFile(path);
    applyHistoryLifecycle(session()->editor().history(),
                          HistoryLifecycleEvent::saveAs,
                          LifecycleCommitState::changed);
    finishLifecycleOperation(lifecycle, true);
    return true;
  } catch (...) {
    return fail("Unexpected exception while saving scene");
  }
}

void EditorController::rememberRecentFile(const std::filesystem::path &path) {
  if (path.empty())
    return;
  const std::filesystem::path normalized = path.lexically_normal();
  m_recentFiles.erase(
      std::remove_if(m_recentFiles.begin(), m_recentFiles.end(),
                     [&](const RecentFile &entry) {
                       return entry.path.lexically_normal() == normalized;
                     }),
      m_recentFiles.end());
  m_recentFiles.insert(m_recentFiles.begin(),
                       RecentFile{normalized, [&] {
                         std::error_code error;
                         return std::filesystem::exists(normalized, error);
                       }()});
  constexpr std::size_t kMaximumRecentFiles = 12u;
  if (m_recentFiles.size() > kMaximumRecentFiles)
    m_recentFiles.resize(kMaximumRecentFiles);
}

void EditorController::resetRuntimeControls() noexcept {
  m_beforeMutationAnimation.reset();
  m_nodeNameTransaction.reset();
  m_nodeNameFastPathUsed = false;
  m_currentAnimation.reset();
  m_animationPlaying = true;
  m_animationSpeed = 1.0;
  m_animationTime = 0.0;
  m_animationLoop = true;
  m_animationPose.reset();
}

RuntimeActionResult EditorController::unsupported(std::string message) {
  if (m_callbacks.onError) m_callbacks.onError(message);
  return rejected(MutationErrorCode::unsupportedKind, std::move(message));
}

bool EditorController::canReuseFrameModelCache() const {
  if (!m_frameModelCache || !hasDocument() || m_currentAnimation ||
      m_animationPose) {
    return false;
  }

  const EditorSessionBundle *active = session();
  if (active == nullptr)
    return false;
  const GltfDocument &document = active->document();
  const EditorSelectionSnapshot selection =
      active->editor().selection().snapshot();
  const SceneProjectionResult *projection =
      m_projection ? &*m_projection : nullptr;
  const std::uint64_t mappingEpoch =
      projection != nullptr ? projection->mappingEpoch : 0u;
  return m_frameModelCache->revision == document.revision() &&
         m_frameModelCache->selectionRevision == selectionRevision() &&
         m_frameModelCache->selection == selection &&
         m_frameModelCacheVariant == m_currentVariant &&
         m_frameModelCacheProjection == projection &&
         m_frameModelCacheMappingEpoch == mappingEpoch;
}

void EditorController::refreshFrameModelTransientState(
    FrameModel &model, LayoutState layout) const {
  model.layout = layout;
  model.hasDocument = hasDocument();
  model.rendererBusy = false;
  bool rendererPublicationFailed = false;
  if (m_callbacks.rendererPublicationFailed) {
    try {
      rendererPublicationFailed = m_callbacks.rendererPublicationFailed();
    } catch (...) {
      rendererPublicationFailed = true;
    }
  }
  if (m_callbacks.rendererBusy) {
    try {
      model.rendererBusy = m_callbacks.rendererBusy();
    } catch (...) {
      model.rendererBusy = true;
    }
  }
  model.debugUiAvailable = false;
#if !defined(NDEBUG)
  model.debugUiAvailable = true;
#endif
  model.lifecycle =
      computedLifecycleState(rendererPublicationFailed, model.rendererBusy);
  model.lifecycleRevision = m_lifecycleRevision;
  model.lifecycleOverridden = m_lifecycleOverride.has_value();
  model.modalPending = m_ui.modalPending() ||
                       model.lifecycle == LifecycleState::modal;
  model.operation = operationState();
  if (model.lifecycle == LifecycleState::error &&
      model.operation.error.empty()) {
    model.operation.error = "Renderer publication failed";
    model.operation.dismissibleError = true;
  }
  model.mutationEnabled =
      model.hasDocument && !model.rendererBusy &&
      isLifecycleDocumentReady(model.lifecycle);
  model.recentFiles.clear();
  model.recentFiles.reserve(m_recentFiles.size());
  for (const RecentFile &recent : m_recentFiles) {
    std::error_code existsError;
    model.recentFiles.push_back(
        RecentFile{recent.path, std::filesystem::exists(recent.path,
                                                       existsError)});
  }
  model.selection = model.hasDocument
                        ? session()->editor().selection().snapshot()
                        : EditorSelectionSnapshot::empty();
  model.selectionRevision = selectionRevision();
  model.viewportHovered = m_viewportHovered;
  model.viewportFocused = m_viewportFocused;
  model.viewportOwnsEscape = m_gizmoDragging;

  if (!model.hasDocument)
    return;

  const GltfDocument &document = session()->document();
  const SceneEditor &editor = session()->editor();
  model.revision = document.revision();
  model.cleanRevision = document.cleanRevision();
  model.canUndo = editor.history().canUndo();
  model.canRedo = editor.history().canRedo();
  model.undoDescription.assign(editor.history().undoDescription());
  model.redoDescription.assign(editor.history().redoDescription());
  model.canFitSelection =
      std::holds_alternative<NodeSelection>(model.selection.value) ||
      std::holds_alternative<PrimitiveSelection>(model.selection.value);
  model.inspector.selection = model.selection;
  model.inspector.selectionRevision = model.selectionRevision;
  model.browser.animation.playing = m_animationPlaying;
  model.browser.animation.speed = m_animationSpeed;
}

FrameModel EditorController::buildFrameModel(LayoutState layout) const {
  FrameModel model;
  refreshFrameModelTransientState(model, layout);
  if (!model.hasDocument) {
    return model;
  }

  const GltfDocument &document = session()->document();

  // Public snapshot callers still receive an owning value. During a material
  // preview, copy the stable cache rather than walking the entire document.
  const bool colorPreviewActive = m_ui.hasMaterialEditPreview();
  if (colorPreviewActive && canReuseFrameModelCache()) {
    FrameModel cached = *m_frameModelCache;
    refreshFrameModelTransientState(cached, layout);
    return cached;
  }

  const SceneEditor &editor = session()->editor();
  const SceneCommands &commands = editor.commands();

  model.scenes.reserve(document.objectCount(EditorObjectKind::scene));
  for (std::size_t index = 0; index < document.objectCount(EditorObjectKind::scene);
       ++index) {
    const SceneId scene = SceneId::from(
                              document.idAt(EditorObjectKind::scene, index))
                              .value_or(SceneId{});
    if (!scene.valid()) continue;
    model.scenes.push_back(SceneChoice{
        scene, static_cast<std::uint32_t>(index),
        std::string(document.objectName(scene.untyped())),
        document.currentSceneId() == scene,
        document.sceneRootTransform(scene)});
  }

  // Asset Info is a read-only projection of the authored glTF asset header.
  // Internal revision/clean-state counters stay in FrameModel and are never
  // rendered as user metadata (the reference UI has no dirty badge here).
  model.browser.asset.version = std::string(document.assetVersion());
  model.browser.asset.minimumVersion = std::string(document.assetMinVersion());
  model.browser.asset.generator = std::string(document.assetGenerator());
  model.browser.asset.copyright = std::string(document.assetCopyright());
  model.browser.asset.extensions = document.documentExtensionNames();
  for (const std::string &extension : document.assetExtensionNames()) {
    if (std::find(model.browser.asset.extensions.begin(),
                  model.browser.asset.extensions.end(), extension) ==
        model.browser.asset.extensions.end())
      model.browser.asset.extensions.push_back(extension);
  }
  if (!document.assetExtrasJson().empty())
    model.browser.asset.metadata.push_back(
        AssetField{"Asset extras", std::string(document.assetExtrasJson())});
  if (!document.documentExtrasJson().empty())
    model.browser.asset.metadata.push_back(
        AssetField{"Document extras", std::string(document.documentExtrasJson())});

  model.materials.reserve(document.objectCount(EditorObjectKind::material));
  for (std::size_t index = 0;
       index < document.objectCount(EditorObjectKind::material); ++index) {
    const MaterialId id = MaterialId::from(
                              document.idAt(EditorObjectKind::material, index))
                              .value_or(MaterialId{});
    if (id.valid()) {
      model.materials.push_back(MaterialChoice{
          id, static_cast<std::uint32_t>(index),
          std::string(document.objectName(id.untyped()))});
    }
  }

  using AttachmentMap =
      std::unordered_map<EditorObjectId, std::vector<EditorObjectId>,
                         EditorObjectIdHash>;
  AttachmentMap camerasByNode;
  AttachmentMap lightsByNode;
  std::unordered_map<EditorObjectId, NodeId, EditorObjectIdHash>
      attachmentNodes;
  std::unordered_map<EditorObjectId, NodeSelection, EditorObjectIdHash>
      firstNodeOccurrences;
  std::unordered_map<EditorObjectId, std::vector<PrimitiveId>,
                     EditorObjectIdHash>
      meshPrimitiveCache;
  const std::size_t nodeCount =
      document.objectCount(EditorObjectKind::node);
  const std::size_t cameraCount =
      document.objectCount(EditorObjectKind::camera);
  const std::size_t lightCount =
      document.objectCount(EditorObjectKind::light);
  camerasByNode.reserve(cameraCount);
  lightsByNode.reserve(lightCount);
  attachmentNodes.reserve(cameraCount + lightCount);
  firstNodeOccurrences.reserve(nodeCount);
  meshPrimitiveCache.reserve(document.objectCount(EditorObjectKind::mesh));
  const auto meshPrimitives = [&](MeshId mesh)
      -> const std::vector<PrimitiveId> & {
    auto [entry, inserted] =
        meshPrimitiveCache.try_emplace(mesh.untyped());
    if (inserted)
      entry->second = commands.meshPrimitives(mesh);
    return entry->second;
  };

  const auto displayNodeSelection = [&](NodeId node) {
    SceneId scene = document.currentSceneId();
    if (!scene.valid() || !document.hasNodeOccurrence(scene, node, std::nullopt)) {
      scene = SceneId{};
      for (const SceneChoice &choice : model.scenes) {
        if (document.hasNodeOccurrence(choice.id, node, std::nullopt)) {
          scene = choice.id;
          break;
        }
      }
    }
    std::vector<NodeId> parents;
    if (scene.valid())
      parents = document.sceneNodeParentPath(scene, node);
    return NodeSelection{scene, node,
                         parents.empty()
                             ? std::nullopt
                             : std::optional<NodeId>{parents.back()}};
  };

  if (m_projection) {
    const SceneAsset &asset = m_projection->asset;
    const SceneRuntimeMapping &mapping = m_projection->mapping;
    model.browser.variants.reserve(asset.materialVariantNames.size());
    for (std::size_t index = 0; index < asset.materialVariantNames.size();
         ++index) {
      model.browser.variants.push_back(
          VariantItem{static_cast<std::uint32_t>(index),
                      asset.materialVariantNames[index],
                      m_currentVariant &&
                          *m_currentVariant == static_cast<std::uint32_t>(index),
                      true});
    }

    model.browser.animation.items.reserve(
        document.objectCount(EditorObjectKind::animation));
    for (std::size_t index = 0;
         index < document.objectCount(EditorObjectKind::animation); ++index) {
      const auto id = AnimationId::from(
          document.idAt(EditorObjectKind::animation, index));
      if (!id)
        continue;
      std::string name(document.objectName(id->untyped()));
      AnimationClipInfo clipInfo{};
      AnimationSampleStatus clipStatus = AnimationSampleStatus::invalidClip;
      if (const auto projected = mapping.sceneAssetIndex(id->untyped());
          projected && *projected < asset.animations.size() && name.empty())
        name = asset.animations[*projected].name;
      const AnimationClipAnalysis analysis =
          analyzeAnimation(document, index);
      clipStatus = analysis.status;
      if (analysis.succeeded())
        clipInfo = analysis.info;
      model.browser.animation.items.push_back(AnimationItem{
          *id, static_cast<std::uint32_t>(index), std::move(name),
          clipInfo.startTime, clipInfo.endTime,
          m_currentAnimation && *m_currentAnimation == *id
              ? m_animationTime
              : clipInfo.startTime});
      if (m_currentAnimation && *m_currentAnimation == *id &&
          clipStatus != AnimationSampleStatus::success) {
        model.operation.error = "Selected animation is unavailable";
      }
    }
    if (m_currentAnimation &&
        document.contains(m_currentAnimation->untyped()))
      model.browser.animation.current = m_currentAnimation;
    else if (!model.browser.animation.items.empty())
      model.browser.animation.current = model.browser.animation.items.front().id;
    model.browser.animation.playing = m_animationPlaying;
    model.browser.animation.speed = m_animationSpeed;
    model.browser.animation.runtimeControlsAvailable = false;
    if (model.browser.animation.current) {
      const auto currentIndex = document.indexOf(
          model.browser.animation.current->untyped());
      if (currentIndex) {
        model.browser.animation.runtimeControlsAvailable =
            analyzeAnimation(document, *currentIndex).succeeded();
      }
    }

    // Build attachment metadata from the document authority, rather than the
    // active SceneAsset projection. This keeps non-current scene rows and
    // inspector contexts visible while preserving typed/stable IDs.
    for (std::size_t index = 0;
         index < document.objectCount(EditorObjectKind::camera); ++index) {
      const auto object = CameraId::from(
          document.idAt(EditorObjectKind::camera, index));
      if (!object) continue;
      const auto node = commands.cameraNode(*object);
      if (!node) continue;
      camerasByNode[node->untyped()].push_back(object->untyped());
      attachmentNodes.emplace(object->untyped(), *node);
    }
    for (std::size_t index = 0;
         index < document.objectCount(EditorObjectKind::light); ++index) {
      const auto object = LightId::from(
          document.idAt(EditorObjectKind::light, index));
      if (!object) continue;
      const auto node = commands.lightNode(*object);
      if (!node) continue;
      lightsByNode[node->untyped()].push_back(object->untyped());
      attachmentNodes.emplace(object->untyped(), *node);
    }

    BrowserDebugState &debug = model.browser.debug;
    debug.documentNodeCount =
        document.objectCount(EditorObjectKind::node);
    debug.meshCount = document.objectCount(EditorObjectKind::mesh);
    debug.renderNodeCount = asset.meshes.size();
    debug.renderPrimitiveCount = asset.primitives.size();
    debug.nodesWithMeshes = static_cast<std::uint64_t>(std::count_if(
        asset.nodes.begin(), asset.nodes.end(),
        [](const SceneNode &node) { return !node.meshRefs.empty(); }));
    for (const SceneNode &node : asset.nodes)
      debug.expectedRenderNodeCount += node.meshRefs.size();
    debug.rows.reserve(asset.meshes.size());
    for (std::size_t index = 0; index < asset.meshes.size(); ++index) {
      const SceneMesh &mesh = asset.meshes[index];
      DebugRenderRow row;
      row.renderNode = static_cast<std::uint32_t>(index);
      row.primitive = mesh.sourcePrimitiveIndex;
      row.valid = mesh.sourceNodeIndex <
                  document.objectCount(EditorObjectKind::node);
      if (row.valid) {
        const EditorObjectId node = document.idAt(
            EditorObjectKind::node, mesh.sourceNodeIndex);
        row.documentNode = static_cast<std::uint32_t>(mesh.sourceNodeIndex);
        row.nodeName = std::string(document.objectName(node));
      }
      if (mesh.materialIndex < asset.materials.size()) {
        if (const auto material = mapping.documentIdAt(
                EditorObjectKind::material, mesh.materialIndex)) {
          if (const auto materialIndex = document.indexOf(*material))
            row.material = static_cast<std::uint32_t>(*materialIndex);
        }
      }
      debug.rows.push_back(std::move(row));
    }
  }

  struct GraphRowOccurrenceDomain final {
    SceneId scene;
    EditorObjectId object;
    std::optional<NodeId> directParent;

    bool operator==(const GraphRowOccurrenceDomain &) const = default;
  };
  struct GraphRowOccurrenceDomainHash final {
    [[nodiscard]] std::size_t
    operator()(const GraphRowOccurrenceDomain &value) const noexcept {
      const EditorObjectIdHash hash;
      std::size_t result = hash(value.scene);
      const auto combine = [&result](std::size_t next) {
        result ^= next + static_cast<std::size_t>(0x9e3779b9u) +
                  (result << 6u) + (result >> 2u);
      };
      combine(hash(value.object));
      combine(value.directParent ? hash(*value.directParent)
                                 : static_cast<std::size_t>(0u));
      return result;
    }
  };
  const std::size_t estimatedGraphRowCount =
      model.scenes.size() + nodeCount +
      document.objectCount(EditorObjectKind::mesh) +
      document.objectCount(EditorObjectKind::primitive) +
      document.objectCount(EditorObjectKind::skin) + cameraCount + lightCount;
  model.browser.graphRows.reserve(estimatedGraphRowCount);
  std::unordered_map<GraphRowOccurrenceDomain, std::uint32_t,
                     GraphRowOccurrenceDomainHash>
      rowOccurrences;
  rowOccurrences.reserve(estimatedGraphRowCount);
  const auto nextOccurrence = [&](SceneId scene, EditorObjectId object,
                                  std::optional<NodeId> directParent) {
    auto [entry, inserted] = rowOccurrences.try_emplace(
        GraphRowOccurrenceDomain{scene, object, directParent}, 0u);
    (void)inserted;
    return entry->second++;
  };
  std::unordered_set<EditorObjectId, EditorObjectIdHash> traversalPath;
  struct TraversalFrame final {
    SceneId scene;
    NodeId node;
    std::optional<NodeId> parent;
    std::uint32_t depth{0};
    bool ancestorsVisible{true};
    bool exit{false};
  };
  constexpr std::uint32_t kMaximumTraversalDepth = 2048u;
  constexpr std::size_t kMaximumTraversalOccurrences = 200000u;
  const auto appendNodeOccurrence = [&](SceneId scene, NodeId node,
                                        std::optional<NodeId> parent,
                                        std::uint32_t depth,
                                        bool ancestorsVisible) {
    const std::vector<NodeId> children = document.nodeChildIds(node);
    const std::optional<MeshId> mesh = commands.nodeMesh(node);
    const auto cameraIt = camerasByNode.find(node.untyped());
    const auto lightIt = lightsByNode.find(node.untyped());
    const bool hasCameras = cameraIt != camerasByNode.end() &&
                            !cameraIt->second.empty();
    const bool hasLights = lightIt != lightsByNode.end() &&
                           !lightIt->second.empty();
    GraphRow row;
    row.key = GraphRowKey{
        scene, node.untyped(), parent,
        nextOccurrence(scene, node.untyped(), parent)};
    row.kind = GraphRowKind::node;
    row.documentIndex = static_cast<std::uint32_t>(
        document.indexOf(node.untyped()).value_or(0u));
    row.depth = depth;
    row.label = std::string(document.objectName(node.untyped()));
    row.secondary = "Node";
    row.hasChildren = !children.empty() || mesh || hasCameras || hasLights;
    row.defaultExpanded = depth <= 1u;
    row.selectable = true;
    row.enabled = true;
    const bool nodeVisible =
        ancestorsVisible && document.objectVisible(node.untyped());
    row.effectiveVisible = nodeVisible;
    row.acceptsNodeDrop = true;
    row.selection = EditorSelectionSnapshot{
        document.documentAuthority(), NodeSelection{scene, node, parent}};
    row.selected = row.selection == model.selection;
    firstNodeOccurrences.try_emplace(
        node.untyped(), NodeSelection{scene, node, parent});
    model.browser.graphRows.push_back(std::move(row));

    if (mesh) {
      const std::vector<PrimitiveId> &primitives = meshPrimitives(*mesh);
      GraphRow meshRow;
      meshRow.key = GraphRowKey{
          scene, mesh->untyped(), node,
          nextOccurrence(scene, mesh->untyped(), node)};
      meshRow.kind = GraphRowKind::mesh;
      meshRow.documentIndex = static_cast<std::uint32_t>(
          document.indexOf(mesh->untyped()).value_or(0u));
      meshRow.depth = depth + 1u;
      meshRow.label = std::string(document.objectName(mesh->untyped()));
      meshRow.secondary = std::to_string(primitives.size()) +
                          (primitives.size() == 1u ? " primitive"
                                                   : " primitives");
      meshRow.hasChildren = !primitives.empty();
      meshRow.defaultExpanded = true;
      meshRow.selectable = true;
      meshRow.effectiveVisible = nodeVisible;
      meshRow.selection = EditorSelectionSnapshot{
          document.documentAuthority(), *mesh};
      meshRow.selected = meshRow.selection == model.selection;
      model.browser.graphRows.push_back(std::move(meshRow));
      for (const PrimitiveId primitive : primitives) {
        GraphRow primitiveRow;
        primitiveRow.key = GraphRowKey{
            scene, primitive.untyped(), node,
            nextOccurrence(scene, primitive.untyped(), node)};
        primitiveRow.kind = GraphRowKind::primitive;
        if (const auto address = document.primitiveAddress(primitive))
          primitiveRow.documentIndex = address->primitiveIndex;
        primitiveRow.depth = depth + 2u;
        primitiveRow.label = "Primitive";
        if (const auto material = commands.primitiveMaterial(primitive)) {
          const auto materialIndex = document.indexOf(material->untyped());
          primitiveRow.secondary =
              "Material " + std::to_string(materialIndex.value_or(0u));
        } else {
          primitiveRow.secondary = "No material";
        }
        primitiveRow.selectable = true;
        primitiveRow.effectiveVisible =
            document.objectVisible(primitive.untyped()) && nodeVisible;
        primitiveRow.selection = EditorSelectionSnapshot{
            document.documentAuthority(),
            PrimitiveSelection{scene, node, primitive, *mesh}};
        primitiveRow.selected = primitiveRow.selection == model.selection;
        model.browser.graphRows.push_back(std::move(primitiveRow));
      }
    }

    // Skins are document resources attached to nodes.  Keep them visible in
    // the hierarchy even though the current selection value intentionally has
    // no mutable SkinId variant; the Inspector exposes the attachment as a
    // read-only field on the owning node.
    if (const auto skin = commands.nodeSkin(node)) {
      GraphRow skinRow;
      skinRow.key = GraphRowKey{
          scene, skin->untyped(), node,
          nextOccurrence(scene, skin->untyped(), node)};
      skinRow.kind = GraphRowKind::skin;
      skinRow.documentIndex = static_cast<std::uint32_t>(
          document.indexOf(skin->untyped()).value_or(0u));
      skinRow.depth = depth + 1u;
      skinRow.label = std::string(document.objectName(skin->untyped()));
      skinRow.secondary = "Skin";
      skinRow.selectable = false;
      skinRow.effectiveVisible = nodeVisible;
      model.browser.graphRows.push_back(std::move(skinRow));
    }

    const auto appendAttachedRows = [&](const AttachmentMap &attachments,
                                        GraphRowKind kind) {
      const auto found = attachments.find(node.untyped());
      if (found == attachments.end())
        return;
      for (const EditorObjectId object : found->second) {
        GraphRow attached;
        attached.key = GraphRowKey{
            scene, object, node, nextOccurrence(scene, object, node)};
        attached.kind = kind;
        attached.documentIndex = static_cast<std::uint32_t>(
            document.indexOf(object).value_or(0u));
        attached.depth = depth + 1u;
        attached.label = std::string(document.objectName(object));
        attached.secondary = kind == GraphRowKind::camera ? "Camera" : "Light";
        attached.selectable = true;
        attached.effectiveVisible = nodeVisible;
        attached.selection.authority = document.documentAuthority();
        if (kind == GraphRowKind::camera)
          attached.selection.value = CameraId::from(object).value_or(CameraId{});
        else
          attached.selection.value = LightId::from(object).value_or(LightId{});
        attached.selected = attached.selection == model.selection;
        model.browser.graphRows.push_back(std::move(attached));
      }
    };
    appendAttachedRows(camerasByNode, GraphRowKind::camera);
    appendAttachedRows(lightsByNode, GraphRowKind::light);
    return std::pair{children, nodeVisible};
  };

  std::vector<TraversalFrame> traversalStack;
  traversalStack.reserve(nodeCount);
  for (const SceneChoice &scene : model.scenes) {
    const std::vector<NodeId> roots = document.sceneRootNodeIds(scene.id);
    GraphRow sceneRow;
    sceneRow.key = GraphRowKey{scene.id, scene.id.untyped(), std::nullopt, 0u};
    sceneRow.kind = GraphRowKind::scene;
    sceneRow.documentIndex = scene.documentIndex;
    sceneRow.label = scene.name;
    sceneRow.secondary = scene.current ? "Current scene" : "Scene";
    sceneRow.hasChildren = !roots.empty();
    sceneRow.defaultExpanded = scene.current || model.scenes.size() == 1u;
    sceneRow.selectable = false;
    sceneRow.acceptsNodeDrop = true;
    model.browser.graphRows.push_back(std::move(sceneRow));
    traversalPath.clear();
    std::size_t traversalOccurrences = 0u;
    traversalStack.clear();
    for (auto root = roots.rbegin(); root != roots.rend(); ++root)
      traversalStack.push_back(TraversalFrame{
          scene.id, *root, std::nullopt, 1u, true, false});
    while (!traversalStack.empty() &&
           traversalOccurrences < kMaximumTraversalOccurrences) {
      TraversalFrame frame = traversalStack.back();
      traversalStack.pop_back();
      if (!frame.node.valid() || frame.depth > kMaximumTraversalDepth) continue;
      if (frame.exit) {
        traversalPath.erase(frame.node.untyped());
        continue;
      }
      if (!traversalPath.insert(frame.node.untyped()).second) continue;
      ++traversalOccurrences;
      auto [children, nodeVisible] = appendNodeOccurrence(
          frame.scene, frame.node, frame.parent, frame.depth,
          frame.ancestorsVisible);
      traversalStack.push_back(TraversalFrame{
          frame.scene, frame.node, frame.parent, frame.depth,
          frame.ancestorsVisible, true});
      for (auto child = children.rbegin(); child != children.rend(); ++child)
        traversalStack.push_back(TraversalFrame{
            frame.scene, *child, frame.node, frame.depth + 1u,
            nodeVisible, false});
    }
  }

  model.browser.resourceGroups.reserve(8u);
  const auto appendGroup = [&](ResourceGroupKind kind, EditorObjectKind objectKind) {
    ResourceGroup group;
    group.kind = kind;
    group.entries.reserve(document.objectCount(objectKind));
    for (std::size_t index = 0; index < document.objectCount(objectKind); ++index) {
      const EditorObjectId id = document.idAt(objectKind, index);
      if (!id.valid()) continue;
      ResourceEntry entry;
      entry.object = id;
      entry.documentIndex = static_cast<std::uint32_t>(index);
      entry.name = std::string(document.objectName(id));
      entry.selection.authority = document.documentAuthority();
      if (const auto meshId = MeshId::from(id)) {
        entry.selection.value = *meshId;
        entry.detail = std::to_string(meshPrimitives(*meshId).size()) +
                       " prim, " +
                       std::to_string(commands.meshReferenceCount(*meshId)) +
                       " refs";
      } else if (const auto materialId = MaterialId::from(id)) {
        entry.selection.value = *materialId;
        entry.detail =
            std::to_string(commands.materialReferenceCount(*materialId)) +
            " refs";
      } else if (const auto cameraId = CameraId::from(id)) {
        entry.selection.value = *cameraId;
        if (const auto properties = commands.cameraProperties(*cameraId))
          entry.detail = properties->type;
      } else if (const auto lightId = LightId::from(id)) {
        entry.selection.value = *lightId;
        if (const auto properties = commands.lightProperties(*lightId))
          entry.detail = properties->type;
      } else if (const auto nodeId = NodeId::from(id)) {
        const auto occurrence = firstNodeOccurrences.find(id);
        entry.selection.value = occurrence != firstNodeOccurrences.end()
                                    ? occurrence->second
                                    : displayNodeSelection(*nodeId);
        const auto children = document.nodeChildIds(*nodeId);
        entry.detail = std::to_string(children.size()) +
                       (children.size() == 1u ? " child" : " children");
        if (commands.nodeMesh(*nodeId))
          entry.detail += ", mesh";
      } else if (const auto textureId = TextureId::from(id); textureId &&
                 m_projection) {
        const auto found = std::find_if(
            m_projection->textureBindings.begin(),
            m_projection->textureBindings.end(),
            [id](const SceneTextureProjectionBinding &binding) {
              return editorObjectId(binding.textureId) == id;
            });
        if (found != m_projection->textureBindings.end()) {
          entry.linkedImage =
              ImageId::from(editorObjectId(found->sourceImageId));
          entry.detail = "image " + std::to_string(found->sourceImageIndex);
          if (found->samplerIndex >= 0)
            entry.detail += ", sampler " +
                            std::to_string(found->samplerIndex);
          entry.tooltip = "Select to reveal the source image";
        }
      } else if (const auto imageId = ImageId::from(id); imageId &&
                 m_projection) {
        if (const auto assetIndex =
                m_projection->mapping.sceneAssetIndex(imageId->untyped());
            assetIndex && *assetIndex < m_projection->asset.textures.size()) {
          const SceneTexture &texture =
              m_projection->asset.textures[*assetIndex];
          entry.detail = std::to_string(texture.width) + " x " +
                         std::to_string(texture.height) + ", " +
                         std::to_string(texture.mipLevels) + " mips";
          entry.tooltip = texture.uri;
        }
      } else if (const auto animationId = AnimationId::from(id);
                 animationId && m_projection) {
        if (const auto assetIndex =
                m_projection->mapping.sceneAssetIndex(animationId->untyped());
            assetIndex && *assetIndex < m_projection->asset.animations.size()) {
          const SceneAnimation &animation =
              m_projection->asset.animations[*assetIndex];
          entry.detail = std::to_string(animation.channels.size()) +
                         " channels";
        }
      }
      entry.selectable = !entry.selection.emptyValue();
      entry.selected = entry.selectable && entry.selection == model.selection;
      group.entries.push_back(std::move(entry));
    }
    model.browser.resourceGroups.push_back(std::move(group));
  };
  appendGroup(ResourceGroupKind::nodes, EditorObjectKind::node);
  appendGroup(ResourceGroupKind::meshes, EditorObjectKind::mesh);
  appendGroup(ResourceGroupKind::materials, EditorObjectKind::material);
  appendGroup(ResourceGroupKind::cameras, EditorObjectKind::camera);
  appendGroup(ResourceGroupKind::lights, EditorObjectKind::light);
  appendGroup(ResourceGroupKind::textures, EditorObjectKind::texture);
  appendGroup(ResourceGroupKind::images, EditorObjectKind::image);
  appendGroup(ResourceGroupKind::animations, EditorObjectKind::animation);

  const auto addSection = [](InspectorModel &inspector, InspectorSectionKind kind,
                             std::string label, bool open) -> InspectorSection & {
    inspector.sections.push_back(InspectorSection{kind, std::move(label), open});
    return inspector.sections.back();
  };
  const auto appendField = [&](InspectorSection &section, FieldId id,
                               Value value, EditorObjectId target = {},
                               bool readOnly = false) -> InspectorField & {
    InspectorField field;
    field.id = id;
    field.value = std::move(value);
    if (target.valid())
      field.target = target;
    field.readOnly = readOnly;
    field.externalValueRevision = document.revision();
    section.fields.push_back(std::move(field));
    return section.fields.back();
  };
  std::vector<Choice> textureChoices;
  textureChoices.reserve(document.objectCount(EditorObjectKind::texture));
  for (std::size_t index = 0;
       index < document.objectCount(EditorObjectKind::texture); ++index) {
    const EditorObjectId texture =
        document.idAt(EditorObjectKind::texture, index);
    if (!texture.valid() ||
        index > static_cast<std::size_t>(
                    std::numeric_limits<std::int32_t>::max()))
      continue;
    std::string label(document.objectName(texture));
    if (label.empty())
      label = "Texture " + std::to_string(index);
    textureChoices.push_back(Choice{static_cast<std::int32_t>(index),
                                    std::move(label), texture});
  }
  const auto textureIndex = [&](MaterialId material,
                                MaterialTextureSlot slot) {
    const auto texture = commands.materialTexture(material, slot);
    if (!texture || !texture->texture)
      return std::int32_t{-1};
    const auto index = document.indexOf(texture->texture->untyped());
    return index &&
                   *index <= static_cast<std::size_t>(
                                 std::numeric_limits<std::int32_t>::max())
               ? static_cast<std::int32_t>(*index)
               : std::int32_t{-1};
  };
  const auto appendTextureField = [&](InspectorSection &section, FieldId id,
                                      std::int32_t index,
                                      EditorObjectId target,
                                      bool enabled = true) {
    InspectorField &field =
        appendField(section, id, Value{index}, target, false);
    field.choices = textureChoices;
    field.enabled = enabled;
  };
  const auto titleFor = [&](EditorObjectId object, std::string_view kind) {
    std::string title(document.objectName(object));
    if (title.empty())
      title = std::string(kind) + " " +
              std::to_string(document.indexOf(object).value_or(0u));
    return title;
  };
  struct PrimitiveStatistics final {
    std::uint32_t vertices{0};
    std::uint32_t triangles{0};
  };
  const auto primitiveStatistics = [&](PrimitiveId primitive) {
    PrimitiveStatistics statistics;
    if (const auto source = document.primitiveGeometryStats(primitive)) {
      statistics.vertices = source->vertexCount;
      statistics.triangles = source->indexCount > 0u
                                 ? source->indexCount / 3u
                                 : source->vertexCount / 3u;
    }
    return statistics;
  };

  const auto effectiveNodeVisible = [&](SceneId scene, NodeId node) {
    bool visible = document.objectVisible(node.untyped());
    for (const NodeId ancestor : document.sceneNodeParentPath(scene, node))
      visible = visible && document.objectVisible(ancestor.untyped());
    return visible;
  };
  const auto appendNodeSections = [&](SceneId scene, NodeId node,
                                      std::optional<NodeId> parent,
                                      bool includeIdentity) {
    if (includeIdentity) {
      InspectorSection &identity = addSection(
          model.inspector, InspectorSectionKind::identity, "Node", true);
      appendField(identity, FieldId::name,
                  Value{std::string(document.objectName(node.untyped()))},
                  node.untyped());
    }
    InspectorSection &transform = addSection(
        model.inspector, InspectorSectionKind::transform, "Transform", true);
    if (const auto value = commands.nodeTransform(node)) {
      appendField(transform, FieldId::translation,
                  Value{value->translation}, node.untyped());
      appendField(transform, FieldId::rotationDegrees,
                  Value{quaternionEulerDegrees(value->rotation)},
                  node.untyped());
      appendField(transform, FieldId::scale, Value{value->scale},
                  node.untyped());
    }

    InspectorSection &attachments = addSection(
        model.inspector, InspectorSectionKind::attachments, "Attachments",
        true);
    appendField(attachments, FieldId::parent,
                Value{parent ? parent->untyped() : EditorObjectId{}},
                node.untyped(), true);
    appendField(attachments, FieldId::children,
                Value{std::to_string(document.nodeChildIds(node).size())},
                node.untyped(), true);
    const auto mesh = commands.nodeMesh(node);
    appendField(attachments, FieldId::meshAttachment,
                Value{mesh ? mesh->untyped() : EditorObjectId{}},
                node.untyped(), true);
    const auto cameraIt = camerasByNode.find(node.untyped());
    appendField(
        attachments, FieldId::cameraAttachment,
        Value{cameraIt != camerasByNode.end() && !cameraIt->second.empty()
                  ? cameraIt->second.front()
                  : EditorObjectId{}},
        node.untyped(), true);
    const auto skinValue = commands.nodeSkin(node);
    InspectorField &skin = appendField(
        attachments, FieldId::skinAttachment,
        Value{skinValue ? skinValue->untyped() : EditorObjectId{}},
        node.untyped(), true);
    skin.tooltip = skinValue ? "Attached skin" : "No attached skin";
    const auto lightIt = lightsByNode.find(node.untyped());
    appendField(
        attachments, FieldId::lightAttachment,
        Value{lightIt != lightsByNode.end() && !lightIt->second.empty()
                  ? lightIt->second.front()
                  : EditorObjectId{}},
        node.untyped(), true);
    appendField(attachments, FieldId::visibility,
                Value{effectiveNodeVisible(scene, node)}, node.untyped());

    if (cameraIt != camerasByNode.end() && !cameraIt->second.empty()) {
      if (const auto camera = CameraId::from(cameraIt->second.front())) {
        InspectorSection &cameraSection = addSection(
            model.inspector, InspectorSectionKind::camera, "Camera", true);
        if (const auto props = commands.cameraProperties(*camera)) {
          const bool perspective = props->type != "orthographic";
          InspectorField &type = appendField(
              cameraSection, FieldId::cameraType,
              Value{std::int32_t{perspective ? 0 : 1}}, camera->untyped());
          type.choices = {{0, "Perspective", std::nullopt},
                          {1, "Orthographic", std::nullopt}};
          if (perspective) {
            appendField(cameraSection, FieldId::cameraYFov,
                        Value{props->yfov}, camera->untyped());
            appendField(cameraSection, FieldId::cameraAspectRatio,
                        Value{props->aspectRatio}, camera->untyped());
          } else {
            appendField(cameraSection, FieldId::cameraXMag,
                        Value{props->xmag}, camera->untyped());
            appendField(cameraSection, FieldId::cameraYMag,
                        Value{props->ymag}, camera->untyped());
          }
          appendField(cameraSection, FieldId::cameraZNear,
                      Value{props->znear}, camera->untyped());
          appendField(cameraSection, FieldId::cameraZFar,
                      Value{props->zfar}, camera->untyped());
        }
        const bool projectedForCurrentView =
            m_projection &&
            m_projection->mapping.sceneAssetIndex(camera->untyped()).has_value();
        model.inspector.camera = CameraContext{
            *camera, node,
            static_cast<bool>(m_callbacks.cameraRequest) && projectedForCurrentView,
            static_cast<bool>(m_callbacks.cameraRequest) && projectedForCurrentView};
      }
    }

    if (lightIt != lightsByNode.end() && !lightIt->second.empty()) {
      if (const auto light = LightId::from(lightIt->second.front())) {
        InspectorSection &lightSection = addSection(
            model.inspector, InspectorSectionKind::light, "Light", true);
        if (const auto props = commands.lightProperties(*light)) {
          const std::int32_t typeValue = props->type == "directional"
                                             ? 0
                                             : props->type == "spot" ? 2 : 1;
          InspectorField &type = appendField(
              lightSection, FieldId::lightType, Value{typeValue},
              light->untyped());
          type.choices = {{0, "Directional", std::nullopt},
                          {1, "Point", std::nullopt},
                          {2, "Spot", std::nullopt}};
          appendField(lightSection, FieldId::lightColorSrgb,
                      Value{props->color}, light->untyped());
          appendField(lightSection, FieldId::lightIntensity,
                      Value{props->intensity}, light->untyped());
          if (typeValue != 0)
            appendField(lightSection, FieldId::lightRange,
                        Value{props->range}, light->untyped());
          if (typeValue == 2) {
            appendField(lightSection, FieldId::lightInnerConeAngle,
                        Value{props->innerConeAngle}, light->untyped());
            appendField(lightSection, FieldId::lightOuterConeAngle,
                        Value{props->outerConeAngle}, light->untyped());
          }
        }
      }
    }
  };

  const auto appendMaterialSections = [&](MaterialId material) {
    InspectorSection &section = addSection(
        model.inspector, InspectorSectionKind::material, "Material", true);
    const MaterialProperties properties =
        commands.materialProperties(material).value_or(MaterialProperties{});
    appendField(section, FieldId::baseColor, Value{properties.baseColorFactor},
                material.untyped());
    appendTextureField(section, FieldId::baseColorTexture,
                       textureIndex(material, MaterialTextureSlot::baseColor),
                       material.untyped());
    appendField(section, FieldId::metallic, Value{properties.metallicFactor},
                material.untyped());
    appendField(section, FieldId::roughness, Value{properties.roughnessFactor},
                material.untyped());
    appendTextureField(
        section, FieldId::metallicRoughnessTexture,
        textureIndex(material, MaterialTextureSlot::metallicRoughness),
        material.untyped());
    appendField(section, FieldId::emissiveColor,
                Value{properties.emissiveFactor}, material.untyped());
    appendTextureField(section, FieldId::emissiveTexture,
                       textureIndex(material, MaterialTextureSlot::emissive),
                       material.untyped());
    appendTextureField(section, FieldId::normalTexture,
                       textureIndex(material, MaterialTextureSlot::normal),
                       material.untyped());
    const MaterialTextureProperties normal =
        commands.materialTexture(material, MaterialTextureSlot::normal)
            .value_or(MaterialTextureProperties{});
    appendField(section, FieldId::normalScale, Value{normal.scalar},
                material.untyped());
    appendTextureField(section, FieldId::occlusionTexture,
                       textureIndex(material, MaterialTextureSlot::occlusion),
                       material.untyped());
    const MaterialTextureProperties occlusion =
        commands.materialTexture(material, MaterialTextureSlot::occlusion)
            .value_or(MaterialTextureProperties{});
    appendField(section, FieldId::occlusionStrength,
                Value{occlusion.scalar}, material.untyped());
    const std::int32_t alphaMode = properties.alphaMode == "MASK"
                                       ? 1
                                       : properties.alphaMode == "BLEND" ? 2
                                                                         : 0;
    InspectorField &alpha = appendField(
        section, FieldId::alphaMode, Value{alphaMode}, material.untyped());
    alpha.choices = {{0, "Opaque", std::nullopt},
                     {1, "Mask", std::nullopt},
                     {2, "Blend", std::nullopt}};
    InspectorField &cutoff = appendField(
        section, FieldId::alphaCutoff, Value{properties.alphaCutoff},
        material.untyped());
    cutoff.visible = alphaMode == 1;
    appendField(section, FieldId::doubleSided,
                Value{properties.doubleSided}, material.untyped());

    for (const MaterialExtensionDescriptor &descriptor :
         materialExtensionDescriptors()) {
      const std::optional<MaterialExtensionValue> extension =
          commands.materialExtension(material, descriptor.extension);
      InspectorSection &extensionSection = addSection(
          model.inspector, InspectorSectionKind::materialExtension,
          std::string(descriptor.label), extension.has_value());
      extensionSection.materialExtension = descriptor.extension;
      extensionSection.extensionPresent = extension.has_value();
      extensionSection.extensionMutable = true;
      const MaterialExtensionValue *root = extension ? &*extension : nullptr;
      forExtensionFieldSpec([&](const ExtensionFieldSpec &spec) {
        if (spec.extension != descriptor.extension)
          return;
        InspectorField &field = appendField(
            extensionSection, spec.field, extensionValueFor(root, spec),
            material.untyped());
        field.enabled = extension.has_value();
        if (spec.texture)
          field.choices = textureChoices;
      });
      if (descriptor.extension == "KHR_materials_volume") {
        bool infinite = true;
        if (root) {
          if (const auto *object =
                  std::get_if<MaterialExtensionValue::Object>(&root->value))
            infinite =
                extensionMember(*object, "attenuationDistance") == nullptr;
        }
        InspectorField &infiniteField = appendField(
            extensionSection, FieldId::infiniteAttenuation, Value{infinite},
            material.untyped());
        infiniteField.enabled = extension.has_value();
        for (InspectorField &field : extensionSection.fields)
          if (field.id == FieldId::attenuationDistance)
            field.enabled = extension.has_value() && !infinite;
      }
    }
    model.inspector.material = material;
  };

  model.inspector.selection = model.selection;
  model.inspector.selectionRevision = model.selectionRevision;
  model.inspector.title = "Inspector";
  if (const auto *selected =
          std::get_if<NodeSelection>(&model.selection.value)) {
    model.inspector.title = titleFor(selected->node.untyped(), "Node");
    appendNodeSections(selected->scene, selected->node, selected->parent, true);
  } else if (const auto *selected =
                 std::get_if<PrimitiveSelection>(&model.selection.value)) {
    const std::vector<NodeId> parentPath =
        document.sceneNodeParentPath(selected->scene, selected->node);
    const std::optional<NodeId> parent =
        parentPath.empty() ? std::nullopt
                           : std::optional<NodeId>{parentPath.back()};
    appendNodeSections(selected->scene, selected->node, parent, false);
    const auto address = document.primitiveAddress(selected->primitive);
    const PrimitiveStatistics statistics =
        primitiveStatistics(selected->primitive);
    InspectorSection &primitive = addSection(
        model.inspector, InspectorSectionKind::primitive, "Primitive", true);
    appendField(primitive, FieldId::primitiveIndex,
                Value{address ? address->primitiveIndex : 0u},
                selected->primitive.untyped(), true);
    appendField(primitive, FieldId::meshIndex,
                Value{address ? address->meshIndex : 0u},
                selected->primitive.untyped(), true);
    appendField(primitive, FieldId::vertexCount, Value{statistics.vertices},
                selected->primitive.untyped(), true);
    appendField(primitive, FieldId::triangleCount,
                Value{statistics.triangles}, selected->primitive.untyped(),
                true);
    const std::optional<MaterialId> material =
        commands.primitiveMaterial(selected->primitive);
    appendField(primitive, FieldId::materialReference,
                Value{material ? material->untyped() : EditorObjectId{}},
                selected->primitive.untyped(), true);
    model.inspector.primitive = PrimitiveContext{
        selected->scene, selected->node, selected->mesh, selected->primitive,
        material, material.has_value(),
        document.objectCount(EditorObjectKind::mesh) > 1u,
        !model.materials.empty()};
    model.inspector.title =
        "Primitive " +
        std::to_string(address ? address->primitiveIndex : 0u);
    if (material)
      appendMaterialSections(*material);
    else
      (void)addSection(model.inspector, InspectorSectionKind::material,
                       "Material", true);
  } else if (const auto *mesh =
                 std::get_if<MeshId>(&model.selection.value)) {
    model.inspector.title = titleFor(mesh->untyped(), "Mesh");
    InspectorSection &identity = addSection(
        model.inspector, InspectorSectionKind::identity, "Mesh", true);
    appendField(identity, FieldId::name,
                Value{std::string(document.objectName(mesh->untyped()))},
                mesh->untyped());
    const std::vector<PrimitiveId> &primitives = meshPrimitives(*mesh);
    PrimitiveStatistics totals;
    for (const PrimitiveId primitive : primitives) {
      const PrimitiveStatistics stats = primitiveStatistics(primitive);
      totals.vertices += stats.vertices;
      totals.triangles += stats.triangles;
    }
    InspectorSection &section = addSection(
        model.inspector, InspectorSectionKind::mesh, "Mesh", true);
    appendField(section, FieldId::primitiveCount,
                Value{static_cast<std::uint32_t>(primitives.size())},
                mesh->untyped(), true);
    appendField(section, FieldId::vertexCount, Value{totals.vertices},
                mesh->untyped(), true);
    appendField(section, FieldId::triangleCount, Value{totals.triangles},
                mesh->untyped(), true);
    appendField(section, FieldId::referencedBy,
                Value{std::to_string(commands.meshReferenceCount(*mesh)) +
                      " nodes"},
                mesh->untyped(), true);
  } else if (const auto *material = std::get_if<MaterialId>(&model.selection.value)) {
    model.inspector.title = titleFor(material->untyped(), "Material");
    InspectorSection &identity = addSection(
        model.inspector, InspectorSectionKind::identity, "Material", true);
    appendField(identity, FieldId::name,
                Value{std::string(document.objectName(material->untyped()))},
                material->untyped());
    appendMaterialSections(*material);
  } else if (const auto *camera = std::get_if<CameraId>(&model.selection.value)) {
    model.inspector.title = titleFor(camera->untyped(), "Camera");
    InspectorSection &identity = addSection(
        model.inspector, InspectorSectionKind::identity, "Camera", true);
    appendField(identity, FieldId::name,
                Value{std::string(document.objectName(camera->untyped()))},
                camera->untyped());
    InspectorSection &section = addSection(
        model.inspector, InspectorSectionKind::camera, "Camera", true);
    if (const auto props = commands.cameraProperties(*camera)) {
      const bool perspective = props->type != "orthographic";
      InspectorField &type = appendField(
          section, FieldId::cameraType,
          Value{std::int32_t{perspective ? 0 : 1}}, camera->untyped());
      type.choices = {{0, "Perspective", std::nullopt},
                      {1, "Orthographic", std::nullopt}};
      if (perspective) {
        appendField(section, FieldId::cameraYFov, Value{props->yfov},
                    camera->untyped());
        appendField(section, FieldId::cameraAspectRatio,
                    Value{props->aspectRatio}, camera->untyped());
      } else {
        appendField(section, FieldId::cameraXMag, Value{props->xmag},
                    camera->untyped());
        appendField(section, FieldId::cameraYMag, Value{props->ymag},
                    camera->untyped());
      }
      appendField(section, FieldId::cameraZNear, Value{props->znear},
                  camera->untyped());
      appendField(section, FieldId::cameraZFar, Value{props->zfar},
                  camera->untyped());
    }
    const auto node = attachmentNodes.find(camera->untyped());
    const std::optional<NodeId> attached =
        node == attachmentNodes.end() ? std::nullopt
                                      : std::optional<NodeId>{node->second};
    const bool projectedForCurrentView =
        m_projection &&
        m_projection->mapping.sceneAssetIndex(camera->untyped()).has_value();
    model.inspector.camera = CameraContext{
        *camera, attached,
        static_cast<bool>(m_callbacks.cameraRequest) && projectedForCurrentView,
        static_cast<bool>(m_callbacks.cameraRequest) && attached.has_value() &&
            projectedForCurrentView};
  } else if (const auto *light = std::get_if<LightId>(&model.selection.value)) {
    model.inspector.title = titleFor(light->untyped(), "Light");
    InspectorSection &identity = addSection(
        model.inspector, InspectorSectionKind::identity, "Light", true);
    appendField(identity, FieldId::name,
                Value{std::string(document.objectName(light->untyped()))},
                light->untyped());
    InspectorSection &section = addSection(
        model.inspector, InspectorSectionKind::light, "Light", true);
    if (const auto props = commands.lightProperties(*light)) {
      const std::int32_t typeValue = props->type == "directional"
                                         ? 0
                                         : props->type == "spot" ? 2 : 1;
      InspectorField &type = appendField(
          section, FieldId::lightType, Value{typeValue}, light->untyped());
      type.choices = {{0, "Directional", std::nullopt},
                      {1, "Point", std::nullopt},
                      {2, "Spot", std::nullopt}};
      appendField(section, FieldId::lightColorSrgb,
                  Value{props->color}, light->untyped());
      appendField(section, FieldId::lightIntensity, Value{props->intensity},
                  light->untyped());
      if (typeValue != 0)
        appendField(section, FieldId::lightRange, Value{props->range},
                    light->untyped());
      if (typeValue == 2) {
        appendField(section, FieldId::lightInnerConeAngle,
                    Value{props->innerConeAngle}, light->untyped());
        appendField(section, FieldId::lightOuterConeAngle,
                    Value{props->outerConeAngle}, light->untyped());
      }
    }
  }

  if (!m_currentAnimation && !m_animationPose) {
    const SceneProjectionResult *projection =
        m_projection ? &*m_projection : nullptr;
    const std::uint64_t mappingEpoch =
        projection != nullptr ? projection->mappingEpoch : 0u;
    const bool cacheKeyChanged =
        !m_frameModelCache ||
        m_frameModelCache->revision != model.revision ||
        m_frameModelCache->selectionRevision != model.selectionRevision ||
        m_frameModelCache->selection != model.selection ||
        m_frameModelCacheVariant != m_currentVariant ||
        m_frameModelCacheProjection != projection ||
        m_frameModelCacheMappingEpoch != mappingEpoch;
    if (cacheKeyChanged) {
      m_frameModelCache = std::make_shared<FrameModel>(model);
      m_frameModelCacheVariant = m_currentVariant;
      m_frameModelCacheProjection = projection;
      m_frameModelCacheMappingEpoch = mappingEpoch;
    }
  }
  return model;
}

RenderResult EditorController::render(LayoutState layout, EditorUiProbe *probe) {
  // Hold a local shared owner while rendering: dispatch callbacks can replace
  // or clear the controller cache synchronously without invalidating the
  // FrameModel currently consumed by SceneEditorUi.
  if (canReuseFrameModelCache()) {
    const std::shared_ptr<FrameModel> model = m_frameModelCache;
    refreshFrameModelTransientState(*model, layout);
    m_persistent = m_ui.state();
    return m_ui.render(*model, *this, probe);
  }

  FrameModel model = buildFrameModel(layout);
  m_persistent = m_ui.state();
  return m_ui.render(model, *this, probe);
}

RuntimeActionResult EditorController::cameraRequest(const CameraRequest &request) {
  if (m_callbacks.cameraRequest) return m_callbacks.cameraRequest(request);
  return unsupported("Camera request is not connected");
}

NodeDropValidation EditorController::validateNodeDrop(
    const ReparentNode &request) const {
  NodeDropValidation result;
  if (!hasDocument()) {
    result.reason = "No active document";
    return result;
  }
  if (!request.source.node.valid() ||
      request.source.revision != session()->document().revision()) {
    result.reason = "The dragged node occurrence is stale";
    return result;
  }
  const SceneId targetScene = request.targetScene.valid()
                                  ? request.targetScene
                                  : request.source.scene;
  if (targetScene.authority() != session()->document().documentAuthority()) {
    result.reason = "The drop target belongs to another document";
    return result;
  }
  if (!targetScene.valid() ||
      !session()->document().contains(targetScene.untyped())) {
    result.reason = "The drop target scene no longer exists";
    return result;
  }
  if (!session()->document().hasNodeOccurrence(
          request.source.scene, request.source.node, request.source.parent)) {
    result.reason = "The dragged node occurrence no longer exists";
    return result;
  }
  std::optional<SceneNodeOccurrence> parent;
  if (request.targetParent) {
    parent = session()->editor().commands().captureNodeOccurrence(
        targetScene, *request.targetParent);
    if (!parent) {
      result.reason = "The drop target no longer exists";
      return result;
    }
  }
  const StructuralPrepareResult prepared = session()->editor().commands().prepareStructural(
      ReparentNodeRequest{request.source, parent,
                          SceneOccurrencePolicy::clickedScene, targetScene});
  if (!prepared.succeeded()) {
    result.reason = prepared.message.empty() ? "Drop would create an invalid hierarchy"
                                             : prepared.message;
    return result;
  }
  result.accepted = true;
  return result;
}

ViewportSettings EditorController::viewportSettings() const {
  return m_persistent.viewport;
}

void EditorController::setViewportSettings(const ViewportSettings &settings) {
  m_persistent.viewport = settings;
}

bool EditorController::viewportControlsAvailable() const noexcept {
  if (!m_callbacks.viewportControlsAvailable)
    return false;
  try {
    return m_callbacks.viewportControlsAvailable();
  } catch (...) {
    return false;
  }
}

void EditorController::setViewportInteraction(bool hovered,
                                               bool focused) noexcept {
  m_viewportHovered = hovered;
  m_viewportFocused = focused;
}

std::optional<std::filesystem::path>
EditorController::nativeFileDialog(const NativeDialogRequest &request) {
  return m_callbacks.fileDialog ? m_callbacks.fileDialog(request)
                                : std::optional<std::filesystem::path>{};
}

std::string EditorController::clipboardText() const {
  if (m_callbacks.clipboard == nullptr) return {};
  return m_callbacks.clipboard->readText().value_or(std::string{});
}

bool EditorController::setClipboardText(std::string_view text) {
  return m_callbacks.clipboard != nullptr &&
         m_callbacks.clipboard->writeText(text);
}

const PersistentState &EditorController::persistentState() const noexcept {
  return m_persistent;
}

void EditorController::setPersistentState(PersistentState state) noexcept {
  m_persistent = std::move(state);
  m_ui.setState(m_persistent);
}

} // namespace demo::editor::ui
