#pragma once

#include "Pass.h"
#include "../rhi/RHITypes.h"

#include <cstddef>
#include <optional>
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
		enum class MissingBindingPolicy : uint8_t
		{
			ignore = 0,
			skipPass,
		};

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
			rhi::TextureHandle rhiTexture{};
			rhi::TextureAspect aspect{rhi::TextureAspect::color};
			rhi::ResourceState initialState{rhi::ResourceState::General};
			bool isSwapchain{false};

			bool operator==(const TextureBinding&) const = default;
		};

		struct BufferBinding
		{
			BufferHandle handle{};
			rhi::BufferHandle rhiBuffer{};

			bool operator==(const BufferBinding&) const = default;
		};

		void clear();
		void addPass(
			const PassNode& pass,
			MissingBindingPolicy missingBindingPolicy = MissingBindingPolicy::ignore);
		[[nodiscard]] bool setPassEnabled(const PassNode& pass, bool enabled);
		[[nodiscard]] bool isPassEnabled(const PassNode& pass) const;
		[[nodiscard]] bool setMissingBindingPolicy(
			const PassNode& pass, MissingBindingPolicy policy);
		[[nodiscard]] MissingBindingPolicy getMissingBindingPolicy(
			const PassNode& pass) const;
		// Optional: when set, bindTexture mirrors each native image into the backend
		// registry so explicit resource boundaries can use TextureHandles.
		void setResourceTable(rhi::Device* device);
		void clearResourceBindings();
		void bindTexture(TextureBinding binding);
		void bindBuffer(BufferBinding binding);
		[[nodiscard]] std::optional<TextureBinding> getTextureBinding(TextureHandle handle) const;
		[[nodiscard]] std::optional<BufferBinding> getBufferBinding(BufferHandle handle) const;
		[[nodiscard]] bool hasBoundDependencies(const PassNode& pass) const;
		[[nodiscard]] size_t getPassCount() const;
		[[nodiscard]] const PassNode* getPass(size_t index) const;
		// Resolvable RHI handle mirroring a bound pass attachment (null if unbound
		// or no resource table was set). Used by present to blit through the registry.
		[[nodiscard]] rhi::TextureHandle getTextureRHIHandle(TextureHandle handle) const;

		void execute(const PassContext& context, const ExecutionHooks* hooks = nullptr) const;

	private:
		struct PassEntry
		{
			const PassNode* pass{nullptr};
			bool enabled{true};
			MissingBindingPolicy missingBindingPolicy{MissingBindingPolicy::ignore};
		};

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

		[[nodiscard]] PassEntry* findPassEntry(const PassNode& pass);
		[[nodiscard]] const PassEntry* findPassEntry(const PassNode& pass) const;

		std::vector<PassEntry> m_passes;
		std::vector<TextureBinding> m_textureBindings;
		std::vector<BufferBinding> m_bufferBindings;
		rhi::Device* m_device{nullptr};
		// owned=false native-image -> handle cache for pass-driven resourceBarrier. Small
		// (a handful of attachments); linear scan avoids a hashmap on the recording path.
		// Per-execute dependency records used only to translate pass declarations into
		// barriers. These are not a legacy per-draw resource-state tracker.
		mutable std::vector<BufferExecutionState> m_executionBufferStates;
		mutable std::vector<TextureExecutionState> m_executionTextureStates;
	};
} // namespace demo
