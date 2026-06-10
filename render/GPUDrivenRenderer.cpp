#include "GPUDrivenRenderer.h"
#include "ArgumentTables.h"
#include "BatchUploadContext.h"
#include "UploadUtils.h"
#include "RHIFormatBridge.h"
#include "../loader/Ktx2Loader.h"
#include "../rhi/vulkan/VulkanDevice.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <span>

namespace demo
{
	namespace
	{
		constexpr bool kEnableExperimentalMeshletPath = true;
		constexpr bool kEnableShippingVisibilitySort = true;
		constexpr uint32_t kDebugCullSegmentCount = 24u;
		constexpr uint32_t kLightCoarseCullingThreadCount = 64u;
		constexpr uint32_t kVisibilitySortCategoryMask = 0xc0000000u;
		constexpr uint32_t kVisibilitySortKeyMask = 0x3fffffffu;
		constexpr uint32_t kVisibilitySortCategoryOpaque = 0x00000000u;
		constexpr uint32_t kVisibilitySortCategoryAlpha = 0x40000000u;
		constexpr uint32_t kVisibilitySortCategoryTransparent = 0x80000000u;
		constexpr uint32_t kMobileMaxTransparentDraws = 2048u;
		constexpr float kGPUDrivenHiZDepthEpsilon = 2.0e-3f;
		constexpr float kGPUDrivenHiZConservativeRadiusScale = 1.1f;
		constexpr float kGPUDrivenHiZConservativeRadiusBias = 0.1f;
		constexpr float kGPUDrivenHiZNearRejectEpsilon = 1.0e-4f;
		constexpr float kGPUDrivenHiZLargeObjectFootprintThreshold = 192.0f;
		constexpr float kGPUDrivenHiZFastCameraFallbackDistance = 8.0f;
		constexpr uint32_t kGPUDrivenLightPassTextureCount = kPackedGBufferTargetCount + 18u;
		constexpr uint32_t kGPUDrivenLightPassDepthTextureIndex = kPackedGBufferTargetCount;
		constexpr uint32_t kGPUDrivenLightPassSceneColorHdrIndex = kPackedGBufferTargetCount + 1u;
		constexpr uint32_t kGPUDrivenLightPassBloomHalfIndex = kPackedGBufferTargetCount + 2u;
		constexpr uint32_t kGPUDrivenLightPassBloomQuarterIndex = kPackedGBufferTargetCount + 3u;
		constexpr uint32_t kGPUDrivenLightPassVelocityIndex = kPackedGBufferTargetCount + 4u;
		constexpr uint32_t kGPUDrivenLightPassHistoryReadIndex = kPackedGBufferTargetCount + 5u;
		constexpr uint32_t kGPUDrivenLightPassHistoryWriteIndex = kPackedGBufferTargetCount + 6u;
		constexpr uint32_t kGPUDrivenLightPassIBLEnvironmentIndex = kPackedGBufferTargetCount + 7u;
		constexpr uint32_t kGPUDrivenLightPassAOIndex = kPackedGBufferTargetCount + 8u;
		constexpr uint32_t kGPUDrivenLightPassSSRIndex = kPackedGBufferTargetCount + 9u;
		constexpr uint32_t kGPUDrivenLightPassBloomEighthIndex = kPackedGBufferTargetCount + 10u;
		constexpr uint32_t kGPUDrivenLightPassBloomSixteenthIndex = kPackedGBufferTargetCount + 11u;
		constexpr uint32_t kGPUDrivenLightPassBloomThirtySecondIndex = kPackedGBufferTargetCount + 12u;
		constexpr uint32_t kGPUDrivenLightPassBloomUpsampleSixteenthIndex = kPackedGBufferTargetCount + 13u;
		constexpr uint32_t kGPUDrivenLightPassBloomUpsampleEighthIndex = kPackedGBufferTargetCount + 14u;
		constexpr uint32_t kGPUDrivenLightPassBloomUpsampleQuarterIndex = kPackedGBufferTargetCount + 15u;
		constexpr uint32_t kGPUDrivenLightPassBloomOutputIndex = kPackedGBufferTargetCount + 16u;
		constexpr uint32_t kGPUDrivenLightPassColorGradingLutIndex = kPackedGBufferTargetCount + 17u;
		constexpr VkFormat kGPUDrivenAOFormat = VK_FORMAT_R16_SFLOAT;
		constexpr VkFormat kGPUDrivenSSRFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
		constexpr VkFormat kGPUDrivenShadowAtlasFormat = VK_FORMAT_D32_SFLOAT;

		uint64_t estimateImageBytes(VkExtent2D extent, uint32_t bytesPerPixel)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * bytesPerPixel;
		}

