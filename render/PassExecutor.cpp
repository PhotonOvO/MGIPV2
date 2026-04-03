#include "PassExecutor.h"

#include <cassert>
#include <unordered_map>

namespace demo {

namespace {

rhi::PipelineStage toPipelineStage(demo::rhi::ShaderStage stageMask)
{
  const uint32_t     mask   = static_cast<uint32_t>(stageMask);
  rhi::PipelineStage result = rhi::PipelineStage::None;
  if((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::vertex)) != 0)
  {
    result |= rhi::PipelineStage::VertexShader;
  }
  if((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::fragment)) != 0)
  {
    result |= rhi::PipelineStage::FragmentShader;
  }
  if((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::compute)) != 0)
  {
    result |= rhi::PipelineStage::Compute;
  }
  return rhi::any(result) ? result : rhi::PipelineStage::TopOfPipe;
}

[[nodiscard]] uint64_t toHandleKey(BufferHandle handle)
{
  return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
}

[[nodiscard]] uint64_t toHandleKey(TextureHandle handle)
{
  return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
}

rhi::BufferHandle toRhiHandle(BufferHandle handle)
{
  return rhi::BufferHandle{handle.index, handle.generation};
}

rhi::TextureHandle toRhiHandle(TextureHandle handle)
{
  return rhi::TextureHandle{handle.index, handle.generation};
}

rhi::ResourceAccess toRhiAccess(ResourceAccess access)
{
  switch(access)
  {
    case ResourceAccess::write:
      return rhi::ResourceAccess::write;
    case ResourceAccess::readWrite:
      return rhi::ResourceAccess::readWrite;
    case ResourceAccess::read:
    default:
      return rhi::ResourceAccess::read;
  }
}

rhi::ResourceState requiredStateForTexture(const PassExecutor::TextureBinding& binding, ResourceAccess access)
{
  if(access == ResourceAccess::read)
  {
    return rhi::ResourceState::General;
  }

  if(binding.aspect == rhi::TextureAspect::depth)
  {
    return rhi::ResourceState::General;
  }

  return rhi::ResourceState::General;
}

[[nodiscard]] bool requiresBarrier(ResourceAccess previous, ResourceAccess next)
{
  return !(previous == ResourceAccess::read && next == ResourceAccess::read);
}

struct BufferUsageState
{
  rhi::ShaderStage stageMask{rhi::ShaderStage::none};
  ResourceAccess   access{ResourceAccess::read};
};

struct TextureUsageState
{
  rhi::ShaderStage   stageMask{rhi::ShaderStage::none};
  ResourceAccess     access{ResourceAccess::read};
  rhi::ResourceState state{rhi::ResourceState::Undefined};
};

}  // namespace

void PassExecutor::clear()
{
  m_passes.clear();
}

void PassExecutor::addPass(const PassNode& pass)
{
  m_passes.push_back(&pass);
}

void PassExecutor::clearResourceBindings()
{
  m_textureBindings.clear();
  m_bufferBindings.clear();
}

void PassExecutor::bindTexture(TextureBinding binding)
{
  m_textureBindings.push_back(binding);
}

void PassExecutor::bindBuffer(BufferBinding binding)
{
  m_bufferBindings.push_back(binding);
}

const PassExecutor::TextureBinding* PassExecutor::findTextureBinding(TextureHandle handle) const
{
  for(const TextureBinding& binding : m_textureBindings)
  {
    if(binding.handle == handle)
    {
      return &binding;
    }
  }
  return nullptr;
}

const PassExecutor::BufferBinding* PassExecutor::findBufferBinding(BufferHandle handle) const
{
  for(const BufferBinding& binding : m_bufferBindings)
  {
    if(binding.handle == handle)
    {
      return &binding;
    }
  }
  return nullptr;
}

size_t PassExecutor::getPassCount() const
{
  return m_passes.size();
}

