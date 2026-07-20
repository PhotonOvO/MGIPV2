#pragma once

#include "../loader/GltfLoader.h"
#include "../rhi/RHITypes.h"
#include "../scene/SceneAsset.h"
#include "ClipSpaceConvention.h"
#include "ShaderInterop.h"
#include "RenderTypes.h"

#include <span>

namespace demo {

void populateCameraUniforms(const glm::mat4& view,
                            const glm::mat4& projection,
                            const glm::vec3& position,
                            shaderio::CameraUniforms& uniforms);

void populateNavigatedSceneCameraUniforms(
    const shaderio::CameraUniforms& sceneCameraUniforms,
    const glm::mat4& localNavigationView,
    shaderio::CameraUniforms& uniforms);

[[nodiscard]] bool populatePrimarySceneCameraUniforms(
    std::span<const SceneCamera> cameras,
    std::span<const SceneNode> sceneNodes,
    std::span<const GltfNodeData> gltfNodes,
    rhi::Extent2D viewportSize,
    const clipspace::ProjectionConvention& projectionConvention,
    shaderio::CameraUniforms& uniforms);

// Returns true when the scene declares lights. In that case the scene is
// authoritative even when it has no enabled directional light.
[[nodiscard]] bool applyPrimarySceneDirectionalLight(
    std::span<const SceneLight> lights,
    std::span<const SceneNode> sceneNodes,
    std::span<const GltfNodeData> gltfNodes,
    DirectionalLightSettings& settings);

}  // namespace demo