		uint64_t estimateImageBytes(rhi::Extent2D extent, uint32_t bytesPerPixel)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * bytesPerPixel;
		}

		constexpr const char* kGPUDrivenDefaultIBLEnvironmentPath = "resources/environment/lilienstein_4k.ktx2";

		uint32_t bytesPerPixelForFormat(VkFormat format)
		{
			switch (format)
			{
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
				return 4u;
			case VK_FORMAT_R16G16_SFLOAT:
				return 4u;
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return 8u;
			default:
				return 0u;
			}
		}

		uint32_t bytesPerPixelForFormat(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::bgra8Unorm:
			case rhi::TextureFormat::rgba8Unorm:
				return 4u;
			case rhi::TextureFormat::rg16Sfloat:
				return 4u;
			case rhi::TextureFormat::rgba16Sfloat:
				return 8u;
			default:
				return 0u;
			}
		}

		uint64_t estimateTextureBytes(VkExtent2D extent, VkFormat format)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height)
				* static_cast<uint64_t>(bytesPerPixelForFormat(format));
		}

		uint64_t estimateTextureBytes(rhi::Extent2D extent, rhi::TextureFormat format)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height)
				* static_cast<uint64_t>(bytesPerPixelForFormat(format));
		}

		VkExtent2D toVkExtent(rhi::Extent2D extent)
		{
			return {extent.width, extent.height};
		}

		uint64_t resolveNativeTexture(rhi::Device& device, rhi::TextureHandle handle)
		{
			if (handle.isNull())
				return 0u;
			auto& interop = static_cast<const rhi::vulkan::VulkanDeviceInterop&>(
			    static_cast<const rhi::vulkan::VulkanDevice&>(device));
			return reinterpret_cast<uintptr_t>(interop.resolveTexture(handle));
		}

		const char* formatDisplayName(VkFormat format)
		{
			switch (format)
			{
			case VK_FORMAT_B8G8R8A8_UNORM:
				return "B8G8R8A8_UNORM";
			case VK_FORMAT_R8G8B8A8_UNORM:
				return "R8G8B8A8_UNORM";
			case VK_FORMAT_R8G8B8A8_SRGB:
				return "R8G8B8A8_SRGB";
			case VK_FORMAT_R16G16B16A16_SFLOAT:
				return "R16G16B16A16_SFLOAT";
			case VK_FORMAT_UNDEFINED:
				return "Undefined";
			default:
				return "Other";
			}
		}

		const char* formatDisplayName(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::bgra8Unorm:
				return "B8G8R8A8_UNORM";
			case rhi::TextureFormat::rgba8Unorm:
				return "R8G8B8A8_UNORM";
			case rhi::TextureFormat::rgba16Sfloat:
				return "R16G16B16A16_SFLOAT";
			case rhi::TextureFormat::rg16Sfloat:
				return "R16G16_SFLOAT";
			case rhi::TextureFormat::undefined:
				return "Undefined";
			default:
				return "Other";
			}
		}

		glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback)
		{
			const float length = glm::length(value);
			return length > 1.0e-5f ? value / length : fallback;
		}

		glm::mat4 resolveSceneLightTransform(const SceneLight& light,
		                                     std::span<const SceneNode> sceneNodes,
		                                     std::span<const GltfNodeData> gltfNodes)
		{
			if (light.nodeIndex >= 0)
			{
				const size_t nodeIndex = static_cast<size_t>(light.nodeIndex);
				if (nodeIndex < sceneNodes.size())
				{
					return sceneNodes[nodeIndex].worldTransform;
				}
				if (nodeIndex < gltfNodes.size())
				{
					return gltfNodes[nodeIndex].worldTransform;
				}
			}
			return glm::mat4(1.0f);
		}

		glm::vec3 sceneLightTravelDirection(const glm::mat4& transform)
		{
			return safeNormalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)),
			                     glm::vec3(-0.45f, -0.8f, -0.25f));
		}

		float sceneLightEffectiveRange(const SceneLight& light, const GPUDrivenSceneView& sceneView)
		{
			if (light.range > 0.0f)
			{
				return light.range;
			}
			if (sceneView.sceneBoundsValid)
			{
				return std::max(glm::length(sceneView.sceneBoundsMax - sceneView.sceneBoundsMin) * 1.25f, 4.0f);
			}
			return 32.0f;
		}

		float halton(uint64_t index, uint32_t base)
		{
			float result = 0.0f;
			float fraction = 1.0f / static_cast<float>(base);
			while (index > 0)
			{
				result += static_cast<float>(index % base) * fraction;
				index /= base;
				fraction /= static_cast<float>(base);
			}
			return result;
		}

		uint32_t buildGPUDrivenFlags(const MeshRecord& mesh)
		{
			uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
			if (mesh.alphaMode == shaderio::LAlphaBlend)
			{
				flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
			}
			else if (mesh.alphaMode == shaderio::LAlphaMask)
			{
				flags |= shaderio::LGPUCullFlagAlphaMask;
			}
			return flags;
		}

		uint32_t buildMeshletGPUDrivenFlags(const MeshRecord& mesh)
		{
			uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
			if (mesh.alphaMode == shaderio::LAlphaBlend)
			{
				flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
			}
			else if (mesh.alphaMode == shaderio::LAlphaMask)
			{
				flags |= shaderio::LGPUCullFlagAlphaMask;
			}
			return flags;
		}

		glm::vec4 transformBoundsSphere(const glm::mat4& transform, const glm::vec4& localSphere)
		{
			const glm::vec3 worldCenter = glm::vec3(transform * glm::vec4(glm::vec3(localSphere), 1.0f));
			const float scaleX = glm::length(glm::vec3(transform[0]));
			const float scaleY = glm::length(glm::vec3(transform[1]));
			const float scaleZ = glm::length(glm::vec3(transform[2]));
			const float maxScale = std::max(scaleX, std::max(scaleY, scaleZ));
			return glm::vec4(worldCenter, std::max(localSphere.w * maxScale, 0.0f));
		}

		glm::vec4 computeTransformedBoundsSphere(const MeshRecord& meshRecord, const glm::mat4& transform)
		{
			const glm::vec3 localMin = meshRecord.localBoundsMin;
			const glm::vec3 localMax = meshRecord.localBoundsMax;
			const glm::vec3 corners[8] = {
				{localMin.x, localMin.y, localMin.z},
				{localMax.x, localMin.y, localMin.z},
				{localMin.x, localMax.y, localMin.z},
				{localMax.x, localMax.y, localMin.z},
				{localMin.x, localMin.y, localMax.z},
				{localMax.x, localMin.y, localMax.z},
				{localMin.x, localMax.y, localMax.z},
				{localMax.x, localMax.y, localMax.z},
			};

			glm::vec3 worldMin = glm::vec3(transform * glm::vec4(corners[0], 1.0f));
			glm::vec3 worldMax = worldMin;
			for (uint32_t i = 1; i < 8; ++i)
			{
				const glm::vec3 worldCorner = glm::vec3(transform * glm::vec4(corners[i], 1.0f));
				worldMin = glm::min(worldMin, worldCorner);
				worldMax = glm::max(worldMax, worldCorner);
			}

			const glm::vec3 center = (worldMin + worldMax) * 0.5f;
			const float radius = glm::length(worldMax - center);
			return glm::vec4(center, radius);
		}

		void includeBoundsSphere(glm::vec3& boundsMin, glm::vec3& boundsMax, bool& boundsValid, const glm::vec4& sphere)
		{
			if (sphere.w <= 0.0f)
			{
				return;
			}

			const glm::vec3 center = glm::vec3(sphere);
			const glm::vec3 extent(sphere.w);
			const glm::vec3 sphereMin = center - extent;
			const glm::vec3 sphereMax = center + extent;
			boundsMin = boundsValid ? glm::min(boundsMin, sphereMin) : sphereMin;
			boundsMax = boundsValid ? glm::max(boundsMax, sphereMax) : sphereMax;
			boundsValid = true;
		}

		void includeMeshBounds(glm::vec3& boundsMin, glm::vec3& boundsMax, bool& boundsValid, const MeshRecord& mesh)
		{
			boundsMin = boundsValid ? glm::min(boundsMin, mesh.worldBoundsMin) : mesh.worldBoundsMin;
			boundsMax = boundsValid ? glm::max(boundsMax, mesh.worldBoundsMax) : mesh.worldBoundsMax;
			boundsValid = true;
		}

		utils::Buffer createHostVisibleStorageBuffer(VkDevice device, VmaAllocator allocator, uint64_t size)
		{
			const VkBufferUsageFlags2CreateInfoKHR usageInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR,
				.usage = VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR | VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR
				| VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR,
			};

			const VkBufferCreateInfo bufferInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.pNext = &usageInfo,
				.size = size,
				.usage = 0,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};

			VmaAllocationCreateInfo allocInfo{};
			allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

			utils::Buffer buffer{};
			VmaAllocationInfo allocationInfo{};
			VK_CHECK(
				vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer.buffer, &buffer.allocation, &allocationInfo
				));
			buffer.mapped = allocationInfo.pMappedData;

			const VkBufferDeviceAddressInfo addressInfo{
				.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
				.buffer = buffer.buffer,
			};
			buffer.address = vkGetBufferDeviceAddress(device, &addressInfo);
			return buffer;
		}

		utils::Buffer createDeviceLocalStorageBuffer(VkDevice device, VmaAllocator allocator, uint64_t size)
		{
			const upload::NativeUploadContext context{
				.device = reinterpret_cast<uintptr_t>(device),
				.allocator = reinterpret_cast<uintptr_t>(allocator),
			};
			const upload::NativeUploadBuffer buffer =
				upload::createStaticBuffer(context,
				                           size,
				                           VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT_KHR |
				                           VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT_KHR
				                           | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT_KHR);
			return utils::Buffer{
				.buffer = reinterpret_cast<VkBuffer>(buffer.buffer),
				.allocation = reinterpret_cast<VmaAllocation>(buffer.allocation),
			};
		}

		utils::Buffer toUtilsBuffer(const upload::NativeUploadBuffer& buffer)
		{
			return utils::Buffer{
				.buffer = reinterpret_cast<VkBuffer>(buffer.buffer),
				.allocation = reinterpret_cast<VmaAllocation>(buffer.allocation),
			};
		}

		void destroyBuffer(VmaAllocator allocator, utils::Buffer& buffer)
		{
			if (buffer.buffer != VK_NULL_HANDLE)
			{
				vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
				buffer = {};
			}
		}

		uint32_t nextPowerOfTwo(uint32_t value)
		{
			if (value <= 1u)
			{
				return 1u;
			}

			--value;
			value |= value >> 1u;
			value |= value >> 2u;
			value |= value >> 4u;
			value |= value >> 8u;
			value |= value >> 16u;
			return value + 1u;
		}

		uint32_t computeBitonicSortPassCount(uint32_t visibleCount)
		{
			uint32_t passes = 0;
			for (uint32_t width = 2; width <= std::max(visibleCount, 1u); width <<= 1u)
			{
				for (uint32_t stride = width >> 1u; stride > 0; stride >>= 1u)
				{
					++passes;
				}
			}
			return passes;
		}

		uint32_t encodeSortableFloatKey(float value)
		{
			uint32_t bits = 0u;
			static_assert(sizeof(bits) == sizeof(value));
			std::memcpy(&bits, &value, sizeof(bits));
			return bits;
		}

		uint32_t encodeVisibilitySortKey(uint32_t categoryValue, uint32_t subKey)
		{
			return categoryValue | (subKey & kVisibilitySortKeyMask);
		}

		constexpr uint32_t kMaxReasonableGPUDrivenObjectCount = 1u << 20;

		[[nodiscard]] rhi::TextureAspect sceneDepthAspect(VkFormat format)
		{
			switch (format)
			{
			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return rhi::TextureAspect::depthStencil;
			default:
				return rhi::TextureAspect::depth;
			}
		}
	} // namespace

	void GPUDrivenRenderer::init(void* window, rhi::Surface& surface, bool vSync)
	{
		m_renderer.init(window, surface, vSync);
		m_sceneRegistry.init(getBackendDeviceToken(),
		                     getAllocatorToken(),
		                     &m_renderer.getRHIDevice());
		initLightingResources();
		initIBLResources();
		m_enableExperimentalMeshletPath = kEnableExperimentalMeshletPath;
		if (m_enableExperimentalMeshletPath)
		{
			m_meshletBuffer.init(getBackendDeviceToken(),
			                     getAllocatorToken(),
			                     &m_renderer.getRHIDevice());
		}
		m_hiZDepthPyramid.init(getRHIDevice(), getSwapchainImageCount(), getSceneExtent());

		m_depthPrepass = std::make_unique<GPUDrivenDepthPrepass>(this);
		m_depthPyramidPass = std::make_unique<GPUDrivenDepthPyramidPass>(this);
		m_gpuCullingPass = std::make_unique<GPUDrivenCullingPass>(this);
		if (kEnableShippingVisibilitySort)
		{
			m_visibilitySortPass = std::make_unique<GPUDrivenVisibilitySortPass>(this);
		}
		m_lightCullingPass = std::make_unique<GPUDrivenLightCullingPass>(this);
		m_clusteredLightCullingPass = std::make_unique<GPUDrivenClusteredLightCullingPass>(this);
		m_csmShadowPass = std::make_unique<GPUDrivenCSMShadowPass>(this);
		m_shadowAtlasPass = std::make_unique<GPUDrivenShadowAtlasPass>(this);
		m_gbufferPass = std::make_unique<GPUDrivenGBufferPass>(this);
		m_lightPass = std::make_unique<GPUDrivenLightPass>(this);
		m_skyboxPass = std::make_unique<GPUDrivenSkyboxPass>(this);
		m_aoPass = std::make_unique<GPUDrivenAOPass>(this);
		m_ssrPass = std::make_unique<GPUDrivenSSRPass>(this);
		m_forwardPass = std::make_unique<GPUDrivenForwardPass>(this);
		m_velocityPass = std::make_unique<GPUDrivenVelocityPass>(this);
		m_taaResolvePass = std::make_unique<GPUDrivenTAAResolvePass>(this);
		m_bloomPrefilterPass = std::make_unique<GPUDrivenBloomPrefilterPass>(this);
		m_bloomDownsamplePass = std::make_unique<GPUDrivenBloomDownsamplePass>(this);
		m_finalColorPass = std::make_unique<GPUDrivenFinalColorPass>(this);
		m_presentPass = std::make_unique<GPUDrivenPresentPass>(this);
		m_imguiPass = std::make_unique<GPUDrivenImguiPass>(this);

		m_passExecutor.clear();
		m_passExecutor.addPass(*m_depthPrepass);
		m_passExecutor.addPass(*m_depthPyramidPass);
		m_passExecutor.addPass(*m_gpuCullingPass);
		if (m_visibilitySortPass != nullptr)
		{
			m_passExecutor.addPass(*m_visibilitySortPass);
		}
		m_passExecutor.addPass(*m_lightCullingPass);
		m_passExecutor.addPass(*m_clusteredLightCullingPass);
		m_passExecutor.addPass(*m_csmShadowPass);
		m_passExecutor.addPass(*m_shadowAtlasPass);
		m_passExecutor.addPass(*m_gbufferPass);
		m_passExecutor.addPass(*m_aoPass);
		m_passExecutor.addPass(*m_ssrPass);
		m_passExecutor.addPass(*m_lightPass);
		m_passExecutor.addPass(*m_skyboxPass);
		m_passExecutor.addPass(*m_forwardPass);
		m_passExecutor.addPass(*m_velocityPass);
		m_passExecutor.addPass(*m_taaResolvePass);
		m_passExecutor.addPass(*m_bloomPrefilterPass);
		m_passExecutor.addPass(*m_bloomDownsamplePass);
		m_passExecutor.addPass(*m_finalColorPass);
		m_passExecutor.addPass(*m_presentPass);
		m_passExecutor.addPass(*m_imguiPass);
		bindStaticPassResources();
		m_passExecutor.bindTexture({
			.handle = kPassDepthPyramidHandle,
			.backendImageToken = resolveNativeTexture(getRHIDevice(), m_hiZDepthPyramid.getImageHandle()),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
		initTransparentVisibilityPatchResources();
		initPhase7Resources();
		bindPhase7PassResources();
		m_sortedBootstrapFrames.assign(std::max(1u, getSwapchainImageCount()), SortedBootstrapFrameState{});
		if (kEnableShippingVisibilitySort)
		{
			initVisibilitySortResources();
		}
		initLightingPipelines();
		initPhase7Pipelines();
	}

	void GPUDrivenRenderer::shutdown(rhi::Surface& surface)
	{
		shutdownLightingPipelines();
		shutdownPhase7Pipelines();
		if (kEnableShippingVisibilitySort)
		{
			shutdownVisibilitySortResources();
		}
		shutdownTransparentVisibilityPatchResources();
		shutdownPhase7Resources();
		m_sortedBootstrapFrames.clear();
		m_passExecutor.clear();
		m_imguiPass.reset();
		m_presentPass.reset();
		m_taaResolvePass.reset();
		m_velocityPass.reset();
		m_forwardPass.reset();
		m_ssrPass.reset();
		m_aoPass.reset();
		m_skyboxPass.reset();
		m_lightPass.reset();
		m_gbufferPass.reset();
		m_shadowAtlasPass.reset();
		m_csmShadowPass.reset();
		m_lightCullingPass.reset();
		m_clusteredLightCullingPass.reset();
		if (m_visibilitySortPass != nullptr)
		{
			m_visibilitySortPass.reset();
		}
		m_gpuCullingPass.reset();
		m_depthPyramidPass.reset();
		m_depthPrepass.reset();
		if (m_enableExperimentalMeshletPath)
		{
			m_meshletBuffer.deinit();
		}
		m_hiZDepthPyramid.shutdown();
		shutdownIBLResources();
		shutdownLightingResources();
		m_sceneRegistry.deinit();
		m_renderer.shutdown(surface);
	}

	void GPUDrivenRenderer::resize(rhi::Extent2D size)
	{
		m_renderer.resize(size);
		m_hiZDepthPyramid.resize(getSceneExtent());
		resizePhase7Resources();
		m_passExecutor.clearResourceBindings();
		bindStaticPassResources();
		m_passExecutor.bindTexture({
			.handle = kPassDepthPyramidHandle,
			.backendImageToken = resolveNativeTexture(getRHIDevice(), m_hiZDepthPyramid.getImageHandle()),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
		bindPhase7PassResources();
	}

	void GPUDrivenRenderer::render(const RenderParams& params)
	{
		const bool sceneRenderingSuspended = m_suspendSceneRendering;
		{
			m_hiZDepthPyramid.resize(getSceneExtent());
		}
		{
			flushPendingSceneUploads();
		}
		{
			refreshSceneView();
		}
		{
			uploadPersistentDrawData();
		}
		const uint32_t safeObjectCount = getSafePersistentObjectCount();
		const shaderio::GPUCullDrawCounts cachedDrawCounts = getLastGPUCullingDrawCounts();
		const uint32_t visibleCount = cachedDrawCounts.totalCount;
		m_runtimeStats.batchStats.visibleCount = visibleCount;
		m_runtimeStats.batchStats.batchCount =
			static_cast<uint32_t>((cachedDrawCounts.opaqueCount > 0u ? 1u : 0u)
				+ (cachedDrawCounts.alphaTestCount > 0u ? 1u : 0u)
				+ (cachedDrawCounts.transparentCount > 0u ? 1u : 0u));
		m_runtimeStats.batchStats.sortPassCount = 0u;
		const uint32_t frameIndex = getCurrentFrameIndexHint();
		if (params.cameraUniforms != nullptr)
		{
			getCSMShadowResources().updateCascadeMatrices(*params.cameraUniforms,
			                                              params.lightSettings.direction,
			                                              params.lightSettings.shadowDistance,
			                                              m_sceneView.sceneBoundsMin,
			                                              m_sceneView.sceneBoundsMax,
			                                              m_sceneView.sceneBoundsValid);
		}
		updateGPUDrivenLights(params, frameIndex);
		m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
		m_runtimeStats.visibilityDiagnostics = GPUDrivenVisibilityDiagnostics{
			.safeObjectCount = safeObjectCount,
			.currentGPUCullingObjectCount = getGPUCullingObjectCount(frameIndex),
			.previousGPUCullingObjectCount = getPreviousGPUCullingObjectCount(frameIndex),
			.opaqueCapacity = static_cast<uint32_t>(m_opaqueDrawIndices.size()),
			.alphaCapacity = static_cast<uint32_t>(m_alphaTestDrawIndices.size()),
			.transparentCapacity = static_cast<uint32_t>(m_transparentDrawIndices.size()),
			.maxMobileTransparentDraws = kMobileMaxTransparentDraws,
			.transparentOrderingCpuSeeded = !m_transparentDrawIndices.empty(),
			.materialSortKeysCpuSeeded = true,
			.transparentCapacityOverflow = m_transparentDrawIndices.size() > kMobileMaxTransparentDraws,
		};
		if (kEnableShippingVisibilitySort && !sceneRenderingSuspended)
		{
			{
				if (m_cachedStaticVisibilitySortTopologyVersion != m_sceneTopologyVersion)
				{
					m_cachedStaticVisibilitySortObjects.clear();
					m_cachedStaticVisibilitySortKeys.clear();
					const size_t staticSortInputCount = m_opaqueDrawIndices.size() + m_alphaTestDrawIndices.size();
					m_cachedStaticVisibilitySortObjects.reserve(staticSortInputCount);
					m_cachedStaticVisibilitySortKeys.reserve(staticSortInputCount);
					const auto appendStaticDraws = [&](std::span<const uint32_t> drawIndices, uint32_t categoryValue)
					{
						for (uint32_t drawIndex : drawIndices)
						{
							MeshHandle meshHandle = kNullMeshHandle;
							if (!tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
							{
								continue;
							}

							const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
							if (mesh == nullptr)
							{
								continue;
							}

							const uint32_t subKey =
								mesh->materialIndex >= 0
									? std::min(static_cast<uint32_t>(mesh->materialIndex), kVisibilitySortKeyMask)
									: kVisibilitySortKeyMask;
							m_cachedStaticVisibilitySortObjects.push_back(drawIndex);
							m_cachedStaticVisibilitySortKeys.push_back(encodeVisibilitySortKey(categoryValue, subKey));
						}
					};
					appendStaticDraws(m_opaqueDrawIndices, kVisibilitySortCategoryOpaque);
					appendStaticDraws(m_alphaTestDrawIndices, kVisibilitySortCategoryAlpha);
					m_cachedStaticVisibilitySortTopologyVersion = m_sceneTopologyVersion;
				}

				const glm::vec3 cameraPos =
					params.cameraUniforms != nullptr ? params.cameraUniforms->cameraPosition : glm::vec3(0.0f);
				const size_t totalSortInputCount = m_cachedStaticVisibilitySortObjects.size() + m_transparentDrawIndices
					.size();
				m_visibilitySortInputObjects.clear();
				m_visibilitySortInputKeys.clear();
				m_visibilitySortInputObjects.reserve(totalSortInputCount);
				m_visibilitySortInputKeys.reserve(totalSortInputCount);
				m_visibilitySortInputObjects.insert(m_visibilitySortInputObjects.end(),
				                                    m_cachedStaticVisibilitySortObjects.begin(),
				                                    m_cachedStaticVisibilitySortObjects.end());
				m_visibilitySortInputKeys.insert(m_visibilitySortInputKeys.end(),
				                                 m_cachedStaticVisibilitySortKeys.begin(),
				                                 m_cachedStaticVisibilitySortKeys.end());
				const auto appendTransparentDraws = [&](std::span<const uint32_t> drawIndices)
				{
					for (uint32_t drawIndex : drawIndices)
					{
						MeshHandle meshHandle = kNullMeshHandle;
						if (!tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
						{
							continue;
						}

						const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
						if (mesh == nullptr)
						{
							continue;
						}

						const glm::vec3 meshCenter = glm::vec3(mesh->transform[3]);
						const float distanceSquared = glm::dot(meshCenter - cameraPos, meshCenter - cameraPos);
						const uint32_t subKey = encodeSortableFloatKey(distanceSquared) >> 2u;

						m_visibilitySortInputObjects.push_back(drawIndex);
						m_visibilitySortInputKeys.push_back(
							encodeVisibilitySortKey(kVisibilitySortCategoryTransparent, subKey));
					}
				};
				appendTransparentDraws(m_transparentDrawIndices);
				m_runtimeStats.batchStats.sortPassCount =
					computeBitonicSortPassCount(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
				m_runtimeStats.visibilityDiagnostics.sortInputCount =
					static_cast<uint32_t>(m_visibilitySortInputObjects.size());
				m_runtimeStats.visibilityDiagnostics.sortPaddedCount =
					m_visibilitySortInputObjects.empty()
						? 0u
						: nextPowerOfTwo(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
			}
			{
				prepareVisibilitySortInputs(frameIndex);
			}
			if (frameIndex < m_visibilitySortFrames.size())
			{
				const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
				m_passExecutor.bindBuffer({
					.handle = kPassGPUDrivenSortKeyBufferHandle,
					.backendBufferToken = reinterpret_cast<uint64_t>(sortResources.keyBuffer.buffer),
				});
				m_passExecutor.bindBuffer({
					.handle = kPassGPUDrivenSortValueBufferHandle,
					.backendBufferToken = reinterpret_cast<uint64_t>(sortResources.valueBuffer.buffer),
				});
			}
		}
		else
		{
			m_visibilitySortInputObjects.clear();
			m_visibilitySortInputKeys.clear();
			m_passExecutor.bindBuffer({
				.handle = kPassGPUDrivenSortKeyBufferHandle,
				.backendBufferToken = 0,
			});
			m_passExecutor.bindBuffer({
				.handle = kPassGPUDrivenSortValueBufferHandle,
				.backendBufferToken = 0,
			});
		}

		RenderParams gpuParams = params;
		m_previousTAAJitterUv = m_currentTAAJitterUv;
		m_currentTAAJitterUv = glm::vec2(0.0f);
		if (params.cameraUniforms != nullptr)
		{
			const shaderio::CameraUniforms unjitteredCamera = *params.cameraUniforms;
			if (!m_previousCameraValid)
			{
				m_previousCameraUniforms = unjitteredCamera;
				m_previousJitteredViewProjection = unjitteredCamera.viewProjection;
			}

			m_temporalCameraUniforms = unjitteredCamera;
			m_temporalCameraUniforms.prevView = m_previousCameraUniforms.view;
			m_temporalCameraUniforms.prevProjection = m_previousCameraUniforms.projection;
			m_temporalCameraUniforms.prevViewProjection = m_previousCameraUniforms.viewProjection;
			m_temporalCameraUniforms.unjitteredViewProjection = unjitteredCamera.viewProjection;
			m_temporalCameraUniforms.unjitteredInverseViewProjection = unjitteredCamera.inverseViewProjection;
			m_temporalCameraUniforms.prevUnjitteredViewProjection = m_previousCameraUniforms.viewProjection;
			m_temporalCameraUniforms.prevJitteredViewProjection = m_previousJitteredViewProjection;
			const rhi::Extent2D temporalExtent = getSceneExtent();
			if (gpuParams.debugOptions.enablePostProcessing && gpuParams.debugOptions.enableTAA
				&& temporalExtent.width > 0u && temporalExtent.height > 0u)
			{
				const uint64_t jitterIndex = m_temporalFrameCounter + 1u;
				const glm::vec2 jitter = (glm::vec2(halton(jitterIndex, 2u), halton(jitterIndex, 3u)) - glm::vec2(0.5f))
					* (2.0f * gpuParams.debugOptions.taaJitterScale);
				const glm::vec2 jitterNdc = glm::vec2(jitter.x / static_cast<float>(temporalExtent.width),
				                                      jitter.y / static_cast<float>(temporalExtent.height));
				m_currentTAAJitterUv = jitterNdc * 0.5f;
				m_temporalCameraUniforms.projection[2][0] += jitterNdc.x;
				m_temporalCameraUniforms.projection[2][1] += jitterNdc.y;
				m_temporalCameraUniforms.viewProjection = m_temporalCameraUniforms.projection * m_temporalCameraUniforms
					.view;
				m_temporalCameraUniforms.inverseViewProjection = glm::inverse(m_temporalCameraUniforms.viewProjection);
			}
			gpuParams.cameraUniforms = &m_temporalCameraUniforms;
		}
		{
			if (isMeshletRenderingActive())
			{
				gpuParams.debugOptions.enableGPUOcclusionCulling = params.debugOptions.enableGPUMeshletOcclusionCulling;
			}
			m_lastHiZCameraDeltaDistance = 0.0f;
			m_lastHiZFastCameraFallbackTriggered = false;
			if (params.cameraUniforms != nullptr)
			{
				const glm::vec3 cameraPosition = params.cameraUniforms->cameraPosition;
				if (m_hiZCameraHistoryValid)
				{
					m_lastHiZCameraDeltaDistance = glm::length(cameraPosition - m_lastHiZCameraPosition);
					m_lastHiZFastCameraFallbackTriggered =
						m_lastHiZCameraDeltaDistance > kGPUDrivenHiZFastCameraFallbackDistance;
					if (m_lastHiZFastCameraFallbackTriggered)
					{
						gpuParams.debugOptions.enableGPUOcclusionCulling = false;
					}
				}
				m_lastHiZCameraPosition = cameraPosition;
				m_hiZCameraHistoryValid = true;
			}
			gpuParams.gpuDrivenSceneView =
				(!sceneRenderingSuspended && m_sceneView.usePersistentCullingObjects) ? &m_sceneView : nullptr;
			if (sceneRenderingSuspended || gpuParams.gpuDrivenSceneView != nullptr)
			{
				gpuParams.gltfModel = nullptr;
			}
			const uint32_t historyReadIndex = (frameIndex + 1u) & 1u;
			const uint32_t historyWriteIndex = frameIndex & 1u;
			m_sceneView.sceneColorHistoryReadImage = getSceneColorHistoryImage(historyReadIndex);
			m_sceneView.sceneColorHistoryReadView = getSceneColorHistoryView(historyReadIndex);
			m_sceneView.sceneColorHistoryWriteImage = getSceneColorHistoryImage(historyWriteIndex);
			m_sceneView.sceneColorHistoryWriteView = getSceneColorHistoryView(historyWriteIndex);
			updatePhase7Descriptors(frameIndex);
			m_passExecutor.bindTexture({
				.handle = kPassVelocityHandle,
				.backendImageToken = resolveNativeTexture(getRHIDevice(), m_sceneView.velocityImage),
				.aspect = rhi::TextureAspect::color,
				.initialState = rhi::ResourceState::General,
				.isSwapchain = false,
			});
			m_passExecutor.bindTexture({
				.handle = kPassSceneColorHistoryReadHandle,
				.backendImageToken = resolveNativeTexture(getRHIDevice(), m_sceneView.sceneColorHistoryReadImage),
				.aspect = rhi::TextureAspect::color,
				.initialState = rhi::ResourceState::General,
				.isSwapchain = false,
			});
			m_passExecutor.bindTexture({
				.handle = kPassSceneColorHistoryWriteHandle,
				.backendImageToken = resolveNativeTexture(getRHIDevice(), m_sceneView.sceneColorHistoryWriteImage),
				.aspect = rhi::TextureAspect::color,
				.initialState = rhi::ResourceState::General,
				.isSwapchain = false,
			});
		}
		{
			submitPassGraph(gpuParams);
		}
		const rhi::ArgumentTableHandle gpuCullingArgumentTable = getGPUCullingArgumentTable(frameIndex);
		{
			m_sceneView.depthPyramidImage = m_hiZDepthPyramid.getImageHandle();
			m_sceneView.depthPyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
			m_sceneView.depthPyramidMipCount = m_hiZDepthPyramid.getMipCount();
			m_sceneView.depthPyramidSourceDepth = m_hiZDepthPyramid.getSourceDepth();
			m_sceneView.depthPyramidGeneration = m_hiZDepthPyramid.getGenerationCount();
			m_sceneView.depthPyramidValid = m_hiZDepthPyramid.isValid();
			m_runtimeStats.indirectDrawCount = m_runtimeStats.batchStats.visibleCount;
			m_runtimeStats.indirectCommandStride = getGPUCullingIndirectCommandStride();
			m_runtimeStats.ownsFullRenderChain = true;
			m_runtimeStats.hiZGeneration = m_hiZDepthPyramid.getGenerationCount();
			m_runtimeStats.ownsHiZVisibilityChain =
				m_hiZDepthPyramid.isValid()
				&& m_hiZDepthPyramid.getSourceDepth().index == kPassSceneDepthHandle.index
				&& m_hiZDepthPyramid.getSourceDepth().generation == kPassSceneDepthHandle.generation
				&& m_hiZDepthPyramid.getLastBoundArgumentTable().index == gpuCullingArgumentTable.index
				&& m_hiZDepthPyramid.getLastBoundArgumentTable().generation == gpuCullingArgumentTable.generation
				&& m_hiZDepthPyramid.getLastBoundBinding() == 5;
			const HiZDepthPyramid::MobilePolicy& hiZPolicy = m_hiZDepthPyramid.getMobilePolicy();
			const rhi::Extent2D hiZSourceExtent = m_hiZDepthPyramid.getSourceExtent();
			const rhi::Extent2D hiZPyramidExtent = m_hiZDepthPyramid.getExtent();
			m_runtimeStats.hiZDiagnostics = GPUDrivenHiZDiagnostics{
				.sourceWidth = hiZSourceExtent.width,
				.sourceHeight = hiZSourceExtent.height,
				.pyramidWidth = hiZPyramidExtent.width,
				.pyramidHeight = hiZPyramidExtent.height,
				.mipCount = m_hiZDepthPyramid.getMipCount(),
				.fullMipCount = m_hiZDepthPyramid.getFullMipCount(),
				.policyDownsampleDivisor = hiZPolicy.downsampleDivisor,
				.policyMaxMipCount = hiZPolicy.maxMipCount,
				.policyMinMipSize = hiZPolicy.minMipSize,
				.estimatedMemoryBytes = m_hiZDepthPyramid.getEstimatedMemoryBytes(),
				.generation = m_hiZDepthPyramid.getGenerationCount(),
				.valid = m_hiZDepthPyramid.isValid(),
				.boundForGpuCulling = m_runtimeStats.ownsHiZVisibilityChain,
				.frustumCullingEnabled = gpuParams.debugOptions.enableGPUFrustumCulling,
				.occlusionCullingEnabled = gpuParams.debugOptions.enableGPUOcclusionCulling,
				.meshletOcclusionEnabled = gpuParams.debugOptions.enableGPUMeshletOcclusionCulling,
				.meshletConeCullingEnabled = gpuParams.debugOptions.enableGPUMeshletConeCulling,
				.depthEpsilon = kGPUDrivenHiZDepthEpsilon,
				.conservativeRadiusScale = kGPUDrivenHiZConservativeRadiusScale,
				.conservativeRadiusBias = kGPUDrivenHiZConservativeRadiusBias,
				.nearRejectEpsilon = kGPUDrivenHiZNearRejectEpsilon,
				.largeObjectFootprintThreshold = kGPUDrivenHiZLargeObjectFootprintThreshold,
				.fastCameraFallbackDistance = kGPUDrivenHiZFastCameraFallbackDistance,
				.cameraDeltaDistance = m_lastHiZCameraDeltaDistance,
				.fastCameraFallbackTriggered = m_lastHiZFastCameraFallbackTriggered,
			};
			const rhi::Extent2D postExtent = getSceneExtent();
			const rhi::TextureFormat outputFormat = getOutputTextureFormat();
			const rhi::TextureFormat sceneColorFormat = getSceneColorHdrFormat();
			constexpr rhi::TextureFormat kRecommendedMobileHdrFormat = rhi::TextureFormat::rgba16Sfloat;
			m_runtimeStats.postProcessDiagnostics = GPUDrivenPostProcessDiagnostics{
				.displayWidth = getSwapchainExtent().width,
				.displayHeight = getSwapchainExtent().height,
				.internalWidth = postExtent.width,
				.internalHeight = postExtent.height,
				.outputWidth = postExtent.width,
				.outputHeight = postExtent.height,
				.outputFormat = outputFormat,
				.sceneColorFormat = sceneColorFormat,
				.recommendedHdrFormat = kRecommendedMobileHdrFormat,
				.outputFormatName = formatDisplayName(outputFormat),
				.sceneColorFormatName = formatDisplayName(sceneColorFormat),
				.recommendedHdrFormatName = formatDisplayName(kRecommendedMobileHdrFormat),
				.outputMemoryBytes = getOutputTextureEstimatedBytes(),
				.sceneColorMemoryBytes = getSceneColorHdrEstimatedBytes(),
				.recommendedHdrMemoryBytes = estimateTextureBytes(postExtent, kRecommendedMobileHdrFormat),
				.bloomHalfQuarterMemoryBytes = getBloomEstimatedBytes(),
				.fixedExposure = gpuParams.debugOptions.postExposure,
				.adaptiveExposureTarget = gpuParams.debugOptions.exposureTargetLuminance,
				.minAutoExposure = gpuParams.debugOptions.minAutoExposure,
				.maxAutoExposure = gpuParams.debugOptions.maxAutoExposure,
				.bloomIntensity = gpuParams.debugOptions.bloomIntensity,
				.bloomThreshold = gpuParams.debugOptions.bloomThreshold,
				.colorSaturation = gpuParams.debugOptions.colorSaturation,
				.colorContrast = gpuParams.debugOptions.colorContrast,
				.colorGamma = gpuParams.debugOptions.colorGamma,
				.colorLutStrength = gpuParams.debugOptions.colorLutStrength,
				.vignetteIntensity = gpuParams.debugOptions.vignetteIntensity,
				.lensDirtIntensity = gpuParams.debugOptions.lensDirtIntensity,
				.taaBlendWeight = gpuParams.debugOptions.taaBlendWeight,
				.taaJitterScale = gpuParams.debugOptions.taaJitterScale,
				.renderScale = gpuParams.debugOptions.renderScale,
				.upscaleMode = static_cast<uint32_t>(std::max(0, gpuParams.debugOptions.upscalingMode)),
				.hdrSceneColorActive = sceneColorFormat == kRecommendedMobileHdrFormat
				&& !m_sceneView.sceneColorHdrImage.isNull()
				&& !getGPUDrivenLightHdrPipelineHandle().isNull(),
				.mobileHdrRecommended = true,
				.toneMapInLightPass = false,
				.finalColorPassActive = !getFinalColorPipelineHandle().isNull(),
				.velocityBufferActive = !m_sceneView.velocityImage.isNull()
				&& !getVelocityPipelineHandle().isNull(),
				.taaPassActive = gpuParams.debugOptions.enablePostProcessing
				&& gpuParams.debugOptions.enableTAA
				&& !getTAAResolvePipelineHandle().isNull(),
				.taaHistoryValid = m_taaHistoryValid,
				.temporalUpscalingActive = false,
				.internalRenderScaleBlocked = gpuParams.debugOptions.renderScale < 0.999f,
				.exposurePassActive = gpuParams.debugOptions.enablePostProcessing,
				.adaptiveExposureActive = gpuParams.debugOptions.enablePostProcessing
				&& gpuParams.debugOptions.enableAdaptiveExposure,
				.bloomPassActive = gpuParams.debugOptions.enableBloom
				&& gpuParams.debugOptions.enablePostProcessing
				&& !getBloomPrefilterPipelineHandle().isNull()
				&& !getBloomDownsamplePipelineHandle().isNull()
				&& !getBloomUpsamplePipelineHandle().isNull(),
				.colorGradingLutActive = gpuParams.debugOptions.enablePostProcessing
				&& gpuParams.debugOptions.enableColorGrading
				&& gpuParams.debugOptions.colorLutStrength > 0.0f
				&& !m_sceneView.colorGradingLutView.isNull(),
				.lensEffectsActive = gpuParams.debugOptions.enablePostProcessing
				&& gpuParams.debugOptions.enableLensEffects,
			};
			const rhi::Extent2D iblExtent = getIBLEnvironmentExtent();
			m_runtimeStats.iblDiagnostics = GPUDrivenIBLDiagnostics{
				.width = iblExtent.width,
				.height = iblExtent.height,
				.mipCount = getIBLEnvironmentMipCount(),
				.format = getIBLEnvironmentFormat(),
				.formatName = formatDisplayName(getIBLEnvironmentFormat()),
				.estimatedMemoryBytes = getIBLEnvironmentEstimatedBytes(),
				.intensity = gpuParams.debugOptions.iblIntensity,
				.enabled = gpuParams.debugOptions.enableIBL,
				.loaded = getIBLEnvironmentLoaded(),
				.fallback = getIBLUsingFallback(),
				.irradianceReady = m_iblResources.isSplitSumReady(),
				.prefilteredReady = m_iblResources.isSplitSumReady(),
				.brdfLutReady = m_iblResources.isInitialized(),
				.splitSumReady = m_iblResources.isSplitSumReady(),
				.debugMode = gpuParams.debugOptions.iblDebugMode,
				.path = getIBLEnvironmentPath(),
				.sourceMode = m_iblResources.isSplitSumReady()
					              ? "split_sum"
					              : (getIBLEnvironmentLoaded() ? "equirect_fallback" : "flat_ambient"),
				.status = getIBLEnvironmentStatus(),
			};
			m_lightResources.cacheClusterStats(frameIndex);
			const GPUDrivenLightResources::Diagnostics lightDiagnostics = m_lightResources.getDiagnostics();
			m_runtimeStats.clusteredLightingDiagnostics = GPUDrivenClusteredLightingDiagnostics{
				.gridX = lightDiagnostics.clusterGridX,
				.gridY = lightDiagnostics.clusterGridY,
				.gridZ = lightDiagnostics.clusterGridZ,
				.clusterCount = lightDiagnostics.clusterCount,
				.maxLightsPerCluster = lightDiagnostics.maxLightsPerCluster,
				.activePointLights = lightDiagnostics.activePointLights,
				.activeSpotLights = lightDiagnostics.activeSpotLights,
				.maxPointLights = lightDiagnostics.maxPointLights,
				.maxSpotLights = lightDiagnostics.maxSpotLights,
				.maxOccupancy = lightDiagnostics.maxOccupancy,
				.overflowClusterCount = lightDiagnostics.overflowClusterCount,
				.appendedLightReferences = lightDiagnostics.appendedLightReferences,
				.clusterMemoryBytes = lightDiagnostics.clusterMemoryBytes,
				.lightMemoryBytes = lightDiagnostics.lightMemoryBytes,
				.enabled = gpuParams.debugOptions.enableClusteredLighting,
				.resourcesOwned = lightDiagnostics.initialized,
				.descriptorsReady = lightDiagnostics.lightingDescriptorsReady && lightDiagnostics.
				clusteredDescriptorsReady,
				.fallbackActive = !gpuParams.debugOptions.enableClusteredLighting || !lightDiagnostics.
				clusteredDescriptorsReady,
			};
			const uint64_t aoBytes = estimateImageBytes(m_phase7HalfExtent, 2u) * 2u;
			const uint64_t ssrBytes = estimateImageBytes(m_phase7HalfExtent, 8u);
			m_runtimeStats.aoReflectionDiagnostics = GPUDrivenAOReflectionDiagnostics{
				.aoWidth = m_phase7HalfExtent.width,
				.aoHeight = m_phase7HalfExtent.height,
				.ssrWidth = m_phase7HalfExtent.width,
				.ssrHeight = m_phase7HalfExtent.height,
				.estimatedMemoryBytes = aoBytes + ssrBytes,
				.aoEnabled = gpuParams.debugOptions.enableAO,
				.aoReady = !m_aoDenoisedView.isNull() && !m_gtaoPipelineHandle.isNull() && !
				m_aoDenoisePipelineHandle.isNull(),
				.ssrEnabled = gpuParams.debugOptions.enableSSR,
				.ssrReady = !m_ssrRawView.isNull() && !m_ssrTracePipelineHandle.isNull(),
			};
			const uint32_t shadowAtlasTileSize = std::max(1u, m_shadowAtlasTileSize);
			const uint32_t shadowAtlasCapacity =
				(m_shadowAtlasExtent.width / shadowAtlasTileSize) * (m_shadowAtlasExtent.height / shadowAtlasTileSize);
			CSMShadowResources& shadowAtlasCsm = getCSMShadowResources();
			const bool shadowAtlasReady = !m_shadowAtlasView.isNull() && !m_shadowAtlasImage.isNull()
				&& !getCSMShadowPipelineHandle().isNull();
			const bool shadowAtlasHasScene = m_sceneView.usePersistentCullingObjects
				&& m_sceneView.shadowPackedMeshes != nullptr
				&& m_sceneView.shadowPackedMeshCount > 0
				&& m_sceneView.shadowPackedVertexBuffer != 0
				&& m_sceneView.shadowPackedIndexBuffer != 0;
			const uint32_t shadowAtlasExpectedTiles =
				gpuParams.debugOptions.enableShadowAtlas && shadowAtlasReady && shadowAtlasHasScene
					? std::min(shadowAtlasCsm.getCascadeCount(), shadowAtlasCapacity)
					: 0u;
			const std::string shadowAtlasStatus =
				!gpuParams.debugOptions.enableShadowAtlas
					? "Disabled; using CSM fallback"
					: (!shadowAtlasReady
						   ? "Enabled, but atlas image or CSM shadow pipeline is missing; using CSM fallback"
						   : (!shadowAtlasHasScene
							      ? "Enabled and allocated; no packed shadow casters this frame; using CSM fallback"
							      : "Rendering cascade tiles into GPUDriven shadow atlas; lighting still samples CSM fallback"));
			m_shadowAtlasAllocatedTiles = shadowAtlasExpectedTiles;
			m_runtimeStats.shadowAtlasDiagnostics = GPUDrivenShadowAtlasDiagnostics{
				.atlasWidth = m_shadowAtlasExtent.width,
				.atlasHeight = m_shadowAtlasExtent.height,
				.tileSize = shadowAtlasTileSize,
				.tileCapacity = shadowAtlasCapacity,
				.allocatedTiles = shadowAtlasExpectedTiles,
				.estimatedMemoryBytes = estimateImageBytes(m_shadowAtlasExtent, 4u),
				.enabled = gpuParams.debugOptions.enableShadowAtlas,
				.ready = shadowAtlasReady,
				.fallbackToCSM = true,
				.status = shadowAtlasStatus,
			};
			if (m_sceneView.usePersistentCullingObjects
				&& getGPUCullingObjectCount(frameIndex) == safeObjectCount && safeObjectCount > 0u)
			{
				m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
			}
			updateOwnershipDiagnostics(frameIndex, sceneRenderingSuspended, safeObjectCount);
		}
		if (params.cameraUniforms != nullptr)
		{
			m_previousCameraUniforms = *params.cameraUniforms;
			m_previousJitteredViewProjection = m_temporalCameraUniforms.viewProjection;
			m_previousCameraValid = true;
		}
		m_taaHistoryValid = gpuParams.debugOptions.enablePostProcessing && gpuParams.debugOptions.enableTAA
			&& !getTAAResolvePipelineHandle().isNull();
		++m_temporalFrameCounter;
	}

	void GPUDrivenRenderer::executeDepthPyramidPass(rhi::CommandBuffer& cmdBuffer, const RenderParams&)
	{
		const uint32_t frameIndex = getCurrentFrameIndexHint();
		const rhi::TextureViewHandle sourceDepthView = m_sceneView.sceneDepthView;
		const rhi::TextureHandle sourceDepthRHI = m_passExecutor.getTextureRHIHandle(kPassSceneDepthHandle);
		m_hiZDepthPyramid.generate(frameIndex,
		                           cmdBuffer,
		                           m_sceneView.sceneDepthExtent,
		                           sourceDepthView,
		                           kPassSceneDepthHandle,
		                           sourceDepthRHI);
		const rhi::ArgumentTableHandle gpuCullingArgumentTable = getGPUCullingArgumentTable(frameIndex);
		if (!gpuCullingArgumentTable.isNull())
		{
			m_renderer.updateGPUCullingDepthPyramidArgumentTable(frameIndex,
			                                                     m_hiZDepthPyramid.getMipViewsData(),
			                                                     m_hiZDepthPyramid.getMipCount());
			m_hiZDepthPyramid.markBoundForCulling(gpuCullingArgumentTable, 5);
		}
		m_passExecutor.bindTexture({
			.handle = kPassDepthPyramidHandle,
			.backendImageToken = resolveNativeTexture(getRHIDevice(), m_hiZDepthPyramid.getImageHandle()),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
	}

	GltfUploadResult GPUDrivenRenderer::uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd)
	{
		GltfUploadResult result = m_renderer.uploadGltfModel(model, cmd);
		m_activeUploadResultStorage = result;
		rebuildGPUDrivenScene(model, m_activeUploadResultStorage, cmd);
		return result;
	}

	SceneUploadResult GPUDrivenRenderer::commitSceneUploadPlan(const SceneAsset& asset,
	                                                           const SceneUploadPlan& plan,
	                                                           rhi::CommandBuffer& cmd)
	{
		SceneUploadResult result = m_renderer.commitSceneUploadPlan(asset, plan, cmd);
		m_activeUploadResultStorage = result;
		rebuildGPUDrivenScene(asset, plan, m_activeUploadResultStorage, cmd);
		return result;
	}

	void GPUDrivenRenderer::uploadGltfModelBatch(const GltfModel& model,
	                                             std::span<const uint32_t> textureIndices,
	                                             std::span<const uint32_t> materialIndices,
	                                             std::span<const uint32_t> meshIndices,
	                                             GltfUploadResult& ioResult,
	                                             rhi::CommandBuffer& cmd)
	{
		m_renderer.uploadGltfModelBatch(model, textureIndices, materialIndices, meshIndices, ioResult, cmd);
		m_activeUploadResultStorage = ioResult;
		rebuildGPUDrivenScene(model, m_activeUploadResultStorage, cmd);
	}

	void GPUDrivenRenderer::destroyGltfResources(const GltfUploadResult& result)
	{
		clearGPUDrivenScene();
		m_activeUploadResultStorage = {};
		m_activeUploadResult = nullptr;
		m_renderer.destroyGltfResources(result);
	}

	void GPUDrivenRenderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
	{
		const uint64_t meshKey = packMeshHandleKey(handle);
		if (const MeshRecord* previousMeshRecord = m_renderer.getMeshPool().tryGet(handle))
		{
			m_previousTransformByMeshHandle[meshKey] = previousMeshRecord->transform;
		}
		m_renderer.updateMeshTransform(handle, transform);

		uint32_t drawIndex = 0;
		const bool hasDrawIndex = tryGetMeshDrawIndex(handle, drawIndex);
		const auto objectIdsIt = m_objectIdsByMeshHandle.find(meshKey);
		const auto objectIdIt = m_objectIdByMeshHandle.find(meshKey);
		if (objectIdsIt == m_objectIdsByMeshHandle.end() && objectIdIt == m_objectIdByMeshHandle.end())
		{
			return;
		}

		const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(handle);
		if (meshRecord == nullptr)
		{
			return;
		}

		const glm::vec4 boundsSphere(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
		if (objectIdsIt != m_objectIdsByMeshHandle.end())
		{
			for (const uint32_t objectId : objectIdsIt->second)
			{
				m_sceneRegistry.updateTransform(objectId, transform, boundsSphere);
			}
		}
		else
		{
			m_sceneRegistry.updateTransform(objectIdIt->second, transform, boundsSphere);
		}
		if (m_enableExperimentalMeshletPath && !m_meshletCullObjectsCpu.empty())
		{
			for (uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size()); ++drawIndex)
			{
				if (packMeshHandleKey(m_meshHandleByDrawIndex[drawIndex]) != packMeshHandleKey(handle)
					|| drawIndex >= m_meshletDataCpu.size() || drawIndex >= m_meshletCullObjectsCpu.size())
				{
					continue;
				}
				m_meshletCullObjectsCpu[drawIndex].sphereCenterRadius =
					transformBoundsSphere(meshRecord->transform, m_meshletDataCpu[drawIndex].boundsSphere);
				markPersistentDrawDirty(drawIndex);
			}
		}
		m_sceneUploadPending = true;
		if (!m_enableExperimentalMeshletPath)
		{
			bool markedDraws = false;
			for (uint32_t candidateDrawIndex = 0; candidateDrawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.
				     size());
			     ++candidateDrawIndex)
			{
				if (packMeshHandleKey(m_meshHandleByDrawIndex[candidateDrawIndex]) == meshKey)
				{
					markPersistentDrawDirty(candidateDrawIndex);
					markedDraws = true;
				}
			}
			if (!markedDraws && hasDrawIndex)
			{
				markPersistentDrawDirty(drawIndex);
			}
		}
		else
		{
			m_persistentDrawDataDirty = true;
		}
		m_runtimeStats.pendingSceneUpdates = 1;
		refreshSceneView();
	}

	void GPUDrivenRenderer::updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform)
	{
		if (m_activeUploadResult == nullptr || instanceIndex >= m_activeUploadResult->instanceToDrawRecord.size())
		{
			return;
		}

		const uint32_t drawRecordIndex = m_activeUploadResult->instanceToDrawRecord[instanceIndex];
		if (drawRecordIndex == UINT32_MAX || drawRecordIndex >= m_sceneDrawRecords.size())
		{
			return;
		}

		SceneUploadResult::SceneDrawRecord& drawRecord = m_sceneDrawRecords[drawRecordIndex];
		const MeshHandle meshHandle = drawRecord.meshHandle;
		if (meshHandle.isNull())
		{
			return;
		}

		const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
		if (meshRecord == nullptr)
		{
			return;
		}

		const glm::mat4 previousTransform = drawRecord.worldTransform;
		drawRecord.worldTransform = transform;
		const glm::vec4 boundsSphere = computeTransformedBoundsSphere(*meshRecord, transform);

		const uint32_t objectId =
			drawRecordIndex < m_objectIdByDrawRecord.size() ? m_objectIdByDrawRecord[drawRecordIndex] : UINT32_MAX;
		const uint32_t drawIndex =
			drawRecordIndex < m_drawIndexByDrawRecord.size() ? m_drawIndexByDrawRecord[drawRecordIndex] : UINT32_MAX;
		if (objectId != UINT32_MAX)
		{
			m_sceneRegistry.updateTransform(objectId, transform, boundsSphere);
		}
		if (drawIndex != UINT32_MAX)
		{
			m_previousTransformByDrawIndex[drawIndex] = previousTransform;
			markPersistentDrawDirty(drawIndex);
		}
		m_sceneUploadPending = true;
		m_runtimeStats.pendingSceneUpdates = 1;
		refreshSceneView();
	}

	bool GPUDrivenRenderer::tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const
	{
		const auto it = m_drawIndexByMeshHandle.find(packMeshHandleKey(handle));
		if (it == m_drawIndexByMeshHandle.end())
		{
			return false;
		}
		outDrawIndex = it->second;
		return true;
	}

	bool GPUDrivenRenderer::tryGetMeshHandleForDrawIndex(uint32_t drawIndex, MeshHandle& outHandle) const
	{
		if (drawIndex >= m_meshHandleByDrawIndex.size())
		{
			return false;
		}
		outHandle = m_meshHandleByDrawIndex[drawIndex];
		return !outHandle.isNull();
	}

	uint64_t GPUDrivenRenderer::packMeshHandleKey(MeshHandle handle)
	{
		return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
	}

	void GPUDrivenRenderer::rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult,
	                                              rhi::CommandBuffer& cmd)
	{
		const bool firstSceneBuild = m_objectIdByMeshHandle.empty();
		if (firstSceneBuild)
		{
			clearGPUDrivenScene();
			m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
		}
		m_activeUploadResult = &uploadResult;

		bool appendedObjects = false;
		bool appendedMeshlets = false;

		for (size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < model.meshes.size(); ++
		     meshIndex)
		{
			const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			if (m_objectIdByMeshHandle.find(meshKey) != m_objectIdByMeshHandle.end())
			{
				continue;
			}

			const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
			if (meshRecord == nullptr)
			{
				continue;
			}

			GPUSceneRegistrationDesc desc{};
			desc.meshHandle = meshHandle;
			desc.meshIndex = static_cast<uint32_t>(meshIndex);
			desc.materialIndex = meshRecord->materialIndex >= 0
				                     ? static_cast<uint32_t>(meshRecord->materialIndex)
				                     : UINT32_MAX;
			desc.transform = meshRecord->transform;
			desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
			desc.flags = buildGPUDrivenFlags(*meshRecord);
			desc.indexCount = meshRecord->indexCount;
			desc.firstIndex = meshRecord->firstIndex;
			desc.vertexOffset = meshRecord->vertexOffset;

			const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
			const uint32_t objectId = m_sceneRegistry.registerObject(desc);
			m_objectIdByMeshHandle[meshKey] = objectId;
			appendedObjects = true;

			if (m_enableExperimentalMeshletPath)
			{
				MeshletConversionResult meshlets = MeshletConverter::convert(model.meshes[meshIndex]);
				const uint32_t baseMeshletIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
				const uint32_t baseIndexOffset = static_cast<uint32_t>(m_meshletIndicesCpu.size());
				const uint32_t flags = buildMeshletGPUDrivenFlags(*meshRecord);
				for (uint32_t localMeshletIndex = 0; localMeshletIndex < static_cast<uint32_t>(meshlets.meshlets.size())
				     ;
				     ++localMeshletIndex)
				{
					shaderio::Meshlet& meshlet = meshlets.meshlets[localMeshletIndex];
					meshlet.materialIndex = desc.materialIndex;
					meshlet.objectIndex = objectId;
					meshlet.flags = flags;
					meshlet.localIndex = localMeshletIndex;
					meshlet.indexOffset += baseIndexOffset;
				}
				if (!meshlets.meshlets.empty())
				{
					m_drawIndexByMeshHandle.emplace(meshKey, baseMeshletIndex);
					m_meshletDataCpu.insert(m_meshletDataCpu.end(), meshlets.meshlets.begin(), meshlets.meshlets.end());
					appendedMeshlets = true;
					if (m_meshHandleByDrawIndex.size() < m_meshletDataCpu.size())
					{
						m_meshHandleByDrawIndex.resize(m_meshletDataCpu.size(), kNullMeshHandle);
					}
					for (uint32_t localMeshletIndex = 0; localMeshletIndex < static_cast<uint32_t>(meshlets.meshlets.
						     size());
					     ++localMeshletIndex)
					{
						const shaderio::Meshlet& meshlet = meshlets.meshlets[localMeshletIndex];
						const uint32_t drawIndex = baseMeshletIndex + localMeshletIndex;
						m_meshHandleByDrawIndex[drawIndex] = meshHandle;
						if (meshRecord->alphaMode == shaderio::LAlphaBlend)
						{
							m_transparentDrawIndices.push_back(drawIndex);
						}
						else if (meshRecord->alphaMode == shaderio::LAlphaMask)
						{
							m_alphaTestDrawIndices.push_back(drawIndex);
						}
						else
						{
							m_opaqueDrawIndices.push_back(drawIndex);
						}

						m_meshletCullObjectsCpu.push_back(shaderio::GPUCullObject{
							.sphereCenterRadius = transformBoundsSphere(meshRecord->transform, meshlet.boundsSphere),
							.indexCount = meshlet.indexCount,
							.firstIndex = meshlet.indexOffset,
							.vertexOffset = meshRecord->vertexOffset,
							.flags = flags,
						});
					}
				}
				if (!meshlets.packedIndices.empty())
				{
					m_meshletIndicesCpu.insert(m_meshletIndicesCpu.end(), meshlets.packedIndices.begin(),
					                           meshlets.packedIndices.end());
				}
				m_runtimeStats.meshletTriangleCount += meshlets.triangleCount;
				if (!meshlets.meshlets.empty())
				{
					continue;
				}
			}

			m_drawIndexByMeshHandle[meshKey] = objectDrawIndex;
			if (objectDrawIndex >= m_meshHandleByDrawIndex.size())
			{
				m_meshHandleByDrawIndex.resize(objectDrawIndex + 1u, kNullMeshHandle);
			}
			m_meshHandleByDrawIndex[objectDrawIndex] = meshHandle;
			if (meshRecord->alphaMode == shaderio::LAlphaBlend)
			{
				m_transparentDrawIndices.push_back(objectDrawIndex);
			}
			else if (meshRecord->alphaMode == shaderio::LAlphaMask)
			{
				m_alphaTestDrawIndices.push_back(objectDrawIndex);
			}
			else
			{
				m_opaqueDrawIndices.push_back(objectDrawIndex);
			}
		}

		if (appendedObjects || firstSceneBuild)
		{
			++m_sceneTopologyVersion;
			invalidateSortedBootstrapStates();
			m_sceneRegistry.syncToGpu(cmd);
		}
		if (m_enableExperimentalMeshletPath && (appendedMeshlets || firstSceneBuild))
		{
			m_meshletBuffer.uploadMeshlets(m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
		}
		m_sceneUploadPending = false;
		m_persistentDrawDataDirty = true;
		m_dirtyPersistentDrawIndices.clear();
		m_runtimeStats.sceneUploadCount += 1;
		m_runtimeStats.pendingSceneUpdates = 0;
		m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
		m_runtimeStats.meshletCount = m_enableExperimentalMeshletPath ? m_meshletBuffer.getMeshletCount() : 0u;
		refreshSceneView();
	}

	void GPUDrivenRenderer::appendSceneObjectDraw(uint64_t meshKey, MeshHandle meshHandle, uint32_t drawIndex,
	                                              SceneDrawBucket bucket)
	{
		m_drawIndexByMeshHandle[meshKey] = drawIndex;
		if (drawIndex >= m_meshHandleByDrawIndex.size())
		{
			m_meshHandleByDrawIndex.resize(drawIndex + 1u, kNullMeshHandle);
		}
		m_meshHandleByDrawIndex[drawIndex] = meshHandle;

		switch (bucket)
		{
		case SceneDrawBucket::Transparent:
			m_transparentDrawIndices.push_back(drawIndex);
			break;
		case SceneDrawBucket::AlphaMask:
			m_alphaTestDrawIndices.push_back(drawIndex);
			break;
		case SceneDrawBucket::Opaque:
		default:
			m_opaqueDrawIndices.push_back(drawIndex);
			break;
		}
	}

	void GPUDrivenRenderer::rebuildGPUDrivenScene(const SceneAsset& asset,
	                                              const SceneUploadPlan& plan,
	                                              const SceneUploadResult& uploadResult,
	                                              rhi::CommandBuffer& cmd)
	{
		const bool firstSceneBuild = m_objectIdByMeshHandle.empty();
		if (firstSceneBuild)
		{
			clearGPUDrivenScene();
			m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
		}
		m_activeUploadResult = &uploadResult;

		bool appendedObjects = false;
		bool appendedMeshlets = false;

		std::vector<SceneDrawBucket> primaryBucketByMeshIndex(asset.meshes.size(), SceneDrawBucket::Opaque);
		std::vector<bool> hasPrimaryBucketByMeshIndex(asset.meshes.size(), false);
		std::vector<glm::vec4> primaryBoundsSphereByMeshIndex(asset.meshes.size(), glm::vec4(0.0f));
		std::vector<bool> hasPrimaryBoundsSphereByMeshIndex(asset.meshes.size(), false);

		for (const DrawCommandBuildPlan& draw : plan.drawCommands)
		{
			if (draw.meshIndex < primaryBucketByMeshIndex.size() && !hasPrimaryBucketByMeshIndex[draw.meshIndex])
			{
				primaryBucketByMeshIndex[draw.meshIndex] = draw.bucket;
				hasPrimaryBucketByMeshIndex[draw.meshIndex] = true;
			}
		}
		for (const InstanceCullRecord& cullRecord : plan.cullRecords)
		{
			if (cullRecord.meshIndex < primaryBoundsSphereByMeshIndex.size() && !hasPrimaryBoundsSphereByMeshIndex[
				cullRecord.meshIndex])
			{
				primaryBoundsSphereByMeshIndex[cullRecord.meshIndex] = cullRecord.boundingSphere;
				hasPrimaryBoundsSphereByMeshIndex[cullRecord.meshIndex] = true;
			}
		}

		if (!m_enableExperimentalMeshletPath && !uploadResult.drawRecords.empty())
		{
			m_sceneDrawRecords = uploadResult.drawRecords;
			m_objectIdByDrawRecord.assign(m_sceneDrawRecords.size(), UINT32_MAX);
			m_drawIndexByDrawRecord.assign(m_sceneDrawRecords.size(), UINT32_MAX);

			for (uint32_t drawRecordIndex = 0; drawRecordIndex < static_cast<uint32_t>(uploadResult.drawRecords.size());
			     ++drawRecordIndex)
			{
				const SceneUploadResult::SceneDrawRecord& drawRecord = uploadResult.drawRecords[drawRecordIndex];
				if (drawRecord.meshIndex >= uploadResult.meshes.size() || drawRecord.meshIndex >= asset.meshes.size())
				{
					continue;
				}

				const MeshHandle meshHandle = drawRecord.meshHandle;
				const uint64_t meshKey = packMeshHandleKey(meshHandle);
				const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
				if (meshRecord == nullptr)
				{
					continue;
				}

				GPUSceneRegistrationDesc desc{};
				desc.meshHandle = meshHandle;
				desc.meshIndex = drawRecord.meshIndex;
				desc.materialIndex = drawRecord.materialIndex;
				desc.transform = drawRecord.worldTransform;
				desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
				desc.flags = buildGPUDrivenFlags(*meshRecord);
				desc.indexCount = meshRecord->indexCount;
				desc.firstIndex = meshRecord->firstIndex;
				desc.vertexOffset = meshRecord->vertexOffset;
				if (drawRecord.boundsSphere.w > 0.0f)
				{
					desc.boundsSphere = drawRecord.boundsSphere;
				}

				const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
				const uint32_t objectId = m_sceneRegistry.registerObject(desc);
				if (m_objectIdByMeshHandle.find(meshKey) == m_objectIdByMeshHandle.end())
				{
					m_objectIdByMeshHandle[meshKey] = objectId;
				}
				if (drawRecordIndex < m_objectIdByDrawRecord.size())
				{
					m_objectIdByDrawRecord[drawRecordIndex] = objectId;
				}
				if (drawRecordIndex < m_drawIndexByDrawRecord.size())
				{
					m_drawIndexByDrawRecord[drawRecordIndex] = objectDrawIndex;
				}
				m_objectIdsByMeshHandle[meshKey].push_back(objectId);

				SceneDrawBucket bucket = SceneDrawBucket::Opaque;
				if (drawRecord.alphaMode == shaderio::LAlphaBlend)
				{
					bucket = SceneDrawBucket::Transparent;
				}
				else if (drawRecord.alphaMode == shaderio::LAlphaMask)
				{
					bucket = SceneDrawBucket::AlphaMask;
				}
				appendSceneObjectDraw(meshKey, meshHandle, objectDrawIndex, bucket);
				appendedObjects = true;
			}

			if (appendedObjects || firstSceneBuild)
			{
				++m_sceneTopologyVersion;
				invalidateSortedBootstrapStates();
				m_sceneRegistry.syncToGpu(cmd);
			}
			m_sceneUploadPending = false;
			m_persistentDrawDataDirty = true;
			m_dirtyPersistentDrawIndices.clear();
			m_runtimeStats.sceneUploadCount += 1;
			m_runtimeStats.pendingSceneUpdates = 0;
			m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
			m_runtimeStats.meshletCount = 0u;
			refreshSceneView();
			return;
		}

		for (size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < asset.meshes.size(); ++
		     meshIndex)
		{
			const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			if (m_objectIdByMeshHandle.find(meshKey) != m_objectIdByMeshHandle.end())
			{
				continue;
			}

			const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
			if (meshRecord == nullptr)
			{
				continue;
			}

			GPUSceneRegistrationDesc desc{};
			desc.meshHandle = meshHandle;
			desc.meshIndex = static_cast<uint32_t>(meshIndex);
			desc.materialIndex = meshRecord->materialIndex >= 0
				                     ? static_cast<uint32_t>(meshRecord->materialIndex)
				                     : UINT32_MAX;
			desc.transform = meshRecord->transform;
			desc.boundsSphere = glm::vec4(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
			desc.flags = buildGPUDrivenFlags(*meshRecord);
			desc.indexCount = meshRecord->indexCount;
			desc.firstIndex = meshRecord->firstIndex;
			desc.vertexOffset = meshRecord->vertexOffset;

			const uint32_t objectDrawIndex = m_sceneRegistry.getObjectCount();
			SceneDrawBucket bucket = SceneDrawBucket::Opaque;
			if (meshIndex < primaryBucketByMeshIndex.size() && hasPrimaryBucketByMeshIndex[meshIndex])
			{
				bucket = primaryBucketByMeshIndex[meshIndex];
			}

			if (meshIndex < primaryBoundsSphereByMeshIndex.size() && hasPrimaryBoundsSphereByMeshIndex[meshIndex])
			{
				desc.boundsSphere = primaryBoundsSphereByMeshIndex[meshIndex];
			}

			const uint32_t objectId = m_sceneRegistry.registerObject(desc);
			m_objectIdByMeshHandle[meshKey] = objectId;
			appendedObjects = true;

			if (m_enableExperimentalMeshletPath && meshIndex < asset.meshletPayloads.size())
			{
				const SceneAsset::MeshletPayload& meshletPayload = asset.meshletPayloads[meshIndex];
				const size_t meshletStride = sizeof(shaderio::Meshlet);
				const uint32_t meshletCount =
					meshletStride > 0 ? static_cast<uint32_t>(meshletPayload.meshletData.size() / meshletStride) : 0u;
				if (meshletCount > 0)
				{
					const uint32_t baseMeshletIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
					const uint32_t baseIndexOffset = static_cast<uint32_t>(m_meshletIndicesCpu.size());
					const uint32_t flags = buildMeshletGPUDrivenFlags(*meshRecord);
					const shaderio::Meshlet* sourceMeshlets =
						reinterpret_cast<const shaderio::Meshlet*>(meshletPayload.meshletData.data());
					const uint32_t* sourcePackedIndices =
						reinterpret_cast<const uint32_t*>(meshletPayload.indexData.data());
					const uint32_t packedIndexCount = static_cast<uint32_t>(meshletPayload.indexData.size() / sizeof(
						uint32_t));

					m_drawIndexByMeshHandle.emplace(meshKey, baseMeshletIndex);
					if (m_meshHandleByDrawIndex.size() < baseMeshletIndex + meshletCount)
					{
						m_meshHandleByDrawIndex.resize(baseMeshletIndex + meshletCount, kNullMeshHandle);
					}
					appendedMeshlets = true;

					for (uint32_t localMeshletIndex = 0; localMeshletIndex < meshletCount; ++localMeshletIndex)
					{
						shaderio::Meshlet meshlet = sourceMeshlets[localMeshletIndex];
						meshlet.materialIndex = desc.materialIndex;
						meshlet.objectIndex = objectId;
						meshlet.flags = flags;
						meshlet.localIndex = localMeshletIndex;
						meshlet.indexOffset += baseIndexOffset;

						const uint32_t drawIndex = baseMeshletIndex + localMeshletIndex;
						m_meshletDataCpu.push_back(meshlet);
						m_meshHandleByDrawIndex[drawIndex] = meshHandle;

						switch (bucket)
						{
						case SceneDrawBucket::Transparent:
							m_transparentDrawIndices.push_back(drawIndex);
							break;
						case SceneDrawBucket::AlphaMask:
							m_alphaTestDrawIndices.push_back(drawIndex);
							break;
						case SceneDrawBucket::Opaque:
						default:
							m_opaqueDrawIndices.push_back(drawIndex);
							break;
						}

						m_meshletCullObjectsCpu.push_back(shaderio::GPUCullObject{
							.sphereCenterRadius = transformBoundsSphere(meshRecord->transform, meshlet.boundsSphere),
							.indexCount = meshlet.indexCount,
							.firstIndex = meshlet.indexOffset,
							.vertexOffset = meshRecord->vertexOffset,
							.flags = flags,
						});
					}

					if (sourcePackedIndices != nullptr && packedIndexCount > 0)
					{
						m_meshletIndicesCpu.insert(m_meshletIndicesCpu.end(),
						                           sourcePackedIndices,
						                           sourcePackedIndices + packedIndexCount);
					}
					m_runtimeStats.meshletTriangleCount += packedIndexCount / 3u;
					continue;
				}
			}

			appendSceneObjectDraw(meshKey, meshHandle, objectDrawIndex, bucket);
		}

		if (appendedObjects || firstSceneBuild)
		{
			++m_sceneTopologyVersion;
			invalidateSortedBootstrapStates();
			m_sceneRegistry.syncToGpu(cmd);
		}
		if (m_enableExperimentalMeshletPath && (appendedMeshlets || firstSceneBuild))
		{
			m_meshletBuffer.uploadMeshlets(m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
		}
		m_sceneUploadPending = false;
		m_persistentDrawDataDirty = true;
		m_dirtyPersistentDrawIndices.clear();
		m_runtimeStats.sceneUploadCount += 1;
		m_runtimeStats.pendingSceneUpdates = 0;
		m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
		m_runtimeStats.meshletCount = m_enableExperimentalMeshletPath ? m_meshletBuffer.getMeshletCount() : 0u;
		refreshSceneView();
	}

	void GPUDrivenRenderer::clearGPUDrivenScene()
	{
		++m_sceneTopologyVersion;
		invalidateSortedBootstrapStates();
		m_sceneRegistry.clear();
		if (m_enableExperimentalMeshletPath)
		{
			m_meshletBuffer.clear();
		}
		m_meshletDataCpu.clear();
		m_meshletIndicesCpu.clear();
		m_meshletCullObjectsCpu.clear();
		m_sceneDrawRecords.clear();
		m_persistentDrawData.clear();
		m_visibilitySortInputObjects.clear();
		m_visibilitySortInputKeys.clear();
		m_cachedStaticVisibilitySortObjects.clear();
		m_cachedStaticVisibilitySortKeys.clear();
		m_cachedStaticVisibilitySortTopologyVersion = 0;
		m_objectIdByMeshHandle.clear();
		m_objectIdsByMeshHandle.clear();
		m_drawIndexByMeshHandle.clear();
		m_previousTransformByMeshHandle.clear();
		m_previousTransformByDrawIndex.clear();
		m_objectIdByDrawRecord.clear();
		m_drawIndexByDrawRecord.clear();
		m_meshHandleByDrawIndex.clear();
		m_opaqueDrawIndices.clear();
		m_alphaTestDrawIndices.clear();
		m_transparentDrawIndices.clear();
		m_sceneView = {};
		m_runtimeStats = {};
		m_activeUploadResult = nullptr;
		m_sceneUploadPending = false;
		m_persistentDrawDataDirty = false;
		m_previousTransformResetPending = false;
		m_dirtyPersistentDrawIndices.clear();
		m_temporalCameraUniforms = {};
		m_previousCameraUniforms = {};
		m_previousJitteredViewProjection = glm::mat4(1.0f);
		m_currentTAAJitterUv = glm::vec2(0.0f);
		m_previousTAAJitterUv = glm::vec2(0.0f);
		m_previousCameraValid = false;
		m_taaHistoryValid = false;
		m_temporalFrameCounter = 0;
	}

	void GPUDrivenRenderer::flushPendingSceneUploads()
	{
		if (!m_sceneUploadPending || (!m_sceneRegistry.isDirty() && !m_enableExperimentalMeshletPath))
		{
			return;
		}

		m_renderer.executeUploadCommand([this](rhi::CommandBuffer& cmdBuffer)
		{
			if (m_sceneRegistry.isDirty())
			{
				m_sceneRegistry.syncToGpu(cmdBuffer);
			}
			if (m_enableExperimentalMeshletPath && !m_meshletDataCpu.empty())
			{
				m_meshletBuffer.uploadMeshlets(m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu);
			}
		});
		m_sceneUploadPending = false;
		m_persistentDrawDataDirty = true;
		m_dirtyPersistentDrawIndices.clear();
		m_runtimeStats.sceneUploadCount += 1;
		m_runtimeStats.pendingSceneUpdates = 0;
		m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
	}

	void GPUDrivenRenderer::invalidateSortedBootstrapStates()
	{
		for (SortedBootstrapFrameState& frameState : m_sortedBootstrapFrames)
		{
			frameState = {};
		}
	}

	void GPUDrivenRenderer::invalidateSortedBootstrapState(uint32_t frameIndex)
	{
		if (frameIndex < m_sortedBootstrapFrames.size())
		{
			m_sortedBootstrapFrames[frameIndex] = {};
		}
	}

	void GPUDrivenRenderer::recordSortedBootstrapState(uint32_t frameIndex, uint32_t opaqueCapacity,
	                                                   uint32_t alphaCapacity)
	{
		if (frameIndex >= m_sortedBootstrapFrames.size())
		{
			m_sortedBootstrapFrames.resize(std::max(frameIndex + 1u, getSwapchainImageCount()),
			                               SortedBootstrapFrameState{});
		}

		m_sortedBootstrapFrames[frameIndex] = SortedBootstrapFrameState{
			.opaqueCapacity = opaqueCapacity,
			.alphaCapacity = alphaCapacity,
			.sceneTopologyVersion = m_sceneTopologyVersion,
			.valid = true,
		};
	}

	bool GPUDrivenRenderer::getPreviousSortedBootstrapState(uint32_t frameIndex,
	                                                        uint32_t& outOpaqueCapacity,
	                                                        uint32_t& outAlphaCapacity) const
	{
		outOpaqueCapacity = 0u;
		outAlphaCapacity = 0u;
		if (m_sortedBootstrapFrames.empty())
		{
			return false;
		}

		const uint32_t previousFrameIndex = getPreviousFrameIndex(frameIndex);
		if (previousFrameIndex >= m_sortedBootstrapFrames.size())
		{
			return false;
		}

		const SortedBootstrapFrameState& frameState = m_sortedBootstrapFrames[previousFrameIndex];
		if (!frameState.valid || frameState.sceneTopologyVersion != m_sceneTopologyVersion)
		{
			return false;
		}

		outOpaqueCapacity = frameState.opaqueCapacity;
		outAlphaCapacity = frameState.alphaCapacity;
		return outOpaqueCapacity + outAlphaCapacity > 0u;
	}

	void GPUDrivenRenderer::markPersistentDrawDirty(uint32_t drawIndex)
	{
		m_persistentDrawDataDirty = true;
		m_dirtyPersistentDrawIndices.push_back(drawIndex);
	}

	std::vector<GPUDrivenRenderer::DirtyRange> GPUDrivenRenderer::buildPersistentDrawDirtyRanges() const
	{
		if (m_dirtyPersistentDrawIndices.empty())
		{
			return {};
		}

		std::vector<uint32_t> sortedIndices = m_dirtyPersistentDrawIndices;
		std::sort(sortedIndices.begin(), sortedIndices.end());
		sortedIndices.erase(std::unique(sortedIndices.begin(), sortedIndices.end()), sortedIndices.end());

		std::vector<DirtyRange> ranges;
		ranges.reserve(sortedIndices.size());

		DirtyRange currentRange{sortedIndices.front(), 1u};
		for (size_t i = 1; i < sortedIndices.size(); ++i)
		{
			const uint32_t drawIndex = sortedIndices[i];
			if (drawIndex == currentRange.first + currentRange.count)
			{
				currentRange.count += 1u;
				continue;
			}

			ranges.push_back(currentRange);
			currentRange = DirtyRange{drawIndex, 1u};
		}

		ranges.push_back(currentRange);
		return ranges;
	}

	void GPUDrivenRenderer::uploadPersistentDrawData()
	{
		if (!m_sceneView.usePersistentCullingObjects)
		{
			return;
		}

		if (!m_persistentDrawDataDirty && !m_previousTransformResetPending && !m_persistentDrawData.empty())
		{
			return;
		}

		const bool resetPreviousTransforms = m_previousTransformResetPending && !m_persistentDrawDataDirty;
		const bool needsFullUpload = m_persistentDrawData.size() != m_sceneView.objectCount || m_persistentDrawData.
			empty()
			|| m_dirtyPersistentDrawIndices.empty() || resetPreviousTransforms;
		if (needsFullUpload)
		{
			m_persistentDrawData.assign(m_sceneView.objectCount, shaderio::DrawUniforms{});
		}
		if (resetPreviousTransforms)
		{
			m_previousTransformByMeshHandle.clear();
			m_previousTransformByDrawIndex.clear();
		}

		const auto updateDrawPayload = [this](uint32_t drawIndex)
		{
			if (drawIndex >= m_persistentDrawData.size() || drawIndex >= m_meshHandleByDrawIndex.size())
			{
				return;
			}

			const MeshHandle meshHandle = m_meshHandleByDrawIndex[drawIndex];
			if (meshHandle.isNull())
			{
				m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
				return;
			}

			const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
			if (mesh == nullptr)
			{
				m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
				return;
			}

			const SceneUploadResult::SceneDrawRecord* drawRecord =
				drawIndex < m_sceneDrawRecords.size() ? &m_sceneDrawRecords[drawIndex] : nullptr;
			const MaterialHandle drawMaterialHandle =
				drawRecord != nullptr ? drawRecord->materialHandle : kNullMaterialHandle;
			const RenderDevice::MaterialTextureIndices drawMaterialTextures =
				drawMaterialHandle.isNull()
					? RenderDevice::MaterialTextureIndices{}
					: m_renderer.getMaterialTextureIndices(drawMaterialHandle, m_activeUploadResult);
			shaderio::DrawUniforms drawData{};
			drawData.modelMatrix = drawRecord != nullptr ? drawRecord->worldTransform : mesh->transform;
			drawData.prevModelMatrix = drawData.modelMatrix;
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			const auto previousDrawTransformIt = m_previousTransformByDrawIndex.find(drawIndex);
			const auto previousTransformIt = m_previousTransformByMeshHandle.find(meshKey);
			if (previousDrawTransformIt != m_previousTransformByDrawIndex.end())
			{
				drawData.prevModelMatrix = previousDrawTransformIt->second;
			}
			else if (previousTransformIt != m_previousTransformByMeshHandle.end())
			{
				drawData.prevModelMatrix = previousTransformIt->second;
			}
			const MeshRecord* materialSource = mesh;
			drawData.baseColorFactor = drawMaterialHandle.isNull()
				                           ? mesh->baseColorFactor
				                           : m_renderer.getMaterialBaseColorFactor(drawMaterialHandle);
			drawData.baseColorTextureIndex = drawMaterialHandle.isNull()
				                                 ? mesh->baseColorTextureIndex
				                                 : drawMaterialTextures.baseColor;
			drawData.normalTextureIndex = drawMaterialHandle.isNull()
				                              ? mesh->normalTextureIndex
				                              : drawMaterialTextures.normal;
			drawData.metallicRoughnessTextureIndex = drawMaterialHandle.isNull()
				                                         ? mesh->metallicRoughnessTextureIndex
				                                         : drawMaterialTextures.metallicRoughness;
			drawData.occlusionTextureIndex = drawMaterialHandle.isNull()
				                                 ? mesh->occlusionTextureIndex
				                                 : drawMaterialTextures.occlusion;
			drawData.emissiveTextureIndex = mesh->emissiveTextureIndex;
			drawData.metallicFactor = mesh->metallicFactor;
			drawData.roughnessFactor = mesh->roughnessFactor;
			drawData.normalScale = mesh->normalScale;
			drawData.occlusionStrength = mesh->occlusionStrength;
			drawData.emissiveFactor = mesh->emissiveFactor;
			drawData.materialWorkflow = mesh->materialWorkflow;
			drawData.alphaMode = drawMaterialHandle.isNull()
				                     ? materialSource->alphaMode
				                     : drawMaterialTextures.alphaMode;
			drawData.alphaCutoff = drawMaterialHandle.isNull()
				                       ? materialSource->alphaCutoff
				                       : drawMaterialTextures.alphaCutoff;
			m_persistentDrawData[drawIndex] = drawData;
		};

		std::vector<DirtyRange> dirtyRanges;
		if (needsFullUpload)
		{
			for (uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_persistentDrawData.size()); ++drawIndex)
			{
				updateDrawPayload(drawIndex);
			}
		}
		else
		{
			dirtyRanges = buildPersistentDrawDirtyRanges();
			for (const DirtyRange& range : dirtyRanges)
			{
				for (uint32_t drawIndex = range.first; drawIndex < range.first + range.count; ++drawIndex)
				{
					updateDrawPayload(drawIndex);
				}
			}
		}

		const uint32_t frameCount = getSwapchainImageCount();
		for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			if (needsFullUpload)
			{
				m_renderer.uploadMDIDrawData(frameIndex, m_persistentDrawData);
				m_renderer.uploadGBufferMDIDrawData(frameIndex, m_persistentDrawData);
				m_renderer.uploadDepthMDIDrawData(frameIndex, m_persistentDrawData);
				continue;
			}

			for (const DirtyRange& range : dirtyRanges)
			{
				const std::span<const shaderio::DrawUniforms> drawRange{
					m_persistentDrawData.data() + range.first, range.count
				};
				m_renderer.uploadMDIDrawDataRange(frameIndex, range.first, drawRange);
				m_renderer.uploadGBufferMDIDrawDataRange(frameIndex, range.first, drawRange);
				m_renderer.uploadDepthMDIDrawDataRange(frameIndex, range.first, drawRange);
			}
		}
		bool hasMotionPreviousTransforms = !m_previousTransformByDrawIndex.empty();
		for (uint32_t drawIndex = 0; drawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size()); ++drawIndex)
		{
			const MeshHandle meshHandle = m_meshHandleByDrawIndex[drawIndex];
			if (meshHandle.isNull())
			{
				continue;
			}
			const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
			if (mesh == nullptr)
			{
				continue;
			}
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			hasMotionPreviousTransforms = hasMotionPreviousTransforms
				|| (m_previousTransformByMeshHandle.find(meshKey) != m_previousTransformByMeshHandle.end());
			m_previousTransformByMeshHandle[meshKey] = mesh->transform;
		}
		m_previousTransformResetPending = hasMotionPreviousTransforms && !resetPreviousTransforms;
		m_persistentDrawDataDirty = false;
		m_dirtyPersistentDrawIndices.clear();
	}

	void GPUDrivenRenderer::refreshSceneView()
	{
		m_sceneView.gpuSceneObjectBufferAddress = m_sceneRegistry.getBufferAddress();
		m_sceneView.gpuCullObjectBufferAddress = m_sceneRegistry.getCullBufferAddress();
		m_sceneView.gpuCullObjectBuffer = m_sceneRegistry.getCullBufferHandle();
		m_sceneView.gpuCullSceneObjectBuffer = m_sceneRegistry.getBufferHandle();
		m_sceneView.gpuCullMeshletBuffer = 0;
		m_sceneView.objectCount = m_sceneRegistry.getObjectCount();
		m_sceneView.overlayObjects = m_sceneRegistry.getOverlayObjects().empty()
			                             ? nullptr
			                             : m_sceneRegistry.getOverlayObjects().data();
		m_sceneView.overlayObjectCount = static_cast<uint32_t>(m_sceneRegistry.getOverlayObjects().size());
		if (m_sceneView.overlayObjectCount > m_sceneView.objectCount)
		{
			m_sceneView.overlayObjectCount = m_sceneView.objectCount;
		}
		m_sceneView.usePersistentCullingObjects = m_sceneView.gpuCullObjectBuffer != 0 && m_sceneView.objectCount > 0;
		m_sceneView.authority = m_sceneView.usePersistentCullingObjects
			                        ? GPUDrivenSceneAuthority::persistentCullObjects
			                        : GPUDrivenSceneAuthority::none;
		m_sceneView.indirectSource = m_sceneView.usePersistentCullingObjects
			                             ? GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect
			                             : GPUDrivenIndirectSourceKind::none;
		m_sceneView.indirectCommandStride =
			m_sceneView.usePersistentCullingObjects ? m_renderer.getGPUCullingIndirectCommandStride() : 0;
		if (m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
			&& m_meshletBuffer.getMeshletCullObjectBuffer() != 0)
		{
			m_sceneView.gpuCullObjectBufferAddress = m_meshletBuffer.getMeshletCullObjectAddress();
			m_sceneView.gpuCullObjectBuffer = m_meshletBuffer.getMeshletCullObjectBuffer();
			m_sceneView.gpuCullMeshletBuffer = m_meshletBuffer.getMeshletDataBuffer();
			m_sceneView.objectCount = m_meshletBuffer.getMeshletCount();
			m_sceneView.overlayObjects = m_meshletCullObjectsCpu.empty() ? nullptr : m_meshletCullObjectsCpu.data();
			m_sceneView.overlayObjectCount = static_cast<uint32_t>(m_meshletCullObjectsCpu.size());
			m_sceneView.usePersistentCullingObjects = true;
			m_sceneView.authority = GPUDrivenSceneAuthority::persistentCullObjects;
			m_sceneView.indirectSource = GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect;
			m_sceneView.indirectCommandStride = m_renderer.getGPUCullingIndirectCommandStride();
		}
		m_sceneView.meshHandles = m_activeUploadResult != nullptr && !m_activeUploadResult->meshes.empty()
			                          ? m_activeUploadResult->meshes.data()
			                          : nullptr;
		m_sceneView.meshHandleCount =
			m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->meshes.size()) : 0;
		m_sceneView.drawMeshHandles = m_activeUploadResult != nullptr && !m_activeUploadResult->drawMeshHandles.empty()
			                              ? m_activeUploadResult->drawMeshHandles.data()
			                              : nullptr;
		m_sceneView.drawMeshHandleCount =
			m_activeUploadResult != nullptr ? static_cast<uint32_t>(m_activeUploadResult->drawMeshHandles.size()) : 0;
		m_sceneView.shadowCasterMeshIndices =
			m_activeUploadResult != nullptr && !m_activeUploadResult->shadowCasterIndices.empty()
				? m_activeUploadResult->shadowCasterIndices.data()
				: nullptr;
		m_sceneView.shadowCasterCount =
			m_activeUploadResult != nullptr
				? static_cast<uint32_t>(m_activeUploadResult->shadowCasterIndices.size())
				: 0;
		m_sceneView.shadowPackedVertexBuffer =
			m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedVertexBuffer.buffer : 0;
		m_sceneView.shadowPackedIndexBuffer =
			m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedIndexBuffer.buffer : 0;
		// Keep stable RHI handles bound to the scene's shadow packed buffers (owned=false:
		// the upload result owns the VMA lifetime, the registry only mirrors the native buffer).
		{
			rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
			const auto rebindShadowPacked = [resourceTable](rhi::BufferHandle& handle, OpaqueGpuBufferHandle buffer)
			{
				if (buffer == 0)
				{
					if (!handle.isNull())
					{
						resourceTable->removeBuffer(handle);
						handle = {};
					}
					return;
				}
				const uint64_t native = static_cast<uint64_t>(buffer);
				if (handle.isNull())
				{
					rhi::vulkan::BufferRecord rec{};
					rec.nativeBuffer = native;
					rec.owned = false;
					handle = resourceTable->registerBuffer(rec);
				}
				else
				{
					resourceTable->updateBuffer(handle, native);
				}
			};
			rebindShadowPacked(m_shadowPackedVertexBufferRHI, m_sceneView.shadowPackedVertexBuffer);
			rebindShadowPacked(m_shadowPackedIndexBufferRHI, m_sceneView.shadowPackedIndexBuffer);
		}
		m_sceneView.shadowPackedMeshes =
			m_activeUploadResult != nullptr && !m_activeUploadResult->shadowPackedMeshes.empty()
				? m_activeUploadResult->shadowPackedMeshes.data()
				: nullptr;
		m_sceneView.shadowPackedMeshCount =
			m_activeUploadResult != nullptr
				? static_cast<uint32_t>(m_activeUploadResult->shadowPackedMeshes.size())
				: 0;
		m_sceneView.sceneBoundsMin = glm::vec3(0.0f);
		m_sceneView.sceneBoundsMax = glm::vec3(0.0f);
		m_sceneView.sceneBoundsValid = false;
		m_sceneView.sceneDepthFormat = getSceneDepthFormat();
		m_sceneView.sceneDepthImage = getSceneDepthImage();
		m_sceneView.sceneDepthView = getSceneDepthImageView();
		m_sceneView.sceneDepthExtent = getSceneExtent();
		for (uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
		{
			m_sceneView.gbufferImages[i] = getSceneGBufferImage(i);
			m_sceneView.gbufferViews[i] = getSceneGBufferImageView(i);
		}
		m_sceneView.outputImage = getOutputTextureImage();
		m_sceneView.outputView = getOutputTextureView();
		m_sceneView.sceneColorHdrImage = getSceneColorHdrImage();
		m_sceneView.sceneColorHdrView = getSceneColorHdrView();
		m_sceneView.bloomHalfImage = getBloomHalfImage();
		m_sceneView.bloomHalfView = getBloomHalfView();
		m_sceneView.bloomHalfExtent = getBloomHalfExtent();
		m_sceneView.bloomQuarterImage = getBloomQuarterImage();
		m_sceneView.bloomQuarterView = getBloomQuarterView();
		m_sceneView.bloomQuarterExtent = getBloomQuarterExtent();
		m_sceneView.bloomEighthImage = getBloomEighthImage();
		m_sceneView.bloomEighthView = getBloomEighthView();
		m_sceneView.bloomEighthExtent = getBloomEighthExtent();
		m_sceneView.bloomSixteenthImage = getBloomSixteenthImage();
		m_sceneView.bloomSixteenthView = getBloomSixteenthView();
		m_sceneView.bloomSixteenthExtent = getBloomSixteenthExtent();
		m_sceneView.bloomThirtySecondImage = getBloomThirtySecondImage();
		m_sceneView.bloomThirtySecondView = getBloomThirtySecondView();
		m_sceneView.bloomThirtySecondExtent = getBloomThirtySecondExtent();
		m_sceneView.bloomUpsampleSixteenthImage = getBloomUpsampleSixteenthImage();
		m_sceneView.bloomUpsampleSixteenthView = getBloomUpsampleSixteenthView();
		m_sceneView.bloomUpsampleSixteenthExtent = getBloomUpsampleSixteenthExtent();
		m_sceneView.bloomUpsampleEighthImage = getBloomUpsampleEighthImage();
		m_sceneView.bloomUpsampleEighthView = getBloomUpsampleEighthView();
		m_sceneView.bloomUpsampleEighthExtent = getBloomUpsampleEighthExtent();
		m_sceneView.bloomUpsampleQuarterImage = getBloomUpsampleQuarterImage();
		m_sceneView.bloomUpsampleQuarterView = getBloomUpsampleQuarterView();
		m_sceneView.bloomUpsampleQuarterExtent = getBloomUpsampleQuarterExtent();
		m_sceneView.bloomOutputImage = getBloomOutputImage();
		m_sceneView.bloomOutputView = getBloomOutputView();
		m_sceneView.bloomOutputExtent = getBloomOutputExtent();
		m_sceneView.colorGradingLutImage = getColorGradingLutImage();
		m_sceneView.colorGradingLutView = getColorGradingLutView();
		m_sceneView.colorGradingLutExtent = getColorGradingLutExtent();
		m_sceneView.velocityImage = getVelocityImage();
		m_sceneView.velocityView = getVelocityView();
		m_sceneView.sceneColorHistoryReadImage = getSceneColorHistoryImage(1);
		m_sceneView.sceneColorHistoryReadView = getSceneColorHistoryView(1);
		m_sceneView.sceneColorHistoryWriteImage = getSceneColorHistoryImage(0);
		m_sceneView.sceneColorHistoryWriteView = getSceneColorHistoryView(0);
		m_sceneView.depthPyramidImage = m_hiZDepthPyramid.getImageHandle();
		m_sceneView.depthPyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
		m_sceneView.depthPyramidMipCount = m_hiZDepthPyramid.getMipCount();
		m_sceneView.depthPyramidSourceDepth = m_hiZDepthPyramid.getSourceDepth();
		m_sceneView.depthPyramidGeneration = m_hiZDepthPyramid.getGenerationCount();
		m_sceneView.depthPyramidValid = m_hiZDepthPyramid.isValid();
		if (m_activeUploadResult != nullptr)
		{
			glm::vec3 boundsMin(std::numeric_limits<float>::max());
			glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
			bool boundsValid = false;

			if (!m_activeUploadResult->shadowPackedMeshes.empty())
			{
				for (const ShadowPackedMesh& packedMesh : m_activeUploadResult->shadowPackedMeshes)
				{
					includeBoundsSphere(boundsMin, boundsMax, boundsValid, packedMesh.boundsSphere);
				}
			}

			if (!boundsValid && !m_sceneDrawRecords.empty())
			{
				for (const SceneUploadResult::SceneDrawRecord& drawRecord : m_sceneDrawRecords)
				{
					includeBoundsSphere(boundsMin, boundsMax, boundsValid, drawRecord.boundsSphere);
				}
			}

			if (!boundsValid)
			{
				for (const MeshHandle meshHandle : m_activeUploadResult->meshes)
				{
					const MeshRecord* mesh = m_renderer.getMeshPool().tryGet(meshHandle);
					if (mesh == nullptr)
					{
						continue;
					}
					includeMeshBounds(boundsMin, boundsMax, boundsValid, *mesh);
				}
			}

			if (boundsValid)
			{
				m_sceneView.sceneBoundsMin = boundsMin;
				m_sceneView.sceneBoundsMax = boundsMax;
				m_sceneView.sceneBoundsValid = true;
			}
		}
		m_runtimeStats.objectCount = m_sceneView.objectCount;
		m_runtimeStats.authority = m_sceneView.authority;
		m_runtimeStats.indirectSource = m_sceneView.indirectSource;
		m_runtimeStats.indirectCommandStride = m_sceneView.indirectCommandStride;
		m_runtimeStats.usesPersistentCullObjects = m_sceneView.usePersistentCullingObjects;
		m_runtimeStats.meshletCount = m_enableExperimentalMeshletPath ? m_meshletBuffer.getMeshletCount() : 0u;
		m_runtimeStats.ownsFullRenderChain = true;
		m_runtimeStats.ownsHiZVisibilityChain = false;
		m_runtimeStats.hiZGeneration = 0;
		const HiZDepthPyramid::MobilePolicy& hiZPolicy = m_hiZDepthPyramid.getMobilePolicy();
		const rhi::Extent2D hiZSourceExtent = m_hiZDepthPyramid.getSourceExtent();
		const rhi::Extent2D hiZPyramidExtent = m_hiZDepthPyramid.getExtent();
		m_runtimeStats.hiZDiagnostics = GPUDrivenHiZDiagnostics{
			.sourceWidth = hiZSourceExtent.width,
			.sourceHeight = hiZSourceExtent.height,
			.pyramidWidth = hiZPyramidExtent.width,
			.pyramidHeight = hiZPyramidExtent.height,
			.mipCount = m_hiZDepthPyramid.getMipCount(),
			.fullMipCount = m_hiZDepthPyramid.getFullMipCount(),
			.policyDownsampleDivisor = hiZPolicy.downsampleDivisor,
			.policyMaxMipCount = hiZPolicy.maxMipCount,
			.policyMinMipSize = hiZPolicy.minMipSize,
			.estimatedMemoryBytes = m_hiZDepthPyramid.getEstimatedMemoryBytes(),
			.generation = m_hiZDepthPyramid.getGenerationCount(),
			.valid = m_hiZDepthPyramid.isValid(),
			.boundForGpuCulling = false,
			.depthEpsilon = kGPUDrivenHiZDepthEpsilon,
			.conservativeRadiusScale = kGPUDrivenHiZConservativeRadiusScale,
			.conservativeRadiusBias = kGPUDrivenHiZConservativeRadiusBias,
			.nearRejectEpsilon = kGPUDrivenHiZNearRejectEpsilon,
			.largeObjectFootprintThreshold = kGPUDrivenHiZLargeObjectFootprintThreshold,
			.fastCameraFallbackDistance = kGPUDrivenHiZFastCameraFallbackDistance,
			.cameraDeltaDistance = m_lastHiZCameraDeltaDistance,
			.fastCameraFallbackTriggered = m_lastHiZFastCameraFallbackTriggered,
		};
	}

	uint32_t GPUDrivenRenderer::getSafePersistentObjectCount() const
	{
		uint32_t safeCount = m_sceneView.objectCount;
		const bool meshletCullStreamActive =
			m_enableExperimentalMeshletPath && m_meshletBuffer.getMeshletCount() > 0u
			&& m_sceneView.gpuCullObjectBuffer == m_meshletBuffer.getMeshletCullObjectBuffer();
		if (!meshletCullStreamActive && m_sceneView.drawMeshHandleCount > 0u)
		{
			safeCount = std::min(safeCount, m_sceneView.drawMeshHandleCount);
		}
		else if (!meshletCullStreamActive && m_sceneView.meshHandleCount > 0u)
		{
			safeCount = std::min(safeCount, m_sceneView.meshHandleCount);
		}
		if (m_sceneView.overlayObjectCount > 0u)
		{
			safeCount = std::min(safeCount, m_sceneView.overlayObjectCount);
		}
		safeCount = std::min(safeCount, kMaxReasonableGPUDrivenObjectCount);
		return safeCount;
	}

	rhi::TextureFormat GPUDrivenRenderer::getIBLEnvironmentFormat() const
	{
		return m_iblEnvironmentFormat;
	}

	uintptr_t GPUDrivenRenderer::getBackendDeviceToken() const
	{
		return m_renderer.getBackendDeviceToken();
	}

	uintptr_t GPUDrivenRenderer::getAllocatorToken() const
	{
		return m_renderer.getAllocatorToken();
	}

	void GPUDrivenRenderer::recordDepthPrepassVisibilitySource(bool usedPreviousFrameIndirect,
	                                                           bool usedSortedBootstrap,
	                                                           uint32_t previousObjectCount,
	                                                           uint32_t opaqueMaxDrawCount,
	                                                           uint32_t alphaMaxDrawCount)
	{
		m_runtimeStats.visibilityDiagnostics.depthUsesPreviousFrameIndirect = usedPreviousFrameIndirect;
		m_runtimeStats.visibilityDiagnostics.depthUsesSortedBootstrap = usedSortedBootstrap;
		m_runtimeStats.visibilityDiagnostics.previousGPUCullingObjectCount = previousObjectCount;
		m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity =
			std::max(m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity, opaqueMaxDrawCount);
		m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity =
			std::max(m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity, alphaMaxDrawCount);
	}

	void GPUDrivenRenderer::recordGBufferVisibilityPatch(bool patched, uint32_t opaqueCapacity, uint32_t alphaCapacity)
	{
		m_runtimeStats.visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched = patched;
		if (patched)
		{
			m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity = opaqueCapacity;
			m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity = alphaCapacity;
		}
	}

	void GPUDrivenRenderer::recordForwardVisibilityPatch(bool patched,
	                                                     uint32_t transparentCapacity,
	                                                     uint32_t)
	{
		m_runtimeStats.visibilityDiagnostics.transparentPatchDispatched = patched;
		m_runtimeStats.visibilityDiagnostics.sameFrameTransparentCapacity = patched ? transparentCapacity : 0u;
		m_runtimeStats.visibilityDiagnostics.transparentCapacityOverflow =
			transparentCapacity > m_runtimeStats.visibilityDiagnostics.maxMobileTransparentDraws;
	}

	void GPUDrivenRenderer::initLightingResources()
	{
		m_lightResources.init(getRHIDevice(),
		                      GPUDrivenLightResources::CreateInfo{
			                      .maxPointLights = 256,
			                      .maxSpotLights = 128,
			                      .frameCount = std::max(1u, getSwapchainImageCount()),
		                      });

		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
		const VkSamplerCreateInfo samplerInfo{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
			.maxLod = VK_LOD_CLAMP_NONE,
		};
		VkSampler linearClampSampler = VK_NULL_HANDLE;
		VK_CHECK(vkCreateSampler(nativeDevice, &samplerInfo, nullptr, &linearClampSampler));
		m_linearClampSampler = reinterpret_cast<uintptr_t>(linearClampSampler);
		// Register the shared samplers (linear-clamp + IBL cube/LUT) as RHI handles for the
		// combinedImageSampler ArgumentWrites of the lighting-input set.
		rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
		m_linearClampSamplerHandle = resourceTable->registerSampler(static_cast<uint64_t>(m_linearClampSampler));
		// IBL samplers are created later in initIBLResources(); their handles are registered
		// lazily on first use in updateLightingArgumentTable.

		// lighting-input set (set LSetTextures): combined image samplers + light buffers + IBL.
		const std::array<ArgumentLayoutEntry, 12> lightingEntries{
			{
				ArgumentLayoutEntry{
					.binding = shaderio::LBindTextures, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::sampledTexture, .count = kGPUDrivenLightPassTextureCount
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindShadowMap, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::sampledTexture, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 2, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 3, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 4, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 5, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 6, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 7, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 8, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindIBLIrradiance, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::sampledTexture, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindIBLPrefiltered, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::sampledTexture, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindIBLBrdfLut, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::sampledTexture, .count = 1
				},
			}
		};
		m_lightingArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = lightingEntries.data(), .entryCount = static_cast<uint32_t>(lightingEntries.size())
			});

		// lighting-scene set (set LSetScene): camera/postProcess dynamic UBO + lighting/culling UBO.
		const std::array<ArgumentLayoutEntry, 4> sceneEntries{
			{
				ArgumentLayoutEntry{
					.binding = shaderio::LBindCamera, .visibility = rhi::ShaderStage::allGraphics,
					.type = rhi::BindlessResourceType::uniformBufferDynamic, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindLighting, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindLightCulling,
					.visibility = static_cast<rhi::ShaderStage>(static_cast<uint32_t>(rhi::ShaderStage::fragment) |
						static_cast<uint32_t>(rhi::ShaderStage::compute)),
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = shaderio::LBindPostProcess, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::uniformBufferDynamic, .count = 1
				},
			}
		};
		m_lightingSceneArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = sceneEntries.data(), .entryCount = static_cast<uint32_t>(sceneEntries.size())
			});

		const std::array<ArgumentLayoutEntry, 9> cullingEntries{
			{
				ArgumentLayoutEntry{
					.binding = 0, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 1, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 2, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 3, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 4, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 5, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::uniformBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 6, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 7, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
				ArgumentLayoutEntry{
					.binding = 8, .visibility = rhi::ShaderStage::compute,
					.type = rhi::BindlessResourceType::storageBuffer, .count = 1
				},
			}
		};
		m_lightCoarseCullingArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = cullingEntries.data(), .entryCount = static_cast<uint32_t>(cullingEntries.size())
			});

		// Owned RHI ArgumentTables for the lighting-input set. The buffer bindings (2-8) come from
		// stable GPUDrivenLightResources buffers; mirror them as owned=false handles. The image
		// bindings are written per-frame in updateLightingArgumentTable (views change on resize).
		m_lightingInputArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_lightingInputArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightingArgumentLayout, "gpu-driven-lighting-input");

			// Buffer bindings 2-8 come from stable light-resource buffers; register + write once.
			const std::array<rhi::BufferHandle, 7> inputBuffers{
				{
					m_lightResources.getPointLightBuffer(i),
					m_lightResources.getPointCoarseBoundsBuffer(i),
					m_lightResources.getCoarseUniformBuffer(i),
					m_lightResources.getClusterCountsBuffer(i),
					m_lightResources.getClusterIndicesBuffer(i),
					m_lightResources.getClusteredUniformBuffer(i),
					m_lightResources.getSpotLightBuffer(i),
				}
			};
			const std::array<uint32_t, 7> inputBindings{{2, 3, 4, 5, 6, 7, 8}};
			const std::array<rhi::ArgumentType, 7> inputTypes{
				{
					rhi::ArgumentType::storageBuffer, rhi::ArgumentType::storageBuffer,
					rhi::ArgumentType::uniformBuffer,
					rhi::ArgumentType::storageBuffer, rhi::ArgumentType::storageBuffer,
					rhi::ArgumentType::uniformBuffer,
					rhi::ArgumentType::storageBuffer,
				}
			};
			const std::array<uint64_t, 7> inputSizes{
				{
					0, 0, sizeof(shaderio::LightCoarseCullingUniforms), 0, 0, sizeof(shaderio::ClusteredLightUniforms),
					0,
				}
			};
			std::array<rhi::ArgumentWrite, 7> inputWrites{};
			for (uint32_t b = 0; b < 7u; ++b)
			{
				inputWrites[b] = rhi::ArgumentWrite{
					.binding = inputBindings[b], .type = inputTypes[b], .buffer = inputBuffers[b], .size = inputSizes[b]
				};
			}
			m_renderer.updateArgumentTable(m_lightingInputArgumentTables[i], inputWrites.data(),
			                               static_cast<uint32_t>(inputWrites.size()));
		}

		// Owned RHI ArgumentTables for the coarse-culling set (point/spot + clustered). The 9
		// light-resource buffers are stable after init (GPUDrivenLightResources has no resize),
		// so mirror them as RHI handles and write each table once here instead of per-frame.
		m_lightCoarseCullingArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		m_lightCoarseCullingBufferHandles.clear();
		m_lightCoarseCullingBufferHandles.reserve(frameCount * 9u);
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_lightCoarseCullingArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightCoarseCullingArgumentLayout,
				                                         "gpu-driven-light-coarse-culling");

			const std::array<rhi::BufferHandle, 9> cullBuffers{
				{
					m_lightResources.getPointLightBuffer(i),
					m_lightResources.getSpotLightBuffer(i),
					m_lightResources.getPointCoarseBoundsBuffer(i),
					m_lightResources.getSpotCoarseBoundsBuffer(i),
					m_lightResources.getCoarseUniformBuffer(i),
					m_lightResources.getClusteredUniformBuffer(i),
					m_lightResources.getClusterCountsBuffer(i),
					m_lightResources.getClusterIndicesBuffer(i),
					m_lightResources.getClusterStatsBuffer(i),
				}
			};
			const std::array<uint64_t, 9> cullSizes{
				{
					0, 0, 0, 0,
					sizeof(shaderio::LightCoarseCullingUniforms),
					sizeof(shaderio::ClusteredLightUniforms),
					0, 0,
					sizeof(GPUDrivenLightResources::ClusterStats),
				}
			};
			std::array<rhi::ArgumentWrite, 9> cullWrites{};
			for (uint32_t b = 0; b < 9u; ++b)
			{
				m_lightCoarseCullingBufferHandles.push_back(cullBuffers[b]);
				cullWrites[b] = rhi::ArgumentWrite{
					.binding = b,
					.type = (b == 4 || b == 5) ? rhi::ArgumentType::uniformBuffer : rhi::ArgumentType::storageBuffer,
					.buffer = cullBuffers[b],
					.size = cullSizes[b],
				};
			}
			m_renderer.updateArgumentTable(m_lightCoarseCullingArgumentTables[i], cullWrites.data(),
			                               static_cast<uint32_t>(cullWrites.size()));
		}

		// Owned RHI ArgumentTables for the lighting-scene set (camera/postProcess dynamic UBO +
		// lighting/culling UBO). Written once below from stable buffers; dynamic offsets supplied
		// at bind time via setDynamicBuffer.
		m_lightingSceneArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_lightingSceneArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightingSceneArgumentLayout, "gpu-driven-lighting-scene");

			const rhi::BufferHandle transientHandle = m_renderer.getTransientBufferHandle(i);
			const rhi::BufferHandle lightingHandle = m_lightResources.getLightingUniformBuffer(i);
			const rhi::BufferHandle coarseHandle = m_lightResources.getCoarseUniformBuffer(i);

			const std::array<rhi::ArgumentWrite, 4> sceneWrites{
				{
					rhi::ArgumentWrite{
						.binding = shaderio::LBindCamera, .type = rhi::ArgumentType::uniformBuffer,
						.buffer = transientHandle, .size = sizeof(shaderio::CameraUniforms)
					},
					rhi::ArgumentWrite{
						.binding = shaderio::LBindLighting, .type = rhi::ArgumentType::uniformBuffer,
						.buffer = lightingHandle, .size = sizeof(shaderio::LightingUniforms)
					},
					rhi::ArgumentWrite{
						.binding = shaderio::LBindLightCulling, .type = rhi::ArgumentType::uniformBuffer,
						.buffer = coarseHandle, .size = sizeof(shaderio::LightCoarseCullingUniforms)
					},
					rhi::ArgumentWrite{
						.binding = shaderio::LBindPostProcess, .type = rhi::ArgumentType::uniformBuffer,
						.buffer = transientHandle, .size = sizeof(shaderio::PostProcessUniforms)
					},
				}
			};
			m_renderer.updateArgumentTable(m_lightingSceneArgumentTables[i], sceneWrites.data(),
			                               static_cast<uint32_t>(sceneWrites.size()));
		}

		m_lightPipelineArgumentLayouts = {m_lightingArgumentLayout, m_lightingSceneArgumentLayout};
	}

	void GPUDrivenRenderer::shutdownLightingResources()
	{
		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		if (nativeDevice != VK_NULL_HANDLE)
		{
			if (m_linearClampSampler != 0)
			{
				vkDestroySampler(nativeDevice, reinterpret_cast<VkSampler>(m_linearClampSampler), nullptr);
				m_linearClampSampler = 0;
			}
		}
		// Owned ArgumentTables + layouts (lighting-input/scene/coarse) are freed by
		// RenderDevice::destroyArgumentTablesAndLayouts(); here we drop the now-stale handles and the
		// owned=false buffer/view/sampler mirrors registered in the resource table.
		if (rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable())
		{
			if (!m_linearClampSamplerHandle.isNull()) resourceTable->removeSampler(m_linearClampSamplerHandle);
			if (!m_iblCubeSamplerHandle.isNull()) resourceTable->removeSampler(m_iblCubeSamplerHandle);
			if (!m_iblLutSamplerHandle.isNull()) resourceTable->removeSampler(m_iblLutSamplerHandle);
		}
		m_lightingInputBufferHandles.clear();
		m_lightingSceneArgumentTables.clear();
		m_lightingInputArgumentTables.clear();
		m_lightingArgumentLayout = {};
		m_lightingSceneArgumentLayout = {};
		m_linearClampSamplerHandle = {};
		m_iblCubeSamplerHandle = {};
		m_iblLutSamplerHandle = {};
		m_lightCoarseCullingBufferHandles.clear();
		m_lightCoarseCullingArgumentTables.clear();
		m_lightCoarseCullingArgumentLayout = {};
		m_lightPipelineArgumentLayouts = {};
		m_lightResources.deinit();
		m_gpuDrivenPointLights.clear();
		m_gpuDrivenSpotLights.clear();
	}

	void GPUDrivenRenderer::initIBLResources()
	{
		shutdownIBLResources();
		m_iblEnvironmentPath = kGPUDrivenDefaultIBLEnvironmentPath;
		m_iblEnvironmentStatus = "Using flat ambient fallback";
		m_iblUsingFallback = true;

		const auto initFallbackSplitSumResources = [&]()
		{
			IBLResources::CreateInfo fallbackInfo{
				.cubeMapSize = 128,
				.dfgLUTSize = 256,
			};
			executeUploadCommand([&](rhi::CommandBuffer& cmdBuffer)
			{
				m_iblResources.init(getRHIDevice(),
				                    cmdBuffer,
				                    fallbackInfo);
			});
		};

		Ktx2Loader loader;
		Ktx2Loader::Ktx2Texture texture{};
		const std::filesystem::path path(kGPUDrivenDefaultIBLEnvironmentPath);
		if (!std::filesystem::exists(path))
		{
			m_iblEnvironmentStatus = "KTX2 environment not found: " + path.string();
			LOGW("%s", m_iblEnvironmentStatus.c_str());
			initFallbackSplitSumResources();
			return;
		}
		if (!loader.load(path, texture) || texture.data.empty() || texture.width == 0 || texture.height == 0)
		{
			m_iblEnvironmentStatus = "Failed to load GPUDriven IBL KTX2: " + loader.getLastError();
			LOGW("%s", m_iblEnvironmentStatus.c_str());
			initFallbackSplitSumResources();
			return;
		}

		m_iblEnvironmentImage = getRHIDevice().createTexture(rhi::TextureDesc{
			.dimension = rhi::TextureDimension::e2D,
			.format = texture.format,
			.usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst,
			.extent = {texture.width, texture.height, 1},
			.mipLevels = std::max(1u, texture.mipLevels),
			.arrayLayers = 1,
			.debugName = "IBL_Environment",
		});
		const rhi::TextureHandle environmentImageHandle = m_iblEnvironmentImage;
		executeUploadCommand([&](rhi::CommandBuffer& cmdBuffer)
		{
			BatchUploadContext upload;
			upload.init(getRHIDevice(), static_cast<uint64_t>(texture.data.size()));
			const rhi::TextureBarrier initBarrier{
				.texture = environmentImageHandle,
				.before = rhi::ResourceState::Undefined,
				.after = rhi::ResourceState::TransferDst,
				.range = {
					.aspect = rhi::TextureAspect::color,
					.baseMipLevel = 0,
					.levelCount = std::max(1u, texture.mipLevels),
					.baseArrayLayer = 0,
					.layerCount = 1
				},
			};
			cmdBuffer.resourceBarrier(&initBarrier, 1, nullptr, 0);
			for (uint32_t level = 0; level < std::max(1u, texture.mipLevels); ++level)
			{
				if (level >= texture.mipOffsets.size() || level >= texture.mipSizes.size())
				{
					continue;
				}
				const uint64_t offset = texture.mipOffsets[level];
				const uint64_t size = texture.mipSizes[level];
				if (size == 0 || offset + size > texture.data.size())
				{
					continue;
				}
				const std::span<const std::byte> payload{
					reinterpret_cast<const std::byte*>(texture.data.data() + static_cast<size_t>(offset)),
					static_cast<size_t>(size)
				};
				const BatchUploadContext::Slice slice = upload.allocate(size, 4);
				upload.copyToSlices(std::span<const BatchUploadContext::Slice>(&slice, 1),
				                    std::span<const std::span<const std::byte>>(&payload, 1));
				rhi::BufferTextureCopyDesc region{
					.bufferOffset = 0,
					.texture = environmentImageHandle,
					.aspect = rhi::TextureAspect::color,
					.mipLevel = level,
					.baseArrayLayer = 0,
					.layerCount = 1,
					.width = std::max(1u, texture.width >> level),
					.height = std::max(1u, texture.height >> level),
					.depth = 1,
				};
				upload.recordTextureUpload(slice, environmentImageHandle, region);
			}
			upload.executeUploads(cmdBuffer);
			rhi::TextureBarrier sampleBarrier = initBarrier;
			sampleBarrier.before = rhi::ResourceState::TransferDst;
			sampleBarrier.after = rhi::ResourceState::ShaderRead;
			cmdBuffer.resourceBarrier(&sampleBarrier, 1, nullptr, 0);
			rhi::BufferHandle staging = upload.releaseStagingBuffer();
			if (!staging.isNull())
			{
				m_gpuDrivenRhiStagingBuffers.push_back(staging);
			}
		});

		m_iblEnvironmentView = getRHIDevice().createTextureView(rhi::TextureViewCreateDesc{
			.image = m_iblEnvironmentImage,
			.format = texture.format,
			.viewType = rhi::ImageViewType::e2D,
			.aspect = rhi::TextureAspect::color,
			.levelCount = std::max(1u, texture.mipLevels),
			.layerCount = 1,
		});
		m_iblEnvironmentFormat = texture.format;
		m_iblEnvironmentExtent = {texture.width, texture.height};
		m_iblEnvironmentMipCount = std::max(1u, texture.mipLevels);
		m_iblEnvironmentEstimatedBytes = static_cast<uint64_t>(texture.data.size());
		m_iblEnvironmentLoaded = true;
		m_iblUsingFallback = false;
		IBLResources::CreateInfo iblCreateInfo{
			.cubeMapSize = 128,
			.dfgLUTSize = 256,
			.sourceEnvironmentView = m_iblEnvironmentView,
			.sourceWidth = texture.width,
			.sourceHeight = texture.height,
			.sourceMipCount = std::max(1u, texture.mipLevels),
		};
		executeUploadCommand([&](rhi::CommandBuffer& cmdBuffer)
		{
			m_iblResources.init(getRHIDevice(),
			                    cmdBuffer,
			                    iblCreateInfo);
		});

		m_iblEnvironmentStatus = m_iblResources.isSplitSumReady()
			                         ? "Loaded GPUDriven equirect HDR KTX2 with split-sum IBL"
			                         : "Loaded GPUDriven equirect HDR KTX2; split-sum IBL unavailable";
		LOGI("Loaded GPUDriven equirect IBL %s (%ux%u, %u mips, %.2f MiB); split-sum %s",
		     m_iblEnvironmentPath.c_str(),
		     m_iblEnvironmentExtent.width,
		     m_iblEnvironmentExtent.height,
		     m_iblEnvironmentMipCount,
		     static_cast<double>(m_iblEnvironmentEstimatedBytes) / (1024.0 * 1024.0),
		     m_iblResources.isSplitSumReady() ? "ready" : "unavailable");
	}

	void GPUDrivenRenderer::shutdownIBLResources()
	{
		m_iblResources.deinit();
		if (!m_iblEnvironmentView.isNull())
		{
			getRHIDevice().destroyTextureView(m_iblEnvironmentView);
			m_iblEnvironmentView = {};
		}
		if (!m_iblEnvironmentImage.isNull())
		{
			getRHIDevice().destroyImage(m_iblEnvironmentImage);
			m_iblEnvironmentImage = {};
		}
		m_iblEnvironmentFormat = rhi::TextureFormat::undefined;
		m_iblEnvironmentExtent = {};
		m_iblEnvironmentMipCount = 0;
		m_iblEnvironmentEstimatedBytes = 0;
		m_iblEnvironmentLoaded = false;
		m_iblUsingFallback = true;
		for (utils::Buffer& buffer : m_gpuDrivenStagingBuffers)
		{
			if (buffer.buffer != VK_NULL_HANDLE)
			{
				vmaDestroyBuffer(reinterpret_cast<VmaAllocator>(getAllocatorToken()), buffer.buffer, buffer.allocation);
				buffer = {};
			}
		}
		m_gpuDrivenStagingBuffers.clear();
		for (rhi::BufferHandle staging : m_gpuDrivenRhiStagingBuffers)
		{
			if (!staging.isNull())
			{
				getRHIDevice().destroyBuffer(staging);
			}
		}
		m_gpuDrivenRhiStagingBuffers.clear();
	}

	void GPUDrivenRenderer::initPhase7Resources()
	{
		shutdownPhase7Resources();

		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		if (nativeDevice == VK_NULL_HANDLE)
		{
			return;
		}
		const uint32_t frameCount = std::max(1u, getSwapchainImageCount());

		// AO set: 2 sampled images + 1 storage image (compute). RHI ArgumentLayout + per-frame
		// owned ArgumentTables (AO trace + denoise). SSR set is already an RHI ArgumentLayout
		// (per-frame temporary argument table via acquireSSRTempArgumentTable).
		const std::array<ArgumentLayoutEntry, 3> aoLayoutEntries{
			{
				{0, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{1, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{2, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageTexture, 1},
			}
		};
		m_aoArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = aoLayoutEntries.data(), .entryCount = static_cast<uint32_t>(aoLayoutEntries.size())
			});

		const std::array<ArgumentLayoutEntry, 6> ssrLayoutEntries{
			{
				{0, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{1, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{2, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{3, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{4, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageTexture, 1},
				{5, rhi::ShaderStage::compute, rhi::BindlessResourceType::uniformBuffer, 1},
			}
		};
		m_ssrLayoutHandle = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = ssrLayoutEntries.data(), .entryCount = static_cast<uint32_t>(ssrLayoutEntries.size())
			});

		// Per-frame owned ArgumentTables for AO trace + denoise (written in updatePhase7Descriptors).
		m_aoArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		m_aoDenoiseArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_aoArgumentTables[i] = m_renderer.createPersistentArgumentTable(m_aoArgumentLayout, "gpu-driven-ao");
			m_aoDenoiseArgumentTables[i] = m_renderer.createPersistentArgumentTable(
				m_aoArgumentLayout, "gpu-driven-ao-denoise");
		}

		resizePhase7Resources();
	}

	void GPUDrivenRenderer::shutdownPhase7Resources()
	{
		const auto destroyTarget = [&](rhi::TextureHandle& image, rhi::TextureViewHandle& view)
		{
			if (!view.isNull()) getRHIDevice().destroyTextureView(view);
			if (!image.isNull()) getRHIDevice().destroyImage(image);
			view = {};
			image = {};
		};
		destroyTarget(m_aoRawImage, m_aoRawView);
		destroyTarget(m_aoDenoisedImage, m_aoDenoisedView);
		destroyTarget(m_ssrRawImage, m_ssrRawView);
		destroyTarget(m_shadowAtlasImage, m_shadowAtlasView);

		// Owned ArgumentTables + layouts are freed by RenderDevice::destroyArgumentTablesAndLayouts at device
		// shutdown; just drop our handle references here.
		m_aoArgumentTables.clear();
		m_aoDenoiseArgumentTables.clear();
		m_aoArgumentLayout = {};
		m_phase7HalfExtent = {};
		m_shadowAtlasAllocatedTiles = 0u;
	}

	void GPUDrivenRenderer::resizePhase7Resources()
	{
		if (getBackendDeviceToken() == 0)
		{
			return;
		}
		const VkExtent2D sceneExtent = toVkExtent(getSceneExtent());
		const rhi::Extent2D halfExtent{
			std::max(1u, (sceneExtent.width + 1u) / 2u),
			std::max(1u, (sceneExtent.height + 1u) / 2u)
		};
		if (m_phase7HalfExtent.width == halfExtent.width && m_phase7HalfExtent.height == halfExtent.height
			&& !m_aoRawImage.isNull() && !m_aoDenoisedImage.isNull() && !m_ssrRawImage.isNull()
			&& !m_shadowAtlasImage.isNull())
		{
			return;
		}
		waitForIdle();
		const auto destroyTarget = [&](rhi::TextureHandle& image, rhi::TextureViewHandle& view)
		{
			if (!view.isNull()) getRHIDevice().destroyTextureView(view);
			if (!image.isNull()) getRHIDevice().destroyImage(image);
			view = {};
			image = {};
		};
		destroyTarget(m_aoRawImage, m_aoRawView);
		destroyTarget(m_aoDenoisedImage, m_aoDenoisedView);
		destroyTarget(m_ssrRawImage, m_ssrRawView);
		destroyTarget(m_shadowAtlasImage, m_shadowAtlasView);
		m_phase7HalfExtent = halfExtent;

		const auto createImageResource = [&](rhi::TextureFormat format, rhi::Extent2D extent,
		                                     rhi::TextureUsageFlags usage, rhi::TextureAspect aspect,
		                                     rhi::TextureHandle& outImage, rhi::TextureViewHandle& outView)
		{
			outImage = getRHIDevice().createTexture(rhi::TextureDesc{
				.dimension = rhi::TextureDimension::e2D,
				.format = format,
				.usage = usage,
				.extent = {extent.width, extent.height, 1},
				.mipLevels = 1,
				.arrayLayers = 1,
			});
			outView = getRHIDevice().createTextureView(rhi::TextureViewCreateDesc{
				.image = outImage,
				.format = format,
				.viewType = rhi::ImageViewType::e2D,
				.aspect = aspect,
				.levelCount = 1,
				.layerCount = 1,
			});
			executeUploadCommand([&](rhi::CommandBuffer& cmdBuffer)
			{
				const rhi::TextureBarrier barrier{
					.texture = outImage,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::General,
					.range = {
						.aspect = aspect,
						.baseMipLevel = 0,
						.levelCount = 1,
						.baseArrayLayer = 0,
						.layerCount = 1
					},
				};
				cmdBuffer.resourceBarrier(&barrier, 1, nullptr, 0);
			});
		};

		constexpr rhi::TextureUsageFlags kStorageColorUsage =
			rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::storage;
		createImageResource(rhi::TextureFormat::r16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_aoRawImage, m_aoRawView);
		createImageResource(rhi::TextureFormat::r16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_aoDenoisedImage, m_aoDenoisedView);
		createImageResource(rhi::TextureFormat::rgba16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_ssrRawImage, m_ssrRawView);
		createImageResource(rhi::TextureFormat::d32Sfloat, m_shadowAtlasExtent,
		                    rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled,
		                    rhi::TextureAspect::depth, m_shadowAtlasImage, m_shadowAtlasView);
	}

	void GPUDrivenRenderer::bindPhase7PassResources()
	{
		if (m_shadowAtlasImage.isNull())
		{
			return;
		}

		m_passExecutor.bindTexture({
			.handle = kPassGPUDrivenShadowAtlasHandle,
			.backendImageToken = resolveNativeTexture(getRHIDevice(), m_shadowAtlasImage),
			.aspect = rhi::TextureAspect::depth,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
			.rhiTexture = m_shadowAtlasImage,
		});
	}

	void GPUDrivenRenderer::initPhase7Pipelines()
	{
		shutdownPhase7Pipelines();
		if (getBackendDeviceToken() == 0 || m_aoArgumentLayout.isNull() || m_ssrLayoutHandle.isNull())
		{
			return;
		}
#ifdef USE_SLANG
		const auto createComputePipeline = [&](const void* shaderData,
		                                       size_t shaderSize,
		                                       const char* entryPoint,
		                                       rhi::ArgumentLayoutHandle layout,
		                                       uint32_t pushSize,
		                                       uint32_t variant)
		{
			const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{layout}};
			const std::array<rhi::PipelinePushConstantRange, 1> pushConstants{
				{
					rhi::PipelinePushConstantRange{.stages = rhi::ShaderStage::compute, .offset = 0, .size = pushSize},
				}
			};
			const rhi::ComputePipelineDesc desc{
				.shaderStage =
				rhi::PipelineShaderStageDesc{
					.stage = rhi::ShaderStage::compute,
					.spirvCode = static_cast<const uint32_t*>(shaderData),
					.spirvSize = shaderSize * sizeof(uint32_t),
					.entryPoint = entryPoint,
				},
				.argumentLayouts = argumentLayouts.data(),
				.argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
				.pushConstantRanges = pushConstants.data(),
				.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size()),
				.specializationVariant = variant,
			};
			return getRHIDevice().createComputePipeline(desc);
		};

		m_gtaoPipelineHandle = createComputePipeline(gtao_slang, std::size(gtao_slang), "kernelGTAO",
		                                             m_aoArgumentLayout, sizeof(shaderio::GPUDrivenAOPushConstants),
		                                             0x6301u);
		m_aoDenoisePipelineHandle = createComputePipeline(ao_denoise_slang, std::size(ao_denoise_slang),
		                                                  "kernelAODenoise",
		                                                  m_aoArgumentLayout,
		                                                  sizeof(shaderio::GPUDrivenAOPushConstants), 0x6302u);
		m_ssrTracePipelineHandle = createComputePipeline(ssr_trace_slang, std::size(ssr_trace_slang), "kernelSSRTrace",
		                                                 m_ssrLayoutHandle, sizeof(shaderio::GPUDrivenSSRPushConstants),
		                                                 0x6303u);