void PassExecutor::execute(const PassContext& context) const
{
  std::unordered_map<uint64_t, BufferUsageState>  bufferStates;
  std::unordered_map<uint64_t, TextureUsageState> textureStates;

  for(const TextureBinding& binding : m_textureBindings)
  {
    textureStates[toHandleKey(binding.handle)] = TextureUsageState{
        .stageMask = rhi::ShaderStage::none,
        .access    = ResourceAccess::read,
        .state     = binding.initialState,
    };

    context.cmd->setResourceState(rhi::ResourceHandle{rhi::ResourceKind::Texture, binding.handle.index, binding.handle.generation},
                                  binding.initialState);
  }
  
  for(uint32_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
  {
    const PassNode* pass = m_passes[passIndex];
    assert(pass != nullptr);

    const PassNode::HandleSlice<PassResourceDependency> dependencies = pass->getDependencies();
    for(uint32_t i = 0; i < dependencies.count; ++i)
    {
      const PassResourceDependency& dependency = dependencies.data[i];
      if(dependency.type == PassResourceType::buffer)
      {
        const BufferBinding* binding = findBufferBinding(dependency.bufferHandle);
        if(binding == nullptr || binding->nativeBuffer == 0)
        {
          continue;
        }

        const uint64_t key = toHandleKey(dependency.bufferHandle);
        const auto     it  = bufferStates.find(key);
        if(it != bufferStates.end() && requiresBarrier(it->second.access, dependency.access))
        {
          context.cmd->transitionBuffer(rhi::BufferBarrierDesc{
              .buffer       = toRhiHandle(dependency.bufferHandle),
              .nativeBuffer = binding->nativeBuffer,
              .srcStage     = toPipelineStage(it->second.stageMask),
              .dstStage     = toPipelineStage(dependency.stageMask),
              .srcAccess    = toRhiAccess(it->second.access),
              .dstAccess    = toRhiAccess(dependency.access),
          });
        }

        bufferStates[key] = BufferUsageState{
            .stageMask = dependency.stageMask,
            .access    = dependency.access,
        };
        continue;
      }

      const TextureBinding* binding = findTextureBinding(dependency.textureHandle);
      if(binding == nullptr || binding->nativeImage == 0)
      {
        continue;
      }

      const uint64_t key            = toHandleKey(dependency.textureHandle);
      auto           textureStateIt = textureStates.find(key);
      if(textureStateIt == textureStates.end())
      {
        textureStateIt = textureStates
                             .emplace(key,
                                      TextureUsageState{
                                          .stageMask = rhi::ShaderStage::none,
                                          .access    = ResourceAccess::read,
                                          .state     = binding->initialState,
                                      })
                             .first;
      }

      const rhi::ResourceState requiredState = requiredStateForTexture(*binding, dependency.access);
      const rhi::ResourceState currentState  = textureStateIt->second.state;

      if(currentState != requiredState)
      {
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = toRhiHandle(dependency.textureHandle),
            .nativeImage = binding->nativeImage,
            .aspect      = binding->aspect,
            .srcStage    = toPipelineStage(textureStateIt->second.stageMask),
            .dstStage    = toPipelineStage(dependency.stageMask),
            .srcAccess   = toRhiAccess(textureStateIt->second.access),
            .dstAccess   = toRhiAccess(dependency.access),
            .oldState    = currentState,
            .newState    = requiredState,
            .isSwapchain = binding->isSwapchain,
        });
      }
      else if(requiresBarrier(textureStateIt->second.access, dependency.access))
      {
        context.cmd->transitionTexture(rhi::TextureBarrierDesc{
            .texture     = toRhiHandle(dependency.textureHandle),
            .nativeImage = binding->nativeImage,
            .aspect      = binding->aspect,
            .srcStage    = toPipelineStage(textureStateIt->second.stageMask),
            .dstStage    = toPipelineStage(dependency.stageMask),
            .srcAccess   = toRhiAccess(textureStateIt->second.access),
            .dstAccess   = toRhiAccess(dependency.access),
            .oldState    = requiredState,
            .newState    = requiredState,
            .isSwapchain = binding->isSwapchain,
        });
      }

      textureStateIt->second.stageMask = dependency.stageMask;
      textureStateIt->second.access    = dependency.access;
      textureStateIt->second.state     = requiredState;

      context.cmd->setResourceState(rhi::ResourceHandle{rhi::ResourceKind::Texture, dependency.textureHandle.index,
                                                        dependency.textureHandle.generation},
                                    requiredState);
    }

    PassContext scopedContext = context;
    scopedContext.passIndex   = passIndex;
    pass->execute(scopedContext);
  }

  for(const TextureBinding& binding : m_textureBindings)
  {
    if(!binding.isSwapchain || binding.nativeImage == 0)
    {
      continue;
    }

    const uint64_t key = toHandleKey(binding.handle);
    auto           it  = textureStates.find(key);
    if(it == textureStates.end())
    {
      continue;
    }

    const rhi::ResourceState presentState = rhi::ResourceState::Present;
    if(it->second.state != presentState)
    {
      context.cmd->transitionTexture(rhi::TextureBarrierDesc{
          .texture     = toRhiHandle(binding.handle),
          .nativeImage = binding.nativeImage,
          .aspect      = binding.aspect,
          .srcStage    = toPipelineStage(it->second.stageMask),
          .dstStage    = rhi::PipelineStage::BottomOfPipe,
          .srcAccess   = toRhiAccess(it->second.access),
          .dstAccess   = rhi::ResourceAccess::read,
          .oldState    = it->second.state,
          .newState    = presentState,
          .isSwapchain = true,
      });
      it->second.state = presentState;
      context.cmd->setResourceState(rhi::ResourceHandle{rhi::ResourceKind::Texture, binding.handle.index, binding.handle.generation},
                                    presentState);
    }
  }
}

}  // namespace demo
