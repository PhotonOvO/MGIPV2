#include "PassExecutor.h"
#include "../common/ProfilerMarkers.h"
#include "../rhi/RHIDevice.h"

#include <cassert>

namespace demo
{
	namespace
	{
		class ScopedCommandEvent
		{
		public:
			ScopedCommandEvent(rhi::CommandBuffer* commandBuffer, const char* name)
				: m_commandBuffer(commandBuffer)
			{
				if (m_commandBuffer != nullptr)
				{
					m_commandBuffer->beginEvent(name);
				}
			}

			~ScopedCommandEvent()
			{
				if (m_commandBuffer != nullptr)
				{
					m_commandBuffer->endEvent();
				}
			}

			ScopedCommandEvent(const ScopedCommandEvent&) = delete;
			ScopedCommandEvent& operator=(const ScopedCommandEvent&) = delete;

		private:
			rhi::CommandBuffer* m_commandBuffer{nullptr};
		};

		// Wave 7: map the declarative ShaderStage mask to the StageFlags used by the
		// stage-barrier main path (CommandBuffer::barrier -> VkMemoryBarrier2).
		rhi::StageFlags toStageFlags(demo::rhi::ShaderStage stageMask)
		{
			const uint32_t mask = static_cast<uint32_t>(stageMask);
			rhi::StageFlags result = rhi::StageFlags::none;
			if ((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::vertex)) != 0) result = result |
				rhi::StageFlags::vertexShader;
			if ((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::fragment)) != 0) result = result |
				rhi::StageFlags::fragmentShader;
			if ((mask & static_cast<uint32_t>(demo::rhi::ShaderStage::compute)) != 0) result = result |
				rhi::StageFlags::compute;
			return result == rhi::StageFlags::none ? rhi::StageFlags::all : result;
		}

		[[nodiscard]] rhi::StageFlags dependencyStages(const PassResourceDependency& dependency)
		{
			return dependency.stages == rhi::StageFlags::none ? toStageFlags(dependency.stageMask) : dependency.stages;
		}

		[[nodiscard]] rhi::HazardFlags dependencyHazards(const PassResourceDependency& dependency)
		{
			if (dependency.hazards != rhi::HazardFlags::none)
			{
				return dependency.hazards;
			}
			return dependency.type == PassResourceType::texture
				       ? rhi::HazardFlags::textureWrites
				       : rhi::HazardFlags::bufferWrites;
		}

		[[nodiscard]] uint64_t toHandleKey(BufferHandle handle)
		{
			return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
		}

		[[nodiscard]] uint64_t toHandleKey(TextureHandle handle)
		{
			return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
		}

		rhi::ResourceState requiredStateForTexture(const PassExecutor::TextureBinding& binding, ResourceAccess access)
		{
			if (access == ResourceAccess::read)
			{
				return rhi::ResourceState::General;
			}

			if (binding.aspect == rhi::TextureAspect::depth || binding.aspect == rhi::TextureAspect::depthStencil)
			{
				return rhi::ResourceState::General;
			}

			return rhi::ResourceState::General;
		}

		rhi::ResourceState resolveRequiredTextureState(const PassExecutor::TextureBinding& binding,
		                                               const PassResourceDependency& dependency)
		{
			if (dependency.requiredState != rhi::ResourceState::Undefined)
			{
				return dependency.requiredState;
			}

			return requiredStateForTexture(binding, dependency.access);
		}

		[[nodiscard]] bool requiresBarrier(ResourceAccess previous, ResourceAccess next)
		{
			return !(previous == ResourceAccess::read && next == ResourceAccess::read);
		}

		[[nodiscard]] bool requiresResourceBoundary(rhi::ResourceState previous, rhi::ResourceState next)
		{
			return previous != next;
		}
	} // namespace

	void PassExecutor::clear()
	{
		m_passes.clear();
	}

	void PassExecutor::addPass(const PassNode& pass)
	{
		m_passes.push_back(&pass);
	}

	void PassExecutor::setResourceTable(rhi::Device* device)
	{
		m_device = device;
	}

	rhi::TextureHandle PassExecutor::getTextureRHIHandle(TextureHandle handle) const
	{
		const TextureBinding* binding = findTextureBinding(handle);
		return binding != nullptr ? binding->rhiTexture : rhi::TextureHandle{};
	}

	rhi::TextureHandle PassExecutor::resolveBarrierTexture(uint64_t backendImageToken) const
	{
		if (backendImageToken == 0 || m_device == nullptr)
		{
			return rhi::TextureHandle{};
		}
		for (const auto& [image, handle] : m_barrierTextureCache)
		{
			if (image == backendImageToken)
			{
				return handle;
			}
		}
		const rhi::TextureHandle handle = m_device->registerExternalTexture(backendImageToken);
		m_barrierTextureCache.emplace_back(backendImageToken, handle);
		return handle;
	}

	void PassExecutor::clearResourceBindings()
	{
		if (m_device != nullptr)
		{
			for (TextureBinding& binding : m_textureBindings)
			{
				if (!binding.rhiTexture.isNull())
				{
					m_device->destroyImage(binding.rhiTexture);
				}
			}
			for (const auto& [image, handle] : m_barrierTextureCache)
			{
				(void)image;
				if (!handle.isNull())
				{
					m_device->destroyImage(handle);
				}
			}
		}
		m_barrierTextureCache.clear();
		m_textureBindings.clear();
		m_bufferBindings.clear();
		m_executionTextureStates.clear();
		m_executionBufferStates.clear();
	}

	void PassExecutor::bindTexture(TextureBinding binding)
	{
		// Mirror the externally owned image into the backend registry so explicit resourceBarrier
		// boundaries can resolve this attachment as a TextureHandle. SceneResources owns
		// the allocation lifetime; the registry only mirrors the backend token.
		if (m_device != nullptr && binding.backendImageToken != 0)
		{
			binding.rhiTexture = m_device->registerExternalTexture(binding.backendImageToken);
		}

		// Update existing binding if handle already bound, otherwise add new
		for (TextureBinding& existing : m_textureBindings)
		{
			if (existing.handle == binding.handle)
			{
				if (m_device != nullptr && !existing.rhiTexture.isNull())
				{
					m_device->destroyImage(existing.rhiTexture);
				}
				existing = binding;
				return;
			}
		}
		m_textureBindings.push_back(binding);
		m_executionTextureStates.reserve(m_textureBindings.size());
	}

	void PassExecutor::bindBuffer(BufferBinding binding)
	{
		// Update existing binding if handle already bound, otherwise add new
		for (BufferBinding& existing : m_bufferBindings)
		{
			if (existing.handle == binding.handle)
			{
				existing = binding;
				return;
			}
		}
		m_bufferBindings.push_back(binding);
		m_executionBufferStates.reserve(m_bufferBindings.size());
	}

	const PassExecutor::TextureBinding* PassExecutor::findTextureBinding(TextureHandle handle) const
	{
		for (const TextureBinding& binding : m_textureBindings)
		{
			if (binding.handle == handle)
			{
				return &binding;
			}
		}
		return nullptr;
	}

	const PassExecutor::BufferBinding* PassExecutor::findBufferBinding(BufferHandle handle) const
	{
		for (const BufferBinding& binding : m_bufferBindings)
		{
			if (binding.handle == handle)
			{
				return &binding;
			}
		}
		return nullptr;
	}

	PassExecutor::BufferUsageState* PassExecutor::findBufferExecutionState(uint64_t key) const
	{
		for (BufferExecutionState& record : m_executionBufferStates)
		{
			if (record.key == key)
			{
				return &record.state;
			}
		}
		return nullptr;
	}

	PassExecutor::TextureUsageState* PassExecutor::findTextureExecutionState(uint64_t key) const
	{
		for (TextureExecutionState& record : m_executionTextureStates)
		{
			if (record.key == key)
			{
				return &record.state;
			}
		}
		return nullptr;
	}

	size_t PassExecutor::getPassCount() const
	{
		return m_passes.size();
	}

	const PassNode* PassExecutor::getPass(size_t index) const
	{
		return index < m_passes.size() ? m_passes[index] : nullptr;
	}

	void PassExecutor::execute(const PassContext& context, const ExecutionHooks* hooks) const
	{
		m_executionBufferStates.clear();
		m_executionTextureStates.clear();

		for (const TextureBinding& binding : m_textureBindings)
		{
			m_executionTextureStates.push_back(TextureExecutionState{
				.key = toHandleKey(binding.handle),
				.state = TextureUsageState{
					.stages = rhi::StageFlags::none,
					.hazards = rhi::HazardFlags::none,
					.access = ResourceAccess::read,
					.state = binding.initialState,
				},
			});
		}

		for (uint32_t passIndex = 0; passIndex < m_passes.size(); ++passIndex)
		{
			const PassNode* pass = m_passes[passIndex];
			assert(pass != nullptr);

			if (hooks != nullptr)
			{
				hooks->beforePass(context, *pass, passIndex);
			}

			const PassNode::HandleSlice<PassResourceDependency> dependencies = pass->getDependencies();
			for (uint32_t i = 0; i < dependencies.count; ++i)
			{
				const PassResourceDependency& dependency = dependencies.data[i];
				if (dependency.type == PassResourceType::buffer)
				{
					const BufferBinding* binding = findBufferBinding(dependency.bufferHandle);
					if (binding == nullptr || binding->backendBufferToken == 0)
					{
						continue;
					}

					const uint64_t key = toHandleKey(dependency.bufferHandle);
					BufferUsageState* previousState = findBufferExecutionState(key);
					if (previousState != nullptr && requiresBarrier(previousState->access, dependency.access) && context
						.commandBuffer != nullptr)
					{
						// Wave 7 dual-barrier model: buffer producer->consumer sync goes through the
						// stage-barrier main path (global VkMemoryBarrier2). Buffers carry no image
						// layout, so a memory barrier is sufficient; the hazard covers both shader
						// buffer writes and indirect-argument reads.
						context.commandBuffer->barrier(previousState->stages,
						                               dependencyStages(dependency),
						                               previousState->hazards | dependencyHazards(dependency));
					}

					if (previousState == nullptr)
					{
						m_executionBufferStates.push_back(BufferExecutionState{.key = key});
						previousState = &m_executionBufferStates.back().state;
					}
					previousState->stages = dependencyStages(dependency);
					previousState->hazards = dependencyHazards(dependency);
					previousState->access = dependency.access;
					continue;
				}

				const TextureBinding* binding = findTextureBinding(dependency.textureHandle);
				if (binding == nullptr || binding->backendImageToken == 0)
				{
					continue;
				}

				const uint64_t key = toHandleKey(dependency.textureHandle);
				TextureUsageState* textureState = findTextureExecutionState(key);
				if (textureState == nullptr)
				{
					m_executionTextureStates.push_back(TextureExecutionState{
						.key = key,
						.state = TextureUsageState{
							.stages = rhi::StageFlags::none,
							.hazards = rhi::HazardFlags::none,
							.access = ResourceAccess::read,
							.state = binding->initialState,
						},
					});
					textureState = &m_executionTextureStates.back().state;
				}

				const rhi::ResourceState requiredState = resolveRequiredTextureState(*binding, dependency);
				const rhi::ResourceState currentState = textureState->state;

				const bool needsMemoryBarrier = requiresBarrier(textureState->access, dependency.access);
				if (needsMemoryBarrier && context.commandBuffer != nullptr)
				{
					context.commandBuffer->barrier(textureState->stages,
					                               dependencyStages(dependency),
					                               textureState->hazards | dependencyHazards(dependency));
				}

				if (requiresResourceBoundary(currentState, requiredState))
				{
					if (context.commandBuffer != nullptr && !binding->rhiTexture.isNull())
					{
						const rhi::TextureBarrier textureBarrier{
							.texture = binding->rhiTexture,
							.before = currentState,
							.after = requiredState,
							.range = {
								.aspect = binding->aspect,
								.baseMipLevel = 0,
								.levelCount = ~0u,
								.baseArrayLayer = 0,
								.layerCount = ~0u
							},
						};
						context.commandBuffer->resourceBarrier(&textureBarrier, 1, nullptr, 0);
					}
					else
					{
						// Contract failure: attachment not mirrored into the registry; explicit
						// resource boundary requires a resource table.
						ASSERT(false,
						       "PassExecutor explicit texture boundaries require registry-backed RHI texture handles");
					}
				}

				textureState->stages = dependencyStages(dependency);
				textureState->hazards = dependencyHazards(dependency);
				textureState->access = dependency.access;
				textureState->state = requiredState;
			}

			PassContext scopedContext = context;
			scopedContext.passIndex = passIndex;

			demo::profiling::ScopedCpuRange passCpuRange(pass->getName());
			ScopedCommandEvent passCommandEvent(context.commandBuffer, pass->getName());
			pass->execute(scopedContext);

			if (hooks != nullptr)
			{
				hooks->afterPass(scopedContext, *pass, passIndex);
			}
		}
	}
} // namespace demo
