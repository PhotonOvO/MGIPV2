#pragma once

#include "Pass.h"
#include "../rhi/RHITypes.h"

#include <cstddef>
#include <vector>

namespace demo::rhi
{
	class Device;
}

namespace demo
{
	class PassExecutor
	{
	public:
		struct ExecutionHooks
		{
			virtual ~ExecutionHooks() = default;

			virtual void beforePass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
			{
				(void)context;
				(void)pass;
				(void)passIndex;
			}

			virtual void afterPass(const PassContext& context, const PassNode& pass, uint32_t passIndex) const
			{
				(void)context;
				(void)pass;
				(void)passIndex;
			}
		};

		struct TextureBinding
		{
			TextureHandle handle{};
			uint64_t backendImageToken{0};
			rhi::TextureAspect aspect{rhi::TextureAspect::color};
			rhi::ResourceState initialState{rhi::ResourceState::general};
			bool isSwapchain{false};
			// Backend registry handle mirroring backendImageToken, so explicit resourceBarrier
			// boundaries can resolve pass attachments by RHI handle.
			rhi::TextureHandle rhiTexture{};
		};

		struct BufferBinding
		{
			BufferHandle handle{};
			uint64_t backendBufferToken{0};
		};

		void clear();
		void addPass(const PassNode& pass);
		// Optional: when set, bindTexture mirrors each native image into the backend
		// registry so explicit resource boundaries can use TextureHandles.
		void setResourceTable(rhi::Device* device);
		void clearResourceBindings();
		void bindTexture(TextureBinding binding);
		void bindBuffer(BufferBinding binding);
		[[nodiscard]] size_t getPassCount() const;
		[[nodiscard]] const PassNode* getPass(size_t index) const;
		// Resolvable RHI handle mirroring a bound pass attachment (null if unbound
		// or no resource table was set). Used by present to blit through the registry.
		[[nodiscard]] rhi::TextureHandle getTextureRHIHandle(TextureHandle handle) const;
		// Mirror an externally owned backend image into the registry and return a cached
		// resolvable TextureHandle. Lets passes express explicit layout boundaries through
		// cmdBuffer->resourceBarrier without holding native handles. Cache is keyed by backend
		// image and cleared with the other resource bindings (rebuilt on resize).
		[[nodiscard]] rhi::TextureHandle resolveBarrierTexture(uint64_t backendImageToken) const;
		void execute(const PassContext& context, const ExecutionHooks* hooks = nullptr) const;

	private:
		struct BufferUsageState
		{
			rhi::StageFlags stages{rhi::StageFlags::none};
			rhi::HazardFlags hazards{rhi::HazardFlags::none};
			ResourceAccess access{ResourceAccess::read};
		};

		struct TextureUsageState
		{
			rhi::StageFlags stages{rhi::StageFlags::none};
			rhi::HazardFlags hazards{rhi::HazardFlags::none};
			ResourceAccess access{ResourceAccess::read};
			rhi::ResourceState state{rhi::ResourceState::Undefined};
		};

		struct BufferExecutionState
		{
			uint64_t key{0};
			BufferUsageState state{};
		};

		struct TextureExecutionState
		{
			uint64_t key{0};
			TextureUsageState state{};
		};

		[[nodiscard]] const TextureBinding* findTextureBinding(TextureHandle handle) const;
		[[nodiscard]] const BufferBinding* findBufferBinding(BufferHandle handle) const;
		[[nodiscard]] BufferUsageState* findBufferExecutionState(uint64_t key) const;
		[[nodiscard]] TextureUsageState* findTextureExecutionState(uint64_t key) const;

		std::vector<const PassNode*> m_passes;
		std::vector<TextureBinding> m_textureBindings;
		std::vector<BufferBinding> m_bufferBindings;
		rhi::Device* m_device{nullptr};
		// owned=false native-image -> handle cache for pass-driven resourceBarrier. Small
		// (a handful of attachments); linear scan avoids a hashmap on the recording path.
		mutable std::vector<std::pair<uint64_t, rhi::TextureHandle>> m_barrierTextureCache;
		// Per-execute dependency records used only to translate pass declarations into
		// barriers. These are not a legacy per-draw resource-state tracker.
		mutable std::vector<BufferExecutionState> m_executionBufferStates;
		mutable std::vector<TextureExecutionState> m_executionTextureStates;
	};
} // namespace demo
