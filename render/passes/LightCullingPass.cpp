#include "LightCullingPass.h"
#include "../Renderer.h"

#include <array>

namespace demo {

LightCullingPass::LightCullingPass(Renderer* renderer)
    : m_renderer(renderer)
{}

PassNode::HandleSlice<PassResourceDependency> LightCullingPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 7> deps = {
        PassResourceDependency::texture(kPassSceneDepthHandle, ResourceAccess::read, rhi::ShaderStage::compute),
        PassResourceDependency::texture(kPassDepthPyramidHandle, ResourceAccess::read, rhi::ShaderStage::compute),
        PassResourceDependency::buffer(kPassPointLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
        PassResourceDependency::buffer(kPassSpotLightBufferHandle, ResourceAccess::read, rhi::ShaderStage::compute),
        PassResourceDependency::buffer(kPassPointLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
        PassResourceDependency::buffer(kPassSpotLightCoarseBoundsHandle, ResourceAccess::write, rhi::ShaderStage::compute),
        PassResourceDependency::buffer(kPassLightCoarseCullingUniformHandle, ResourceAccess::read, rhi::ShaderStage::compute),
    };
    return {deps.data(), static_cast<uint32_t>(deps.size())};
}

void LightCullingPass::execute(const PassContext& context) const
{
    if(m_renderer == nullptr || context.params == nullptr)
        return;

    context.cmd->beginEvent("LightCoarseCulling");
    m_renderer->executeLightCoarseCullingPass(*context.cmd, *context.params);
    context.cmd->endEvent();
}

}  // namespace demo
