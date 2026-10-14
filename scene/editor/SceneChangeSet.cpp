#include <mgif/scene_editor/SceneChangeSet.h>

#include <algorithm>
#include <type_traits>

namespace demo::editor {

bool sceneDigestPresent(const SceneDigest &digest) noexcept {
  return std::any_of(digest.begin(), digest.end(),
                     [](std::uint8_t byte) { return byte != 0u; });
}

SceneValueTag sceneFieldValueTag(const SceneFieldValue &value) noexcept {
  return static_cast<SceneValueTag>(value.index());
}

bool sceneFieldValueMatchesField(SceneField field,
                                 const SceneFieldValue &value) noexcept {
  const auto tag = sceneFieldValueTag(value);
  switch (field) {
  case SceneField::NodeLocalTransform:
  case SceneField::SceneRootTransform:
    return tag == SceneValueTag::NodeTransform;
  case SceneField::NodeLocalVisibility:
    return tag == SceneValueTag::Visibility;
  case SceneField::MaterialParameters:
    return tag == SceneValueTag::MaterialParameters;
  case SceneField::MaterialExtension:
    return tag == SceneValueTag::MaterialExtension;
  case SceneField::MaterialTextureBinding:
    return tag == SceneValueTag::TextureBinding;
  case SceneField::PrimitiveMaterialBinding:
    return tag == SceneValueTag::PrimitiveMaterialBinding;
  case SceneField::CameraParameters:
    return tag == SceneValueTag::Camera;
  case SceneField::CameraAttachment:
  case SceneField::LightAttachment:
    return tag == SceneValueTag::ObjectAttachment;
  case SceneField::LightParameters:
    return tag == SceneValueTag::Light;
  case SceneField::HierarchyParent:
  case SceneField::HierarchyAttachment:
    return tag == SceneValueTag::Hierarchy;
  case SceneField::TopologyMetadata:
    return tag == SceneValueTag::Topology;
  case SceneField::GeometryBufferRange:
    return tag == SceneValueTag::BufferRange;
  case SceneField::TexturePayload:
    return tag == SceneValueTag::TexturePayload;
  }
  return false;
}

std::vector<StableSceneId>
sceneFieldValueReferences(const SceneFieldValue &value) {
  std::vector<StableSceneId> result;
  std::visit(
      [&result](const auto &typedValue) {
        using Value = std::decay_t<decltype(typedValue)>;
        if constexpr (std::is_same_v<Value, SceneTextureBindingValue>) {
          if (typedValue.texture) {
            result.push_back(*typedValue.texture);
          }
          if (typedValue.sampler) {
            result.push_back(*typedValue.sampler);
          }
        } else if constexpr (std::is_same_v<
                                 Value, ScenePrimitiveMaterialBindingValue>) {
          if (typedValue.material) {
            result.push_back(*typedValue.material);
          }
        } else if constexpr (std::is_same_v<Value,
                                            SceneObjectAttachmentValue>) {
          if (typedValue.node) {
            result.push_back(*typedValue.node);
          }
        } else if constexpr (std::is_same_v<Value, SceneHierarchyValue>) {
          if (typedValue.parent) {
            result.push_back(*typedValue.parent);
          }
          result.insert(result.end(), typedValue.attachments.begin(),
                        typedValue.attachments.end());
        } else if constexpr (std::is_same_v<Value, SceneTopologyValue>) {
          result.insert(result.end(), typedValue.dependencies.begin(),
                        typedValue.dependencies.end());
        }
      },
      value);

  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

} // namespace demo::editor