#endif
	}

	void GPUDrivenRenderer::shutdownPhase7Pipelines()
	{
		getRHIDevice().destroyPipeline(m_gtaoPipelineHandle);
		getRHIDevice().destroyPipeline(m_aoDenoisePipelineHandle);
		getRHIDevice().destroyPipeline(m_ssrTracePipelineHandle);
		m_gtaoPipelineHandle = {};
		m_aoDenoisePipelineHandle = {};
		m_ssrTracePipelineHandle = {};
	}

	void GPUDrivenRenderer::updatePhase7Descriptors(uint32_t frameIndex)
	{
		if (frameIndex >= m_aoArgumentTables.size() || frameIndex >= m_aoDenoiseArgumentTables.size())
		{
			return;
		}
		const GPUDrivenSceneView& sceneView = m_sceneView;
		if (sceneView.sceneDepthView.isNull() || sceneView.gbufferViews[1].isNull()
			|| m_aoRawView.isNull() || m_aoDenoisedView.isNull())
		{
			return;
		}

		const rhi::TextureViewHandle aoRawView = m_aoRawView;
		const rhi::TextureViewHandle aoDenoisedView = m_aoDenoisedView;

		// AO trace set: depth + normals sampled, aoRaw storage out.
		const std::array<rhi::ArgumentWrite, 3> aoWrites{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.sceneDepthView
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.gbufferViews[1],
					.accessIntent = rhi::ArgumentAccessIntent::readWrite
				},
				rhi::ArgumentWrite{.binding = 2, .type = rhi::ArgumentType::storageTexture, .textureView = aoRawView},
			}
		};
		m_renderer.updateArgumentTable(m_aoArgumentTables[frameIndex], aoWrites.data(),
		                               static_cast<uint32_t>(aoWrites.size()));

		// Denoise set: aoRaw + depth sampled, aoDenoised storage out.
		const std::array<rhi::ArgumentWrite, 3> denoiseWrites{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::sampledTexture, .textureView = aoRawView,
					.accessIntent = rhi::ArgumentAccessIntent::readWrite
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.sceneDepthView
				},
				rhi::ArgumentWrite{
					.binding = 2, .type = rhi::ArgumentType::storageTexture, .textureView = aoDenoisedView
				},
			}
		};
		m_renderer.updateArgumentTable(m_aoDenoiseArgumentTables[frameIndex], denoiseWrites.data(),
		                               static_cast<uint32_t>(denoiseWrites.size()));
	}

	rhi::ArgumentTableHandle GPUDrivenRenderer::acquireSSRTempArgumentTable(
		uint64_t cameraBuffer, uint32_t cameraOffset)
	{
		if (m_ssrLayoutHandle.isNull() || cameraBuffer == 0)
		{
			return rhi::ArgumentTableHandle{};
		}
		const GPUDrivenSceneView& sceneView = m_sceneView;
		const rhi::TextureViewHandle historyView = !sceneView.sceneColorHistoryReadView.isNull()
			                                           ? sceneView.sceneColorHistoryReadView
			                                           : sceneView.sceneColorHdrView;
		if (historyView.isNull() || sceneView.sceneDepthView.isNull()
			|| sceneView.gbufferViews[0].isNull() || sceneView.gbufferViews[1].isNull()
			|| m_ssrRawView.isNull())
		{
			return rhi::ArgumentTableHandle{};
		}

		const rhi::BufferHandle cameraBufferHandle = m_renderer.getCurrentTransientBufferHandle();
		if (cameraBufferHandle.isNull())
		{
			return rhi::ArgumentTableHandle{};
		}

		const std::array<rhi::ArgumentWrite, 6> writes{
			{
				rhi::ArgumentWrite{.binding = 0, .type = rhi::ArgumentType::sampledTexture, .textureView = historyView},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.sceneDepthView
				},
				rhi::ArgumentWrite{
					.binding = 2, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.gbufferViews[1]
				},
				rhi::ArgumentWrite{
					.binding = 3, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.gbufferViews[0]
				},
				rhi::ArgumentWrite{
					.binding = 4, .type = rhi::ArgumentType::storageTexture, .textureView = m_ssrRawView
				},
				rhi::ArgumentWrite{
					.binding = 5, .type = rhi::ArgumentType::uniformBuffer, .buffer = cameraBufferHandle,
					.offset = cameraOffset, .size = sizeof(shaderio::CameraUniforms)
				},
			}
		};
		return m_renderer.createTemporaryArgumentTable(m_ssrLayoutHandle, writes.data(),
		                                               static_cast<uint32_t>(writes.size()),
		                                               ArgumentSlot::shaderSpecific, "gpu-driven-ssr-temp");
	}

	void GPUDrivenRenderer::updateGPUDrivenLights(const RenderParams& params, uint32_t frameIndex)
	{
		m_gpuDrivenPointLights.clear();
		m_gpuDrivenSpotLights.clear();
		if (params.debugOptions.enablePointLights)
		{
			m_gpuDrivenPointLights.reserve(
				std::min<size_t>(params.sceneLights.size(), m_lightResources.getMaxPointLights()));
			m_gpuDrivenSpotLights.reserve(
				std::min<size_t>(params.sceneLights.size(), m_lightResources.getMaxSpotLights()));
			for (const SceneLight& sceneLight : params.sceneLights)
			{
				if (!sceneLight.enabled)
				{
					continue;
				}
				if (sceneLight.type != SceneLightType::point && sceneLight.type != SceneLightType::spot)
				{
					continue;
				}

				const glm::mat4 worldTransform =
					resolveSceneLightTransform(sceneLight, params.sceneLightSceneNodes, params.sceneLightGltfNodes);
				shaderio::LightData light{};
				light.positionOrDirection = glm::vec3(worldTransform[3]);
				light.intensity = sceneLight.intensity;
				light.color = sceneLight.color;
				light.range = sceneLightEffectiveRange(sceneLight, m_sceneView);
				light.lightType = sceneLight.type == SceneLightType::spot
					                  ? shaderio::LLightTypeSpot
					                  : shaderio::LLightTypePoint;
				light.spotDirection = sceneLightTravelDirection(worldTransform);
				light.spotInnerAngle = sceneLight.innerConeAngle;
				light.spotOuterAngle = std::max(sceneLight.outerConeAngle, sceneLight.innerConeAngle + 0.001f);

				if (sceneLight.type == SceneLightType::spot)
				{
					if (m_gpuDrivenSpotLights.size() < m_lightResources.getMaxSpotLights())
					{
						m_gpuDrivenSpotLights.push_back(light);
					}
				}
				else if (m_gpuDrivenPointLights.size() < m_lightResources.getMaxPointLights())
				{
					m_gpuDrivenPointLights.push_back(light);
				}
			}
		}

		const shaderio::CameraUniforms* camera = params.cameraUniforms;
		const VkExtent2D extent = toVkExtent(getSceneExtent());
		const glm::mat4 view = camera != nullptr ? camera->view : glm::mat4(1.0f);
		const glm::mat4 projection = camera != nullptr ? camera->projection : glm::mat4(1.0f);
		const glm::mat4 inverseView = glm::inverse(view);
		const uint32_t tileCountX = (extent.width + shaderio::LTileSizeX - 1u) / shaderio::LTileSizeX;
		const uint32_t tileCountY = (extent.height + shaderio::LTileSizeY - 1u) / shaderio::LTileSizeY;
		shaderio::LightingUniforms lightingUniforms{};
		const shaderio::ShadowUniforms* shadowData = getCSMShadowResources().getShadowUniformsData();
		if (shadowData != nullptr)
		{
			for (int i = 0; i < shaderio::LCascadeCount; ++i)
			{
				lightingUniforms.light.worldToShadow[i] = shadowData->cascadeViewProjection[i];
			}
			lightingUniforms.light.cascadeSplitDistances = shadowData->cascadeSplitDistances;
		}
		lightingUniforms.light.lightDirectionAndShadowStrength =
			glm::vec4(glm::normalize(-params.lightSettings.direction), params.lightSettings.shadowStrength);
		lightingUniforms.light.lightColorAndNormalBias =
			glm::vec4(params.lightSettings.color, params.lightSettings.normalBias);
		lightingUniforms.light.ambientColorAndTexelSize =
			glm::vec4(params.lightSettings.ambient,
			          1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())));
		lightingUniforms.light.shadowMetrics =
			glm::vec4(1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())),
			          params.lightSettings.depthBias,
			          params.lightSettings.normalBias,
			          static_cast<float>(shaderio::LCascadeCount));
		lightingUniforms.light.iblParams =
			glm::vec4(params.debugOptions.enableIBL ? 1.0f : 0.0f,
			          params.debugOptions.iblIntensity,
			          static_cast<float>(m_iblResources.isSplitSumReady()
				                             ? m_iblResources.getMaxMipLevel()
				                             : (getIBLEnvironmentMipCount() > 0
					                                ? getIBLEnvironmentMipCount() - 1u
					                                : 0u)),
			          getIBLEnvironmentLoaded() ? 1.0f : 0.0f);
		lightingUniforms.light.iblDebugInfo =
			glm::vec4(static_cast<float>(params.debugOptions.iblDebugMode),
			          m_iblResources.isSplitSumReady() ? 1.0f : 0.0f, 0.0f, 0.0f);
		lightingUniforms.light.phase7Info =
			glm::vec4(params.debugOptions.enableAO && !m_aoDenoisedView.isNull() ? 1.0f : 0.0f,
			          params.debugOptions.enableSSR && !m_ssrRawView.isNull() ? 1.0f : 0.0f,
			          0.0f,
			          0.0f);

		const shaderio::LightCoarseCullingUniforms coarseUniforms{
			.viewProjection = camera != nullptr ? camera->viewProjection : glm::mat4(1.0f),
			.cameraRight = glm::vec4(glm::normalize(glm::vec3(inverseView[0])), 0.0f),
			.cameraUp = glm::vec4(glm::normalize(glm::vec3(inverseView[1])), 0.0f),
			.screenTileInfo = glm::vec4(extent.width, extent.height, tileCountX, tileCountY),
			.lightCountInfo = glm::vec4(static_cast<float>(m_gpuDrivenPointLights.size()),
			                            static_cast<float>(m_gpuDrivenSpotLights.size()), 0.0f, 0.0f),
			.debugInfo = glm::vec4(params.debugOptions.showLightCoarseCullingHeatmap ? 1.0f : 0.0f,
			                       params.debugOptions.showClusteredLightingHeatmap ? 1.0f : 0.0f,
			                       params.debugOptions.showClusteredLightingOverflow ? 1.0f : 0.0f,
			                       params.debugOptions.enableClusteredLighting ? 1.0f : 0.0f),
		};
		const shaderio::ClusteredLightUniforms clusteredUniforms{
			.screenSizeAndClusterInfo = glm::vec4(extent.width, extent.height, shaderio::LClusterGridSizeX,
			                                      shaderio::LClusterGridSizeY),
			.clusterZAndLightInfo = glm::vec4(shaderio::LClusterGridSizeZ, shaderio::LMaxLightsPerCluster,
			                                  static_cast<float>(m_gpuDrivenPointLights.size()),
			                                  static_cast<float>(m_gpuDrivenSpotLights.size())),
			.clipInfo = glm::vec4(0.01f, 1000.0f, 1.0f, 0.0f),
			.debugInfo = glm::vec4(params.debugOptions.enableClusteredLighting ? 1.0f : 0.0f,
			                       params.debugOptions.showClusteredLightingHeatmap ? 1.0f : 0.0f,
			                       params.debugOptions.showClusteredLightingOverflow ? 1.0f : 0.0f,
			                       0.0f),
			.viewMatrix = view,
			.projectionMatrix = projection,
			.invProjectionMatrix = glm::inverse(projection),
			.invViewMatrix = inverseView,
		};
		m_lightResources.updateLights(frameIndex, m_gpuDrivenPointLights, m_gpuDrivenSpotLights);
		m_lightResources.updateUniforms(frameIndex, lightingUniforms, coarseUniforms, clusteredUniforms);
		updateLightingArgumentTable(frameIndex);
	}

	rhi::ArgumentTableHandle GPUDrivenRenderer::getCurrentLightCullingArgumentTable() const
	{
		const uint32_t frameIndex = getCurrentFrameIndexHint();
		return frameIndex < m_lightCoarseCullingArgumentTables.size()
			       ? m_lightCoarseCullingArgumentTables[frameIndex]
			       : rhi::ArgumentTableHandle{};
	}

	void GPUDrivenRenderer::updateLightingArgumentTable(uint32_t frameIndex)
	{
		if (frameIndex >= m_lightingInputArgumentTables.size() || m_lightingInputArgumentTables[frameIndex].isNull())
		{
			return;
		}

		const GPUDrivenSceneView& sceneView = m_sceneView;
		if (sceneView.sceneDepthView.isNull() || sceneView.sceneColorHdrView.isNull())
		{
			return;
		}
		for (uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
		{
			if (sceneView.gbufferViews[i].isNull())
			{
				return;
			}
		}

		const auto viewOr = [](rhi::TextureViewHandle h, rhi::TextureViewHandle fb) { return h.isNull() ? fb : h; };

		const rhi::TextureViewHandle fallbackColor = sceneView.gbufferViews[0];
		const rhi::TextureViewHandle fallbackDepth = sceneView.sceneDepthView;
		std::array<rhi::TextureViewHandle, kGPUDrivenLightPassTextureCount> texViews{};
		for (uint32_t i = 0; i < kPackedGBufferTargetCount; ++i)
		{
			texViews[i] = sceneView.gbufferViews[i];
		}
		texViews[kGPUDrivenLightPassDepthTextureIndex] = sceneView.sceneDepthView;
		texViews[kGPUDrivenLightPassSceneColorHdrIndex] = sceneView.sceneColorHdrView;
		texViews[kGPUDrivenLightPassBloomHalfIndex] = viewOr(sceneView.bloomHalfView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomQuarterIndex] = viewOr(sceneView.bloomQuarterView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomEighthIndex] = viewOr(sceneView.bloomEighthView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomSixteenthIndex] = viewOr(sceneView.bloomSixteenthView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomThirtySecondIndex] = viewOr(sceneView.bloomThirtySecondView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomUpsampleSixteenthIndex] = viewOr(
			sceneView.bloomUpsampleSixteenthView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomUpsampleEighthIndex] =
			viewOr(sceneView.bloomUpsampleEighthView, fallbackColor);
		texViews[kGPUDrivenLightPassBloomUpsampleQuarterIndex] = viewOr(sceneView.bloomUpsampleQuarterView,
		                                                                fallbackColor);
		texViews[kGPUDrivenLightPassBloomOutputIndex] = viewOr(sceneView.bloomOutputView, fallbackColor);
		texViews[kGPUDrivenLightPassColorGradingLutIndex] = viewOr(sceneView.colorGradingLutView, fallbackColor);
		texViews[kGPUDrivenLightPassVelocityIndex] = viewOr(sceneView.velocityView, fallbackColor);
		texViews[kGPUDrivenLightPassHistoryReadIndex] = viewOr(sceneView.sceneColorHistoryReadView, fallbackColor);
		texViews[kGPUDrivenLightPassHistoryWriteIndex] = viewOr(sceneView.sceneColorHistoryWriteView, fallbackColor);
		texViews[kGPUDrivenLightPassIBLEnvironmentIndex] = viewOr(m_iblEnvironmentView, fallbackColor);
		texViews[kGPUDrivenLightPassAOIndex] = viewOr(m_aoDenoisedView, fallbackDepth);
		texViews[kGPUDrivenLightPassSSRIndex] = viewOr(m_ssrRawView, fallbackColor);

		const rhi::TextureViewHandle shadowView = viewOr(m_renderer.getShadowMapView(), fallbackDepth);

		// IBLResources now owns RHI sampler handles directly; cache them lazily so the
		// lighting table can fall back before the IBL resources finish initializing.
		if (m_iblCubeSamplerHandle.isNull())
		{
			m_iblCubeSamplerHandle = m_iblResources.getCubeMapSampler();
		}
		if (m_iblLutSamplerHandle.isNull())
		{
			m_iblLutSamplerHandle = m_iblResources.getLUTSampler();
		}
		const rhi::SamplerHandle iblCubeSampler = m_iblCubeSamplerHandle.isNull()
			                                          ? m_linearClampSamplerHandle
			                                          : m_iblCubeSamplerHandle;
		const rhi::SamplerHandle iblLutSampler = m_iblLutSamplerHandle.isNull()
			                                         ? m_linearClampSamplerHandle
			                                         : m_iblLutSamplerHandle;

		std::vector<rhi::ArgumentWrite> writes;
		writes.reserve(kGPUDrivenLightPassTextureCount + 4);
		for (uint32_t i = 0; i < kGPUDrivenLightPassTextureCount; ++i)
		{
			writes.push_back(rhi::ArgumentWrite{
				.binding = shaderio::LBindTextures, .arrayElement = i, .type = rhi::ArgumentType::combinedImageSampler,
				.textureView = texViews[i], .sampler = m_linearClampSamplerHandle
			});
		}
		writes.push_back(rhi::ArgumentWrite{
			.binding = shaderio::LBindShadowMap, .type = rhi::ArgumentType::combinedImageSampler,
			.textureView = shadowView, .sampler = m_linearClampSamplerHandle
		});
		writes.push_back(rhi::ArgumentWrite{
			.binding = shaderio::LBindIBLIrradiance, .type = rhi::ArgumentType::combinedImageSampler,
			.textureView = m_iblResources.getIrradianceMapView(), .sampler = iblCubeSampler
		});
		writes.push_back(rhi::ArgumentWrite{
			.binding = shaderio::LBindIBLPrefiltered, .type = rhi::ArgumentType::combinedImageSampler,
			.textureView = m_iblResources.getPrefilteredMapView(), .sampler = iblCubeSampler
		});
		writes.push_back(rhi::ArgumentWrite{
			.binding = shaderio::LBindIBLBrdfLut, .type = rhi::ArgumentType::combinedImageSampler,
			.textureView = m_iblResources.getDFGLUTView(), .sampler = iblLutSampler
		});

		m_renderer.updateArgumentTable(m_lightingInputArgumentTables[frameIndex], writes.data(),
		                               static_cast<uint32_t>(writes.size()));
		// Buffer bindings 2-8 are written once in initLightingResources (stable light-resource
		// buffers); the coarse-culling table is likewise written once there.
	}


	void GPUDrivenRenderer::initLightingPipelines()
	{
		if (getBackendDeviceToken() == 0 || m_lightPipelineArgumentLayouts[0].isNull()
			|| m_lightPipelineArgumentLayouts[1].isNull())
		{
			return;
		}

#ifdef USE_SLANG
		const auto createFullscreenPipeline = [&](const char* fragmentEntry,
		                                          rhi::TextureFormat colorFormat,
		                                          bool depthTest,
		                                          uint32_t variant) -> PipelineHandle
		{
			const std::array<rhi::PipelineShaderStageDesc, 2> stages{
				{
					rhi::PipelineShaderStageDesc{
						.stage = rhi::ShaderStage::vertex,
						.spirvCode = shader_light_gpu_driven_slang,
						.spirvSize = std::size(shader_light_gpu_driven_slang) * sizeof(uint32_t),
						.entryPoint = "vertexMain"
					},
					rhi::PipelineShaderStageDesc{
						.stage = rhi::ShaderStage::fragment,
						.spirvCode = shader_light_gpu_driven_slang,
						.spirvSize = std::size(shader_light_gpu_driven_slang) * sizeof(uint32_t),
						.entryPoint = fragmentEntry
					},
				}
			};
			const std::array<rhi::DynamicState, 2> dynamicStates{
				{
					rhi::DynamicState::viewport,
					rhi::DynamicState::scissor,
				}
			};
			const std::array<rhi::BlendAttachmentState, 1> blendStates{
				{
					rhi::BlendAttachmentState{.blendEnable = false, .colorWriteMask = rhi::ColorComponentFlags::all},
				}
			};
			const std::array<rhi::TextureFormat, 1> colorFormats{{colorFormat}};
			rhi::GraphicsPipelineDesc desc{
				.shaderStages = stages.data(),
				.shaderStageCount = static_cast<uint32_t>(stages.size()),
				.vertexInput = rhi::VertexInputLayoutDesc{},
				.rasterState = rhi::RasterState{},
				.depthState = rhi::DepthState{depthTest, false, rhi::CompareOp::equal},
				.blendStates = blendStates.data(),
				.blendStateCount = static_cast<uint32_t>(blendStates.size()),
				.dynamicStates = dynamicStates.data(),
				.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
				.renderingInfo =
				{
					.colorFormats = colorFormats.data(),
					.colorFormatCount = static_cast<uint32_t>(colorFormats.size()),
					.depthFormat = depthTest
						               ? toPortableTextureFormat(getSceneDepthFormat())
						               : rhi::TextureFormat::undefined,
				},
				.argumentLayouts = m_lightPipelineArgumentLayouts.data(),
				.argumentLayoutCount = static_cast<uint32_t>(m_lightPipelineArgumentLayouts.size()),
				.specializationVariant = variant,
			};
			desc.rasterState.topology = rhi::PrimitiveTopology::triangleList;
			desc.rasterState.cullMode = rhi::CullMode::none;
			desc.rasterState.frontFace = rhi::FrontFace::counterClockwise;
			desc.rasterState.sampleCount = rhi::SampleCount::count1;
			return getRHIDevice().createGraphicsPipeline(desc);
		};

		m_gpuDrivenLightHdrPipeline = createFullscreenPipeline("fragmentHdrMain", getSceneColorHdrFormat(), false,
		                                                       0x6101u);
		m_gpuDrivenSkyboxPipeline = createFullscreenPipeline("fragmentSkyboxMain", getSceneColorHdrFormat(), true,
		                                                     0x6102u);
		m_gpuDrivenTAAResolvePipeline = createFullscreenPipeline("fragmentTAAResolveMain", getSceneColorHdrFormat(),
		                                                         false, 0x6103u);
		m_gpuDrivenBloomPrefilterPipeline = createFullscreenPipeline("fragmentBloomPrefilterMain",
		                                                             SceneResources::kBloomFormat, false, 0x6104u);
		m_gpuDrivenBloomDownsamplePipeline = createFullscreenPipeline("fragmentBloomDownsampleMain",
		                                                              SceneResources::kBloomFormat, false, 0x6105u);
		m_gpuDrivenFinalColorPipeline = createFullscreenPipeline("fragmentFinalColorMain", getOutputTextureFormat(),
		                                                         false, 0x6106u);
		m_gpuDrivenVelocityPipeline = createFullscreenPipeline("fragmentVelocityMain", getVelocityFormat(), false,
		                                                       0x6107u);
		m_gpuDrivenBloomUpsamplePipeline = createFullscreenPipeline("fragmentBloomUpsampleMain",
		                                                            SceneResources::kBloomFormat, false, 0x6108u);

		if (!m_lightCoarseCullingArgumentLayout.isNull())
		{
			const std::array<rhi::ArgumentLayoutHandle, 1> cullingArgumentLayouts{{m_lightCoarseCullingArgumentLayout}};
			const auto createComputePipeline = [&](const char* entryPoint, uint32_t variant)
			{
				const rhi::ComputePipelineDesc desc{
					.shaderStage =
					rhi::PipelineShaderStageDesc{
						.stage = rhi::ShaderStage::compute,
						.spirvCode = shader_light_culling_slang,
						.spirvSize = std::size(shader_light_culling_slang) * sizeof(uint32_t),
						.entryPoint = entryPoint,
					},
					.argumentLayouts = cullingArgumentLayouts.data(),
					.argumentLayoutCount = static_cast<uint32_t>(cullingArgumentLayouts.size()),
					.specializationVariant = variant,
				};
				return getRHIDevice().createComputePipeline(desc);
			};
			m_pointLightCoarseCullingPipeline =
				createComputePipeline("kernelPointLightCoarseCulling", 0x6201u);
			m_spotLightCoarseCullingPipeline =
				createComputePipeline("kernelSpotLightCoarseCulling", 0x6202u);

			const rhi::ComputePipelineDesc clusteredDesc{
				.shaderStage =
				rhi::PipelineShaderStageDesc{
					.stage = rhi::ShaderStage::compute,
					.spirvCode = clustered_light_cull_slang,
					.spirvSize = std::size(clustered_light_cull_slang) * sizeof(uint32_t),
					.entryPoint = "kernelClusteredLightCulling",
				},
				.argumentLayouts = cullingArgumentLayouts.data(),
				.argumentLayoutCount = static_cast<uint32_t>(cullingArgumentLayouts.size()),
				.specializationVariant = 0x6203u,
			};
			m_clusteredLightCullingPipeline = getRHIDevice().createComputePipeline(clusteredDesc);
		}
#endif
	}

	void GPUDrivenRenderer::shutdownLightingPipelines()
	{
		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		if (nativeDevice == VK_NULL_HANDLE)
		{
			return;
		}
		const auto destroyGraphicsPipeline = [&](PipelineHandle& handle)
		{
			if (!handle.isNull())
			{
				handle = {};
			}
		};
		// Fullscreen graphics pipelines live in the device pipeline registry and are
		// destroyed by RenderDevice::destroyPipelines(); compute culling pipelines are
		// backend-owned handles destroyed here.
		getRHIDevice().destroyPipeline(m_pointLightCoarseCullingPipeline);
		getRHIDevice().destroyPipeline(m_spotLightCoarseCullingPipeline);
		getRHIDevice().destroyPipeline(m_clusteredLightCullingPipeline);
		m_pointLightCoarseCullingPipeline = {};
		m_spotLightCoarseCullingPipeline = {};
		m_clusteredLightCullingPipeline = {};
		destroyGraphicsPipeline(m_gpuDrivenLightHdrPipeline);
		destroyGraphicsPipeline(m_gpuDrivenSkyboxPipeline);
		destroyGraphicsPipeline(m_gpuDrivenTAAResolvePipeline);
		destroyGraphicsPipeline(m_gpuDrivenBloomPrefilterPipeline);
		destroyGraphicsPipeline(m_gpuDrivenBloomDownsamplePipeline);
		destroyGraphicsPipeline(m_gpuDrivenBloomUpsamplePipeline);
		destroyGraphicsPipeline(m_gpuDrivenFinalColorPipeline);
		destroyGraphicsPipeline(m_gpuDrivenVelocityPipeline);
		destroyGraphicsPipeline(m_pointLightCoarseCullingPipeline);
		destroyGraphicsPipeline(m_spotLightCoarseCullingPipeline);
		destroyGraphicsPipeline(m_clusteredLightCullingPipeline);
	}

	void GPUDrivenRenderer::updateOwnershipDiagnostics(uint32_t frameIndex,
	                                                   bool sceneRenderingSuspended,
	                                                   uint32_t safeObjectCount)
	{
		const bool hasSceneAttachments = !m_sceneView.sceneDepthImage.isNull()
			&& !m_sceneView.sceneDepthView.isNull()
			&& !m_sceneView.outputImage.isNull()
			&& !m_sceneView.outputView.isNull()
			&& !m_sceneView.sceneColorHdrImage.isNull()
			&& !m_sceneView.sceneColorHdrView.isNull()
			&& !m_sceneView.gbufferImages[0].isNull()
			&& !m_sceneView.gbufferViews[0].isNull();
		const bool hasLightingResources = !getGPUDrivenLightHdrPipelineHandle().isNull()
			&& frameIndex < m_lightingInputArgumentTables.size()
			&& !m_lightingInputArgumentTables[frameIndex].isNull();
		const bool hasShadowResources = !getCSMShadowPipelineHandle().isNull()
			&& !getCSMShadowResources().getCascadeImage().isNull();
		const bool hasMaterialDescriptors = !getGraphicsMaterialArgumentTable().isNull();
		const bool hasCurrentVisibility = m_sceneView.usePersistentCullingObjects
			&& getGPUCullingObjectCount(frameIndex) == safeObjectCount
			&& safeObjectCount > 0u;
		m_runtimeStats.visibilityDiagnostics.currentGPUCullingObjectCount = getGPUCullingObjectCount(frameIndex);
		m_runtimeStats.visibilityDiagnostics.safeObjectCount = safeObjectCount;

		m_runtimeStats.resourceOwnership.sceneAttachments =
			hasSceneAttachments ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
		m_runtimeStats.resourceOwnership.depthPyramid =
			m_runtimeStats.ownsHiZVisibilityChain
				? GPUDrivenOwnershipState::gpuOwned
				: (m_hiZDepthPyramid.isValid() ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled);
		m_runtimeStats.resourceOwnership.visibility =
			hasCurrentVisibility
				? GPUDrivenOwnershipState::gpuOwned
				: (m_sceneView.usePersistentCullingObjects
					   ? GPUDrivenOwnershipState::bridged
					   : GPUDrivenOwnershipState::disabled);
		m_runtimeStats.resourceOwnership.lightingResources =
			hasLightingResources && m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
				? GPUDrivenOwnershipState::gpuOwned
				: (hasLightingResources ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled);
		m_runtimeStats.resourceOwnership.shadowResources =
			hasShadowResources ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
		m_runtimeStats.resourceOwnership.materialDescriptors =
			hasMaterialDescriptors ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::disabled;
		const bool transparentVisibilityAuthoritative =
			m_runtimeStats.visibilityDiagnostics.transparentCapacity == 0u
			|| m_runtimeStats.visibilityDiagnostics.transparentPatchDispatched;
		if (hasCurrentVisibility && m_runtimeStats.visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched
			&& transparentVisibilityAuthoritative)
		{
			m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
		}
		else if (m_visibilitySortPass != nullptr && m_runtimeStats.visibilityDiagnostics.sortInputCount > 0u)
		{
			m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuSortCpuFeedback;
		}
		else
		{
			m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::cpuBootstrap;
		}

		m_runtimeStats.passDiagnostics.clear();
		m_runtimeStats.passDiagnostics.reserve(m_passExecutor.getPassCount());
		const auto addPassDiagnostic = [this](const char* name, GPUDrivenOwnershipState ownership, const char* note)
		{
			m_runtimeStats.passDiagnostics.push_back(GPUDrivenPassDiagnostic{
				.name = name != nullptr ? name : "",
				.ownership = ownership,
				.note = note != nullptr ? note : "",
			});
		};

		addPassDiagnostic("GPUDrivenDepthPrepass",
		                  m_depthPrepass != nullptr
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  hasSceneAttachments
			                  ? "MDI submission; scene attachments still bridged"
			                  : "Disabled until scene attachments are valid");
		addPassDiagnostic("GPUDrivenDepthPyramid",
		                  m_runtimeStats.resourceOwnership.depthPyramid,
		                  m_runtimeStats.ownsHiZVisibilityChain
			                  ? "GPU-driven Hi-Z is bound to GPU culling"
			                  : "Hi-Z resource exists but ownership is not complete");
		addPassDiagnostic("GPUDrivenCulling",
		                  hasCurrentVisibility ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::bridged,
		                  hasCurrentVisibility
			                  ? "Current GPU culling object stream is authoritative"
			                  : "Bootstrap or suspended scene path is active");
		addPassDiagnostic("GPUDrivenVisibilitySortPass",
		                  m_visibilitySortPass != nullptr
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_visibilitySortPass != nullptr
			                  ? "GPU sort executes; transparent distance keys are still CPU-seeded"
			                  : "Visibility sort pass is not registered");
		addPassDiagnostic("GPUDrivenLightCulling",
		                  m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.clusteredLightingDiagnostics.resourcesOwned
			                  ? "GPUDriven-owned local light buffers and culling descriptors"
			                  : "GPUDriven light resources are unavailable");
		addPassDiagnostic("GPUDrivenCSMShadow",
		                  hasShadowResources ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
		                  hasShadowResources
			                  ? "GPU-driven submission; CSM atlas/resources are still shared"
			                  : "CSM resources are unavailable");
		addPassDiagnostic("GPUDrivenGBuffer",
		                  hasSceneAttachments && hasMaterialDescriptors
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  hasSceneAttachments && hasMaterialDescriptors
			                  ? "MDI submission; attachments/material descriptors are still bridged"
			                  : "Missing scene attachments or material descriptors");
		addPassDiagnostic("GPUDrivenLightPass",
		                  hasLightingResources ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
		                  hasLightingResources
			                  ? "Deferred lighting writes FP16 HDR scene color using GPUDriven-owned lighting descriptors"
			                  : "Lighting resources are unavailable");
		addPassDiagnostic("GPUDrivenSkybox",
		                  m_skyboxPass != nullptr && m_runtimeStats.iblDiagnostics.enabled
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.iblDiagnostics.enabled
			                  ? "Depth-tested HDRI skybox fills reverse-Z background pixels"
			                  : "Skybox is disabled with IBL");
		addPassDiagnostic("GPUDrivenForwardPass",
		                  hasSceneAttachments ? GPUDrivenOwnershipState::gpuOwned : GPUDrivenOwnershipState::disabled,
		                  hasSceneAttachments
			                  ? "Transparent visibility is GPU-patched into HDR scene color; ordering seed is still CPU-generated"
			                  : "Disabled until scene attachments are valid");
		addPassDiagnostic("GPUDrivenVelocity",
		                  m_velocityPass != nullptr && m_runtimeStats.postProcessDiagnostics.velocityBufferActive
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.postProcessDiagnostics.velocityBufferActive
			                  ? "Fullscreen camera velocity writes R16G16 motion vectors for temporal passes"
			                  : "Velocity buffer or pipeline is unavailable");
		addPassDiagnostic("GPUDrivenTAAResolve",
		                  m_taaResolvePass != nullptr && m_runtimeStats.postProcessDiagnostics.taaPassActive
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.postProcessDiagnostics.taaPassActive
			                  ? "Mobile temporal resolve writes the current HDR history target"
			                  : "TAA is disabled or pipeline is unavailable");
		addPassDiagnostic("GPUDrivenBloomPrefilter",
		                  m_bloomPrefilterPass != nullptr && m_runtimeStats.postProcessDiagnostics.bloomPassActive
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.postProcessDiagnostics.bloomPassActive
			                  ? "Half-resolution mobile bloom prefilter is active"
			                  : "Bloom is disabled or pipeline is unavailable");
		addPassDiagnostic("GPUDrivenBloomDownsample",
		                  m_bloomDownsamplePass != nullptr && m_runtimeStats.postProcessDiagnostics.bloomPassActive
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.postProcessDiagnostics.bloomPassActive
			                  ? "Quarter-resolution mobile bloom downsample is active"
			                  : "Bloom is disabled or pipeline is unavailable");
		addPassDiagnostic("GPUDrivenFinalColor",
		                  m_finalColorPass != nullptr && m_runtimeStats.postProcessDiagnostics.finalColorPassActive
			                  ? GPUDrivenOwnershipState::gpuOwned
			                  : GPUDrivenOwnershipState::disabled,
		                  m_runtimeStats.postProcessDiagnostics.finalColorPassActive
			                  ? "Final color pass owns exposure, bloom composite, tone mapping, and SDR output"
			                  : "Final color pipeline is unavailable");
		addPassDiagnostic("GPUDrivenDebug",
		                  GPUDrivenOwnershipState::disabled,
		                  "Pass is registered but debug execution is currently disabled");
		addPassDiagnostic("GPUDrivenPresent",
		                  GPUDrivenOwnershipState::gpuOwned,
		                  "GPU-driven present path owns the copy/blit step");
		addPassDiagnostic("GPUDrivenImgui",
		                  sceneRenderingSuspended ? GPUDrivenOwnershipState::bridged : GPUDrivenOwnershipState::bridged,
		                  "ImGui rendering is intentionally shared with the app UI backend");
	}

	void GPUDrivenRenderer::initVisibilitySortResources()
	{
		shutdownVisibilitySortResources();

		if (getBackendDeviceToken() == 0)
		{
			return;
		}

		const uint32_t frameCount = std::max(1u, getSwapchainImageCount());

		// 2 storage buffers (key + value), compute. RHI ArgumentLayout + per-frame owned tables.
		const std::array<ArgumentLayoutEntry, 2> sortEntries{
			{
				{0, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{1, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
			}
		};
		m_visibilitySortArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{.entries = sortEntries.data(), .entryCount = static_cast<uint32_t>(sortEntries.size())});

#ifdef USE_SLANG
		const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{m_visibilitySortArgumentLayout}};
		const std::array<rhi::PipelinePushConstantRange, 1> pushConstants{
			{
				rhi::PipelinePushConstantRange{
					.stages = rhi::ShaderStage::compute,
					.offset = 0,
					.size = sizeof(shaderio::BitonicSortPushConstants)
				},
			}
		};
		const rhi::ComputePipelineDesc desc{
			.shaderStage =
			rhi::PipelineShaderStageDesc{
				.stage = rhi::ShaderStage::compute,
				.spirvCode = shader_bitonic_sort_slang,
				.spirvSize = std::size(shader_bitonic_sort_slang) * sizeof(uint32_t),
				.entryPoint = "bitonicSortMain",
			},
			.argumentLayouts = argumentLayouts.data(),
			.argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
			.pushConstantRanges = pushConstants.data(),
			.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size()),
			.specializationVariant = 0x6401u,
		};
		m_visibilitySortPipelineHandle = getRHIDevice().createComputePipeline(desc);
#endif

		m_visibilitySortFrames.resize(frameCount);
		for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			m_visibilitySortFrames[frameIndex].argumentTable =
				m_renderer.createPersistentArgumentTable(m_visibilitySortArgumentLayout, "gpu-driven-visibility-sort");
		}
	}

	void GPUDrivenRenderer::shutdownVisibilitySortResources()
	{
		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		const VmaAllocator allocator = reinterpret_cast<VmaAllocator>(getAllocatorToken());
		rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
		for (VisibilitySortFrameResources& frameResources : m_visibilitySortFrames)
		{
			destroyBuffer(allocator, frameResources.uploadKeyBuffer);
			destroyBuffer(allocator, frameResources.uploadValueBuffer);
			destroyBuffer(allocator, frameResources.keyBuffer);
			destroyBuffer(allocator, frameResources.valueBuffer);
			if (resourceTable != nullptr)
			{
				if (!frameResources.keyBufferHandle.isNull())
					resourceTable->removeBuffer(
						frameResources.keyBufferHandle);
				if (!frameResources.valueBufferHandle.isNull())
					resourceTable->removeBuffer(
						frameResources.valueBufferHandle);
				if (!frameResources.uploadKeyBufferHandle.isNull())
					resourceTable->removeBuffer(
						frameResources.uploadKeyBufferHandle);
				if (!frameResources.uploadValueBufferHandle.isNull())
					resourceTable->removeBuffer(
						frameResources.uploadValueBufferHandle);
			}
			frameResources = {}; // owned ArgumentTable freed by RenderDevice::destroyArgumentTablesAndLayouts
		}
		m_visibilitySortFrames.clear();

		getRHIDevice().destroyPipeline(m_visibilitySortPipelineHandle);
		m_visibilitySortPipelineHandle = {};
		if (nativeDevice != VK_NULL_HANDLE)
		{
		}
		m_visibilitySortArgumentLayout = {};
	}

	void GPUDrivenRenderer::initTransparentVisibilityPatchResources()
	{
		shutdownTransparentVisibilityPatchResources();

		if (getBackendDeviceToken() == 0)
		{
			return;
		}

		const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
		const std::array<ArgumentLayoutEntry, 6> patchEntries{
			{
				{0, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{1, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{2, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{3, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{4, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
				{5, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageBuffer, 1},
			}
		};
		m_transparentVisibilityPatchArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = patchEntries.data(), .entryCount = static_cast<uint32_t>(patchEntries.size())
			});

#ifdef USE_SLANG
		const std::array<rhi::ArgumentLayoutHandle, 1> argumentLayouts{{m_transparentVisibilityPatchArgumentLayout}};
		const std::array<rhi::PipelinePushConstantRange, 1> pushConstants{
			{
				rhi::PipelinePushConstantRange{
					.stages = rhi::ShaderStage::compute,
					.offset = 0,
					.size = sizeof(shaderio::TransparentVisibilityPatchPushConstants)
				},
			}
		};
		const rhi::ComputePipelineDesc desc{
			.shaderStage =
			rhi::PipelineShaderStageDesc{
				.stage = rhi::ShaderStage::compute,
				.spirvCode = shader_transparent_visibility_patch_slang,
				.spirvSize = std::size(shader_transparent_visibility_patch_slang) * sizeof(uint32_t),
				.entryPoint = "transparentVisibilityPatchMain",
			},
			.argumentLayouts = argumentLayouts.data(),
			.argumentLayoutCount = static_cast<uint32_t>(argumentLayouts.size()),
			.pushConstantRanges = pushConstants.data(),
			.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size()),
			.specializationVariant = 0x7001u,
		};
		m_transparentVisibilityPatchPipelineHandle = getRHIDevice().createComputePipeline(desc);
#endif

		m_transparentVisibilityPatchFrames.resize(frameCount);
		for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			m_transparentVisibilityPatchFrames[frameIndex].argumentTables[0] =
				m_renderer.createPersistentArgumentTable(m_transparentVisibilityPatchArgumentLayout,
				                                         "transparent-visibility-patch-gbuffer");
			m_transparentVisibilityPatchFrames[frameIndex].argumentTables[1] =
				m_renderer.createPersistentArgumentTable(m_transparentVisibilityPatchArgumentLayout,
				                                         "transparent-visibility-patch-forward");
		}
	}

	void GPUDrivenRenderer::shutdownTransparentVisibilityPatchResources()
	{
		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		const VmaAllocator allocator = reinterpret_cast<VmaAllocator>(getAllocatorToken());
		rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
		for (TransparentVisibilityFrameResources& frameResources : m_transparentVisibilityPatchFrames)
		{
			destroyBuffer(allocator, frameResources.prefixBuffers[0]);
			destroyBuffer(allocator, frameResources.prefixBuffers[1]);
			if (resourceTable != nullptr)
			{
				for (rhi::BufferHandle& handle : frameResources.prefixBufferHandles)
				{
					if (!handle.isNull()) resourceTable->removeBuffer(handle);
				}
				for (rhi::BufferHandle& handle : frameResources.sourceIndirectBufferHandles)
				{
					if (!handle.isNull()) resourceTable->removeBuffer(handle);
				}
				for (rhi::BufferHandle& handle : frameResources.targetIndirectBufferHandles)
				{
					if (!handle.isNull()) resourceTable->removeBuffer(handle);
				}
			}
			frameResources = {};
			// owned ArgumentTables/layout are freed by RenderDevice::destroyArgumentTablesAndLayouts
		}
		m_transparentVisibilityPatchFrames.clear();

		getRHIDevice().destroyPipeline(m_transparentVisibilityPatchPipelineHandle);
		m_transparentVisibilityPatchPipelineHandle = {};

		if (nativeDevice != VK_NULL_HANDLE)
		{
			(void)nativeDevice;
		}
		m_transparentVisibilityPatchArgumentLayout = {};
	}

	void GPUDrivenRenderer::updateTransparentVisibilityPatchArgumentTable(uint32_t frameIndex,
	                                                                      uint64_t sortKeyBufferHandle,
	                                                                      uint64_t sortValueBufferHandle,
	                                                                      uint64_t sourceIndirectBufferHandle,
	                                                                      uint64_t targetIndirectBufferHandle)
	{
		if (frameIndex >= m_transparentVisibilityPatchFrames.size())
		{
			return;
		}

		TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
		const uint32_t descriptorSetIndex =
			targetIndirectBufferHandle == m_renderer.getForwardMDIIndirectBuffer(frameIndex) ? 1u : 0u;
		const uint64_t prefixABufferHandle = reinterpret_cast<uint64_t>(frameResources.prefixBuffers[0].buffer);
		const uint64_t prefixBBufferHandle = reinterpret_cast<uint64_t>(frameResources.prefixBuffers[1].buffer);
		if (frameResources.argumentTables[descriptorSetIndex].isNull() || sortKeyBufferHandle == 0
			|| sortValueBufferHandle == 0 || sourceIndirectBufferHandle == 0 || targetIndirectBufferHandle == 0
			|| prefixABufferHandle == 0 || prefixBBufferHandle == 0)
		{
			return;
		}

		if (frameResources.boundSortKeyHandles[descriptorSetIndex] == sortKeyBufferHandle
			&& frameResources.boundSortValueHandles[descriptorSetIndex] == sortValueBufferHandle
			&& frameResources.boundSourceIndirectHandles[descriptorSetIndex] == sourceIndirectBufferHandle
			&& frameResources.boundTargetIndirectHandles[descriptorSetIndex] == targetIndirectBufferHandle
			&& frameResources.boundPrefixAHandles[descriptorSetIndex] == prefixABufferHandle
			&& frameResources.boundPrefixBHandles[descriptorSetIndex] == prefixBBufferHandle)
		{
			return;
		}

		rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
		if (resourceTable == nullptr)
		{
			return;
		}
		const auto rebind = [&](rhi::BufferHandle& handle, uint64_t nativeBuffer)
		{
			if (handle.isNull())
			{
				rhi::vulkan::BufferRecord rec{};
				rec.nativeBuffer = nativeBuffer;
				rec.owned = false;
				handle = resourceTable->registerBuffer(rec);
			}
			else
			{
				resourceTable->updateBuffer(handle, nativeBuffer);
			}
		};
		rebind(frameResources.sourceIndirectBufferHandles[descriptorSetIndex], sourceIndirectBufferHandle);
		rebind(frameResources.targetIndirectBufferHandles[descriptorSetIndex], targetIndirectBufferHandle);
		rebind(frameResources.prefixBufferHandles[0], prefixABufferHandle);
		rebind(frameResources.prefixBufferHandles[1], prefixBBufferHandle);

		const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
		const std::array<rhi::ArgumentWrite, 6> writes{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = sortResources.keyBufferHandle
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = sortResources.valueBufferHandle
				},
				rhi::ArgumentWrite{
					.binding = 2, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.sourceIndirectBufferHandles[descriptorSetIndex]
				},
				rhi::ArgumentWrite{
					.binding = 3, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.targetIndirectBufferHandles[descriptorSetIndex]
				},
				rhi::ArgumentWrite{
					.binding = 4, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.prefixBufferHandles[0]
				},
				rhi::ArgumentWrite{
					.binding = 5, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.prefixBufferHandles[1]
				},
			}
		};
		if (sortResources.keyBufferHandle.isNull() || sortResources.valueBufferHandle.isNull()
			|| frameResources.sourceIndirectBufferHandles[descriptorSetIndex].isNull()
			|| frameResources.targetIndirectBufferHandles[descriptorSetIndex].isNull()
			|| frameResources.prefixBufferHandles[0].isNull()
			|| frameResources.prefixBufferHandles[1].isNull())
		{
			return;
		}
		m_renderer.updateArgumentTable(frameResources.argumentTables[descriptorSetIndex], writes.data(),
		                               static_cast<uint32_t>(writes.size()));
		frameResources.boundSortKeyHandles[descriptorSetIndex] = sortKeyBufferHandle;
		frameResources.boundSortValueHandles[descriptorSetIndex] = sortValueBufferHandle;
		frameResources.boundSourceIndirectHandles[descriptorSetIndex] = sourceIndirectBufferHandle;
		frameResources.boundTargetIndirectHandles[descriptorSetIndex] = targetIndirectBufferHandle;
		frameResources.boundPrefixAHandles[descriptorSetIndex] = prefixABufferHandle;
		frameResources.boundPrefixBHandles[descriptorSetIndex] = prefixBBufferHandle;
	}

	uint32_t GPUDrivenRenderer::getPreviousFrameIndex(uint32_t frameIndex) const
	{
		const uint32_t frameCount = std::max(1u, getSwapchainImageCount());
		return (frameIndex + frameCount - 1u) % frameCount;
	}

	bool GPUDrivenRenderer::prepareAndDispatchVisibilityPatch(rhi::CommandBuffer& cmdBuffer,
	                                                          uint32_t frameIndex,
	                                                          uint64_t targetIndirectBufferHandle,
	                                                          uint32_t categoryValue,
	                                                          uint32_t outputOffset)
	{
		if (frameIndex >= m_transparentVisibilityPatchFrames.size()
			|| frameIndex >= m_visibilitySortFrames.size()
			|| m_transparentVisibilityPatchPipelineHandle.isNull()
			|| targetIndirectBufferHandle == 0)
		{
			return false;
		}

		const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
		if (sortResources.activeElementCount == 0 || sortResources.valueBuffer.buffer == VK_NULL_HANDLE)
		{
			return false;
		}

		const uint64_t sourceIndirectBufferHandle = m_renderer.getGPUCullingIndirectBufferOpaque(frameIndex);
		if (sourceIndirectBufferHandle == 0)
		{
			return false;
		}

		updateTransparentVisibilityPatchArgumentTable(frameIndex,
		                                              reinterpret_cast<uint64_t>(sortResources.keyBuffer.buffer),
		                                              reinterpret_cast<uint64_t>(sortResources.valueBuffer.buffer),
		                                              sourceIndirectBufferHandle,
		                                              targetIndirectBufferHandle);

		const TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
		const uint32_t descriptorSetIndex =
			targetIndirectBufferHandle == m_renderer.getForwardMDIIndirectBuffer(frameIndex) ? 1u : 0u;
		if (frameResources.argumentTables[descriptorSetIndex].isNull()
			|| frameResources.prefixBuffers[0].buffer == VK_NULL_HANDLE
			|| frameResources.prefixBuffers[1].buffer == VK_NULL_HANDLE
			|| frameResources.prefixCapacity < sortResources.paddedElementCount)
		{
			return false;
		}

		const PipelineHandle pipelineHandle = m_transparentVisibilityPatchPipelineHandle;
		const rhi::ArgumentTableHandle tableHandle = frameResources.argumentTables[descriptorSetIndex];
		if (pipelineHandle.isNull() || tableHandle.isNull())
		{
			return false;
		}

		const uint32_t elementCount = sortResources.paddedElementCount;
		const uint32_t groupCount = (elementCount + 63u) / 64u;

		shaderio::TransparentVisibilityPatchPushConstants pushConstants{
			.elementCount = sortResources.paddedElementCount,
			.categoryMask = kVisibilitySortCategoryMask,
			.categoryValue = categoryValue,
			.outputOffset = outputOffset,
			.mode = 0u,
			.scanOffset = 0u,
			.scanBufferIndex = 0u,
			._padding0 = 0u,
		};

		// Each dispatch records in its own compute-pass scope so the inter-dispatch barriers
		// (which live on the CommandBuffer) can sit between them.
		const auto dispatchPatch = [&](const shaderio::TransparentVisibilityPatchPushConstants& pc)
		{
			rhi::ComputeEncoder* enc = cmdBuffer.beginComputePass();
			enc->setPipeline(pipelineHandle);
			enc->setArgumentTable(0, tableHandle);
			enc->setRootConstants(kPrimaryRootConstantsSlot, &pc, sizeof(pc));
			enc->dispatch(rhi::DispatchDesc{.groupCountX = groupCount, .groupCountY = 1u, .groupCountZ = 1u});
			cmdBuffer.endEncoding();
		};
		// Ping-pong prefix scan: producer compute writes are made visible to the next compute read
		// (RAW); write-after-read on the alternate buffer is covered by the execution dependency.
		const auto barrierComputeToCompute = [&]()
		{
			cmdBuffer.barrier(rhi::StageFlags::compute, rhi::StageFlags::compute, rhi::HazardFlags::bufferWrites);
		};

		dispatchPatch(pushConstants);
		barrierComputeToCompute();

		uint32_t scanBufferIndex = 0u;
		for (uint32_t scanOffset = 1u; scanOffset < elementCount; scanOffset <<= 1u)
		{
			pushConstants.mode = 1u;
			pushConstants.scanOffset = scanOffset;
			pushConstants.scanBufferIndex = scanBufferIndex;
			dispatchPatch(pushConstants);
			barrierComputeToCompute();
			scanBufferIndex = 1u - scanBufferIndex;
		}

		pushConstants.mode = 2u;
		pushConstants.scanOffset = 0u;
		pushConstants.scanBufferIndex = scanBufferIndex;
		dispatchPatch(pushConstants);

		// Same-pass/local barrier: final patched indirect args are consumed by
		// drawIndexedIndirect(Count) on the same command stream.
		cmdBuffer.barrier(rhi::StageFlags::compute, rhi::StageFlags::commandInput,
		                  rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites);
		return true;
	}

	void GPUDrivenRenderer::ensureVisibilitySortCapacity(uint32_t frameIndex, uint32_t requiredCount)
	{
		if (frameIndex >= m_visibilitySortFrames.size())
		{
			return;
		}

		VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
		TransparentVisibilityFrameResources* patchFrameResources = frameIndex < m_transparentVisibilityPatchFrames.
		                                                           size()
			                                                           ? &m_transparentVisibilityPatchFrames[frameIndex]
			                                                           : nullptr;
		const bool growSortBuffers = frameResources.capacity < requiredCount;
		const bool growPatchBuffers = patchFrameResources != nullptr && patchFrameResources->prefixCapacity <
			requiredCount;
		if (!growSortBuffers && !growPatchBuffers)
		{
			return;
		}

		const VkDevice nativeDevice = reinterpret_cast<VkDevice>(getBackendDeviceToken());
		const VmaAllocator allocator = reinterpret_cast<VmaAllocator>(getAllocatorToken());
		if ((growSortBuffers && frameResources.capacity > 0) || (growPatchBuffers && patchFrameResources->prefixCapacity
			> 0))
		{
			waitForIdle();
		}

		const uint64_t bufferSize = static_cast<uint64_t>(requiredCount) * sizeof(uint32_t);
		if (growSortBuffers)
		{
			destroyBuffer(allocator, frameResources.uploadKeyBuffer);
			destroyBuffer(allocator, frameResources.uploadValueBuffer);
			destroyBuffer(allocator, frameResources.keyBuffer);
			destroyBuffer(allocator, frameResources.valueBuffer);

			const upload::NativeUploadContext uploadContext{
				.device = reinterpret_cast<uintptr_t>(nativeDevice),
				.allocator = reinterpret_cast<uintptr_t>(allocator),
			};
			frameResources.uploadKeyBuffer = toUtilsBuffer(
				upload::createMappedUploadStagingBuffer(uploadContext, bufferSize));
			frameResources.uploadValueBuffer = toUtilsBuffer(
				upload::createMappedUploadStagingBuffer(uploadContext, bufferSize));
			frameResources.keyBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
			frameResources.valueBuffer = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
			frameResources.capacity = requiredCount;
			updateVisibilitySortArgumentTable(frameIndex);
		}

		if (growPatchBuffers)
		{
			destroyBuffer(allocator, patchFrameResources->prefixBuffers[0]);
			destroyBuffer(allocator, patchFrameResources->prefixBuffers[1]);
			patchFrameResources->prefixBuffers[0] = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
			patchFrameResources->prefixBuffers[1] = createDeviceLocalStorageBuffer(nativeDevice, allocator, bufferSize);
			patchFrameResources->prefixCapacity = requiredCount;
			patchFrameResources->boundPrefixAHandles = {};
			patchFrameResources->boundPrefixBHandles = {};
		}
	}

	void GPUDrivenRenderer::updateVisibilitySortArgumentTable(uint32_t frameIndex)
	{
		if (frameIndex >= m_visibilitySortFrames.size())
		{
			return;
		}

		VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
		if (frameResources.argumentTable.isNull()
			|| frameResources.keyBuffer.buffer == VK_NULL_HANDLE || frameResources.valueBuffer.buffer == VK_NULL_HANDLE)
		{
			return;
		}

		// key/value buffers grow on capacity change: register the owned=false mirror once, then
		// update it in place so the ArgumentTable resolves the current native buffer.
		rhi::vulkan::VulkanResourceTable* resourceTable = m_renderer.getResourceTable();
		const auto rebind = [&](rhi::BufferHandle& handle, VkBuffer buffer)
		{
			const uint64_t native = reinterpret_cast<uint64_t>(buffer);
			if (handle.isNull())
			{
				rhi::vulkan::BufferRecord rec{};
				rec.nativeBuffer = native;
				rec.owned = false;
				handle = resourceTable->registerBuffer(rec);
			}
			else
			{
				resourceTable->updateBuffer(handle, native);
			}
		};
		rebind(frameResources.keyBufferHandle, frameResources.keyBuffer.buffer);
		rebind(frameResources.valueBufferHandle, frameResources.valueBuffer.buffer);
		rebind(frameResources.uploadKeyBufferHandle, frameResources.uploadKeyBuffer.buffer);
		rebind(frameResources.uploadValueBufferHandle, frameResources.uploadValueBuffer.buffer);

		const std::array<rhi::ArgumentWrite, 2> writes{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = frameResources.keyBufferHandle
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = frameResources.valueBufferHandle
				},
			}
		};
		m_renderer.updateArgumentTable(frameResources.argumentTable, writes.data(),
		                               static_cast<uint32_t>(writes.size()));
	}

	void GPUDrivenRenderer::prepareVisibilitySortInputs(uint32_t frameIndex)
	{
		if (frameIndex >= m_visibilitySortFrames.size())
		{
			return;
		}

		VisibilitySortFrameResources& frameResources = m_visibilitySortFrames[frameIndex];
		if (m_visibilitySortInputObjects.empty() || m_visibilitySortInputKeys.size() != m_visibilitySortInputObjects.
			size())
		{
			frameResources.activeElementCount = 0;
			frameResources.paddedElementCount = 0;
			return;
		}

		const uint32_t paddedCount = nextPowerOfTwo(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
		ensureVisibilitySortCapacity(frameIndex, paddedCount);
		if (frameResources.uploadKeyBuffer.mapped == nullptr || frameResources.uploadValueBuffer.mapped == nullptr)
		{
			return;
		}

		auto* mappedKeys = static_cast<uint32_t*>(frameResources.uploadKeyBuffer.mapped);
		auto* mappedValues = static_cast<uint32_t*>(frameResources.uploadValueBuffer.mapped);
		const uint32_t activeCount = static_cast<uint32_t>(m_visibilitySortInputObjects.size());
		std::memcpy(mappedKeys, m_visibilitySortInputKeys.data(), activeCount * sizeof(uint32_t));
		std::memcpy(mappedValues, m_visibilitySortInputObjects.data(), activeCount * sizeof(uint32_t));
		if (paddedCount > activeCount)
		{
			std::fill(mappedKeys + activeCount, mappedKeys + paddedCount, 0xffffffffu);
			std::fill(mappedValues + activeCount, mappedValues + paddedCount, 0xffffffffu);
		}
		VK_CHECK(
			vmaFlushAllocation(reinterpret_cast<VmaAllocator>(getAllocatorToken()), frameResources.uploadKeyBuffer.
				allocation, 0, VK_WHOLE_SIZE));
		VK_CHECK(
			vmaFlushAllocation(reinterpret_cast<VmaAllocator>(getAllocatorToken()), frameResources.uploadValueBuffer.
				allocation, 0, VK_WHOLE_SIZE));

		frameResources.activeElementCount = activeCount;
		frameResources.paddedElementCount = paddedCount;
	}
} // namespace demo
