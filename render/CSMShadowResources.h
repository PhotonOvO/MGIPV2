#pragma once

#include "../common/Handles.h"
#include "../rhi/RHICommandBuffer.h"
#include "../rhi/RHIDevice.h"
#include "../rhi/RHITypes.h"
#include "ClipSpaceConvention.h"
#include "ShaderInterop.h"

#include <array>
#include <cassert>
#include <glm/glm.hpp>

namespace demo
{
	class CSMShadowResources
	{
	public:
		struct CascadeData
		{
			float splitNear{0.0f};
			float splitFar{0.0f};
			float texelSize{0.0f};
			float receiverRadius{0.0f};
			float nearPlane{0.0f};
			float farPlane{0.0f};
			glm::vec3 receiverCenter{0.0f};
			glm::vec3 lightPosition{0.0f};
			glm::mat4 lightView{1.0f};
			glm::mat4 lightProjection{1.0f};
			glm::mat4 viewProjection{1.0f};
			glm::mat4 cullingViewProjection{1.0f};
			glm::mat4 worldToShadowTexture{1.0f};
			glm::vec3 receiverMinLightSpace{0.0f};
			glm::vec3 receiverMaxLightSpace{0.0f};
			glm::vec3 casterMinLightSpace{0.0f};
			glm::vec3 casterMaxLightSpace{0.0f};
			std::array<glm::vec3, 8> receiverCornersWorld{};
			std::array<glm::vec4, shaderio::LGPUCullingFrustumPlaneCount> cullingPlanes{};
		};

		struct FrameData
		{
			std::array<CascadeData, shaderio::LCascadeCount> cascades{};
			uint32_t cascadeCount{0};
			glm::vec4 splitDistances{0.0f};
			glm::vec3 lightDirection{0.0f, -1.0f, 0.0f};
			float maxShadowDistance{0.0f};
			glm::vec3 casterBoundsMin{0.0f};
			glm::vec3 casterBoundsMax{0.0f};
			bool casterBoundsValid{false};
		};

		struct CreateInfo
		{
			uint32_t cascadeCount{4};
			uint32_t cascadeResolution{1024}; // Per cascade
			rhi::TextureFormat shadowFormat{rhi::TextureFormat::d32Sfloat};
			clipspace::ProjectionConvention projectionConvention{
				clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)
			};
		};

		CSMShadowResources() = default;
		~CSMShadowResources() { assert(m_device == nullptr && "Missing deinit()"); }

		void init(rhi::Device& device, rhi::CommandBuffer& cmd, const CreateInfo& createInfo);
		void deinit();

		void updateCascadeMatrices(const shaderio::CameraUniforms& camera, const glm::vec3& lightDir);
		void updateCascadeMatrices(const shaderio::CameraUniforms& camera,
		                           const glm::vec3& lightDir,
		                           float maxShadowDistance);
		void updateCascadeMatrices(const shaderio::CameraUniforms& camera,
		                           const glm::vec3& lightDir,
		                           float maxShadowDistance,
		                           const glm::vec3& casterBoundsMin,
		                           const glm::vec3& casterBoundsMax,
		                           bool casterBoundsValid);

		// Texture2DArray image access (all cascades). The sampling array view + per-cascade
		// render-target views are owned by the RHI texture-view registry (RenderDevice), not here.
		[[nodiscard]] rhi::TextureHandle getCascadeImage() const { return m_cascadeArray; }

		// Per-cascade render-target views are owned by the RHI texture-view registry
		// (RenderDevice::getCSMCascadeViewHandle), not by this subsystem.

		[[nodiscard]] uint32_t getCascadeCount() const { return m_cascadeCount; }
		[[nodiscard]] uint32_t getCascadeResolution() const { return m_cascadeResolution; }
		[[nodiscard]] rhi::TextureFormat getShadowFormat() const { return m_shadowFormat; }

		[[nodiscard]] rhi::Extent2D getCascadeExtent() const
		{
			return {m_cascadeResolution, m_cascadeResolution};
		}

		// Uniform buffer access
		[[nodiscard]] rhi::BufferHandle getShadowUniformBuffer() const { return m_shadowUniformBuffer; }
		[[nodiscard]] shaderio::ShadowUniforms* getShadowUniformsData() { return &m_shadowUniformsData; }
		[[nodiscard]] const shaderio::ShadowUniforms* getShadowUniformsData() const { return &m_shadowUniformsData; }
		[[nodiscard]] const FrameData& getFrameData() const { return m_frameData; }

		[[nodiscard]] const CascadeData& getCascadeData(uint32_t cascadeIndex) const
		{
			assert(cascadeIndex < m_cascadeCount);
			return m_frameData.cascades[cascadeIndex];
		}

	private:
		rhi::Device* m_device{nullptr};
		clipspace::ProjectionConvention m_projectionConvention{
			clipspace::getProjectionConvention(clipspace::BackendConvention::vulkan)
		};

		rhi::TextureHandle m_cascadeArray{}; // Texture2DArray (arrayLayers = cascadeCount)

		rhi::BufferHandle m_shadowUniformBuffer{};
		shaderio::ShadowUniforms m_shadowUniformsData{};
		FrameData m_frameData{};
		void* m_shadowUniformMapped{nullptr};

		uint32_t m_cascadeCount{4};
		uint32_t m_cascadeResolution{1024};
		rhi::TextureFormat m_shadowFormat{rhi::TextureFormat::d32Sfloat};
	};
} // namespace demo
