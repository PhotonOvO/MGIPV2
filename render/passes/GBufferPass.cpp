#include "GBufferPass.h"
#include "../Renderer.h"

namespace demo {

GBufferPass::GBufferPass(Renderer* renderer)
    : m_renderer(renderer)
{
}

PassNode::HandleSlice<PassResourceDependency> GBufferPass::getDependencies() const
{
    static const std::array<PassResourceDependency, 3> dependencies = {
        PassResourceDependency::buffer(
            kPassVertexBufferHandle,
            ResourceAccess::read,
            rhi::ShaderStage::vertex
        ),
        PassResourceDependency::texture(
            kPassGBufferColorHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
        PassResourceDependency::texture(
            kPassGBufferDepthHandle,
            ResourceAccess::write,
            rhi::ShaderStage::fragment
        ),
    };
    return {dependencies.data(), static_cast<uint32_t>(dependencies.size())};
}

void GBufferPass::execute(const PassContext& context) const
{
    // Implementation will be added in Task 11
}

}  // namespace demo