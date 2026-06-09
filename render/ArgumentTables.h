#pragma once

#include "../common/Handles.h"
#include "../rhi/RHIBindlessTypes.h"
#include "../rhi/RHITypes.h"

#include <cstdint>

namespace demo
{
	enum class ArgumentSlot : uint32_t
	{
		passGlobals = 0,
		material = 1,
		scene = material,
		drawDynamic = 2,
		shaderSpecific = 3,
	};

	inline constexpr uint32_t kArgumentSlotCount = 4;
	inline constexpr rhi::ResourceIndex kMaterialBindlessTexturesIndex = 0;
	inline constexpr rhi::ResourceIndex kSceneBindlessInfoIndex = 0;

	enum class RootBindingSlot : uint32_t
	{
		primaryConstants = 0,
	};

	inline constexpr uint32_t kPrimaryRootConstantsSlot = static_cast<uint32_t>(RootBindingSlot::primaryConstants);
	inline constexpr uint32_t kSceneDynamicBufferTableSlot = static_cast<uint32_t>(ArgumentSlot::scene);
	inline constexpr uint32_t kSceneDynamicBufferBinding = 0;

	static_assert(static_cast<uint32_t>(ArgumentSlot::passGlobals) == 0, "ArgumentSlot::passGlobals must stay set 0");
	static_assert(static_cast<uint32_t>(ArgumentSlot::material) == 1, "ArgumentSlot::material must stay set 1");
	static_assert(static_cast<uint32_t>(ArgumentSlot::drawDynamic) == 2, "ArgumentSlot::drawDynamic must stay set 2");
	static_assert(static_cast<uint32_t>(ArgumentSlot::shaderSpecific) == 3,
	              "ArgumentSlot::shaderSpecific must stay set 3");

	[[nodiscard]] inline constexpr bool isStableArgumentSlot(ArgumentSlot slot)
	{
		const uint32_t index = static_cast<uint32_t>(slot);
		return index < kArgumentSlotCount;
	}

	[[nodiscard]] inline constexpr bool isStableRootBindingSlot(RootBindingSlot slot)
	{
		return slot == RootBindingSlot::primaryConstants;
	}

	struct ArgumentLayoutEntry
	{
		uint32_t binding{0};
		rhi::ShaderStage visibility{rhi::ShaderStage::none};
		rhi::BindlessResourceType type{rhi::BindlessResourceType::sampledTexture};
		uint32_t count{1};
	};

	struct ArgumentLayoutDesc
	{
		const ArgumentLayoutEntry* entries{nullptr};
		uint32_t entryCount{0};
	};

	// Renderer-local metadata for an RHI ArgumentTable and its stable shader set slot.
	struct ArgumentTableDesc
	{
		ArgumentSlot slot{ArgumentSlot::shaderSpecific};
		rhi::ArgumentLayoutHandle layout{};
		rhi::ArgumentTableHandle table{};
		rhi::ResourceIndex primaryLogicalIndex{rhi::kInvalidResourceIndex};
		const char* debugName{"argument-table"};
	};
} // namespace demo
