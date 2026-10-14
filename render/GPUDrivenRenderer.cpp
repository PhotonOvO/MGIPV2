#include "GPUDrivenRenderer.h"
#include "../common/ProfilerMarkers.h"
#include "ArgumentTables.h"
#include "BatchUploadContext.h"
#include "EmbeddedShaderLibrary.h"
#include "MeshSDFQuality.h"
#include "UploadUtils.h"
#include "../loader/Ktx2Loader.h"
#include "../loader/SDFLoader.h"

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
#include "../scene/SceneUploadPlanner.h"
#include "../scene/TransformSystem.h"
#include <mgif/scene_editor/SceneChangeCanonicalizer.h>
#endif

#include "_autogen/ao_denoise.slang.h"
#include "_autogen/clustered_light_cull.slang.h"
#include "_autogen/gtao.slang.h"
#include "_autogen/shader.bitonic_sort.slang.h"
#include "_autogen/shader.light_culling.slang.h"
#include "_autogen/shader.light_gpu_driven.slang.h"
#include "_autogen/shader.transparent_visibility_patch.slang.h"
#include "_autogen/ssr_denoise.slang.h"
#include "_autogen/ssr_trace.slang.h"
#if !defined(__ANDROID__)
#ifdef USE_SLANG
#include "_autogen/editor_highlight.slang.h"
#include "_autogen/editor_overlay.slang.h"
#include "_autogen/editor_picking.slang.h"
#endif
#endif
#ifdef DEMO_HAS_DXC_FINAL_SPIRV
#include "_autogen/shader.light.final.dxc.spv.h"
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <utility>

namespace demo
{
	namespace
	{
		[[nodiscard]] uint64_t nextEditorRendererIdentityValue() noexcept
		{
			static std::atomic<uint64_t> next{1u};
			uint64_t value = next.load(std::memory_order_relaxed);
			while (value != 0u && value != std::numeric_limits<uint64_t>::max())
			{
				if (next.compare_exchange_weak(
					value, value + 1u, std::memory_order_relaxed))
				{
					return value;
				}
			}
			return 0u;
		}

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
		constexpr uint32_t kGPUDrivenLightPassTextureCount = kPackedGBufferTargetCount + 23u;
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
		// DDGI (Wave D3-1): probe atlas SRVs for the lighting SampleProbe path
		// (shader.light.slang kDDGIIrradianceAtlasIndex / kDDGIDepthAtlasIndex).
		constexpr uint32_t kGPUDrivenLightPassDDGIIrradianceIndex = kPackedGBufferTargetCount + 18u;
		constexpr uint32_t kGPUDrivenLightPassDDGIDepthIndex = kPackedGBufferTargetCount + 19u;
		constexpr uint32_t kGPUDrivenLightPassFlaxProbesDataIndex = kPackedGBufferTargetCount + 20u;
		constexpr uint32_t kGPUDrivenLightPassFlaxProbesDistanceIndex = kPackedGBufferTargetCount + 21u;
		constexpr uint32_t kGPUDrivenLightPassFlaxProbesIrradianceIndex = kPackedGBufferTargetCount + 22u;
		static_assert(kGPUDrivenLightPassTextureCount == shaderio::LLightingTextureCapacity,
		              "Metal lighting texture array must match the RHI lighting table");
		constexpr rhi::TextureFormat kGPUDrivenAOFormat          = rhi::TextureFormat::r16Sfloat;
		constexpr rhi::TextureFormat kGPUDrivenSSRFormat         = rhi::TextureFormat::rgba16Sfloat;
		constexpr rhi::TextureFormat kGPUDrivenShadowAtlasFormat = rhi::TextureFormat::d32Sfloat;

		// Visibility-patch synchronization contract:
		// 1. Every prefix producer is followed by compute-write -> compute-read
		//    visibility before the next scan/final dispatch consumes its output.
		//    The compute execution dependency also orders the alternating
		//    write-after-read side of the ping-pong pair.
		// 2. Reusing the pair for another category requires an explicit
		//    compute-read -> compute-write dependency before mode 0 overwrites A.
		// 3. Final indirect/count output remains compute-write -> draw-indirect-read.
		void recordVisibilityPatchScanReadAfterWriteBarrier(rhi::CommandBuffer& cmdBuffer)
		{
			cmdBuffer.barrier(rhi::StageFlags::compute,
			                  rhi::StageFlags::compute,
			                  rhi::HazardFlags::bufferWrites);
		}

		void recordVisibilityPatchPrefixReuseWriteAfterReadBarrier(rhi::CommandBuffer& cmdBuffer)
		{
			cmdBuffer.barrier(rhi::StageFlags::compute,
			                  rhi::StageFlags::compute,
			                  rhi::HazardFlags::readBeforeWrite);
		}

		void recordVisibilityPatchIndirectReadBarrier(rhi::CommandBuffer& cmdBuffer)
		{
			cmdBuffer.barrier(rhi::StageFlags::compute,
			                  rhi::StageFlags::commandInput,
			                  rhi::HazardFlags::drawArguments | rhi::HazardFlags::bufferWrites);
		}

		uint64_t estimateImageBytes(rhi::Extent2D extent, uint32_t bytesPerPixel)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height) * bytesPerPixel;
		}

		[[nodiscard]] uint32_t textureUploadMipExtent(uint32_t extent, uint32_t mip) noexcept
		{
			return mip >= 32u ? 1u : std::max(extent >> mip, 1u);
		}

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		[[nodiscard]] editor::SceneDigest concreteProjectionIdentityDigest(
			editor::StableSceneId id,
			uint64_t runtimeIndex)
		{
			std::vector<uint8_t> bytes;
			bytes.reserve(25u);
			const auto append = [&bytes](uint64_t value, unsigned count)
			{
				for (unsigned index = 0u; index < count; ++index)
				{
					bytes.push_back(static_cast<uint8_t>(value & 0xffu));
					value >>= 8u;
				}
			};
			append(static_cast<uint8_t>(id.kind), 1u);
			append(id.authority.value, 8u);
			append(id.slot, 4u);
			append(id.generation, 4u);
			append(runtimeIndex, 8u);
			return editor::SceneChangeCanonicalizer::computeSha256(bytes);
		}
#endif

		constexpr const char* kGPUDrivenDefaultIBLEnvironmentPath = "resources/environment/lilienstein_4k.ktx2";

		uint32_t bytesPerPixelForFormat(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::bgra8Unorm:
			case rhi::TextureFormat::rgba8Unorm:
			case rhi::TextureFormat::rgba8Srgb:
			case rhi::TextureFormat::r11g11b10Ufloat:
				return 4u;
			case rhi::TextureFormat::rg16Sfloat:
				return 4u;
			case rhi::TextureFormat::rgba16Sfloat:
				return 8u;
			default:
				return 0u;
			}
		}

		uint64_t estimateTextureBytes(rhi::Extent2D extent, rhi::TextureFormat format)
		{
			return static_cast<uint64_t>(extent.width) * static_cast<uint64_t>(extent.height)
				* static_cast<uint64_t>(bytesPerPixelForFormat(format));
		}


		const char* formatDisplayName(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::bgra8Unorm:
				return "B8G8R8A8_UNORM";
			case rhi::TextureFormat::rgba8Unorm:
				return "R8G8B8A8_UNORM";
			case rhi::TextureFormat::rgba8Srgb:
				return "R8G8B8A8_SRGB";
			case rhi::TextureFormat::rgba16Sfloat:
				return "R16G16B16A16_SFLOAT";
			case rhi::TextureFormat::r11g11b10Ufloat:
				return "B10G11R11_UFLOAT";
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

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		enum class TransactionTexturePlanError : uint8_t
		{
			none = 0,
			payloadRange,
			payloadKind,
			rawMetadata,
			ktxDecode,
			ktxMipRange,
			unsupportedFormat,
			footprint,
		};

		struct TransactionTexturePlanValidation
		{
			TransactionTexturePlanError error{TransactionTexturePlanError::none};
			TextureUploadValidationError footprintError{TextureUploadValidationError::none};
			uint32_t textureIndex{0u};
			std::string detail;

			[[nodiscard]] bool valid() const noexcept
			{
				return error == TransactionTexturePlanError::none;
			}
		};

		[[nodiscard]] const char* transactionTexturePlanErrorName(TransactionTexturePlanError error) noexcept
		{
			switch (error)
			{
			case TransactionTexturePlanError::none:              return "none";
			case TransactionTexturePlanError::payloadRange:      return "payload-range";
			case TransactionTexturePlanError::payloadKind:       return "payload-kind";
			case TransactionTexturePlanError::rawMetadata:       return "raw-metadata";
			case TransactionTexturePlanError::ktxDecode:         return "ktx-decode";
			case TransactionTexturePlanError::ktxMipRange:       return "ktx-mip-range";
			case TransactionTexturePlanError::unsupportedFormat: return "unsupported-format";
			case TransactionTexturePlanError::footprint:         return "footprint";
			default:                                             return "unknown";
			}
		}

		[[nodiscard]] std::string transactionTexturePlanErrorMessage(
			const TransactionTexturePlanValidation& validation)
		{
			std::string message = "transaction texture upload validation failed: texture=" +
				std::to_string(validation.textureIndex) + " code=" +
				transactionTexturePlanErrorName(validation.error);
			if (validation.footprintError != TextureUploadValidationError::none)
			{
				message += " footprint=";
				message += textureUploadValidationErrorName(validation.footprintError);
			}
			if (!validation.detail.empty())
			{
				message += " detail=";
				message += validation.detail;
			}
			return message;
		}

		[[nodiscard]] TransactionTexturePlanValidation validateTransactionTextureUploadPlans(
			const SceneAsset& asset,
			const SceneUploadPlan& plan,
			const rhi::Device* device)
		{
			for (const TextureUploadPlan& texturePlan : plan.textures)
			{
				TransactionTexturePlanValidation failure{};
				failure.textureIndex = texturePlan.textureIndex;
				if (texturePlan.textureIndex >= asset.textures.size())
				{
					failure.error = TransactionTexturePlanError::payloadRange;
					failure.detail = "texture index exceeds the candidate asset";
					return failure;
				}
				const SceneTexture& texture = asset.textures[texturePlan.textureIndex];
				const uint64_t payloadCapacity = static_cast<uint64_t>(asset.texturePayload.size());
				if (texturePlan.payloadByteSize == 0u ||
					texturePlan.payloadSrcOffset > payloadCapacity ||
					texturePlan.payloadByteSize > payloadCapacity - texturePlan.payloadSrcOffset ||
					texture.payloadOffset > payloadCapacity ||
					texture.payloadSize > payloadCapacity - texture.payloadOffset ||
					texturePlan.payloadSrcOffset != texture.payloadOffset ||
					texturePlan.payloadByteSize != texture.payloadSize)
				{
					failure.error = TransactionTexturePlanError::payloadRange;
					failure.detail = "plan and asset payload ranges are not the same bounded slice";
					return failure;
				}

				const bool isKtx = texturePlan.payloadKind == TexturePayloadKind::Ktx2Container;
				if (isKtx != texture.isKtx2)
				{
					failure.error = TransactionTexturePlanError::payloadKind;
					failure.detail = "plan payload kind differs from the candidate asset";
					return failure;
				}
				const uint8_t* payload = asset.texturePayload.data() + texturePlan.payloadSrcOffset;
				const size_t payloadSize = static_cast<size_t>(texturePlan.payloadByteSize);
				if (!isKtx)
				{
					if ((texturePlan.format != rhi::TextureFormat::rgba8Unorm &&
						 texturePlan.format != rhi::TextureFormat::rgba8Srgb) ||
						texturePlan.format != texture.format ||
						texturePlan.width != texture.width || texturePlan.height != texture.height ||
						texturePlan.mipLevels != 1u || texture.mipLevels != 1u)
					{
						failure.error = TransactionTexturePlanError::rawMetadata;
						failure.detail = "raw payload must describe one RGBA8 mip";
						return failure;
					}
					const rhi::TextureDesc desc{
						.dimension = rhi::TextureDimension::e2D,
						.format = texturePlan.format,
						.extent = {texturePlan.width, texturePlan.height, 1u},
						.mipLevels = 1u,
						.arrayLayers = 1u,
					};
					const rhi::BufferTextureCopyDesc region{
						.aspect = rhi::TextureAspect::color,
						.layerCount = 1u,
						.width = texturePlan.width,
						.height = texturePlan.height,
						.depth = 1u,
					};
					const TextureUploadValidationResult footprint = validateTextureUploadRegion(
						desc, region, 0u, texturePlan.payloadByteSize, texturePlan.payloadByteSize);
					if (!footprint.valid() || footprint.requiredBytes != texturePlan.payloadByteSize)
					{
						failure.error = TransactionTexturePlanError::footprint;
						failure.footprintError = footprint.error;
						failure.detail = "raw bytes are not the exact decoded RGBA8 footprint";
						return failure;
					}
					continue;
				}

				Ktx2Loader loader;
				Ktx2Loader::Ktx2Texture ktx;
				const Ktx2ContainerValidationResult containerValidation =
					validateKtx2ContainerRanges(std::span<const uint8_t>{payload, payloadSize});
				if (!containerValidation.valid())
				{
					failure.error = TransactionTexturePlanError::ktxMipRange;
					failure.detail = std::string("container-") +
						ktx2ContainerValidationErrorName(containerValidation.error);
					return failure;
				}
				if (!loader.loadFromMemory(payload, payloadSize, ktx))
				{
					failure.error = TransactionTexturePlanError::ktxDecode;
					failure.detail = loader.getLastError();
					return failure;
				}
				if (device != nullptr && !device->isFormatSupported(
						ktx.format, rhi::FormatFeatureFlag::sampledImage))
				{
					failure.error = TransactionTexturePlanError::unsupportedFormat;
					failure.detail = "decoded KTX format is not sampleable";
					return failure;
				}
				const rhi::TextureDesc desc{
					.dimension = rhi::TextureDimension::e2D,
					.format = ktx.format,
					.extent = {ktx.width, ktx.height, 1u},
					.mipLevels = std::max(ktx.mipLevels, 1u),
					.arrayLayers = 1u,
				};
				const uint64_t decodedBytes = static_cast<uint64_t>(ktx.data.size());
				if (ktx.mipOffsets.size() < desc.mipLevels || ktx.mipSizes.size() < desc.mipLevels)
				{
					failure.error = TransactionTexturePlanError::ktxMipRange;
					failure.detail = "decoded KTX mip tables are incomplete";
					return failure;
				}
				for (uint32_t mip = 0u; mip < desc.mipLevels; ++mip)
				{
					const uint64_t offset = ktx.mipOffsets[mip];
					const uint64_t declaredBytes = ktx.mipSizes[mip];
					if (declaredBytes == 0u || offset > decodedBytes || declaredBytes > decodedBytes - offset)
					{
						failure.error = TransactionTexturePlanError::ktxMipRange;
						failure.detail = "decoded KTX mip range exceeds its staging payload";
						return failure;
					}
					const rhi::BufferTextureCopyDesc region{
						.bufferOffset = offset,
						.aspect = rhi::TextureAspect::color,
						.mipLevel = mip,
						.layerCount = 1u,
						.width = textureUploadMipExtent(desc.extent.width, mip),
						.height = textureUploadMipExtent(desc.extent.height, mip),
						.depth = 1u,
					};
					const TextureUploadValidationResult footprint = validateTextureUploadRegion(
						desc, region, 0u, decodedBytes, decodedBytes);
					if (!footprint.valid() || footprint.requiredBytes > declaredBytes)
					{
						failure.error = TransactionTexturePlanError::footprint;
						failure.footprintError = footprint.error;
						failure.detail = "decoded KTX mip is smaller than its exact block footprint";
						return failure;
					}
				}
			}
			return {};
		}
#endif

		[[nodiscard]] rhi::TextureAspect sceneDepthAspect(rhi::TextureFormat format)
		{
			switch (format)
			{
			case rhi::TextureFormat::d24UnormS8:
			case rhi::TextureFormat::d32SfloatS8:
				return rhi::TextureAspect::depthStencil;
			default:
				return rhi::TextureAspect::depth;
			}
		}
	} // namespace

#if !defined(__ANDROID__)
	class GPUDrivenEditorOccurrencePass final : public RenderPassNode
	{
	public:
		explicit GPUDrivenEditorOccurrencePass(GPUDrivenRenderer& renderer) noexcept
			: m_renderer(&renderer)
		{
		}

		[[nodiscard]] const char* getName() const override
		{
			return "GPUDrivenEditorOccurrence";
		}

		[[nodiscard]] HandleSlice<PassResourceDependency>
		getDependencies() const override
		{
			static const std::array dependencies{
				PassResourceDependency::buffer(
					kPassVertexBufferHandle, ResourceAccess::read,
					rhi::ShaderStage::vertex),
			};
			return {dependencies.data(),
			        static_cast<uint32_t>(dependencies.size())};
		}

		void execute(const PassContext& context) const override
		{
			if (m_renderer != nullptr)
			{
				m_renderer->executeEditorOccurrencePass(context);
			}
		}

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};

	class GPUDrivenEditorHighlightPass final : public RenderPassNode
	{
	public:
		explicit GPUDrivenEditorHighlightPass(GPUDrivenRenderer& renderer) noexcept
			: m_renderer(&renderer)
		{
		}

		[[nodiscard]] const char* getName() const override
		{
			return "GPUDrivenEditorHighlight";
		}

		[[nodiscard]] HandleSlice<PassResourceDependency>
		getDependencies() const override
		{
				static const std::array dependencies{
					PassResourceDependency::texture(
						kPassOutputHandle, ResourceAccess::readWrite,
						rhi::StageFlags::rasterColorOut,
						rhi::HazardFlags::textureWrites,
						rhi::ResourceState::ColorAttachment),
			};
			return {dependencies.data(),
			        static_cast<uint32_t>(dependencies.size())};
		}

		void execute(const PassContext& context) const override
		{
			if (m_renderer != nullptr)
			{
				m_renderer->executeEditorHighlightPass(context);
			}
		}

	private:
		GPUDrivenRenderer* m_renderer{nullptr};
	};
#endif

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
	class GPUDrivenSceneTransactionBridge final : public editor::SceneTransactionBackend
	{
	public:
		explicit GPUDrivenSceneTransactionBridge(GPUDrivenRenderer& owner)
			: m_owner(owner)
		{
			m_nextConcreteHandleIndices.fill(1u);
		}

		~GPUDrivenSceneTransactionBridge() override
		{
			(void)destroyResourceSet(m_candidateConcrete != nullptr
				? m_candidateConcrete->resources
				: nullptr);
			(void)destroyResourceSet(m_activeConcrete != nullptr
				? m_activeConcrete->resources
				: nullptr);
			for (RetirementRecord& retirement : m_retirements)
			{
				(void)destroyResourceSet(retirement.resources);
			}
			for (auto& [_, retirement] : m_retirementReservations)
			{
				(void)destroyResourceSet(retirement.resources);
			}
			while (!m_resourceOwners.empty())
			{
				const uint64_t diagnosticId = m_resourceOwners.begin()->first;
				const std::shared_ptr<ResourceSet> resources =
					m_resourceOwners.begin()->second;
				if (resources != nullptr && !resources->destroyed)
				{
					(void)destroyResourceSet(resources);
				}
				m_resourceOwners.erase(diagnosticId);
			}
			m_owner.m_renderer.m_sceneTransactionResources.store(
				nullptr, std::memory_order_release);
			m_owner.m_sceneTransactionRendererState.store(
				nullptr, std::memory_order_release);
			m_owner.m_renderer.m_sceneTransactionPublishedGeneration.store(
				0u, std::memory_order_release);
		}

		RendererSceneTransactionInitializationResult initialize(
			RendererSceneTransactionInitialState initialState) noexcept
		{
			const std::lock_guard lock(m_mutex);
			RendererSceneTransactionInitializationResult result;
			const editor::RendererTargetFence& fence = initialState.publication.fence;
			if (m_publisher != nullptr)
			{
				result.error = editor::SceneApplyError::InvalidState;
				result.message = "scene transaction publisher is already initialized";
				return result;
			}
			if (fence.backend != m_owner.m_renderer.sceneTransactionBackend())
			{
				result.error = editor::SceneApplyError::RendererMismatch;
				result.message = "initial scene fence backend differs from renderer backend";
				return result;
			}
			if (fence.backend == rhi::BackendType::metal &&
				fence.capability != editor::RendererSceneUpdateCapability::Unsupported)
			{
				result.error = editor::SceneApplyError::UnsupportedCapability;
				result.message = "Metal scene transactions are not verified";
				return result;
			}
			std::string ownerError;
			if (!validateProjectionOwner(initialState.ownerCapability,
				initialState.activeAsset, initialState.publication.mapping,
				ownerError))
			{
				result.error = editor::SceneApplyError::StaleObjectGeneration;
				result.message = ownerError.empty()
					? "initial projection owner capability is invalid"
					: std::move(ownerError);
				return result;
			}
			if (!uploadPlanHeaderMatchesFence(initialState.activePlan.header, fence))
			{
				result.error = editor::SceneApplyError::MappingMismatch;
				result.message = "initial upload plan envelope differs from renderer fence";
				return result;
			}
			const SceneUploadPlanValidationResult planValidation =
				SceneUploadPlanner::validate(
					makeSceneAssetView(initialState.activeAsset),
					initialState.activePlan);
			if (!planValidation.valid)
			{
				result.error = editor::SceneApplyError::InvalidPayload;
				result.message = planValidation.error;
				return result;
			}

			try
			{
				ConcreteAuthorityBuild concreteAuthority = buildConcreteAuthority(
					initialState.activeAsset,
					initialState.publication.mapping,
					{},
					m_nextConcreteHandleIndices);
				if (!concreteAuthority.valid())
				{
					result.error = editor::SceneApplyError::StaleObjectGeneration;
					result.message = std::move(concreteAuthority.error);
					return result;
				}
				const rhi::Device* validationDevice =
					m_owner.m_renderer.sceneTransactionDeviceInitialized()
						? &m_owner.m_renderer.getRHIDevice()
						: nullptr;
				const TransactionTexturePlanValidation textureValidation =
					validateTransactionTextureUploadPlans(
						initialState.activeAsset, initialState.activePlan, validationDevice);
				if (!textureValidation.valid())
				{
					result.error = editor::SceneApplyError::InvalidPayload;
					result.message = transactionTexturePlanErrorMessage(textureValidation);
					return result;
				}
				std::shared_ptr<ConcreteGeneration> concrete = buildConcreteGeneration(
					initialState.activeAsset,
					initialState.activePlan,
					initialState.publication.mapping,
					std::move(concreteAuthority.records),
					concreteAuthority.nextHandleIndices);
				concrete->generation = initialState.publication.generation;
				concrete->descriptorGeneration =
					initialState.publication.descriptorGeneration;
				if (m_owner.m_renderer.sceneTransactionDeviceInitialized() &&
					!initialState.publication.resources.empty())
				{
					auto resources = std::make_shared<ResourceSet>();
					resources->device = &m_owner.m_renderer.getRHIDevice();
					resources->deviceRecords = concrete->deviceRecords;
					resources->deviceRecords->frameUse =
						std::make_shared<RenderDevice::SceneTransactionResources::FrameUseState>();
					resources->lastUseTokens =
						initialState.publication.lastUseSubmissionTokens;
					if (rhi::Queue* graphics = resources->device->getQueue(
							rhi::QueueClass::graphics))
					{
						resources->deviceRecords->frameUse->queue = graphics->identity();
					}
					resources->owned.reserve(initialState.publication.resources.size());
					for (const editor::SceneJournalResource& resource :
					     initialState.publication.resources)
					{
						resources->owned.push_back(OwnedResource{.journal = resource});
						m_resourceOwners[resource.diagnosticId.value] = resources;
						m_nextResourceDiagnosticId = std::max(
							m_nextResourceDiagnosticId, resource.diagnosticId.value);
					}
					concrete->resources = std::move(resources);
				}
				m_counters = std::make_unique<editor::SceneDeltaCounters>();
				m_publisher = std::make_unique<editor::RendererScenePublisher>(
					std::move(initialState.publication), *this, *m_counters);
				m_activeAsset = std::move(initialState.activeAsset);
				m_activePlan = std::move(initialState.activePlan);
				m_activeOwnerCapability = std::move(initialState.ownerCapability);
				m_activeConcrete = std::move(concrete);
				m_activeConcrete->rendererState->generation =
					m_activeConcrete->generation;
				m_activeConcrete->rendererState->descriptorGeneration =
					m_activeConcrete->descriptorGeneration;
				m_activeConcrete->deviceRecords->publicationGeneration =
					m_activeConcrete->generation;
				m_owner.m_renderer.m_sceneTransactionResources.store(
					m_activeConcrete->deviceRecords, std::memory_order_release);
				m_owner.m_sceneTransactionRendererState.store(
					m_activeConcrete->rendererState, std::memory_order_release);
				m_owner.m_renderer.m_sceneTransactionPublishedGeneration.store(
					m_activeConcrete->generation, std::memory_order_release);
				m_nextConcreteHandleIndices =
					m_activeConcrete->nextConcreteHandleIndices;
				result.initialized = true;
				result.error = editor::SceneApplyError::None;
			}
			catch (const std::bad_alloc&)
			{
				m_publisher.reset();
				m_counters.reset();
				m_activeConcrete.reset();
				result.error = editor::SceneApplyError::AllocationFailure;
				result.message = "scene transaction initialization allocation failed";
			}
			catch (const std::exception& exception)
			{
				m_publisher.reset();
				m_counters.reset();
				m_activeConcrete.reset();
				result.error = editor::SceneApplyError::InvalidState;
				result.message = exception.what();
			}
			return result;
		}

		editor::SceneApplyResult applySceneChanges(
			const editor::ApplySceneChangeRequest& request) noexcept
		{
			const std::lock_guard lock(m_mutex);
			if (m_publisher == nullptr)
			{
				return uninitializedResult(request);
			}
			std::string ownerError;
			if (m_activeConcrete == nullptr ||
				!validateProjectionOwner(m_activeOwnerCapability,
					m_activeConcrete->asset, m_activeConcrete->mapping,
					ownerError) ||
				!request.ownerCapability.sameOwner(m_activeOwnerCapability) ||
				!validateProjectionOwner(request.ownerCapability,
					m_activeConcrete->asset, request.targetMapping, ownerError))
			{
				return reject(request,
					editor::SceneApplyError::StaleObjectGeneration,
					ownerError.empty()
						? "incremental projection owner capability is invalid"
						: std::move(ownerError),
					editor::FullRebuildReason::MappingMismatch);
			}
			const std::optional<std::string> provenanceError =
				validateLiveHandles(request.changes, request.targetMapping);
			if (provenanceError)
			{
				return reject(request, editor::SceneApplyError::StaleObjectGeneration,
					*provenanceError, editor::FullRebuildReason::MappingMismatch);
			}
			if (matchesPublishedCanonicalIdentity(request))
			{
				if (!canonicalBytesMatchPublished(request))
				{
					return identityCollisionResult(request);
				}
				return m_publisher->applySceneChanges(request);
			}
			if (const auto rejection = m_publisher->preflightSceneChanges(request))
			{
				return *rejection;
			}
			const editor::SceneChangeValidation canonical =
				editor::SceneChangeCanonicalizer::validateCanonical(
					request.changes, request.canonicalBytes);
			if (!canonical.valid())
			{
				return m_publisher->applySceneChanges(request);
			}
			const auto& uploads = request.stagePlan.uploads;
			if (m_owner.m_renderer.sceneTransactionDeviceInitialized() &&
				(!request.stagePlan.submissions.empty() ||
				 !request.stagePlan.retireResourceDiagnosticIds.empty() ||
				 request.stagePlan.placeholderResourceCount != 0u ||
				 uploads.geometryUploadCalls != 0u ||
				 uploads.geometryBytesUploaded != 0u ||
				 uploads.geometryRangesUploaded != 0u ||
				 uploads.textureUploadCalls != 0u ||
				 uploads.textureBytesUploaded != 0u ||
				 uploads.textureSubresourcesUploaded != 0u ||
				 uploads.otherUploadCalls != 0u ||
				 uploads.otherBytesUploaded != 0u ||
				 uploads.otherRangesUploaded != 0u))
			{
				return reject(request, editor::SceneApplyError::InvalidPayload,
					"incremental scene edits cannot stage uploads, submissions, or retirements",
					editor::FullRebuildReason::StructuralTopology);
			}
			if (containsStructuralIncrementalWork(request.changes))
			{
				return reject(request, editor::SceneApplyError::InvalidPayload,
					"hierarchy and topology changes require an explicit full snapshot",
					editor::FullRebuildReason::StructuralTopology);
			}
			m_pendingFullSnapshot = false;
			m_pendingOwnerCapability = request.ownerCapability;
			m_pendingSubmissionCount = static_cast<uint32_t>(
				request.stagePlan.submissions.size());
			m_candidatePrepared = false;
			editor::SceneApplyResult result = m_publisher->applySceneChanges(request);
			if (result.disposition == editor::SceneApplyDisposition::AcceptedIncremental)
			{
				commitCandidate(request.changes, request.targetMapping);
			}
			clearCandidate();
			if (result.disposition == editor::SceneApplyDisposition::AcceptedIncremental)
			{
				// Maintenance is deliberately an accepted-transaction boundary. A
				// stale, gap, mapping, handle, or plan rejection must not collect a
				// retirement that happened to complete just before the request.
				(void)drainRetirements();
				result.countersAfter = m_counters->snapshot();
				result.counterDelta = editor::sceneDeltaCounterDelta(
					result.countersBefore, result.countersAfter);
			}
			return result;
		}

		editor::SceneApplyResult applyFullSceneSnapshot(
			const ApplyFullSceneSnapshotRequest& request) noexcept
		{
			const std::lock_guard lock(m_mutex);
			if (m_publisher == nullptr)
			{
				return uninitializedResult(request.transaction);
			}
			std::string ownerError;
			if (m_activeConcrete == nullptr ||
				!validateProjectionOwner(m_activeOwnerCapability,
					m_activeConcrete->asset, m_activeConcrete->mapping,
					ownerError) ||
				!request.ownerCapability.sameOwner(m_activeOwnerCapability) ||
				!request.ownerCapability.sameSeal(
					request.transaction.ownerCapability) ||
				!validateProjectionOwner(request.ownerCapability,
				request.candidateAsset, request.transaction.targetMapping,
				ownerError))
			{
				return reject(request.transaction,
					editor::SceneApplyError::StaleObjectGeneration,
					ownerError.empty()
					? "full-snapshot projection owner capability is invalid"
					: std::move(ownerError),
					editor::FullRebuildReason::MappingMismatch);
			}
			ConcreteAuthorityBuild concreteAuthority;
			try
			{
				concreteAuthority = buildConcreteAuthority(
					request.candidateAsset,
					request.transaction.targetMapping,
					m_activeConcrete != nullptr
						? std::span<const ConcreteLiveHandleRecord>{
							m_activeConcrete->liveHandles}
						: std::span<const ConcreteLiveHandleRecord>{},
					m_nextConcreteHandleIndices);
			}
			catch (const std::bad_alloc&)
			{
				clearCandidate();
				return reject(request.transaction,
					editor::SceneApplyError::AllocationFailure,
					"full snapshot concrete-authority validation allocation failed",
					editor::FullRebuildReason::MappingMismatch);
			}
			if (!concreteAuthority.valid())
			{
				return reject(request.transaction,
					editor::SceneApplyError::StaleObjectGeneration,
					std::move(concreteAuthority.error),
					editor::FullRebuildReason::MappingMismatch);
			}
			if (matchesPublishedCanonicalIdentity(request.transaction))
			{
				if (!canonicalBytesMatchPublished(request.transaction))
				{
					return identityCollisionResult(request.transaction);
				}
				return m_publisher->applyFullSceneSnapshot(request.transaction);
			}
			if (const auto rejection =
					m_publisher->preflightFullSceneSnapshot(request.transaction))
			{
				return *rejection;
			}
			const editor::SceneChangeValidation canonical =
				editor::SceneChangeCanonicalizer::validateCanonical(
					request.transaction.changes,
					request.transaction.canonicalBytes);
			if (!canonical.valid())
			{
				return m_publisher->applyFullSceneSnapshot(request.transaction);
			}
			const editor::RendererTargetFence& targetFence =
				request.transaction.targetMapping.fence();
			if (!uploadPlanHeaderMatchesFence(request.candidatePlan.header,
			                                  targetFence))
			{
				return reject(request.transaction,
					editor::SceneApplyError::MappingMismatch,
					"full snapshot upload plan envelope differs from target mapping",
					editor::FullRebuildReason::MappingMismatch);
			}
			const SceneUploadPlanValidationResult planValidation =
				SceneUploadPlanner::validate(makeSceneAssetView(request.candidateAsset),
				                             request.candidatePlan);
			if (!planValidation.valid)
			{
				return reject(request.transaction,
					editor::SceneApplyError::InvalidPayload,
					planValidation.error,
					request.transaction.changes.fullSnapshot
						? std::optional{request.transaction.changes.fullSnapshot->reason}
						: std::optional<editor::FullRebuildReason>{});
			}
			try
			{
				const rhi::Device* validationDevice =
					m_owner.m_renderer.sceneTransactionDeviceInitialized()
						? &m_owner.m_renderer.getRHIDevice()
						: nullptr;
				const TransactionTexturePlanValidation textureValidation =
					validateTransactionTextureUploadPlans(
						request.candidateAsset, request.candidatePlan, validationDevice);
				if (!textureValidation.valid())
				{
					return reject(request.transaction,
						editor::SceneApplyError::InvalidPayload,
						transactionTexturePlanErrorMessage(textureValidation),
						request.transaction.changes.fullSnapshot
							? std::optional{request.transaction.changes.fullSnapshot->reason}
							: std::optional<editor::FullRebuildReason>{});
				}
			}
			catch (const std::bad_alloc&)
			{
				return reject(request.transaction,
					editor::SceneApplyError::AllocationFailure,
					"transaction texture upload validation allocation failed",
					request.transaction.changes.fullSnapshot
						? std::optional{request.transaction.changes.fullSnapshot->reason}
						: std::optional<editor::FullRebuildReason>{});
			}
			editor::ApplySceneChangeRequest transaction = request.transaction;
			if (m_owner.m_renderer.sceneTransactionDeviceInitialized() &&
				(transaction.stagePlan.uploads.geometryUploadCalls != 0u ||
				 transaction.stagePlan.uploads.geometryBytesUploaded != 0u ||
				 transaction.stagePlan.uploads.geometryRangesUploaded != 0u ||
				 transaction.stagePlan.uploads.textureUploadCalls != 0u ||
				 transaction.stagePlan.uploads.textureBytesUploaded != 0u ||
				 transaction.stagePlan.uploads.textureSubresourcesUploaded != 0u ||
				 transaction.stagePlan.uploads.otherUploadCalls != 0u ||
				 transaction.stagePlan.uploads.otherBytesUploaded != 0u ||
				 transaction.stagePlan.uploads.otherRangesUploaded != 0u))
			{
				return reject(request.transaction,
					editor::SceneApplyError::InvalidPayload,
					"full snapshot upload counters are renderer-owned",
					request.transaction.changes.fullSnapshot
						? std::optional{request.transaction.changes.fullSnapshot->reason}
						: std::optional<editor::FullRebuildReason>{});
			}
			if (m_owner.m_renderer.sceneTransactionDeviceInitialized() &&
				std::any_of(transaction.stagePlan.submissions.begin(),
				            transaction.stagePlan.submissions.end(),
				            [](const editor::SceneQueueSubmissionPlan& submission)
				            {
					            return submission.queueClass != rhi::QueueClass::graphics;
				            }))
			{
				return reject(request.transaction,
					editor::SceneApplyError::InvalidPayload,
					"private scene uploads require graphics queue submissions",
					request.transaction.changes.fullSnapshot
						? std::optional{request.transaction.changes.fullSnapshot->reason}
						: std::optional<editor::FullRebuildReason>{});
			}
			try
			{
				m_pendingConcreteAuthority = std::move(concreteAuthority);
				m_pendingOwnerCapability = request.ownerCapability;
				m_pendingFullAsset = request.candidateAsset;
				m_pendingFullPlan = request.candidatePlan;
				if (m_owner.m_renderer.sceneTransactionDeviceInitialized())
				{
					const bool resourceBearing =
						!request.candidatePlan.meshes.empty() ||
						!request.candidatePlan.textures.empty() ||
						!request.candidatePlan.materials.empty() ||
						!request.candidatePlan.instances.instances.empty();
					if (resourceBearing && transaction.stagePlan.submissions.empty())
					{
						transaction.stagePlan.submissions.push_back(
							editor::SceneQueueSubmissionPlan{
								.queueClass = rhi::QueueClass::graphics,
								.diagnosticId = ++m_nextSubmissionDiagnosticId,
							});
					}
					if (resourceBearing)
					{
						transaction.stagePlan.retireResourceDiagnosticIds.clear();
						for (const editor::SceneJournalResource& activeResource :
						     m_publisher->activeGeneration()->resources())
						{
							transaction.stagePlan.retireResourceDiagnosticIds.push_back(
								activeResource.diagnosticId);
						}
						if (!request.candidatePlan.meshes.empty())
						{
							transaction.stagePlan.uploads.geometryUploadCalls = 1u;
							for (const MeshUploadPlan& mesh : request.candidatePlan.meshes)
							{
								transaction.stagePlan.uploads.geometryBytesUploaded +=
									mesh.vertexByteSize + mesh.indexByteSize;
								transaction.stagePlan.uploads.geometryRangesUploaded += 2u;
							}
						}
						if (!request.candidatePlan.textures.empty())
						{
							transaction.stagePlan.uploads.textureUploadCalls = 1u;
							for (const TextureUploadPlan& texture : request.candidatePlan.textures)
							{
								transaction.stagePlan.uploads.textureBytesUploaded +=
									texture.payloadByteSize;
								transaction.stagePlan.uploads.textureSubresourcesUploaded +=
									std::max(texture.mipLevels, 1u);
							}
						}
					}
				}
			}
			catch (const std::bad_alloc&)
			{
				clearCandidate();
				return reject(request.transaction,
					editor::SceneApplyError::AllocationFailure,
					"full snapshot immutable candidate copy failed",
					request.transaction.changes.fullSnapshot
						? std::optional{request.transaction.changes.fullSnapshot->reason}
						: std::optional<editor::FullRebuildReason>{});
			}
			m_pendingSubmissionCount = static_cast<uint32_t>(
				transaction.stagePlan.submissions.size());
			m_pendingFullSnapshot = true;
			m_candidatePrepared = false;
			editor::SceneApplyResult result =
				m_publisher->applyFullSceneSnapshot(transaction);
			if (result.disposition == editor::SceneApplyDisposition::AcceptedFullSnapshot)
			{
				commitCandidate(transaction.changes,
				                transaction.targetMapping);
			}
			clearCandidate();
			if (result.disposition == editor::SceneApplyDisposition::AcceptedFullSnapshot)
			{
				(void)drainRetirements();
				result.countersAfter = m_counters->snapshot();
				result.counterDelta = editor::sceneDeltaCounterDelta(
					result.countersBefore, result.countersAfter);
			}
			return result;
		}

		RendererSceneStateSnapshot state() const noexcept
		{
			const std::lock_guard lock(m_mutex);
			RendererSceneStateSnapshot result;
			if (m_publisher == nullptr)
			{
				return result;
			}
			const std::shared_ptr<const editor::RendererSceneGeneration> active =
				m_publisher->activeGeneration();
			result.initialized = true;
			result.fence = active->fence();
			result.activeGenerationDigest = active->activeGenerationDigest();
			result.generation = active->generation();
			result.descriptorGeneration = active->descriptorGeneration();
			result.mappingEntryCount = active->mapping().entries().size();
			const SceneAsset& activeAsset = m_activeConcrete != nullptr
				? m_activeConcrete->asset
				: m_activeAsset;
			result.meshCount = activeAsset.meshes.size();
			result.textureCount = activeAsset.textures.size();
			result.materialCount = activeAsset.materials.size();
			result.nodeCount = activeAsset.nodes.size();
			if (m_activeConcrete != nullptr &&
				m_activeConcrete->occurrenceSnapshot != nullptr)
			{
				result.occurrenceGeneration =
					m_activeConcrete->occurrenceSnapshot->generation();
				result.occurrenceCount =
					m_activeConcrete->occurrenceSnapshot->records().size();
			}
			return result;
		}

		editor::SceneDeltaCounterSnapshot counters() const noexcept
		{
			const std::lock_guard lock(m_mutex);
			return m_counters != nullptr
				? m_counters->snapshot()
				: editor::SceneDeltaCounterSnapshot{};
		}

		SceneTransactionConcreteStateSnapshot concreteState() const
		{
			const std::lock_guard lock(m_mutex);
			SceneTransactionConcreteStateSnapshot result;
			if (m_activeConcrete == nullptr)
			{
				return result;
			}
			result.initialized = true;
			result.resourceBearing = m_activeConcrete->resources != nullptr;
			result.generation = m_activeConcrete->generation;
			result.descriptorGeneration = m_activeConcrete->descriptorGeneration;
			result.fence = m_activeConcrete->mapping.fence();
			result.mapping = m_activeConcrete->mapping;
			result.pendingRetiredGenerationCount = m_retirements.size() +
				static_cast<uint64_t>(std::count_if(
					m_retirementReservations.begin(),
					m_retirementReservations.end(),
					[](const auto& entry) { return entry.second.committed; }));
			result.immediatelyDestroyedResourceCount = m_immediatelyDestroyedResourceCount;
			result.eventuallyDestroyedResourceCount = m_eventuallyDestroyedResourceCount;
			result.queueSubmissionCount = m_queueSubmissionCount;
			result.lastSubmissionToken = m_lastSubmissionToken;
			if (m_activeConcrete->resources != nullptr)
			{
				result.resourceCount = m_activeConcrete->resources->owned.size();
			}
			for (const auto& entry : m_activeConcrete->mapping.entries())
			{
				switch (entry.rendererHandle.kind)
				{
				case editor::RendererObjectKind::Geometry:
					result.meshHandles.push_back(entry.rendererHandle);
					break;
				case editor::RendererObjectKind::Texture:
					result.textureHandles.push_back(entry.rendererHandle);
					break;
				case editor::RendererObjectKind::Material:
					result.materialHandles.push_back(entry.rendererHandle);
					break;
				default:
					break;
				}
			}
			for (const ConcreteLiveHandleRecord& record :
			     m_activeConcrete->liveHandles)
			{
				switch (record.handle.kind)
				{
				case editor::RendererObjectKind::SceneObject:
					result.sceneObjectHandles.push_back(record.handle);
					break;
				case editor::RendererObjectKind::Camera:
					result.cameraHandles.push_back(record.handle);
					break;
				case editor::RendererObjectKind::Light:
					result.lightHandles.push_back(record.handle);
					break;
				default:
					break;
				}
			}
			const SceneAsset& asset = m_activeConcrete->asset;
			if (!asset.nodes.empty())
			{
				result.firstNodeWorldTransform = asset.nodes.front().worldTransform;
				result.firstNodeVisibility = asset.nodes.front().effectiveVisible;
			}
			if (!asset.materials.empty()) result.firstMaterial = asset.materials.front();
			if (!asset.primitives.empty())
				result.firstPrimitiveMaterialIndex = asset.primitives.front().materialIndex;
			if (!asset.cameras.empty()) result.firstCamera = asset.cameras.front();
			if (!asset.lights.empty()) result.firstLight = asset.lights.front();
			if (m_activeConcrete->rendererState != nullptr)
			{
				const auto& rendererState = *m_activeConcrete->rendererState;
				result.rendererObjectCount = static_cast<uint32_t>(
					m_activeConcrete->objects.size());
				result.rendererDrawCount = static_cast<uint32_t>(
					rendererState.upload.drawRecords.size());
				result.deferredRegistryUpdateCount = static_cast<uint32_t>(
					rendererState.registryUpdates.size());
				if (!m_activeConcrete->objects.empty())
				{
					const ConcreteObjectRecord& object =
						m_activeConcrete->objects.front();
					result.firstRendererWorldTransform = object.registration.transform;
					result.firstRendererVisibility = object.registration.visible;
					result.firstRendererMaterialIndex =
						object.registration.materialIndex;
				}
				if (!m_activeConcrete->plan.instances.instances.empty())
				{
					result.firstPlanInstanceMaterialIndex =
						m_activeConcrete->plan.instances.instances.front().materialIndex;
				}
				if (!m_activeConcrete->plan.drawCommands.empty())
				{
					result.firstPlanDrawMaterialIndex =
						m_activeConcrete->plan.drawCommands.front().materialIndex;
				}
				result.rendererStateMatchesGeneration =
					rendererState.generation == m_activeConcrete->generation &&
					rendererState.descriptorGeneration ==
						m_activeConcrete->descriptorGeneration;
			}
			return result;
		}

		void setFailurePoint(
			SceneTransactionFailurePoint point,
			uint32_t submissionIndex) noexcept
		{
			const std::lock_guard lock(m_mutex);
			m_failurePoint = point;
			m_failureSubmissionIndex = submissionIndex;
		}

		uint64_t collectGarbage() noexcept
		{
			const std::lock_guard lock(m_mutex);
			if (m_publisher != nullptr && m_publisher->hasPendingOrphanRetirement())
			{
				(void)m_publisher->retryPendingOrphanRetirement();
			}
			return drainRetirements();
		}

		[[nodiscard]] uint64_t visibleGeneration() const noexcept
		{
			return m_owner.m_renderer.m_sceneTransactionPublishedGeneration.load(
				std::memory_order_acquire);
		}

	private:
		struct ConcreteObjectRecord
		{
			editor::StableSceneId sourceId{};
			editor::StableSceneId primitiveSourceId{};
			editor::RendererObjectHandle handle{};
			uint32_t drawRecordIndex{UINT32_MAX};
			GPUSceneRegistrationDesc registration{};
		};

		struct ConcreteLiveHandleRecord
		{
			editor::StableSceneId sourceId{};
			editor::RendererProjectionCoordinate projection{};
			editor::RendererObjectHandle handle{};

			bool operator==(const ConcreteLiveHandleRecord&) const = default;
		};

		struct ConcreteAuthorityBuild
		{
			std::vector<ConcreteLiveHandleRecord> records;
			std::array<uint32_t, 8u> nextHandleIndices{};
			std::string error;

			[[nodiscard]] bool valid() const noexcept { return error.empty(); }
		};

		struct BufferCopy
		{
			rhi::BufferHandle source{};
			uint64_t sourceOffset{0};
			rhi::BufferHandle destination{};
			uint64_t destinationOffset{0};
			uint64_t size{0};
		};

		struct TextureCopy
		{
			rhi::BufferTextureCopyDesc region{};
		};

		struct OwnedResource
		{
			editor::SceneJournalResource journal{};
			bool registryOwned{false};
			bool destroyed{false};
		};

		struct ResourceSet
		{
			rhi::Device* device{nullptr};
			std::shared_ptr<RenderDevice::SceneTransactionResources> deviceRecords;
			std::unique_ptr<GPUSceneRegistry> registry;
			std::vector<OwnedResource> owned;
			std::vector<BufferCopy> bufferCopies;
			std::vector<TextureCopy> textureCopies;
			std::vector<rhi::BufferBarrier> bufferBeginBarriers;
			std::vector<rhi::BufferBarrier> bufferEndBarriers;
			std::vector<rhi::TextureBarrier> textureBeginBarriers;
			std::vector<rhi::TextureBarrier> textureEndBarriers;
			std::unique_ptr<rhi::CommandAllocator> commandAllocator;
			std::vector<std::unique_ptr<rhi::CommandBuffer>> commandBuffers;
			// Indexed by SceneQueueSubmissionPlan ordinal. This survives a
			// successful submit whose reported token is invalid/non-monotonic, so
			// rollback can recover the queue's real last-submitted dependency.
			std::vector<rhi::Queue*> submissionQueues;
			std::vector<std::pair<rhi::QueueIdentity, rhi::Queue*>> submittedQueues;
			std::vector<rhi::SubmissionToken> lastUseTokens;
			bool descriptorsStaged{false};
			bool recorded{false};
			bool destroyed{false};
		};

		struct ConcreteGeneration
		{
			SceneAsset asset;
			SceneUploadPlan plan;
			editor::RendererSceneMappingSnapshot mapping;
			std::shared_ptr<RenderDevice::SceneTransactionResources> deviceRecords;
			std::vector<ConcreteObjectRecord> objects;
			std::vector<ConcreteLiveHandleRecord> liveHandles;
			std::array<uint32_t, 8u> nextConcreteHandleIndices{};
			std::shared_ptr<ResourceSet> resources;
			std::shared_ptr<GPUDrivenRenderer::SceneTransactionRendererState>
				rendererState;
			std::vector<rhi::SubmissionToken> submissionTokens;
			editor::SceneRenderOccurrenceSnapshot occurrenceSnapshot;
			uint64_t generation{0};
			uint64_t descriptorGeneration{0};
		};

		struct RetirementRecord
		{
			std::shared_ptr<ResourceSet> resources;
			std::vector<rhi::SubmissionToken> dependencies;
			bool committed{false};
		};

		[[nodiscard]] bool consumeFailure(
			SceneTransactionFailurePoint point,
			uint32_t submissionIndex = 0u) noexcept
		{
			if (m_failurePoint != point ||
				(point == SceneTransactionFailurePoint::queueSubmission &&
				 m_failureSubmissionIndex != submissionIndex))
			{
				return false;
			}
			m_failurePoint = SceneTransactionFailurePoint::none;
			m_failureSubmissionIndex = 0u;
			return true;
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::BufferHandle handle) noexcept
		{
			return {rhi::ResourceKind::Buffer, handle.index, handle.generation};
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::TextureHandle handle) noexcept
		{
			return {rhi::ResourceKind::Texture, handle.index, handle.generation};
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::TextureViewHandle handle) noexcept
		{
			return {rhi::ResourceKind::TextureView, handle.index, handle.generation};
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::SamplerHandle handle) noexcept
		{
			return {rhi::ResourceKind::Sampler, handle.index, handle.generation};
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::ArgumentLayoutHandle handle) noexcept
		{
			return {rhi::ResourceKind::ArgumentLayout, handle.index, handle.generation};
		}

		[[nodiscard]] static rhi::ResourceHandle resourceHandle(
			rhi::ArgumentTableHandle handle) noexcept
		{
			return {rhi::ResourceKind::ArgumentTable, handle.index, handle.generation};
		}

		void appendOwnedResource(
			ResourceSet& resources,
			rhi::ResourceHandle handle,
			bool registryOwned = false)
		{
			if (!handle.isValid())
			{
				throw std::runtime_error(
					"private scene RHI allocation returned an invalid handle");
			}
			resources.owned.push_back(OwnedResource{
				.journal = {
					.diagnosticId = {++m_nextResourceDiagnosticId},
					.handle = handle,
				},
				.registryOwned = registryOwned,
			});
		}

		[[nodiscard]] static editor::RendererObjectHandle rendererHandleFor(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::StableSceneId id,
			editor::RendererObjectKind kind) noexcept
		{
			const editor::RendererSceneMappingEntry* entry = mapping.find(id);
			return entry != nullptr && entry->rendererHandle.kind == kind
				? entry->rendererHandle
				: editor::RendererObjectHandle{};
		}

		[[nodiscard]] static bool concreteLiveHandleKind(
			editor::EditorObjectKind sourceKind,
			editor::RendererObjectKind& rendererKind) noexcept
		{
			switch (sourceKind)
			{
			case editor::EditorObjectKind::scene:
			case editor::EditorObjectKind::node:
			case editor::EditorObjectKind::primitive:
				rendererKind = editor::RendererObjectKind::SceneObject;
				return true;
			case editor::EditorObjectKind::camera:
				rendererKind = editor::RendererObjectKind::Camera;
				return true;
			case editor::EditorObjectKind::light:
				rendererKind = editor::RendererObjectKind::Light;
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] static bool concreteLiveRendererKind(
			editor::RendererObjectKind kind) noexcept
		{
			return kind == editor::RendererObjectKind::SceneObject ||
			       kind == editor::RendererObjectKind::Camera ||
			       kind == editor::RendererObjectKind::Light;
		}

		[[nodiscard]] ConcreteAuthorityBuild buildConcreteAuthority(
			const SceneAsset& asset,
			const editor::RendererSceneMappingSnapshot& mapping,
			std::span<const ConcreteLiveHandleRecord> activeRecords,
			std::array<uint32_t, 8u> nextHandleIndices) const
		{
			ConcreteAuthorityBuild result;
			result.nextHandleIndices = nextHandleIndices;
			const editor::RendererSceneMappingValidation validation = mapping.validate();
			if (!validation.valid())
			{
				result.error = "renderer mapping provenance is invalid: " + validation.message;
				return result;
			}
			const editor::SceneRuntimeMapping* authority =
				mapping.projectionAuthority();
			if (authority == nullptr)
			{
				result.error = "renderer mapping has no trusted projection authority";
				return result;
			}

			result.records.reserve(authority->entries().size());
			for (const editor::SceneRuntimeMappingEntry& source : authority->entries())
			{
				editor::RendererObjectKind expectedKind{};
				if (!concreteLiveHandleKind(source.documentId.kind, expectedKind))
				{
					continue;
				}
				const editor::StableSceneId stable = editor::stableSceneId(source.documentId);
				const uint64_t expectedIdentity =
					editor::SceneRuntimeMapping::deriveProjectionIdentity(source.documentId);
				if (!stable.valid() || expectedIdentity == 0u ||
					expectedIdentity == editor::SceneRuntimeMapping::kInvalidProjectionIdentity ||
					(source.sceneAssetIndex != editor::SceneRuntimeMapping::kInvalidIndex &&
					 (source.projectionIdentity != expectedIdentity ||
					  source.runtimeIndex != expectedIdentity)))
				{
					result.error = "trusted concrete projection identity is not source-derived";
					return result;
				}
				const editor::RendererSceneMappingEntry* candidate = mapping.find(stable);
				if (candidate == nullptr)
				{
					result.error = "concrete projection source has no renderer mapping record";
					return result;
				}
				const uint32_t expectedAssetIndex =
					source.sceneAssetIndex != editor::SceneRuntimeMapping::kInvalidIndex
						? source.sceneAssetIndex
						: stable.slot;
				const editor::RendererProjectionCoordinate expectedProjection{
					.kind = stable.kind,
					.sceneAssetIndex = expectedAssetIndex,
					.runtimeIndex = expectedIdentity,
					.identityDigest = concreteProjectionIdentityDigest(
						stable, expectedIdentity),
				};
				if (candidate->stableId != stable ||
					candidate->projection != expectedProjection ||
					candidate->rendererHandle.kind != expectedKind)
				{
					result.error =
						"concrete handle kind, source, or projection association is stale";
					return result;
				}

				editor::RendererObjectHandle expectedHandle{};
				const auto active = std::find_if(
					activeRecords.begin(), activeRecords.end(),
					[stable](const ConcreteLiveHandleRecord& record)
					{
						return record.sourceId == stable;
					});
				if (active != activeRecords.end())
				{
					if (active->handle.kind != expectedKind ||
						active->handle.rendererGeneration !=
							mapping.fence().renderer.generation ||
						active->handle.deviceGeneration !=
							mapping.fence().device.generation)
					{
						result.error =
							"active concrete handle fence differs from target provenance";
						return result;
					}
					expectedHandle = active->handle;
				}
				else
				{
					const size_t kindIndex = static_cast<size_t>(expectedKind);
					if (kindIndex >= result.nextHandleIndices.size() ||
						result.nextHandleIndices[kindIndex] == 0u ||
						result.nextHandleIndices[kindIndex] ==
							std::numeric_limits<uint32_t>::max())
					{
						result.error = "renderer concrete handle namespace is exhausted";
						return result;
					}
					expectedHandle = {
						.kind = expectedKind,
						.index = result.nextHandleIndices[kindIndex]++,
						.generation =
							m_owner.m_sceneTransactionConcreteObjectGeneration,
						.rendererGeneration = mapping.fence().renderer.generation,
						.deviceGeneration = mapping.fence().device.generation,
					};
				}
				if (candidate->rendererHandle != expectedHandle)
				{
					result.error =
						"concrete handle index or generation differs from renderer authority";
					return result;
				}
				result.records.push_back({stable, expectedProjection, expectedHandle});
			}

			size_t concreteCandidateCount = 0u;
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				editor::RendererObjectKind expectedKind{};
				const bool concreteSource =
					concreteLiveHandleKind(entry.stableId.kind, expectedKind);
				if (concreteSource || concreteLiveRendererKind(entry.rendererHandle.kind))
				{
					++concreteCandidateCount;
					if (!concreteSource || entry.rendererHandle.kind != expectedKind)
					{
						result.error = "renderer mapping contains a forged concrete record";
						return result;
					}
				}
			}
			if (concreteCandidateCount != result.records.size())
			{
				result.error = "renderer concrete mapping is incomplete or ambiguous";
				return result;
			}

			const auto requireEveryAssetCoordinate =
				[&](editor::EditorObjectKind kind, size_t count) -> bool
				{
					for (size_t index = 0u; index < count; ++index)
					{
						if (index > std::numeric_limits<uint32_t>::max())
						{
							result.error = "concrete SceneAsset coordinate exceeds renderer limits";
							return false;
						}
						const std::optional<editor::EditorObjectId> owner =
							authority->documentIdAt(kind, static_cast<uint32_t>(index));
						if (!owner || owner->kind != kind)
						{
							result.error =
								"concrete SceneAsset coordinate has no unique trusted source";
							return false;
						}
						const uint64_t identity =
							editor::SceneRuntimeMapping::deriveProjectionIdentity(*owner);
						const editor::RendererSceneMappingEntry* rendererEntry =
							mapping.find(editor::stableSceneId(*owner));
						if (rendererEntry == nullptr || rendererEntry->stableId.kind != kind)
						{
							result.error =
								"trusted concrete source has no unique renderer record";
							return false;
						}
						if (kind == editor::EditorObjectKind::node &&
							asset.nodes[index].runtimeIdentity != identity)
						{
							result.error = "node asset identity differs from its trusted source";
							return false;
						}
						if (kind == editor::EditorObjectKind::primitive &&
							asset.primitives[index].runtimeIdentity != identity)
						{
							result.error =
								"primitive asset identity differs from its trusted source";
							return false;
						}
						if (kind == editor::EditorObjectKind::camera &&
							(asset.cameras[index].sourceCameraIndex ==
								std::numeric_limits<uint32_t>::max() ||
							 asset.cameras[index].runtimeIdentity != identity))
						{
							result.error =
								"camera asset identity differs from its trusted source";
							return false;
						}
						if (kind == editor::EditorObjectKind::light &&
							(asset.lights[index].sourceLightIndex ==
								std::numeric_limits<uint32_t>::max() ||
							 asset.lights[index].runtimeIdentity != identity))
						{
							result.error =
								"light asset identity differs from its trusted source";
							return false;
						}
					}
					return true;
				};
			if (!requireEveryAssetCoordinate(
					editor::EditorObjectKind::node, asset.nodes.size()) ||
				!requireEveryAssetCoordinate(
					editor::EditorObjectKind::primitive, asset.primitives.size()) ||
				!requireEveryAssetCoordinate(
					editor::EditorObjectKind::camera, asset.cameras.size()) ||
				!requireEveryAssetCoordinate(
					editor::EditorObjectKind::light, asset.lights.size()))
			{
				return result;
			}
			if (mapping.fence().activeScene &&
				std::none_of(result.records.begin(), result.records.end(),
					[&](const ConcreteLiveHandleRecord& record)
					{
						return record.sourceId == *mapping.fence().activeScene &&
							record.handle.kind == editor::RendererObjectKind::SceneObject;
					}))
			{
				result.error = "active scene has no renderer-owned concrete handle";
			}
			return result;
		}

		[[nodiscard]] static uint32_t firstPrivateHandleIndex(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::RendererObjectKind kind)
		{
			uint32_t maximum = 0u;
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				if (entry.rendererHandle.kind == kind)
				{
					maximum = std::max(maximum, entry.rendererHandle.index);
				}
			}
			if (maximum == std::numeric_limits<uint32_t>::max())
			{
				throw std::runtime_error("private renderer handle namespace is exhausted");
			}
			return maximum + 1u;
		}

		[[nodiscard]] static uint32_t privateHandleGeneration(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::RendererObjectKind kind) noexcept
		{
			uint32_t generation = 1u;
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				if (entry.rendererHandle.kind == kind)
				{
					generation = std::max(generation,
					                      entry.rendererHandle.generation);
				}
			}
			return generation;
		}

		[[nodiscard]] static editor::RendererObjectHandle allocatePrivateHandle(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::RendererObjectKind kind,
			uint32_t generation,
			uint32_t& nextIndex)
		{
			if (nextIndex == 0u ||
				nextIndex == std::numeric_limits<uint32_t>::max())
			{
				throw std::runtime_error("private renderer handle namespace is exhausted");
			}
			const editor::RendererTargetFence& fence = mapping.fence();
			return editor::RendererObjectHandle{
				.kind = kind,
				.index = nextIndex++,
				.generation = generation,
				.rendererGeneration = fence.renderer.generation,
				.deviceGeneration = fence.device.generation,
			};
		}

		[[nodiscard]] static const editor::RendererSceneMappingEntry*
		mappingEntryForProjection(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::RendererObjectKind rendererKind,
			editor::EditorObjectKind projectionKind,
			uint32_t sceneAssetIndex) noexcept
		{
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				if (entry.rendererHandle.kind == rendererKind &&
					entry.projection.kind == projectionKind &&
					entry.projection.sceneAssetIndex == sceneAssetIndex)
				{
					return &entry;
				}
			}
			return nullptr;
		}

		[[nodiscard]] static const editor::RendererSceneMappingEntry*
		mappingEntryForTextureCoordinate(
			const editor::RendererSceneMappingSnapshot& mapping,
			editor::SceneTextureSemantic semantic,
			uint32_t sceneAssetIndex) noexcept
		{
			if (const editor::RendererSceneMappingEntry* imageEntry =
					mappingEntryForProjection(
						mapping, editor::RendererObjectKind::Texture,
						editor::EditorObjectKind::image, sceneAssetIndex))
			{
				return imageEntry;
			}
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				if (entry.rendererHandle.kind == editor::RendererObjectKind::Texture &&
					entry.projection.kind == editor::EditorObjectKind::texture &&
					entry.projection.sceneAssetIndexFor(semantic) == sceneAssetIndex)
				{
					return &entry;
				}
			}
			return nullptr;
		}

		[[nodiscard]] std::shared_ptr<ConcreteGeneration> buildConcreteGeneration(
			SceneAsset asset,
			SceneUploadPlan plan,
			const editor::RendererSceneMappingSnapshot& mapping,
			std::vector<ConcreteLiveHandleRecord> liveHandles,
			std::array<uint32_t, 8u> nextHandleIndices) const;
		void applyCpuChanges(
			ConcreteGeneration& generation,
			const editor::SceneChangeSet& changes) const;
		void normalizeOccurrenceIdentities(ConcreteGeneration& generation) const;
		void sealOccurrenceSnapshot(ConcreteGeneration& generation) const;
		void rebuildRendererState(ConcreteGeneration& generation) const;
		[[nodiscard]] rhi::RHIResult prepareRendererStateForPublication() noexcept;
		[[nodiscard]] rhi::RHIResult allocateCandidateResources(
			std::vector<editor::SceneJournalResource>& createdResources) noexcept;
		[[nodiscard]] rhi::RHIResult destroyResourceSet(
			const std::shared_ptr<ResourceSet>& resources) noexcept;
		[[nodiscard]] std::shared_ptr<ResourceSet> resourceSetFor(
			std::span<const editor::SceneJournalResource> resources) const noexcept;
		[[nodiscard]] bool retirementComplete(
			const RetirementRecord& retirement) const noexcept;
		uint64_t drainRetirements() noexcept;

		bool matchesPublishedCanonicalIdentity(
			const editor::ApplySceneChangeRequest& request) const noexcept
		{
			return m_publisher->activeGeneration()->canonicalIdentity() ==
			       request.canonicalIdentity;
		}

		bool canonicalBytesMatchPublished(
			const editor::ApplySceneChangeRequest& request) const noexcept
		{
			const auto active = m_publisher->activeGeneration();
			return std::equal(active->canonicalBytes().begin(),
			                  active->canonicalBytes().end(),
			                  request.canonicalBytes.begin(),
			                  request.canonicalBytes.end());
		}

		editor::SceneApplyResult identityCollisionResult(
			const editor::ApplySceneChangeRequest& request) const noexcept
		{
			editor::SceneApplyResult result;
			result.disposition = editor::SceneApplyDisposition::Rejected;
			result.error = editor::SceneApplyError::InvalidState;
			result.message = "canonical identity was reused with different bytes";
			result.rhiResult = rhi::RHIResult::ok();
			result.canonicalIdentity = request.canonicalIdentity;
			result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
			result.oldFence = m_publisher->activeGeneration()->fence();
			result.countersBefore = m_counters->snapshot();
			result.countersAfter = result.countersBefore;
			return result;
		}

		[[nodiscard]] static bool validateProjectionOwner(
			const editor::SceneProjectionOwnerCapability& capability,
			const SceneAsset& asset,
			const editor::RendererSceneMappingSnapshot& mapping,
			std::string& error) noexcept
		{
			const editor::SceneRuntimeMapping* authority =
				mapping.projectionAuthority();
			if (authority == nullptr)
			{
				error = "renderer mapping has no projection-owner authority";
				return false;
			}
			if (!capability.validate(asset, *authority, &error))
			{
				if (error.empty())
					error = "renderer projection-owner capability rejected the request";
				return false;
			}
			error.clear();
			return true;
		}

		static bool uploadPlanHeaderMatchesFence(
			const SceneUploadPlanHeader& header,
			const editor::RendererTargetFence& fence) noexcept
		{
			return header.valid() &&
				header.authority == fence.authority &&
				header.documentRevision == fence.appliedDocumentRevision &&
				header.activeScene == fence.activeScene &&
				header.sceneActivationEpoch == fence.sceneActivationEpoch &&
				header.rootExternalStateSerial == fence.rootExternalStateSerial &&
				header.projectionDigest == fence.projectionDigest &&
				header.mappingEpoch == fence.mappingEpoch;
		}

		static bool containsStructuralIncrementalWork(
			const editor::SceneChangeSet& changes) noexcept
		{
			return changes.header.domain != editor::SceneChangeDomain::FullSnapshot &&
				std::any_of(changes.canonicalChanges.begin(),
				            changes.canonicalChanges.end(),
				            [](const editor::SceneFieldChange& change)
				            {
					            return change.changeClass == editor::SceneChangeClass::Hierarchy ||
					                   change.changeClass == editor::SceneChangeClass::Topology ||
					                   change.changeClass == editor::SceneChangeClass::FullRebuild;
				            });
		}

		static glm::mat4 transformMatrix(
			const editor::SceneNodeTransformValue& transform) noexcept
		{
			if (const auto* matrix =
					std::get_if<editor::SceneMatrixValue>(&transform.value))
			{
				glm::mat4 result(1.0f);
				for (size_t column = 0; column < 4u; ++column)
				{
					for (size_t row = 0; row < 4u; ++row)
					{
						result[column][row] = static_cast<float>(
							matrix->columnMajor[column * 4u + row]);
					}
				}
				return result;
			}
			const auto& trs = std::get<editor::SceneTrsValue>(transform.value);
			const glm::vec3 translation(
				static_cast<float>(trs.translation[0]),
				static_cast<float>(trs.translation[1]),
				static_cast<float>(trs.translation[2]));
			const glm::quat rotation(
				static_cast<float>(trs.rotation[3]),
				static_cast<float>(trs.rotation[0]),
				static_cast<float>(trs.rotation[1]),
				static_cast<float>(trs.rotation[2]));
			const glm::vec3 scale(
				static_cast<float>(trs.scale[0]),
				static_cast<float>(trs.scale[1]),
				static_cast<float>(trs.scale[2]));
			return glm::translate(glm::mat4(1.0f), translation) *
			       glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
		}

		std::optional<std::string> validateLiveHandles(
			const editor::SceneChangeSet& changes,
			const editor::RendererSceneMappingSnapshot& mapping) const
		{
			if (m_activeConcrete == nullptr)
			{
				return "active concrete scene generation is missing";
			}
			const editor::SceneRuntimeMapping* activeAuthority =
				m_activeConcrete->mapping.projectionAuthority();
			const editor::SceneRuntimeMapping* targetAuthority =
				mapping.projectionAuthority();
			if (activeAuthority == nullptr || targetAuthority == nullptr ||
				activeAuthority->entries() != targetAuthority->entries() ||
				activeAuthority->aliases() != targetAuthority->aliases())
			{
				return "incremental projection provenance differs from the active authority";
			}
			try
			{
				const ConcreteAuthorityBuild authority = buildConcreteAuthority(
					m_activeConcrete->asset, mapping,
					m_activeConcrete->liveHandles,
					m_nextConcreteHandleIndices);
				if (!authority.valid())
				{
					return authority.error;
				}
				if (authority.records != m_activeConcrete->liveHandles ||
					authority.nextHandleIndices != m_nextConcreteHandleIndices)
				{
					return "incremental concrete authority differs from the active generation";
				}
			}
			catch (const std::bad_alloc&)
			{
				return "incremental concrete-authority validation allocation failed";
			}
			const auto staleMessage = [](editor::RendererObjectKind kind) noexcept
			{
				switch (kind)
				{
				case editor::RendererObjectKind::SceneObject:
					return "scene object handle generation is stale";
				case editor::RendererObjectKind::Camera:
					return "camera handle generation is stale";
				case editor::RendererObjectKind::Light:
					return "light handle generation is stale";
				default:
					return "concrete live handle association is stale";
				}
			};
			const auto findLiveRecord =
				[&](editor::StableSceneId sourceId)
			{
					return std::find_if(
						m_activeConcrete->liveHandles.begin(),
						m_activeConcrete->liveHandles.end(),
						[&](const ConcreteLiveHandleRecord& record)
						{
							return record.sourceId == sourceId;
						});
				};

			// Incremental edits cannot replace concrete identity. Compare the entire
			// generation-bearing scene-object/camera/light mapping in both directions,
			// including unchanged entries, before a candidate or backend call exists.
			size_t concreteEntryCount = 0u;
			for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
			{
				editor::RendererObjectKind expectedKind{};
				const bool concreteSource = concreteLiveHandleKind(
					entry.stableId.kind, expectedKind);
				if (!concreteSource &&
					!concreteLiveRendererKind(entry.rendererHandle.kind))
				{
					continue;
				}
				++concreteEntryCount;
				const auto live = findLiveRecord(entry.stableId);
				if (!concreteSource || live == m_activeConcrete->liveHandles.end() ||
					entry.rendererHandle.kind != expectedKind ||
					live->handle != entry.rendererHandle ||
					live->projection != entry.projection)
				{
					return staleMessage(concreteSource
						? expectedKind : entry.rendererHandle.kind);
				}
			}
			if (concreteEntryCount != m_activeConcrete->liveHandles.size())
			{
				return "concrete live handle mapping is incomplete";
			}

			for (const editor::SceneFieldChange& change : changes.canonicalChanges)
			{
				const editor::RendererSceneMappingEntry* entry = mapping.find(change.target);
				if (entry == nullptr)
				{
					return "target stable ID has no renderer mapping entry";
				}
				const editor::RendererObjectHandle handle = entry->rendererHandle;
				const auto liveRecord = findLiveRecord(change.target);
				const bool sceneRootTransform =
					change.field == editor::SceneField::SceneRootTransform;
				if ((!sceneRootTransform &&
					 (change.changeClass == editor::SceneChangeClass::Transform ||
					  change.changeClass == editor::SceneChangeClass::Visibility)) ||
					(change.changeClass == editor::SceneChangeClass::MaterialBinding &&
					 handle.kind == editor::RendererObjectKind::SceneObject))
				{
					const bool concreteObject = std::any_of(
						m_activeConcrete->objects.begin(),
						m_activeConcrete->objects.end(),
						[&](const ConcreteObjectRecord& object)
						{
							return object.sourceId == change.target ||
							       object.primitiveSourceId == change.target;
						});
					const bool exactLiveHandle =
						liveRecord != m_activeConcrete->liveHandles.end() &&
						liveRecord->handle == handle &&
						liveRecord->handle.kind ==
							editor::RendererObjectKind::SceneObject;
					if (!exactLiveHandle || !concreteObject)
					{
						return "scene object handle generation is stale";
					}
				}
				if (sceneRootTransform &&
					(liveRecord == m_activeConcrete->liveHandles.end() ||
					 liveRecord->handle != handle ||
					 handle.kind != editor::RendererObjectKind::SceneObject ||
					 entry->projection.kind != editor::EditorObjectKind::scene))
				{
					return "scene-root transform has no live root handle";
				}
				if (change.changeClass == editor::SceneChangeClass::MaterialParameters ||
					(change.changeClass == editor::SceneChangeClass::MaterialBinding &&
					 handle.kind == editor::RendererObjectKind::Material))
				{
					const MaterialHandle materialHandle{handle.index, handle.generation};
					const bool live = m_activeConcrete->deviceRecords != nullptr &&
						std::any_of(
							m_activeConcrete->deviceRecords->materials.begin(),
							m_activeConcrete->deviceRecords->materials.end(),
							[&](const auto& material)
							{
								return material.handle == materialHandle;
							});
					if (handle.kind != editor::RendererObjectKind::Material || !live)
					{
						return "material handle generation is stale";
					}
				}
				if (change.changeClass == editor::SceneChangeClass::Camera &&
					(liveRecord == m_activeConcrete->liveHandles.end() ||
					 liveRecord->handle != handle ||
					 liveRecord->projection != entry->projection ||
					 handle.kind != editor::RendererObjectKind::Camera ||
					 entry->stableId.kind != editor::EditorObjectKind::camera ||
					 entry->projection.sceneAssetIndex >= m_activeConcrete->asset.cameras.size()))
				{
					return "camera handle generation is stale";
				}
				if (change.changeClass == editor::SceneChangeClass::Light &&
					(liveRecord == m_activeConcrete->liveHandles.end() ||
					 liveRecord->handle != handle ||
					 liveRecord->projection != entry->projection ||
					 handle.kind != editor::RendererObjectKind::Light ||
					 entry->stableId.kind != editor::EditorObjectKind::light ||
					 entry->projection.sceneAssetIndex >= m_activeConcrete->asset.lights.size()))
				{
					return "light handle generation is stale";
				}
			}
			return std::nullopt;
		}

		editor::SceneApplyResult uninitializedResult(
			const editor::ApplySceneChangeRequest& request) const noexcept
		{
			editor::SceneApplyResult result;
			result.disposition = editor::SceneApplyDisposition::Rejected;
			result.error = editor::SceneApplyError::InvalidState;
			result.message = "scene transaction publisher is not initialized";
			result.canonicalIdentity = request.canonicalIdentity;
			result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
			result.rhiResult = rhi::RHIResult::ok();
			return result;
		}

		editor::SceneApplyResult reject(
			const editor::ApplySceneChangeRequest& request,
			editor::SceneApplyError error,
			std::string message,
			std::optional<editor::FullRebuildReason> reason) noexcept
		{
			editor::SceneApplyResult result;
			const editor::SceneDeltaCounterSnapshot before = m_counters->snapshot();
			result.disposition = editor::SceneApplyDisposition::Rejected;
			result.error = error;
			result.message = std::move(message);
			result.rhiResult = rhi::RHIResult::ok();
			result.canonicalIdentity = request.canonicalIdentity;
			result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
			result.oldFence = m_publisher->activeGeneration()->fence();
			result.countersBefore = before;
			result.countersAfter = before;
			result.requestedFullSnapshotReason = reason;
			return result;
		}

		void clearCandidate() noexcept
		{
			m_candidatePrepared = false;
			m_pendingFullSnapshot = false;
			m_pendingSubmissionCount = 0u;
			m_candidateAsset = {};
			m_candidatePlan = {};
			m_pendingFullAsset = {};
			m_pendingFullPlan = {};
			m_pendingConcreteAuthority.reset();
			m_pendingOwnerCapability = {};
			m_candidateConcrete.reset();
		}

		void commitCandidate(
			const editor::SceneChangeSet&,
			const editor::RendererSceneMappingSnapshot&) noexcept
		{
			if (!m_candidatePrepared || m_candidateConcrete == nullptr)
			{
				return;
			}
			m_activeAsset = std::move(m_candidateAsset);
			m_activePlan = std::move(m_candidatePlan);
			if (m_pendingOwnerCapability.valid())
				m_activeOwnerCapability = m_pendingOwnerCapability;
			const std::shared_ptr<const editor::RendererSceneGeneration> published =
				m_publisher->activeGeneration();
			m_candidateConcrete->generation = published->generation();
			m_candidateConcrete->descriptorGeneration =
				published->descriptorGeneration();
			m_candidateConcrete->deviceRecords->descriptorGeneration =
				published->descriptorGeneration();
			m_candidateConcrete->deviceRecords->publicationGeneration =
				published->generation();
			m_candidateConcrete->rendererState->generation = published->generation();
			m_candidateConcrete->rendererState->descriptorGeneration =
				published->descriptorGeneration();
			m_nextConcreteHandleIndices =
				m_candidateConcrete->nextConcreteHandleIndices;
			if (m_candidateConcrete->resources != nullptr)
			{
				m_candidateConcrete->resources->deviceRecords =
					m_candidateConcrete->deviceRecords;
			}
			m_activeConcrete.swap(m_candidateConcrete);
			m_owner.m_renderer.m_sceneTransactionResources.store(
				m_activeConcrete->deviceRecords, std::memory_order_release);
			m_owner.m_sceneTransactionRendererState.store(
				m_activeConcrete->rendererState, std::memory_order_release);
			m_owner.m_renderer.m_sceneTransactionPublishedGeneration.store(
				m_activeConcrete->generation, std::memory_order_release);
		}

		rhi::RHIResult stageCpuGeneration(
			const editor::SceneChangeSet& changes,
			const editor::RendererSceneMappingSnapshot& targetMapping) noexcept override
		{
			if (consumeFailure(SceneTransactionFailurePoint::cpuStaging))
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"injected scene CPU candidate staging failure");
			}
			try
			{
				if (m_pendingFullSnapshot)
				{
					if (!m_pendingConcreteAuthority)
					{
						return rhi::RHIResult::fail(
							rhi::RHIErrorCode::invalidState,
							"full snapshot has no prevalidated concrete authority");
					}
					ConcreteAuthorityBuild authority =
						std::move(*m_pendingConcreteAuthority);
					m_pendingConcreteAuthority.reset();
					m_candidateConcrete = buildConcreteGeneration(
						m_pendingFullAsset,
						m_pendingFullPlan,
						targetMapping,
						std::move(authority.records),
						authority.nextHandleIndices);
				}
				else
				{
					if (m_activeConcrete == nullptr)
					{
						return rhi::RHIResult::fail(
							rhi::RHIErrorCode::invalidState,
							"active concrete scene generation is missing");
					}
					m_candidateConcrete = std::make_shared<ConcreteGeneration>(*m_activeConcrete);
					m_candidateConcrete->mapping = targetMapping;
					m_candidateConcrete->resources = m_activeConcrete->resources;
					if (m_activeConcrete->rendererState != nullptr)
					{
						m_candidateConcrete->rendererState =
							std::make_shared<GPUDrivenRenderer::SceneTransactionRendererState>(
								*m_activeConcrete->rendererState);
					}
					if (m_activeConcrete->deviceRecords != nullptr)
					{
						m_candidateConcrete->deviceRecords =
							std::make_shared<RenderDevice::SceneTransactionResources>(
								*m_activeConcrete->deviceRecords);
					}
					m_candidateConcrete->asset.documentRevision =
						targetMapping.fence().appliedDocumentRevision;
					m_candidateConcrete->plan.header = SceneUploadPlanHeader{
						.authority = targetMapping.fence().authority,
						.documentRevision = targetMapping.fence().appliedDocumentRevision,
						.activeScene = targetMapping.fence().activeScene,
						.sceneActivationEpoch = targetMapping.fence().sceneActivationEpoch,
						.rootExternalStateSerial = targetMapping.fence().rootExternalStateSerial,
						.projectionDigest = targetMapping.fence().projectionDigest,
						.mappingEpoch = targetMapping.fence().mappingEpoch,
					};
				}
				applyCpuChanges(*m_candidateConcrete, changes);
				const rhi::RHIResult prepared = prepareRendererStateForPublication();
				if (!prepared.succeeded()) return prepared;
				m_candidateAsset = m_candidateConcrete->asset;
				m_candidatePlan = m_candidateConcrete->plan;
				m_candidatePrepared = true;
				return rhi::RHIResult::ok();
			}
			catch (const std::bad_alloc&)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"scene CPU candidate allocation failed");
			}
			catch (...)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"scene CPU candidate staging failed");
			}
		}

		rhi::RHIResult allocateSceneResources(
			const editor::SceneChangeSet&,
			std::vector<editor::SceneJournalResource>& createdResources) noexcept override
		{
			return allocateCandidateResources(createdResources);
		}

		rhi::RHIResult stageSceneDescriptors(
			const editor::SceneChangeSet&,
			std::span<const editor::SceneJournalResource>) noexcept override
		{
			if (!m_pendingFullSnapshot)
			{
				return rhi::RHIResult::ok();
			}
			if (consumeFailure(SceneTransactionFailurePoint::descriptorStaging))
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"injected private descriptor staging failure");
			}
			if (m_candidateConcrete == nullptr ||
				m_candidateConcrete->resources == nullptr)
			{
				return rhi::RHIResult::ok();
			}
			try
			{
				ResourceSet& resources = *m_candidateConcrete->resources;
				auto& records = *resources.deviceRecords;
				std::vector<rhi::ArgumentWrite> writes;
				writes.reserve(records.textures.size());
				for (uint32_t index = 0u; index < records.textures.size(); ++index)
				{
					writes.push_back(rhi::ArgumentWrite{
						.binding = 0u,
						.arrayElement = index,
						.type = rhi::ArgumentType::combinedImageSampler,
						.textureView = records.textures[index].record.hot.sampledViewHandle,
						.sampler = records.sampler,
					});
				}
				if (!writes.empty())
				{
					resources.device->updateArgumentTable(records.descriptorTable, writes);
				}
				resources.descriptorsStaged = true;
				return rhi::RHIResult::ok();
			}
			catch (const std::bad_alloc&)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"private descriptor write allocation failed");
			}
			catch (const std::exception& exception)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState, exception.what());
			}
		}

		rhi::RHIResult recordSceneGeneration(
			const editor::SceneChangeSet&,
			std::span<const editor::SceneJournalResource>) noexcept override
		{
			if (!m_pendingFullSnapshot)
			{
				return rhi::RHIResult::ok();
			}
			if (consumeFailure(SceneTransactionFailurePoint::commandRecording))
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"injected private scene recording failure");
			}
			if (m_candidateConcrete == nullptr ||
				m_candidateConcrete->resources == nullptr)
			{
				return rhi::RHIResult::ok();
			}
			try
			{
				ResourceSet& resources = *m_candidateConcrete->resources;
				resources.commandAllocator =
					resources.device->createCommandAllocator(rhi::QueueClass::graphics);
				if (resources.commandAllocator == nullptr)
				{
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::unsupported,
						"graphics command allocator is unavailable");
				}
				const uint32_t commandBufferCount = std::max(m_pendingSubmissionCount, 1u);
				resources.commandBuffers.reserve(commandBufferCount);
				for (uint32_t index = 0u; index < commandBufferCount; ++index)
				{
					auto commandBuffer = resources.commandAllocator->createCommandBuffer();
					if (commandBuffer == nullptr)
					{
						return rhi::RHIResult::fail(
							rhi::RHIErrorCode::unsupported,
							"graphics command buffer is unavailable");
					}
					commandBuffer->begin(*resources.commandAllocator);
					if (index == 0u)
					{
						if (!resources.bufferBeginBarriers.empty())
						{
							commandBuffer->resourceBarrier({}, resources.bufferBeginBarriers);
						}
						if (!resources.textureBeginBarriers.empty())
						{
							commandBuffer->resourceBarrier(resources.textureBeginBarriers, {});
						}
						if (!resources.bufferCopies.empty() || !resources.textureCopies.empty())
						{
							rhi::ComputeEncoder* copy = commandBuffer->beginComputePass();
							if (copy == nullptr)
							{
								return rhi::RHIResult::fail(
									rhi::RHIErrorCode::unsupported,
									"transfer encoder is unavailable");
							}
							for (const BufferCopy& operation : resources.bufferCopies)
							{
								copy->copyBuffer(operation.source, operation.sourceOffset,
								                 operation.destination, operation.destinationOffset,
								                 operation.size);
							}
							for (const TextureCopy& operation : resources.textureCopies)
							{
								copy->copyBufferToTexture(operation.region);
							}
							commandBuffer->endEncoding();
						}
						if (!resources.bufferEndBarriers.empty())
						{
							commandBuffer->resourceBarrier({}, resources.bufferEndBarriers);
						}
						if (resources.registry != nullptr)
						{
							resources.registry->syncToGpu(*commandBuffer);
						}
						if (!resources.textureEndBarriers.empty())
						{
							commandBuffer->resourceBarrier(resources.textureEndBarriers, {});
						}
					}
					commandBuffer->end();
					resources.commandBuffers.push_back(std::move(commandBuffer));
				}
				resources.recorded = true;
				return rhi::RHIResult::ok();
			}
			catch (const std::bad_alloc&)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"private scene command allocation failed");
			}
			catch (const std::exception& exception)
			{
				static thread_local std::string recordingError;
				recordingError = "private scene command recording failed: ";
				recordingError += exception.what();
				return rhi::RHIResult::fail(rhi::RHIErrorCode::invalidState,
				                           recordingError.c_str());
			}
		}

		rhi::Result<rhi::SubmissionToken> submitSceneGeneration(
			const editor::SceneQueueSubmissionPlan& submission,
			uint32_t submissionIndex) noexcept override
		{
			if (consumeFailure(SceneTransactionFailurePoint::queueSubmission,
			                   submissionIndex))
			{
				return rhi::Result<rhi::SubmissionToken>::fail(
					rhi::RHIErrorCode::deviceLost,
					"injected private scene queue submission failure");
			}
			if (m_candidateConcrete == nullptr ||
				m_candidateConcrete->resources == nullptr)
			{
				return rhi::Result<rhi::SubmissionToken>::fail(
					rhi::RHIErrorCode::invalidState,
					"scene submission has no private recorded resource generation");
			}
			try
			{
				ResourceSet& resources = *m_candidateConcrete->resources;
				if (!resources.recorded || submissionIndex >= resources.commandBuffers.size())
				{
					return rhi::Result<rhi::SubmissionToken>::fail(
						rhi::RHIErrorCode::invalidState,
						"scene submission index has no executable command buffer");
				}
				rhi::Queue* queue = resources.device->getQueue(submission.queueClass);
				if (queue == nullptr)
				{
					return rhi::Result<rhi::SubmissionToken>::fail(
						rhi::RHIErrorCode::unsupported,
						"requested scene submission queue is unavailable");
				}
				if (resources.submissionQueues.size() <= submissionIndex)
				{
					resources.submissionQueues.resize(
						static_cast<size_t>(submissionIndex) + 1u, nullptr);
				}
				resources.submissionQueues[submissionIndex] = queue;
				rhi::CommandBuffer* commandBuffer =
					resources.commandBuffers[submissionIndex].get();
				const std::array<rhi::CommandBuffer*, 1> commands{{commandBuffer}};
				const rhi::SubmissionToken token = queue->submit(rhi::SubmitBatch{
					.commandBuffers = commands,
					.debugLabel = "MGIF.SceneTransaction.PrivateGeneration",
				});
				++m_queueSubmissionCount;
				if (!token.isValid() || token.queue != queue->identity())
				{
					// Work may already be queued. Report success verbatim so the
					// publisher takes its after-submit rollback path and asks this
					// backend to recover the actual fence dependency.
					m_lastSubmissionToken = token;
					return rhi::Result<rhi::SubmissionToken>::ok(token);
				}
				resources.submittedQueues.emplace_back(token.queue, queue);
				const auto existingToken = std::find_if(
					resources.lastUseTokens.begin(), resources.lastUseTokens.end(),
					[token](rhi::SubmissionToken candidate)
					{
						return candidate.queue == token.queue;
					});
				if (existingToken == resources.lastUseTokens.end())
				{
					resources.lastUseTokens.push_back(token);
				}
				else if (existingToken->value < token.value)
				{
					*existingToken = token;
				}
				m_candidateConcrete->submissionTokens.push_back(token);
				m_lastSubmissionToken = token;
				return rhi::Result<rhi::SubmissionToken>::ok(token);
			}
			catch (const std::exception& exception)
			{
				return rhi::Result<rhi::SubmissionToken>::fail(
					rhi::RHIErrorCode::deviceLost, exception.what());
			}
		}

		rhi::RHIResult validateBeforePublication(
			const editor::RendererSceneMappingSnapshot& targetMapping) noexcept override
		{
			if (m_failurePoint == SceneTransactionFailurePoint::retirementEnqueue)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"injected post-submit failure before orphan retirement enqueue");
			}
			if (consumeFailure(SceneTransactionFailurePoint::prePublication))
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"injected private scene prepublication failure");
			}
			const rhi::RHIResult rendererState = prepareRendererStateForPublication();
			if (!rendererState.succeeded()) return rendererState;
			return m_candidatePrepared &&
			       uploadPlanHeaderMatchesFence(m_candidatePlan.header,
			                                    targetMapping.fence())
				? rhi::RHIResult::ok()
				: rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidState,
					"scene CPU candidate does not match target mapping");
		}

		rhi::Result<editor::SceneRetirementReservation> reserveSceneRetirement(
			std::span<const editor::SceneJournalResource> resources,
			std::span<const rhi::SubmissionToken> dependencies) noexcept override
		{
			if (consumeFailure(SceneTransactionFailurePoint::retirementReservation))
			{
				return rhi::Result<editor::SceneRetirementReservation>::fail(
					rhi::RHIErrorCode::outOfMemory,
					"injected superseded scene retirement reservation failure");
			}
			if (m_nextRetirementReservation ==
				std::numeric_limits<uint64_t>::max())
			{
				return rhi::Result<editor::SceneRetirementReservation>::fail(
					rhi::RHIErrorCode::invalidState,
					"scene retirement reservation identity overflow");
			}
			try
			{
				std::shared_ptr<ResourceSet> resourceSet = resourceSetFor(resources);
				if (resourceSet == nullptr)
				{
					return rhi::Result<editor::SceneRetirementReservation>::fail(
						rhi::RHIErrorCode::invalidHandle,
						"superseded resources do not belong to one private generation");
				}
				// Retirement must be fenced by the complete, duplicate-free token
				// frontier that belongs to this private generation. Checking only
				// that every expected token appears would allow an extra or duplicate
				// dependency to hide a missing frontier entry.
				const auto exactSubmissionFrontier = [&]() noexcept
				{
					if (dependencies.size() != resourceSet->lastUseTokens.size())
					{
						return false;
					}
					return std::all_of(
						resourceSet->lastUseTokens.begin(),
						resourceSet->lastUseTokens.end(),
						[dependencies](rhi::SubmissionToken expected)
						{
							return expected.isValid() &&
							       std::count(dependencies.begin(), dependencies.end(), expected) == 1;
						});
				}();
				if (!exactSubmissionFrontier)
				{
					return rhi::Result<editor::SceneRetirementReservation>::fail(
						rhi::RHIErrorCode::invalidState,
						"superseded generation lost its exact submission token");
				}
				const uint64_t reservationId = ++m_nextRetirementReservation;
				m_retirementReservations.emplace(
					reservationId,
					RetirementRecord{
						.resources = std::move(resourceSet),
						.dependencies = std::vector<rhi::SubmissionToken>(
							dependencies.begin(), dependencies.end()),
					});
				return rhi::Result<editor::SceneRetirementReservation>::ok(
					editor::SceneRetirementReservation{reservationId});
			}
			catch (const std::bad_alloc&)
			{
				return rhi::Result<editor::SceneRetirementReservation>::fail(
					rhi::RHIErrorCode::outOfMemory,
					"scene retirement reservation allocation failed");
			}
		}

		void commitSceneRetirement(
			editor::SceneRetirementReservation reservation) noexcept override
		{
			const auto found = m_retirementReservations.find(reservation.value);
			if (found != m_retirementReservations.end())
			{
				found->second.committed = true;
			}
		}

		void cancelSceneRetirement(
			editor::SceneRetirementReservation reservation) noexcept override
		{
			m_retirementReservations.erase(reservation.value);
		}

		rhi::RHIResult destroyUnpublishedResources(
			std::span<const editor::SceneJournalResource> resources) noexcept override
		{
			if (resources.empty())
			{
				return rhi::RHIResult::ok();
			}
			std::shared_ptr<ResourceSet> resourceSet = resourceSetFor(resources);
			if (resourceSet == nullptr)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidHandle,
					"unpublished resources do not belong to one private generation");
			}
			const rhi::RHIResult destroyed = destroyResourceSet(resourceSet);
			if (destroyed.succeeded())
			{
				m_immediatelyDestroyedResourceCount += resources.size();
			}
			return destroyed;
		}

		rhi::RHIResult enqueueResourceRetirement(
			std::span<const editor::SceneJournalResource> resources,
			std::span<const rhi::SubmissionToken> dependencies) noexcept override
		{
			if (resources.empty())
			{
				return rhi::RHIResult::ok();
			}
			if (consumeFailure(SceneTransactionFailurePoint::retirementEnqueue))
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"injected orphan resource retirement enqueue failure");
			}
			try
			{
				std::shared_ptr<ResourceSet> resourceSet = resourceSetFor(resources);
				if (resourceSet == nullptr || dependencies.empty() ||
					std::any_of(dependencies.begin(), dependencies.end(),
					            [](rhi::SubmissionToken token) { return !token.isValid(); }))
				{
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::invalidHandle,
						"orphan retirement requires one owned generation and exact tokens");
				}
				const auto exactSubmissionFrontier = [&]() noexcept
				{
					if (dependencies.size() != resourceSet->lastUseTokens.size())
					{
						return false;
					}
					return std::all_of(
						resourceSet->lastUseTokens.begin(),
						resourceSet->lastUseTokens.end(),
						[dependencies](rhi::SubmissionToken expected)
						{
							return expected.isValid() &&
							       std::count(dependencies.begin(), dependencies.end(), expected) == 1;
						});
				}();
				if (!exactSubmissionFrontier)
				{
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::invalidState,
						"orphan generation lost its exact submission token");
				}
				m_retirements.push_back(RetirementRecord{
					.resources = std::move(resourceSet),
					.dependencies = std::vector<rhi::SubmissionToken>(
						dependencies.begin(), dependencies.end()),
					.committed = true,
				});
				return rhi::RHIResult::ok();
			}
			catch (const std::bad_alloc&)
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"orphan retirement queue allocation failed");
			}
		}

		rhi::Result<rhi::SubmissionToken> recoverSubmissionDependency(
			std::span<const editor::SceneJournalResource> resources,
			std::uint32_t submissionIndex,
			rhi::SubmissionToken reportedToken) noexcept override
		{
			try
			{
				const std::shared_ptr<ResourceSet> resourceSet =
					resourceSetFor(resources);
				if (resourceSet == nullptr || resourceSet->device == nullptr)
				{
					return rhi::Result<rhi::SubmissionToken>::fail(
						rhi::RHIErrorCode::invalidHandle,
						"submitted orphan has no owned device resource set");
				}
				rhi::Queue* queue =
					submissionIndex < resourceSet->submissionQueues.size()
						? resourceSet->submissionQueues[submissionIndex]
						: nullptr;
				if (queue == nullptr && reportedToken.queue.isValid())
				{
					for (const auto& [identity, candidate] :
						 resourceSet->submittedQueues)
					{
						if (identity == reportedToken.queue)
						{
							queue = candidate;
							break;
						}
					}
				}
				if (queue == nullptr)
				{
					return rhi::Result<rhi::SubmissionToken>::fail(
						rhi::RHIErrorCode::invalidState,
						"submitted orphan queue identity could not be recovered");
				}
				const rhi::SubmissionToken recovered = queue->lastSubmittedToken();
				if (!recovered.isValid() || recovered.queue != queue->identity())
				{
					return rhi::Result<rhi::SubmissionToken>::fail(
						rhi::RHIErrorCode::invalidState,
						"queue did not expose a valid recovered submission token");
				}
				const auto submittedQueue = std::find_if(
					resourceSet->submittedQueues.begin(),
					resourceSet->submittedQueues.end(),
					[&](const auto& candidate)
					{
						return candidate.first == recovered.queue;
					});
				if (submittedQueue == resourceSet->submittedQueues.end())
				{
					resourceSet->submittedQueues.emplace_back(recovered.queue, queue);
				}
				const auto existingToken = std::find_if(
					resourceSet->lastUseTokens.begin(),
					resourceSet->lastUseTokens.end(),
					[&](rhi::SubmissionToken candidate)
					{
						return candidate.queue == recovered.queue;
					});
				if (existingToken == resourceSet->lastUseTokens.end())
				{
					resourceSet->lastUseTokens.push_back(recovered);
				}
				else if (existingToken->value < recovered.value)
				{
					*existingToken = recovered;
				}
				m_lastSubmissionToken = recovered;
				return rhi::Result<rhi::SubmissionToken>::ok(recovered);
			}
			catch (const std::exception& exception)
			{
				return rhi::Result<rhi::SubmissionToken>::fail(
					rhi::RHIErrorCode::deviceLost, exception.what());
			}
		}

		GPUDrivenRenderer& m_owner;
		mutable std::mutex m_mutex;
		std::unique_ptr<editor::SceneDeltaCounters> m_counters;
		std::unique_ptr<editor::RendererScenePublisher> m_publisher;
		SceneAsset m_activeAsset;
		SceneUploadPlan m_activePlan;
		editor::SceneProjectionOwnerCapability m_activeOwnerCapability;
		SceneAsset m_candidateAsset;
		SceneUploadPlan m_candidatePlan;
		SceneAsset m_pendingFullAsset;
		SceneUploadPlan m_pendingFullPlan;
		std::shared_ptr<ConcreteGeneration> m_activeConcrete;
		std::shared_ptr<ConcreteGeneration> m_candidateConcrete;
		std::optional<ConcreteAuthorityBuild> m_pendingConcreteAuthority;
		editor::SceneProjectionOwnerCapability m_pendingOwnerCapability;
		std::array<uint32_t, 8u> m_nextConcreteHandleIndices{};
		std::unordered_map<uint64_t, RetirementRecord> m_retirementReservations;
		std::vector<RetirementRecord> m_retirements;
		// The diagnostic journal is also the durable owner while a submitted
		// orphan is waiting for a failed retirement enqueue to be retried.
		std::unordered_map<uint64_t, std::shared_ptr<ResourceSet>> m_resourceOwners;
		bool m_pendingFullSnapshot{false};
		bool m_candidatePrepared{false};
		uint64_t m_nextRetirementReservation{0};
		uint64_t m_nextResourceDiagnosticId{0};
		uint64_t m_nextSubmissionDiagnosticId{0};
		uint32_t m_pendingSubmissionCount{0u};
		uint64_t m_immediatelyDestroyedResourceCount{0};
		uint64_t m_eventuallyDestroyedResourceCount{0};
		uint64_t m_queueSubmissionCount{0};
		rhi::SubmissionToken m_lastSubmissionToken{};
		SceneTransactionFailurePoint m_failurePoint{SceneTransactionFailurePoint::none};
		uint32_t m_failureSubmissionIndex{0u};
	};

	std::shared_ptr<GPUDrivenSceneTransactionBridge::ConcreteGeneration>
	GPUDrivenSceneTransactionBridge::buildConcreteGeneration(
		SceneAsset asset,
		SceneUploadPlan plan,
		const editor::RendererSceneMappingSnapshot& mapping,
		std::vector<ConcreteLiveHandleRecord> liveHandles,
		std::array<uint32_t, 8u> nextHandleIndices) const
	{
		auto generation = std::make_shared<ConcreteGeneration>();
		generation->asset = std::move(asset);
		generation->plan = std::move(plan);
		generation->mapping = mapping;
		generation->liveHandles = std::move(liveHandles);
		generation->nextConcreteHandleIndices = nextHandleIndices;
		generation->deviceRecords =
			std::make_shared<RenderDevice::SceneTransactionResources>();

		auto& deviceRecords = *generation->deviceRecords;
		deviceRecords.meshes.reserve(generation->plan.meshes.size());
		uint32_t nextGeometryHandle = firstPrivateHandleIndex(
			mapping, editor::RendererObjectKind::Geometry);
		const uint32_t geometryHandleGeneration = privateHandleGeneration(
			mapping, editor::RendererObjectKind::Geometry);
		for (const MeshUploadPlan& mesh : generation->plan.meshes)
		{
			const editor::RendererObjectHandle handle = allocatePrivateHandle(
				mapping, editor::RendererObjectKind::Geometry,
				geometryHandleGeneration, nextGeometryHandle);
			MeshRecord record{};
			record.vertexCount = mesh.vertexCount;
			record.indexCount = mesh.indexCount;
			record.vertexStride = mesh.vertexStride;
			record.firstIndex = 0u;
			record.vertexOffset = 0;
			record.localBoundsMin = mesh.localBoundsMin;
			record.localBoundsMax = mesh.localBoundsMax;
			if (mesh.meshIndex < generation->asset.meshes.size())
			{
				const SceneMesh& assetMesh = generation->asset.meshes[mesh.meshIndex];
				record.transform = assetMesh.exportTransform;
				record.materialIndex = static_cast<int32_t>(assetMesh.materialIndex);
				record.alphaMode = assetMesh.alphaMode;
				record.alphaCutoff = assetMesh.alphaCutoff;
				const glm::vec4 bounds = computeTransformedBoundsSphere(
					record, record.transform);
				record.worldBoundsCenter = glm::vec3(bounds);
				record.worldBoundsRadius = bounds.w;
				record.worldBoundsMin = record.worldBoundsCenter - glm::vec3(bounds.w);
				record.worldBoundsMax = record.worldBoundsCenter + glm::vec3(bounds.w);
			}
			deviceRecords.meshes.push_back({
				.handle = {handle.index, handle.generation},
				.record = record,
			});
		}

		deviceRecords.textures.reserve(generation->plan.textures.size());
		for (const TextureUploadPlan& texture : generation->plan.textures)
		{
			const editor::RendererObjectHandle handle = rendererHandleFor(
				mapping, texture.sourceId, editor::RendererObjectKind::Texture);
			if (!handle.valid())
			{
				throw std::runtime_error("texture upload plan has no concrete texture handle");
			}
			deviceRecords.textures.push_back({
				.handle = {handle.index, handle.generation},
			});
		}

		// Texture and image IDs can project to one SceneAsset texture. Preserve both
		// generation-safe handles as aliases of the same eventual RHI view.
		for (const editor::RendererSceneMappingEntry& entry : mapping.entries())
		{
			if (entry.rendererHandle.kind != editor::RendererObjectKind::Texture ||
				(entry.stableId.kind != editor::EditorObjectKind::texture &&
				 entry.stableId.kind != editor::EditorObjectKind::image))
			{
				continue;
			}
			const TextureHandle handle{
				entry.rendererHandle.index, entry.rendererHandle.generation};
			if (std::none_of(deviceRecords.textures.begin(), deviceRecords.textures.end(),
			                 [&](const auto& candidate) { return candidate.handle == handle; }))
			{
				deviceRecords.textures.push_back({.handle = handle});
			}
		}

		deviceRecords.materials.reserve(generation->plan.materials.size());
		for (const MaterialCreatePlan& material : generation->plan.materials)
		{
			const editor::RendererObjectHandle handle = rendererHandleFor(
				mapping, material.sourceId, editor::RendererObjectKind::Material);
			if (!handle.valid())
			{
				throw std::runtime_error("material upload plan has no concrete material handle");
			}
			RenderDevice::MaterialResources::MaterialRecord record{};
			const auto textureHandleAt = [&](int32_t textureIndex,
			                                 editor::SceneTextureSemantic semantic)
				-> TextureHandle
			{
				if (textureIndex < 0 ||
					static_cast<size_t>(textureIndex) >= generation->asset.textures.size())
				{
					return {};
				}
				const editor::RendererSceneMappingEntry* textureEntry =
					mappingEntryForTextureCoordinate(
						mapping, semantic,
						static_cast<uint32_t>(textureIndex));
				return textureEntry != nullptr
					? TextureHandle{textureEntry->rendererHandle.index,
					                textureEntry->rendererHandle.generation}
					: TextureHandle{};
			};
			record.baseColorTexture = textureHandleAt(
				material.baseColorTexture, editor::SceneTextureSemantic::BaseColor);
			record.sampledTexture = record.baseColorTexture;
			record.normalTexture = textureHandleAt(
				material.normalTexture, editor::SceneTextureSemantic::Normal);
			record.metallicRoughnessTexture =
				textureHandleAt(material.metallicRoughnessTexture,
				                editor::SceneTextureSemantic::MetallicRoughness);
			record.occlusionTexture = textureHandleAt(
				material.occlusionTexture, editor::SceneTextureSemantic::Occlusion);
			record.emissiveTexture = textureHandleAt(
				material.emissiveTexture, editor::SceneTextureSemantic::Emissive);
			record.baseColorFactor = material.baseColorFactor;
			record.metallicFactor = material.metallicFactor;
			record.roughnessFactor = material.roughnessFactor;
			record.normalScale = material.normalScale;
			record.occlusionStrength = material.occlusionStrength;
			record.emissiveFactor = material.emissiveFactor;
			record.alphaMode = material.alphaMode;
			record.alphaCutoff = material.alphaCutoff;
			record.doubleSided = material.doubleSided;
			record.descriptorIndex = material.materialIndex;
			deviceRecords.materials.push_back({
				.handle = {handle.index, handle.generation},
				.record = record,
			});
		}

		generation->rendererState =
			std::make_shared<GPUDrivenRenderer::SceneTransactionRendererState>();
		rebuildRendererState(*generation);
		return generation;
	}

	void GPUDrivenSceneTransactionBridge::rebuildRendererState(
		ConcreteGeneration& generation) const
	{
		normalizeOccurrenceIdentities(generation);
		if (generation.rendererState == nullptr)
		{
			generation.rendererState =
				std::make_shared<GPUDrivenRenderer::SceneTransactionRendererState>();
		}
		auto& state = *generation.rendererState;
		state.deviceResources = generation.deviceRecords;
		state.upload = {};
		state.opaqueDrawIndices.clear();
		state.alphaTestDrawIndices.clear();
		state.transparentDrawIndices.clear();
		generation.objects.clear();

		state.upload.meshes.assign(generation.asset.meshes.size(), kNullMeshHandle);
		for (size_t planIndex = 0u;
		     planIndex < generation.plan.meshes.size(); ++planIndex)
		{
			const MeshUploadPlan& mesh = generation.plan.meshes[planIndex];
			if (mesh.meshIndex >= state.upload.meshes.size()) continue;
			if (generation.deviceRecords == nullptr ||
				planIndex >= generation.deviceRecords->meshes.size())
			{
				throw std::runtime_error(
					"candidate mesh upload has no private geometry record");
			}
			state.upload.meshes[mesh.meshIndex] =
				generation.deviceRecords->meshes[planIndex].handle;
		}
		state.upload.textures.assign(generation.asset.textures.size(), kNullTextureHandle);
		for (const TextureUploadPlan& texture : generation.plan.textures)
		{
			if (texture.textureIndex >= state.upload.textures.size()) continue;
			const editor::RendererObjectHandle handle = rendererHandleFor(
				generation.mapping, texture.sourceId, editor::RendererObjectKind::Texture);
			state.upload.textures[texture.textureIndex] = {handle.index, handle.generation};
		}
		state.upload.materials.assign(generation.asset.materials.size(), kNullMaterialHandle);
		for (const MaterialCreatePlan& material : generation.plan.materials)
		{
			if (material.materialIndex >= state.upload.materials.size()) continue;
			const editor::RendererObjectHandle handle = rendererHandleFor(
				generation.mapping, material.sourceId, editor::RendererObjectKind::Material);
			state.upload.materials[material.materialIndex] = {handle.index, handle.generation};
		}

		state.upload.instanceToDrawRecord.assign(
			generation.plan.instances.instances.size(), UINT32_MAX);
		state.upload.drawCommandToDrawRecord.assign(
			generation.plan.drawCommands.size(), UINT32_MAX);
		generation.objects.reserve(generation.plan.drawCommands.size());
		state.upload.drawRecords.reserve(generation.plan.drawCommands.size());
		state.upload.drawMeshHandles.reserve(generation.plan.drawCommands.size());
		// The registry is a private concrete namespace. Its generation must never
		// be inferred from caller-supplied mapping handles.
		uint32_t nextSceneObjectHandle = 1u;
		constexpr uint32_t sceneObjectHandleGeneration = 1u;

		for (size_t commandIndex = 0u;
		     commandIndex < generation.plan.drawCommands.size(); ++commandIndex)
		{
			const DrawCommandBuildPlan& draw = generation.plan.drawCommands[commandIndex];
			if (draw.instanceIndex >= generation.plan.instances.instances.size() ||
				draw.meshIndex >= state.upload.meshes.size())
			{
				throw std::runtime_error("candidate draw command references invalid renderer state");
			}
			const SceneDrawInstance& instance =
				generation.plan.instances.instances[draw.instanceIndex];
			const editor::RendererSceneMappingEntry* primitiveEntry =
				generation.mapping.find(draw.sourceId);
			const editor::RendererSceneMappingEntry* nodeEntry =
				generation.mapping.find(draw.nodeSourceId);
			if (primitiveEntry == nullptr || nodeEntry == nullptr ||
				primitiveEntry->rendererHandle.kind !=
					editor::RendererObjectKind::SceneObject ||
				nodeEntry->rendererHandle.kind !=
					editor::RendererObjectKind::SceneObject)
			{
				throw std::runtime_error(
					"candidate draw source has no logical scene-object mapping");
			}
			const editor::RendererObjectHandle objectHandle = allocatePrivateHandle(
				generation.mapping, editor::RendererObjectKind::SceneObject,
				sceneObjectHandleGeneration, nextSceneObjectHandle);

			const MeshHandle meshHandle = state.upload.meshes[draw.meshIndex];
			MaterialHandle materialHandle{};
			if (draw.materialIndex < state.upload.materials.size())
			{
				materialHandle = state.upload.materials[draw.materialIndex];
			}
			glm::vec4 boundsSphere(0.0f);
			for (const InstanceCullRecord& cull : generation.plan.cullRecords)
			{
				if (cull.instanceIndex == draw.instanceIndex)
				{
					boundsSphere = cull.boundingSphere;
					break;
				}
			}
			uint32_t alphaMode = shaderio::LAlphaOpaque;
			float alphaCutoff = 0.5f;
			bool doubleSided = false;
			if (draw.materialIndex < generation.asset.materials.size())
			{
				alphaMode = static_cast<uint32_t>(
					generation.asset.materials[draw.materialIndex].alphaMode);
				alphaCutoff = generation.asset.materials[draw.materialIndex].alphaCutoff;
				doubleSided = generation.asset.materials[draw.materialIndex].doubleSided;
			}
			const uint32_t drawRecordIndex =
				static_cast<uint32_t>(state.upload.drawRecords.size());
			state.upload.drawRecords.push_back({
				.instanceIndex = draw.instanceIndex,
				.meshIndex = draw.meshIndex,
				.materialIndex = draw.materialIndex,
				.meshHandle = meshHandle,
				.materialHandle = materialHandle,
				.worldTransform = instance.worldTransform,
				.boundsSphere = boundsSphere,
				.alphaMode = alphaMode,
				.alphaCutoff = alphaCutoff,
				.doubleSided = doubleSided,
			});
			state.upload.drawMeshHandles.push_back(meshHandle);
			state.upload.instanceToDrawRecord[draw.instanceIndex] = drawRecordIndex;
			state.upload.drawCommandToDrawRecord[commandIndex] = drawRecordIndex;

			GPUSceneRegistrationDesc registration{};
			registration.meshHandle = meshHandle;
			registration.meshIndex = draw.meshIndex;
			registration.materialIndex = draw.materialIndex;
			registration.transform = instance.worldTransform;
			registration.boundsSphere = boundsSphere;
			registration.flags = alphaMode == shaderio::LAlphaBlend
				? shaderio::LGPUCullFlagFrustumCulling |
				  shaderio::LGPUCullFlagTransparent
				: shaderio::LGPUCullFlagFrustumCulling |
				  shaderio::LGPUCullFlagOcclusionCulling |
				  (alphaMode == shaderio::LAlphaMask
					  ? shaderio::LGPUCullFlagAlphaMask : 0u);
			registration.visible = instance.nodeIndex < generation.asset.nodes.size()
				? generation.asset.nodes[instance.nodeIndex].effectiveVisible : true;
			for (const auto& mesh : generation.deviceRecords->meshes)
			{
				if (mesh.handle != meshHandle) continue;
				registration.indexCount = mesh.record.indexCount;
				registration.firstIndex = mesh.record.firstIndex;
				registration.vertexOffset = mesh.record.vertexOffset;
				break;
			}
			generation.objects.push_back({
				.sourceId = draw.nodeSourceId,
				.primitiveSourceId = draw.sourceId,
				.handle = objectHandle,
				.drawRecordIndex = drawRecordIndex,
				.registration = registration,
			});

			if (alphaMode == shaderio::LAlphaBlend)
			{
				state.transparentDrawIndices.push_back(drawRecordIndex);
				state.upload.transparentDrawIndices.push_back(drawRecordIndex);
				state.upload.transparentDrawDistances.push_back(0.0f);
			}
			else if (alphaMode == shaderio::LAlphaMask)
			{
				state.alphaTestDrawIndices.push_back(drawRecordIndex);
				state.upload.alphaTestDrawIndices.push_back(drawRecordIndex);
				state.upload.shadowCasterDrawIndices.push_back(drawRecordIndex);
			}
			else
			{
				state.opaqueDrawIndices.push_back(drawRecordIndex);
				state.upload.opaqueDrawIndices.push_back(drawRecordIndex);
				state.upload.shadowCasterDrawIndices.push_back(drawRecordIndex);
			}
		}
		sealOccurrenceSnapshot(generation);
	}

	void GPUDrivenSceneTransactionBridge::normalizeOccurrenceIdentities(
		ConcreteGeneration& generation) const
	{
		SceneUploadPlan& plan = generation.plan;
		if (!plan.header.activeScene)
		{
			if (!plan.instances.instances.empty())
			{
				throw std::runtime_error(
					"drawable occurrence metadata has no active scene");
			}
			return;
		}

		const editor::StableSceneId activeScene = *plan.header.activeScene;
		for (size_t index = 0u; index < plan.instances.instances.size(); ++index)
		{
			SceneDrawInstance& instance = plan.instances.instances[index];
			instance.sceneSourceId = activeScene;
			instance.instanceOrdinal = 0u;
			for (size_t previous = 0u; previous < index; ++previous)
			{
				const SceneDrawInstance& prior = plan.instances.instances[previous];
				if (prior.sceneSourceId == instance.sceneSourceId &&
					prior.nodeSourceId == instance.nodeSourceId &&
					prior.sourceId == instance.sourceId)
				{
					++instance.instanceOrdinal;
				}
			}
			if (!instance.sourceIdentity().valid())
			{
				throw std::runtime_error(
					"drawable occurrence identity is invalid");
			}
		}

		for (MeshUploadPlan& mesh : plan.meshes)
		{
			const auto instance = std::find_if(
				plan.instances.instances.begin(), plan.instances.instances.end(),
				[&](const SceneDrawInstance& candidate)
				{
					return candidate.primitiveIndex == mesh.primitiveIndex &&
					       candidate.meshIndex == mesh.meshIndex;
				});
			if (instance == plan.instances.instances.end())
			{
				throw std::runtime_error(
					"mesh upload has no drawable occurrence identity");
			}
			mesh.nodeSourceId = instance->nodeSourceId;
			mesh.sceneSourceId = instance->sceneSourceId;
			mesh.instanceOrdinal = instance->instanceOrdinal;
		}
		for (InstanceCullRecord& cull : plan.cullRecords)
		{
			if (cull.instanceIndex >= plan.instances.instances.size())
			{
				throw std::runtime_error(
					"cull record occurrence index is out of bounds");
			}
			const SceneDrawInstance& instance =
				plan.instances.instances[cull.instanceIndex];
			cull.sourceId = instance.sourceId;
			cull.nodeSourceId = instance.nodeSourceId;
			cull.sceneSourceId = instance.sceneSourceId;
			cull.instanceOrdinal = instance.instanceOrdinal;
		}
		for (DrawCommandBuildPlan& draw : plan.drawCommands)
		{
			if (draw.instanceIndex >= plan.instances.instances.size())
			{
				throw std::runtime_error(
					"draw command occurrence index is out of bounds");
			}
			const SceneDrawInstance& instance =
				plan.instances.instances[draw.instanceIndex];
			draw.sourceId = instance.sourceId;
			draw.nodeSourceId = instance.nodeSourceId;
			draw.sceneSourceId = instance.sceneSourceId;
			draw.instanceOrdinal = instance.instanceOrdinal;
		}

		std::vector<bool> usedMeshes(plan.meshes.size(), false);
		for (SceneRequiredResource& resource : plan.requiredResources)
		{
			if (resource.kind != SceneRequiredResourceKind::Mesh)
			{
				resource.nodeSourceId = {};
				resource.sceneSourceId = {};
				resource.instanceOrdinal = 0u;
				continue;
			}
			size_t match = plan.meshes.size();
			for (size_t index = 0u; index < plan.meshes.size(); ++index)
			{
				const MeshUploadPlan& mesh = plan.meshes[index];
				if (!usedMeshes[index] && mesh.sourceId == resource.sourceId &&
					(resource.nodeSourceId == editor::StableSceneId{} ||
					 resource.nodeSourceId == mesh.nodeSourceId))
				{
					match = index;
					break;
				}
			}
			if (match == plan.meshes.size())
			{
				throw std::runtime_error(
					"required mesh resource has no drawable occurrence");
			}
			usedMeshes[match] = true;
			const MeshUploadPlan& mesh = plan.meshes[match];
			resource.nodeSourceId = mesh.nodeSourceId;
			resource.sceneSourceId = mesh.sceneSourceId;
			resource.instanceOrdinal = mesh.instanceOrdinal;
		}
		const SceneUploadPlanValidationResult validation =
			SceneUploadPlanner::validate(
				makeSceneAssetView(generation.asset), generation.plan);
		if (!validation.valid)
		{
			throw std::runtime_error(validation.error.empty()
				? "normalized occurrence upload plan failed validation"
				: validation.error);
		}
	}

	void GPUDrivenSceneTransactionBridge::sealOccurrenceSnapshot(
		ConcreteGeneration& generation) const
	{
		auto& state = *generation.rendererState;
		generation.occurrenceSnapshot.reset();
		state.occurrenceSnapshot.reset();
		for (SceneUploadResult::SceneDrawRecord& record : state.upload.drawRecords)
		{
			record.occurrence = {};
			record.pickToken = {};
		}

		const editor::RendererTargetFence& rendererFence =
			generation.mapping.fence();
		if (!rendererFence.activeScene)
		{
			if (!generation.plan.instances.instances.empty())
			{
				throw std::runtime_error(
					"active renderer scene is absent while draw occurrences exist");
			}
			return;
		}
		const std::optional<editor::SceneId> scene = editor::SceneId::from(
			editor::editorObjectId(*rendererFence.activeScene));
		if (!scene || rendererFence.sceneActivationEpoch ==
			std::numeric_limits<uint64_t>::max())
		{
			throw std::runtime_error(
				"renderer scene fence cannot identify a pickable active scene");
		}
		const uint64_t pickingSceneEpoch =
			rendererFence.sceneActivationEpoch + 1u;
		const uint32_t idGeneration = rendererFence.renderer.generation;

		std::vector<editor::SceneRenderNodeRecord> nodes;
		nodes.reserve(generation.asset.nodes.size());
		std::vector<uint8_t> visited(generation.asset.nodes.size(), 0u);
		struct PendingNode final
		{
			uint32_t index{0u};
			uint32_t depth{0u};
			std::optional<editor::NodeId> parent;
		};
		std::vector<PendingNode> pending;
		pending.reserve(generation.asset.nodes.size());
		for (auto root = generation.asset.rootNodes.rbegin();
		     root != generation.asset.rootNodes.rend(); ++root)
		{
			pending.push_back({*root, 0u, std::nullopt});
		}
		while (!pending.empty())
		{
			const PendingNode current = pending.back();
			pending.pop_back();
			if (current.index >= generation.asset.nodes.size() ||
				visited[current.index] != 0u)
			{
				throw std::runtime_error(
					"renderer occurrence node topology is not a canonical tree");
			}
			const editor::RendererSceneMappingEntry* entry =
				mappingEntryForProjection(
					generation.mapping, editor::RendererObjectKind::SceneObject,
					editor::EditorObjectKind::node, current.index);
			const std::optional<editor::NodeId> node = entry != nullptr
				? editor::NodeId::from(editor::editorObjectId(entry->stableId))
				: std::nullopt;
			if (!node)
			{
				throw std::runtime_error(
					"renderer occurrence node has no stable identity");
			}
			const SceneNode& assetNode = generation.asset.nodes[current.index];
			if ((current.depth == 0u && assetNode.parent >= 0) ||
				(current.depth != 0u &&
				 (assetNode.parent < 0 || !current.parent)))
			{
				throw std::runtime_error(
					"renderer occurrence node parent is not canonical");
			}
			if (assetNode.parent >= 0)
			{
				const uint32_t parentIndex =
					static_cast<uint32_t>(assetNode.parent);
				const editor::RendererSceneMappingEntry* parentEntry =
					mappingEntryForProjection(
						generation.mapping,
						editor::RendererObjectKind::SceneObject,
						editor::EditorObjectKind::node, parentIndex);
				const std::optional<editor::NodeId> declaredParent =
					parentEntry != nullptr
						? editor::NodeId::from(
							editor::editorObjectId(parentEntry->stableId))
						: std::nullopt;
				if (!declaredParent || !current.parent ||
					*declaredParent != *current.parent)
				{
					throw std::runtime_error(
						"renderer occurrence node parent identity is inconsistent");
				}
			}
			visited[current.index] = 1u;
			nodes.push_back({*node, current.parent, current.depth});
			if (current.depth == std::numeric_limits<uint32_t>::max())
			{
				throw std::runtime_error(
					"renderer occurrence node depth capacity is exhausted");
			}
			for (auto child = assetNode.children.rbegin();
			     child != assetNode.children.rend(); ++child)
			{
				pending.push_back({*child, current.depth + 1u, *node});
			}
		}
		if (std::any_of(visited.begin(), visited.end(),
			[](uint8_t value) { return value == 0u; }))
		{
			throw std::runtime_error(
				"renderer occurrence topology omits a projected node");
		}

		if (generation.plan.instances.instances.size() >=
			static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		{
			throw std::runtime_error(
				"renderer occurrence slot capacity is exhausted");
		}
		std::vector<editor::RenderOccurrenceRecord> records;
		records.reserve(generation.plan.instances.instances.size());
		for (size_t index = 0u;
		     index < generation.plan.instances.instances.size(); ++index)
		{
			const SceneDrawInstance& instance =
				generation.plan.instances.instances[index];
			if (instance.nodeIndex >= visited.size() ||
				visited[instance.nodeIndex] == 0u ||
				instance.primitiveIndex >= generation.asset.primitives.size())
			{
				throw std::runtime_error(
					"drawable occurrence is outside the active scene topology");
			}
			const std::optional<editor::SceneId> instanceScene =
				editor::SceneId::from(editor::editorObjectId(instance.sceneSourceId));
			const std::optional<editor::NodeId> node =
				editor::NodeId::from(editor::editorObjectId(instance.nodeSourceId));
			const std::optional<editor::PrimitiveId> primitive =
				editor::PrimitiveId::from(editor::editorObjectId(instance.sourceId));
			if (!instanceScene || !node || !primitive || *instanceScene != *scene)
			{
				throw std::runtime_error(
					"drawable occurrence stable identity is invalid");
			}
			const ScenePrimitive& assetPrimitive =
				generation.asset.primitives[instance.primitiveIndex];
			const SceneNode& assetNode = generation.asset.nodes[instance.nodeIndex];
			records.push_back({
				.id = {static_cast<uint32_t>(index + 1u), idGeneration},
				.key = {*instanceScene, *node, *primitive,
				        instance.instanceOrdinal},
				.visible = assetPrimitive.effectiveVisible &&
				           assetNode.effectiveVisible,
			});
		}

		const editor::EditorSceneFence occurrenceFence{
			.authority = rendererFence.authority,
			.documentRevision = rendererFence.appliedDocumentRevision,
			.projectionRevision = rendererFence.mappingEpoch,
			.scene = *scene,
			.sceneEpoch = pickingSceneEpoch,
			.deviceGeneration = rendererFence.device.generation,
		};
		editor::SceneRenderOccurrenceMappingCreateResult created =
			editor::SceneRenderOccurrenceMapping::create(
				occurrenceFence, rendererFence.mappingEpoch,
				std::move(nodes), std::move(records));
		if (!created.succeeded())
		{
			throw std::runtime_error(created.message.empty()
				? "renderer occurrence snapshot failed validation"
				: std::string{created.message});
		}
		generation.occurrenceSnapshot = std::move(created.snapshot);
		state.occurrenceSnapshot = generation.occurrenceSnapshot;

		for (size_t commandIndex = 0u;
		     commandIndex < generation.plan.drawCommands.size(); ++commandIndex)
		{
			const DrawCommandBuildPlan& draw =
				generation.plan.drawCommands[commandIndex];
			if (commandIndex >= state.upload.drawCommandToDrawRecord.size())
			{
				throw std::runtime_error(
					"draw occurrence sidecar index is out of bounds");
			}
			const uint32_t drawRecordIndex =
				state.upload.drawCommandToDrawRecord[commandIndex];
			if (drawRecordIndex >= state.upload.drawRecords.size())
			{
				throw std::runtime_error(
					"draw occurrence has no renderer draw record");
			}
			const auto drawScene = editor::SceneId::from(
				editor::editorObjectId(draw.sceneSourceId));
			const auto drawNode = editor::NodeId::from(
				editor::editorObjectId(draw.nodeSourceId));
			const auto drawPrimitive = editor::PrimitiveId::from(
				editor::editorObjectId(draw.sourceId));
			if (!drawScene || !drawNode || !drawPrimitive)
			{
				throw std::runtime_error(
					"draw occurrence sidecar identity is invalid");
			}
			const editor::PrimitiveOccurrenceKey key{
				*drawScene, *drawNode, *drawPrimitive, draw.instanceOrdinal};
			const std::optional<editor::RenderOccurrenceId> occurrence =
				generation.occurrenceSnapshot->occurrence(key);
			if (!occurrence)
			{
				throw std::runtime_error(
					"draw occurrence is absent from the sealed snapshot");
			}
			const std::optional<editor::FramebufferPickToken> token =
				generation.occurrenceSnapshot->framebufferToken(*occurrence);
			if (!token)
			{
				throw std::runtime_error(
					"draw occurrence has no framebuffer token");
			}
			SceneUploadResult::SceneDrawRecord& record =
				state.upload.drawRecords[drawRecordIndex];
			record.occurrence = *occurrence;
			record.pickToken = *token;
		}
	}

	void GPUDrivenSceneTransactionBridge::applyCpuChanges(
		ConcreteGeneration& generation,
		const editor::SceneChangeSet& changes) const
	{
		for (const editor::SceneFieldChange& change : changes.canonicalChanges)
		{
			if (!change.afterValue)
			{
				continue;
			}
			const editor::RendererSceneMappingEntry* entry =
				generation.mapping.find(change.target);
			if (entry == nullptr)
			{
				throw std::runtime_error("candidate change target has no mapping entry");
			}
			const uint32_t index = entry->projection.sceneAssetIndex;
			if (const auto* transform =
					std::get_if<editor::SceneNodeTransformValue>(&*change.afterValue))
			{
				const glm::mat4 matrix = transformMatrix(*transform);
				if (change.field == editor::SceneField::SceneRootTransform)
				{
					generation.asset.sceneRootTransform.matrix = matrix;
				}
				else if (index < generation.asset.nodes.size())
				{
					SceneNode& node = generation.asset.nodes[index];
					if (const auto* trs = std::get_if<editor::SceneTrsValue>(&transform->value))
					{
						node.translation = glm::vec3(
							static_cast<float>(trs->translation[0]),
							static_cast<float>(trs->translation[1]),
							static_cast<float>(trs->translation[2]));
						node.rotation = glm::quat(
							static_cast<float>(trs->rotation[3]),
							static_cast<float>(trs->rotation[0]),
							static_cast<float>(trs->rotation[1]),
							static_cast<float>(trs->rotation[2]));
						node.scale = glm::vec3(
							static_cast<float>(trs->scale[0]),
							static_cast<float>(trs->scale[1]),
							static_cast<float>(trs->scale[2]));
					}
					node.localTransform = matrix;
				}
				std::vector<glm::mat4> worldTransforms =
					TransformSystem::buildWorldTransforms(makeSceneAssetView(generation.asset));
				const auto applyRootTransform =
					[&](const auto& self, uint32_t nodeIndex,
					    const glm::mat4& rootTransform) -> void
					{
						if (nodeIndex >= generation.asset.nodes.size() ||
							nodeIndex >= worldTransforms.size()) return;
						worldTransforms[nodeIndex] =
							rootTransform * worldTransforms[nodeIndex];
						for (uint32_t child : generation.asset.nodes[nodeIndex].children)
						{
							self(self, child, rootTransform);
						}
					};
				for (size_t rootIndex = 0u;
				     rootIndex < generation.asset.rootNodes.size(); ++rootIndex)
				{
					const bool applies =
						rootIndex >= generation.asset.sceneRootTransformApplyToRoot.size() ||
						generation.asset.sceneRootTransformApplyToRoot[rootIndex] != 0u;
					if (applies)
					{
						applyRootTransform(applyRootTransform,
						                   generation.asset.rootNodes[rootIndex],
						                   generation.asset.sceneRootTransform.matrix);
					}
				}
				generation.plan.transforms.worldTransforms = worldTransforms;
				for (uint32_t nodeIndex = 0u;
				     nodeIndex < generation.asset.nodes.size() &&
				     nodeIndex < worldTransforms.size(); ++nodeIndex)
				{
					generation.asset.nodes[nodeIndex].worldTransform =
						worldTransforms[nodeIndex];
				}
				for (SceneDrawInstance& instance : generation.plan.instances.instances)
				{
					if (instance.nodeIndex < worldTransforms.size())
					{
						instance.worldTransform = worldTransforms[instance.nodeIndex];
					}
				}
				generation.plan.cullRecords = TransformSystem::buildCullRecords(
					makeSceneAssetView(generation.asset), generation.plan.instances);
				for (InstanceCullRecord& cull : generation.plan.cullRecords)
				{
					if (cull.instanceIndex < generation.plan.instances.instances.size())
					{
						const SceneDrawInstance& instance =
							generation.plan.instances.instances[cull.instanceIndex];
						cull.sourceId = instance.sourceId;
						cull.nodeSourceId = instance.nodeSourceId;
					}
				}
			}
			else if (const auto* visibility =
					std::get_if<editor::SceneVisibilityValue>(&*change.afterValue))
			{
				if (entry->projection.kind == editor::EditorObjectKind::node &&
					index < generation.asset.nodes.size())
				{
					generation.asset.nodes[index].localVisible = visibility->visible;
					std::vector<uint8_t> resolved(generation.asset.nodes.size(), 0u);
					const auto resolveVisibility = [&](const auto& self, uint32_t nodeIndex) -> bool
					{
						if (nodeIndex >= generation.asset.nodes.size()) return false;
						SceneNode& node = generation.asset.nodes[nodeIndex];
						if (resolved[nodeIndex] != 0u) return node.effectiveVisible;
						const bool parentVisible = node.parent < 0
							? true : self(self, static_cast<uint32_t>(node.parent));
						node.effectiveVisible = parentVisible && node.localVisible;
						resolved[nodeIndex] = 1u;
						return node.effectiveVisible;
					};
					for (uint32_t nodeIndex = 0u;
					     nodeIndex < generation.asset.nodes.size(); ++nodeIndex)
					{
						(void)resolveVisibility(resolveVisibility, nodeIndex);
					}
					for (SceneMesh& mesh : generation.asset.meshes)
					{
						mesh.effectiveVisible = false;
					}
					for (const SceneNode& node : generation.asset.nodes)
					{
						for (uint32_t meshIndex : node.meshRefs)
						{
							if (meshIndex < generation.asset.meshes.size())
							{
								generation.asset.meshes[meshIndex].effectiveVisible =
									generation.asset.meshes[meshIndex].effectiveVisible ||
									node.effectiveVisible;
							}
						}
					}
					for (ScenePrimitive& primitive : generation.asset.primitives)
					{
						primitive.effectiveVisible =
							primitive.localVisible &&
							primitive.nodeIndex < generation.asset.nodes.size() &&
							generation.asset.nodes[primitive.nodeIndex].effectiveVisible;
					}
				}
			}
			else if (const auto* parameters =
					std::get_if<editor::SceneMaterialParametersValue>(&*change.afterValue))
			{
				if (index >= generation.asset.materials.size())
				{
					throw std::runtime_error("material candidate index is out of bounds");
				}
				SceneMaterial& material = generation.asset.materials[index];
				material.baseColorFactor = glm::vec4(
					static_cast<float>(parameters->baseColorFactor[0]),
					static_cast<float>(parameters->baseColorFactor[1]),
					static_cast<float>(parameters->baseColorFactor[2]),
					static_cast<float>(parameters->baseColorFactor[3]));
				material.metallicFactor = static_cast<float>(parameters->metallicFactor);
				material.roughnessFactor = static_cast<float>(parameters->roughnessFactor);
				material.emissiveFactor = glm::vec3(
					static_cast<float>(parameters->emissiveFactor[0]),
					static_cast<float>(parameters->emissiveFactor[1]),
					static_cast<float>(parameters->emissiveFactor[2]));
				material.alphaCutoff = static_cast<float>(parameters->alphaCutoff);
				material.alphaMode = static_cast<int32_t>(parameters->alphaMode);
				material.doubleSided = parameters->doubleSided;
				for (MaterialCreatePlan& candidate : generation.plan.materials)
				{
					if (candidate.materialIndex != index) continue;
					candidate.baseColorFactor = material.baseColorFactor;
					candidate.metallicFactor = material.metallicFactor;
					candidate.roughnessFactor = material.roughnessFactor;
					candidate.emissiveFactor = material.emissiveFactor;
					candidate.alphaCutoff = material.alphaCutoff;
					candidate.alphaMode = material.alphaMode;
					candidate.doubleSided = material.doubleSided;
				}
				for (DrawCommandBuildPlan& draw : generation.plan.drawCommands)
				{
					if (draw.materialIndex != index) continue;
					draw.bucket = material.alphaMode == shaderio::LAlphaBlend
						? SceneDrawBucket::Transparent
						: (material.alphaMode == shaderio::LAlphaMask
							? SceneDrawBucket::AlphaMask : SceneDrawBucket::Opaque);
				}
				if (generation.deviceRecords != nullptr)
				{
					const MaterialHandle handle{
						entry->rendererHandle.index, entry->rendererHandle.generation};
					for (auto& candidate : generation.deviceRecords->materials)
					{
						if (candidate.handle == handle)
						{
							candidate.record.baseColorFactor = material.baseColorFactor;
							candidate.record.metallicFactor = material.metallicFactor;
							candidate.record.roughnessFactor = material.roughnessFactor;
							candidate.record.emissiveFactor = material.emissiveFactor;
							candidate.record.alphaCutoff = material.alphaCutoff;
							candidate.record.alphaMode = material.alphaMode;
							candidate.record.doubleSided = material.doubleSided;
						}
					}
				}
			}
			else if (const auto* binding =
					std::get_if<editor::SceneTextureBindingValue>(&*change.afterValue))
			{
				if (index >= generation.asset.materials.size())
				{
					throw std::runtime_error("texture-binding material index is out of bounds");
				}
				int32_t textureIndex = -1;
				TextureHandle textureHandle{};
				if (binding->texture)
				{
					const auto* textureEntry = generation.mapping.find(*binding->texture);
					if (textureEntry == nullptr ||
						textureEntry->rendererHandle.kind != editor::RendererObjectKind::Texture ||
						textureEntry->projection.kind != editor::EditorObjectKind::texture)
					{
						throw std::runtime_error("texture binding references no live texture handle");
					}
					const uint32_t selectedCoordinate =
						textureEntry->projection.sceneAssetIndexFor(binding->semantic);
					if (selectedCoordinate >= generation.asset.textures.size() ||
						selectedCoordinate >
							static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
					{
						throw std::runtime_error(
							"semantic texture coordinate is out of bounds");
					}
					textureIndex = static_cast<int32_t>(selectedCoordinate);
					const editor::RendererSceneMappingEntry* imageEntry =
						mappingEntryForProjection(
							generation.mapping, editor::RendererObjectKind::Texture,
							editor::EditorObjectKind::image,
							selectedCoordinate);
					const editor::RendererObjectHandle& selectedHandle =
						imageEntry != nullptr ? imageEntry->rendererHandle
						                      : textureEntry->rendererHandle;
					textureHandle = {selectedHandle.index, selectedHandle.generation};
				}
				SceneMaterial& material = generation.asset.materials[index];
				MaterialCreatePlan* materialPlan = nullptr;
				for (MaterialCreatePlan& candidate : generation.plan.materials)
				{
					if (candidate.materialIndex == index)
					{
						materialPlan = &candidate;
						break;
					}
				}
				DescriptorUpdatePlan* descriptorPlan = nullptr;
				for (DescriptorUpdatePlan& candidate : generation.plan.descriptorUpdates)
				{
					if (candidate.materialIndex == index)
					{
						descriptorPlan = &candidate;
						break;
					}
				}
				for (auto& candidate : generation.deviceRecords->materials)
				{
					if (candidate.handle.index != entry->rendererHandle.index ||
						candidate.handle.generation != entry->rendererHandle.generation)
					{
						continue;
					}
					switch (binding->semantic)
					{
					case editor::SceneTextureSemantic::BaseColor:
						material.baseColorTexture = textureIndex;
						if (materialPlan != nullptr) materialPlan->baseColorTexture = textureIndex;
						if (descriptorPlan != nullptr) descriptorPlan->textureIndices[0] = textureIndex;
						candidate.record.baseColorTexture = textureHandle;
						candidate.record.sampledTexture = textureHandle;
						break;
					case editor::SceneTextureSemantic::MetallicRoughness:
						material.metallicRoughnessTexture = textureIndex;
						if (materialPlan != nullptr) materialPlan->metallicRoughnessTexture = textureIndex;
						if (descriptorPlan != nullptr) descriptorPlan->textureIndices[2] = textureIndex;
						candidate.record.metallicRoughnessTexture = textureHandle;
						break;
					case editor::SceneTextureSemantic::Normal:
						material.normalTexture = textureIndex;
						if (materialPlan != nullptr) materialPlan->normalTexture = textureIndex;
						if (descriptorPlan != nullptr) descriptorPlan->textureIndices[1] = textureIndex;
						candidate.record.normalTexture = textureHandle;
						break;
					case editor::SceneTextureSemantic::Occlusion:
						material.occlusionTexture = textureIndex;
						if (materialPlan != nullptr) materialPlan->occlusionTexture = textureIndex;
						if (descriptorPlan != nullptr) descriptorPlan->textureIndices[3] = textureIndex;
						candidate.record.occlusionTexture = textureHandle;
						break;
					case editor::SceneTextureSemantic::Emissive:
						material.emissiveTexture = textureIndex;
						if (materialPlan != nullptr) materialPlan->emissiveTexture = textureIndex;
						if (descriptorPlan != nullptr) descriptorPlan->textureIndices[4] = textureIndex;
						candidate.record.emissiveTexture = textureHandle;
						break;
					}
				}
			}
			else if (const auto* binding =
					std::get_if<editor::ScenePrimitiveMaterialBindingValue>(&*change.afterValue))
			{
				if (index >= generation.asset.primitives.size())
				{
					throw std::runtime_error("primitive binding index is out of bounds");
				}
				int32_t materialIndex = -1;
				if (binding->material)
				{
					const auto* materialEntry = generation.mapping.find(*binding->material);
					if (materialEntry == nullptr ||
						materialEntry->rendererHandle.kind != editor::RendererObjectKind::Material)
					{
						throw std::runtime_error("primitive binding references no live material handle");
					}
					materialIndex = static_cast<int32_t>(materialEntry->projection.sceneAssetIndex);
				}
				bool updatedOccurrence = false;
				for (SceneDrawInstance& instance : generation.plan.instances.instances)
				{
					if (instance.sourceId != change.target)
					{
						continue;
					}
					if (instance.primitiveIndex >= generation.asset.primitives.size())
					{
						throw std::runtime_error(
							"primitive binding occurrence index is out of bounds");
					}
					updatedOccurrence = true;
					generation.asset.primitives[instance.primitiveIndex].materialIndex =
						materialIndex;
					instance.materialIndex = materialIndex >= 0
						? static_cast<uint32_t>(materialIndex) : UINT32_MAX;
					for (DrawCommandBuildPlan& draw : generation.plan.drawCommands)
					{
						if (draw.instanceIndex != instance.instanceIndex) continue;
						draw.materialIndex = instance.materialIndex;
						if (materialIndex >= 0 &&
							static_cast<size_t>(materialIndex) < generation.asset.materials.size())
						{
							const int32_t alphaMode = generation.asset.materials[
								static_cast<size_t>(materialIndex)].alphaMode;
							draw.bucket = alphaMode == shaderio::LAlphaBlend
								? SceneDrawBucket::Transparent
								: (alphaMode == shaderio::LAlphaMask
									? SceneDrawBucket::AlphaMask : SceneDrawBucket::Opaque);
						}
					}
				}
				if (!updatedOccurrence)
				{
					generation.asset.primitives[index].materialIndex = materialIndex;
				}
			}
			else if (const auto* camera =
					std::get_if<editor::SceneCameraValue>(&*change.afterValue))
			{
				if (index >= generation.asset.cameras.size())
				{
					throw std::runtime_error("camera candidate index is out of bounds");
				}
				SceneCamera& runtime = generation.asset.cameras[index];
				runtime.type = camera->projection == editor::SceneCameraProjection::Perspective
					? demo::SceneCameraType::perspective
					: demo::SceneCameraType::orthographic;
				runtime.verticalFieldOfViewRadians = static_cast<float>(camera->yfov);
				runtime.nearPlane = static_cast<float>(camera->znear);
				runtime.farPlane = static_cast<float>(camera->zfar);
				runtime.aspectRatio = static_cast<float>(camera->aspectRatio);
				runtime.horizontalMagnification = static_cast<float>(camera->xmag);
				runtime.verticalMagnification = static_cast<float>(camera->ymag);
			}
			else if (const auto* light =
					std::get_if<editor::SceneLightValue>(&*change.afterValue))
			{
				if (index >= generation.asset.lights.size())
				{
					throw std::runtime_error("light candidate index is out of bounds");
				}
				SceneLight& runtime = generation.asset.lights[index];
				switch (light->type)
				{
				case editor::SceneLightType::Directional:
					runtime.type = demo::SceneLightType::directional;
					break;
				case editor::SceneLightType::Point:
					runtime.type = demo::SceneLightType::point;
					break;
				case editor::SceneLightType::Spot:
					runtime.type = demo::SceneLightType::spot;
					break;
				}
				runtime.color = glm::vec3(
					static_cast<float>(light->color[0]),
					static_cast<float>(light->color[1]),
					static_cast<float>(light->color[2]));
				runtime.intensity = static_cast<float>(light->intensity);
				runtime.range = static_cast<float>(light->range);
				runtime.innerConeAngle = static_cast<float>(light->innerConeAngle);
				runtime.outerConeAngle = static_cast<float>(light->outerConeAngle);
			}
			else if (const auto* attachment =
					std::get_if<editor::SceneObjectAttachmentValue>(&*change.afterValue))
			{
				const bool cameraAttachment =
					change.field == editor::SceneField::CameraAttachment;
				const bool lightAttachment =
					change.field == editor::SceneField::LightAttachment;
				if (!cameraAttachment && !lightAttachment)
				{
					continue;
				}

				int32_t nodeIndex = -1;
				if (attachment->node)
				{
					const editor::RendererSceneMappingEntry* nodeEntry =
						generation.mapping.find(*attachment->node);
					if (nodeEntry == nullptr ||
						nodeEntry->projection.kind != editor::EditorObjectKind::node ||
						nodeEntry->projection.sceneAssetIndex >= generation.asset.nodes.size() ||
						nodeEntry->projection.sceneAssetIndex >
							static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
					{
						throw std::runtime_error(
							"attachment candidate node has no valid projection coordinate");
					}
					nodeIndex = static_cast<int32_t>(
						nodeEntry->projection.sceneAssetIndex);
				}

				if (cameraAttachment)
				{
					if (index >= generation.asset.cameras.size())
					{
						throw std::runtime_error("camera candidate index is out of bounds");
					}
					generation.asset.cameras[index].nodeIndex = nodeIndex;
				}
				else
				{
					if (index >= generation.asset.lights.size())
					{
						throw std::runtime_error("light candidate index is out of bounds");
					}
					generation.asset.lights[index].nodeIndex = nodeIndex;
				}
			}
		}
		rebuildRendererState(generation);
	}

	rhi::RHIResult
	GPUDrivenSceneTransactionBridge::prepareRendererStateForPublication() noexcept
	{
		if (m_candidateConcrete == nullptr)
		{
			return rhi::RHIResult::fail(
				rhi::RHIErrorCode::invalidState,
				"scene candidate has no private renderer state");
		}
		try
		{
			rebuildRendererState(*m_candidateConcrete);
			auto& state = *m_candidateConcrete->rendererState;
			state.generation = m_candidateConcrete->generation;
			state.descriptorGeneration =
				m_candidateConcrete->descriptorGeneration;
			state.objectBufferAddress = {};
			state.cullBufferAddress = {};
			state.objectBuffer = {};
			state.cullBuffer = {};
			state.registry = nullptr;
			state.overlayObjects.clear();
			state.registryUpdates.clear();
			if (m_candidateConcrete->resources != nullptr &&
				m_candidateConcrete->resources->registry != nullptr)
			{
				GPUSceneRegistry& registry =
					*m_candidateConcrete->resources->registry;
				state.registry = &registry;
				for (const ConcreteObjectRecord& object :
				     m_candidateConcrete->objects)
				{
					if (!registry.isLiveObject({
						object.handle.index, object.handle.generation}))
					{
						return rhi::RHIResult::fail(
							rhi::RHIErrorCode::invalidHandle,
							"candidate renderer state references a stale registry handle");
					}
				}
				state.objectBufferAddress = registry.getBufferAddress();
				state.cullBufferAddress = registry.getCullBufferAddress();
				state.objectBuffer = registry.getBufferHandle();
				state.cullBuffer = registry.getCullBufferHandle();
			}
			state.overlayObjects.reserve(m_candidateConcrete->objects.size());
			if (!m_pendingFullSnapshot)
			{
				state.registryUpdates.reserve(m_candidateConcrete->objects.size());
			}
			for (const ConcreteObjectRecord& object :
			     m_candidateConcrete->objects)
			{
				state.overlayObjects.push_back(shaderio::GPUCullObject{
					.sphereCenterRadius = object.registration.boundsSphere,
					.indexCount = object.registration.visible
						? object.registration.indexCount : 0u,
					.firstIndex = object.registration.firstIndex,
					.vertexOffset = object.registration.vertexOffset,
					.flags = object.registration.flags,
				});
				if (!m_pendingFullSnapshot)
				{
					state.registryUpdates.push_back({
						.handle = {object.handle.index, object.handle.generation},
						.registration = object.registration,
					});
				}
			}
			return rhi::RHIResult::ok();
		}
		catch (const std::bad_alloc&)
		{
			return rhi::RHIResult::fail(
				rhi::RHIErrorCode::outOfMemory,
				"private renderer-state publication allocation failed");
		}
		catch (const std::exception& exception)
		{
			return rhi::RHIResult::fail(
				rhi::RHIErrorCode::invalidState, exception.what());
		}
	}

	rhi::RHIResult GPUDrivenSceneTransactionBridge::allocateCandidateResources(
		std::vector<editor::SceneJournalResource>& createdResources) noexcept
	{
		createdResources.clear();
		if (!m_pendingFullSnapshot || m_candidateConcrete == nullptr ||
			!m_owner.m_renderer.sceneTransactionDeviceInitialized())
		{
			return rhi::RHIResult::ok();
		}

		try
		{
			const TransactionTexturePlanValidation textureValidation =
				validateTransactionTextureUploadPlans(
					m_candidateConcrete->asset,
					m_candidateConcrete->plan,
					&m_owner.m_renderer.getRHIDevice());
			if (!textureValidation.valid())
			{
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::invalidArgument,
					"private texture upload footprint changed after preflight");
			}
			auto resources = std::make_shared<ResourceSet>();
			resources->device = &m_owner.m_renderer.getRHIDevice();
			resources->deviceRecords = m_candidateConcrete->deviceRecords;
			resources->deviceRecords->frameUse =
				std::make_shared<RenderDevice::SceneTransactionResources::FrameUseState>();
			if (rhi::Queue* graphics = resources->device->getQueue(
					rhi::QueueClass::graphics))
			{
				resources->deviceRecords->frameUse->queue = graphics->identity();
			}
			m_candidateConcrete->resources = resources;
			const auto publishOwned = [&]()
			{
				createdResources.clear();
				createdResources.reserve(resources->owned.size());
				for (const OwnedResource& owned : resources->owned)
				{
					createdResources.push_back(owned.journal);
					m_resourceOwners[owned.journal.diagnosticId.value] = resources;
				}
			};

			if (consumeFailure(SceneTransactionFailurePoint::resourceAllocation))
			{
				const rhi::BufferHandle probe = resources->device->createBuffer(rhi::BufferDesc{
					.size = 16u,
					.usage = rhi::BufferUsageFlags::transferSrc,
					.memoryUsage = rhi::MemoryUsage::cpuToGpu,
					.debugName = "SceneTransaction.injectedAllocationProbe",
				});
				appendOwnedResource(*resources, resourceHandle(probe));
				publishOwned();
				return rhi::RHIResult::fail(
					rhi::RHIErrorCode::outOfMemory,
					"injected private scene allocation failure after one resource");
			}

			for (size_t meshIndex = 0u;
			     meshIndex < m_candidateConcrete->plan.meshes.size(); ++meshIndex)
			{
				const MeshUploadPlan& mesh = m_candidateConcrete->plan.meshes[meshIndex];
				if (mesh.vertexByteSize == 0u || mesh.indexByteSize == 0u)
				{
					continue;
				}
				const rhi::BufferHandle vertex = resources->device->createBuffer(rhi::BufferDesc{
					.size = mesh.vertexByteSize,
					.usage = rhi::BufferUsageFlags::vertex |
					         rhi::BufferUsageFlags::transferDst |
					         rhi::BufferUsageFlags::shaderDeviceAddress,
					.memoryUsage = rhi::MemoryUsage::gpuOnly,
					.allowGpuAddress = true,
					.debugName = "SceneTransaction.meshVertex",
				});
				appendOwnedResource(*resources, resourceHandle(vertex));
				const rhi::BufferHandle index = resources->device->createBuffer(rhi::BufferDesc{
					.size = mesh.indexByteSize,
					.usage = rhi::BufferUsageFlags::index |
					         rhi::BufferUsageFlags::transferDst,
					.memoryUsage = rhi::MemoryUsage::gpuOnly,
					.debugName = "SceneTransaction.meshIndex",
				});
				appendOwnedResource(*resources, resourceHandle(index));

				const uint64_t stagingBytes = mesh.vertexByteSize + mesh.indexByteSize;
				const rhi::BufferHandle staging = resources->device->createBuffer(rhi::BufferDesc{
					.size = stagingBytes,
					.usage = rhi::BufferUsageFlags::transferSrc,
					.memoryUsage = rhi::MemoryUsage::cpuToGpu,
					.debugName = "SceneTransaction.meshStaging",
				});
				appendOwnedResource(*resources, resourceHandle(staging));
				void* mapped = resources->device->mapBuffer(staging);
				if (mapped == nullptr)
				{
					publishOwned();
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::outOfMemory,
						"private mesh staging buffer could not be mapped");
				}
				std::memcpy(mapped,
				            m_candidateConcrete->asset.vertexPayload.data() + mesh.vertexSrcOffset,
				            static_cast<size_t>(mesh.vertexByteSize));
				std::memcpy(static_cast<std::byte*>(mapped) + mesh.vertexByteSize,
				            m_candidateConcrete->asset.indexPayload.data() + mesh.indexSrcOffset,
				            static_cast<size_t>(mesh.indexByteSize));
				resources->device->unmapBuffer(staging);
				resources->bufferCopies.push_back({staging, 0u, vertex, 0u, mesh.vertexByteSize});
				resources->bufferCopies.push_back(
					{staging, mesh.vertexByteSize, index, 0u, mesh.indexByteSize});
				resources->bufferBeginBarriers.push_back({
					.buffer = vertex,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.size = mesh.vertexByteSize,
				});
				resources->bufferBeginBarriers.push_back({
					.buffer = index,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.size = mesh.indexByteSize,
				});
				resources->bufferEndBarriers.push_back({
					.buffer = vertex,
					.before = rhi::ResourceState::TransferDst,
					.after = rhi::ResourceState::General,
					.size = mesh.vertexByteSize,
				});
				resources->bufferEndBarriers.push_back({
					.buffer = index,
					.before = rhi::ResourceState::TransferDst,
					.after = rhi::ResourceState::General,
					.size = mesh.indexByteSize,
				});
				if (meshIndex < resources->deviceRecords->meshes.size())
				{
					auto& record = resources->deviceRecords->meshes[meshIndex].record;
					record.vertexBuffer = vertex;
					record.indexBuffer = index;
				}
			}

			std::vector<uint8_t> textureRecordAssignmentPriorities(
				resources->deviceRecords->textures.size(), 0u);
			for (const TextureUploadPlan& texturePlan : m_candidateConcrete->plan.textures)
			{
				const uint8_t* source = m_candidateConcrete->asset.texturePayload.data() +
					texturePlan.payloadSrcOffset;
				size_t sourceSize = static_cast<size_t>(texturePlan.payloadByteSize);
				rhi::TextureFormat format = texturePlan.format;
				uint32_t width = texturePlan.width;
				uint32_t height = texturePlan.height;
				uint32_t mipLevels = std::max(texturePlan.mipLevels, 1u);
				Ktx2Loader::Ktx2Texture ktx;
				if (texturePlan.payloadKind == TexturePayloadKind::Ktx2Container)
				{
					Ktx2Loader loader;
					if (!loader.loadFromMemory(source, sourceSize, ktx) ||
						!resources->device->isFormatSupported(
							ktx.format, rhi::FormatFeatureFlag::sampledImage))
					{
						publishOwned();
						return rhi::RHIResult::fail(
							rhi::RHIErrorCode::unsupported,
							"private KTX2 candidate format is invalid or unsupported");
					}
					source = ktx.data.data();
					sourceSize = ktx.data.size();
					format = ktx.format;
					width = ktx.width;
					height = ktx.height;
					mipLevels = std::max(ktx.mipLevels, 1u);
				}

				const rhi::TextureHandle texture = resources->device->createTexture(rhi::TextureDesc{
					.dimension = rhi::TextureDimension::e2D,
					.format = format,
					.usage = rhi::TextureUsageFlags::sampled |
					         rhi::TextureUsageFlags::transferDst,
					.extent = {width, height, 1u},
					.mipLevels = mipLevels,
					.arrayLayers = 1u,
					.debugName = "SceneTransaction.texture",
				});
				appendOwnedResource(*resources, resourceHandle(texture));
				const rhi::TextureViewHandle view = resources->device->createTextureView(
					rhi::TextureViewCreateDesc{
						.image = texture,
						.format = format,
						.viewType = rhi::ImageViewType::e2D,
						.aspect = rhi::TextureAspect::color,
						.levelCount = mipLevels,
						.layerCount = 1u,
						.debugName = "SceneTransaction.textureView",
					});
				appendOwnedResource(*resources, resourceHandle(view));
				const rhi::BufferHandle staging = resources->device->createBuffer(rhi::BufferDesc{
					.size = sourceSize,
					.usage = rhi::BufferUsageFlags::transferSrc,
					.memoryUsage = rhi::MemoryUsage::cpuToGpu,
					.debugName = "SceneTransaction.textureStaging",
				});
				appendOwnedResource(*resources, resourceHandle(staging));
				void* mapped = resources->device->mapBuffer(staging);
				if (mapped == nullptr)
				{
					publishOwned();
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::outOfMemory,
						"private texture staging buffer could not be mapped");
				}
				std::memcpy(mapped, source, sourceSize);
				resources->device->unmapBuffer(staging);
				const rhi::TextureSubresourceRange range{
					.aspect = rhi::TextureAspect::color,
					.levelCount = mipLevels,
					.layerCount = 1u,
				};
				resources->textureBeginBarriers.push_back({
					.texture = texture,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.range = range,
				});
				resources->textureEndBarriers.push_back({
					.texture = texture,
					.before = rhi::ResourceState::TransferDst,
					.after = rhi::ResourceState::ShaderRead,
					.range = range,
				});
				if (texturePlan.payloadKind == TexturePayloadKind::Ktx2Container)
				{
					for (uint32_t mip = 0u; mip < mipLevels; ++mip)
					{
						resources->textureCopies.push_back({rhi::BufferTextureCopyDesc{
							.buffer = staging,
							.bufferOffset = ktx.mipOffsets[mip],
							.texture = texture,
							.aspect = rhi::TextureAspect::color,
							.mipLevel = mip,
							.layerCount = 1u,
							.width = textureUploadMipExtent(width, mip),
							.height = textureUploadMipExtent(height, mip),
							.depth = 1u,
						}});
					}
				}
				else
				{
					resources->textureCopies.push_back({rhi::BufferTextureCopyDesc{
						.buffer = staging,
						.texture = texture,
						.aspect = rhi::TextureAspect::color,
						.layerCount = 1u,
						.width = width,
						.height = height,
						.depth = 1u,
					}});
				}

				for (size_t textureRecordIndex = 0u;
				     textureRecordIndex < resources->deviceRecords->textures.size();
				     ++textureRecordIndex)
				{
					auto& textureRecord =
						resources->deviceRecords->textures[textureRecordIndex];
					const auto mappingEntry = std::find_if(
						m_candidateConcrete->mapping.entries().begin(),
						m_candidateConcrete->mapping.entries().end(),
						[&](const editor::RendererSceneMappingEntry& entry)
						{
							return entry.rendererHandle.kind == editor::RendererObjectKind::Texture &&
							       entry.rendererHandle.index == textureRecord.handle.index &&
							       entry.rendererHandle.generation == textureRecord.handle.generation;
						});
					if (mappingEntry == m_candidateConcrete->mapping.entries().end())
					{
						continue;
					}
					uint8_t assignmentPriority = 0u;
					if (mappingEntry->projection.kind == editor::EditorObjectKind::image &&
						mappingEntry->projection.sceneAssetIndex == texturePlan.textureIndex)
					{
						assignmentPriority = 4u;
					}
					else if (mappingEntry->projection.kind == editor::EditorObjectKind::texture)
					{
						if (mappingEntry->projection.colorSceneAssetIndex ==
							texturePlan.textureIndex)
						{
							assignmentPriority = 3u;
						}
						else if (mappingEntry->projection.linearSceneAssetIndex ==
							texturePlan.textureIndex)
						{
							assignmentPriority = 2u;
						}
						else if (mappingEntry->projection.sceneAssetIndex ==
								texturePlan.textureIndex &&
							(mappingEntry->projection.colorSceneAssetIndex ==
								std::numeric_limits<uint32_t>::max() ||
							 mappingEntry->projection.linearSceneAssetIndex ==
								std::numeric_limits<uint32_t>::max()))
						{
							assignmentPriority = 1u;
						}
					}
					if (assignmentPriority == 0u ||
						assignmentPriority <=
							textureRecordAssignmentPriorities[textureRecordIndex])
					{
						continue;
					}
					textureRecordAssignmentPriorities[textureRecordIndex] =
						assignmentPriority;
					textureRecord.record.hot.runtimeKind =
						RenderDevice::MaterialResources::TextureRuntimeKind::materialSampled;
					textureRecord.record.hot.sampledViewHandle = view;
					textureRecord.record.cold.ownedTexture = texture;
					textureRecord.record.cold.ownedTextureView = view;
					textureRecord.record.cold.sourceExtent = {width, height};
					textureRecord.record.cold.mipLevels = mipLevels;
				}
			}

			resources->registry = std::make_unique<GPUSceneRegistry>();
			resources->registry->init(resources->device);
			for (const ConcreteObjectRecord& object : m_candidateConcrete->objects)
			{
				if (object.handle.index > kMaxReasonableGPUDrivenObjectCount ||
					!resources->registry->registerObjectAt(
						GPUSceneObjectHandle{object.handle.index, object.handle.generation},
						object.registration))
				{
					publishOwned();
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::invalidHandle,
						"candidate registry handle is invalid or duplicated");
				}
			}
			resources->registry->prepareGpuStorage();
			resources->registry->prepareUpdates(static_cast<uint32_t>(
				m_candidateConcrete->objects.size()));
			for (rhi::ResourceHandle handle : resources->registry->ownedResourceHandles())
			{
				appendOwnedResource(*resources, handle, true);
			}

				if (!resources->deviceRecords->materials.empty() ||
					!resources->deviceRecords->textures.empty())
				{
					// A normally initialized renderer owns one shared material sampler and
					// argument table. Borrow those handles so overlapping transaction
					// generations do not consume another D3D12 sampler-heap descriptor.
					// Headless/injected transaction devices intentionally skip RenderDevice::init;
					// in that mode create equivalent private bindings and retire them with the
					// candidate generation.
					resources->deviceRecords->sampler =
						m_owner.m_renderer.m_materials.materialSamplerHandle;
					if (resources->deviceRecords->sampler.isNull())
					{
						resources->deviceRecords->sampler = resources->device->createSampler(
							rhi::SamplerDesc{
								.magFilter = rhi::Filter::linear,
								.minFilter = rhi::Filter::linear,
								.mipmapMode = rhi::MipmapMode::linear,
								.addressModeU = rhi::AddressMode::repeat,
								.addressModeV = rhi::AddressMode::repeat,
								.addressModeW = rhi::AddressMode::repeat,
								.debugName = "SceneTransaction.materialSampler",
							});
						appendOwnedResource(
							*resources, resourceHandle(resources->deviceRecords->sampler));
					}

					const rhi::ArgumentTableHandle rendererMaterialTable =
						m_owner.m_renderer.getGraphicsMaterialArgumentTable();
					if (!rendererMaterialTable.isNull())
					{
						resources->deviceRecords->descriptorLayout =
							resources->device->getArgumentTableLayout(rendererMaterialTable);
						if (!resources->deviceRecords->descriptorLayout.isValid())
						{
							publishOwned();
							return rhi::RHIResult::fail(
								rhi::RHIErrorCode::invalidState,
								"renderer material argument layout is unavailable");
						}
					}
					else
					{
						const uint32_t descriptorCount = static_cast<uint32_t>(std::max(
							std::size_t{1u}, resources->deviceRecords->textures.size()));
						const rhi::ArgumentBinding binding{
							.binding = kMaterialBindlessTexturesIndex,
							.type = rhi::ArgumentType::combinedImageSampler,
							.visibility = rhi::ShaderStage::allGraphics,
							.arrayCount = descriptorCount,
							.bindless = true,
							.dynamicOffset = false,
						};
						resources->deviceRecords->descriptorLayout =
							resources->device->createArgumentLayout(
								rhi::ArgumentLayoutDesc{
									.bindings = std::span<const rhi::ArgumentBinding>{&binding, 1u},
									.debugName = "SceneTransaction.materialLayout",
								});
						appendOwnedResource(
							*resources, resourceHandle(resources->deviceRecords->descriptorLayout));
					}
					resources->deviceRecords->descriptorTable =
						resources->device->createArgumentTable(rhi::ArgumentTableCreateDesc{
							.layout = resources->deviceRecords->descriptorLayout,
							.lifetime = rhi::ArgumentTableLifetime::persistent,
							.debugName = "SceneTransaction.materialTable",
						});
					appendOwnedResource(
						*resources, resourceHandle(resources->deviceRecords->descriptorTable));
					resources->deviceRecords->descriptorGeneration = 1u;
				}

			publishOwned();
			return rhi::RHIResult::ok();
		}
		catch (const std::bad_alloc&)
		{
			if (m_candidateConcrete->resources != nullptr)
			{
				for (const OwnedResource& owned : m_candidateConcrete->resources->owned)
				{
					createdResources.push_back(owned.journal);
					m_resourceOwners[owned.journal.diagnosticId.value] =
						m_candidateConcrete->resources;
				}
			}
			return rhi::RHIResult::fail(
				rhi::RHIErrorCode::outOfMemory,
				"private scene resource allocation exhausted memory");
		}
		catch (const std::exception& exception)
		{
			if (m_candidateConcrete->resources != nullptr)
			{
				for (const OwnedResource& owned : m_candidateConcrete->resources->owned)
				{
					createdResources.push_back(owned.journal);
					m_resourceOwners[owned.journal.diagnosticId.value] =
						m_candidateConcrete->resources;
				}
			}
			return rhi::RHIResult::fail(
				rhi::RHIErrorCode::invalidState, exception.what());
		}
	}

	std::shared_ptr<GPUDrivenSceneTransactionBridge::ResourceSet>
	GPUDrivenSceneTransactionBridge::resourceSetFor(
		std::span<const editor::SceneJournalResource> resources) const noexcept
	{
		std::shared_ptr<ResourceSet> result;
		for (const editor::SceneJournalResource& resource : resources)
		{
			const auto found = m_resourceOwners.find(resource.diagnosticId.value);
			if (found == m_resourceOwners.end()) return nullptr;
			std::shared_ptr<ResourceSet> owner = found->second;
			if (owner == nullptr) return nullptr;
			const bool exact = std::any_of(
				owner->owned.begin(), owner->owned.end(),
				[&](const OwnedResource& candidate)
				{
					return candidate.journal == resource && !candidate.destroyed;
				});
			if (!exact || (result != nullptr && result != owner)) return nullptr;
			result = std::move(owner);
		}
		return result;
	}

	rhi::RHIResult GPUDrivenSceneTransactionBridge::destroyResourceSet(
		const std::shared_ptr<ResourceSet>& resources) noexcept
	{
		if (resources == nullptr || resources->destroyed)
		{
			return rhi::RHIResult::ok();
		}
		try
		{
			if (resources->registry != nullptr)
			{
				resources->registry->deinit();
				resources->registry.reset();
				for (OwnedResource& owned : resources->owned)
				{
					if (owned.registryOwned)
					{
						owned.destroyed = true;
						m_resourceOwners.erase(owned.journal.diagnosticId.value);
					}
				}
			}
			for (auto iterator = resources->owned.rbegin();
			     iterator != resources->owned.rend(); ++iterator)
			{
				OwnedResource& owned = *iterator;
				if (owned.destroyed) continue;
				const rhi::ResourceHandle handle = owned.journal.handle;
				switch (handle.kind)
				{
				case rhi::ResourceKind::Buffer:
					resources->device->destroyBuffer({handle.index, handle.generation});
					break;
				case rhi::ResourceKind::Texture:
					resources->device->destroyTexture({handle.index, handle.generation});
					break;
				case rhi::ResourceKind::TextureView:
					resources->device->destroyTextureView(
						rhi::TextureViewHandle{{handle.index, handle.generation}});
					break;
				case rhi::ResourceKind::Sampler:
					resources->device->destroySampler({handle.index, handle.generation});
					break;
				case rhi::ResourceKind::ArgumentLayout:
					resources->device->destroyArgumentLayout({handle.index, handle.generation});
					break;
				case rhi::ResourceKind::ArgumentTable:
					resources->device->destroyArgumentTable({handle.index, handle.generation});
					break;
				default:
					return rhi::RHIResult::fail(
						rhi::RHIErrorCode::invalidHandle,
						"private scene owns an unsupported RHI resource kind");
				}
				owned.destroyed = true;
				m_resourceOwners.erase(owned.journal.diagnosticId.value);
			}
			resources->commandBuffers.clear();
			resources->commandAllocator.reset();
			resources->destroyed = true;
			return rhi::RHIResult::ok();
		}
		catch (const std::exception& exception)
		{
			return rhi::RHIResult::fail(rhi::RHIErrorCode::deviceLost,
			                           exception.what());
		}
	}

	bool GPUDrivenSceneTransactionBridge::retirementComplete(
		const RetirementRecord& retirement) const noexcept
	{
		if (!retirement.committed || retirement.resources == nullptr)
		{
			return false;
		}
		if (retirement.resources->deviceRecords != nullptr)
		{
			// A renderer-state lease or RenderDevice lookup still holds the concrete
			// generation. Destruction waits until only the retirement owner remains.
			if (retirement.resources->deviceRecords.use_count() > 1u) return false;
			const auto& frameUse = retirement.resources->deviceRecords->frameUse;
			if (frameUse != nullptr &&
				frameUse->recordingCount.load(std::memory_order_acquire) != 0u)
			{
				return false;
			}
		}
		const auto queueFor = [&](rhi::QueueIdentity identity) noexcept -> rhi::Queue*
		{
			for (const auto& [candidateIdentity, candidateQueue] :
			     retirement.resources->submittedQueues)
			{
				if (candidateIdentity == identity) return candidateQueue;
			}
			if (retirement.resources->device == nullptr) return nullptr;
			constexpr std::array queueClasses{
				rhi::QueueClass::graphics,
				rhi::QueueClass::compute,
				rhi::QueueClass::transfer,
			};
			for (const rhi::QueueClass queueClass : queueClasses)
			{
				rhi::Queue* candidate =
					retirement.resources->device->getQueue(queueClass);
				if (candidate != nullptr && candidate->identity() == identity)
				{
					return candidate;
				}
			}
			return nullptr;
		};
		for (const rhi::SubmissionToken token : retirement.dependencies)
		{
			if (!token.isValid()) return false;
			rhi::Queue* queue = queueFor(token.queue);
			if (queue == nullptr || !queue->isComplete(token)) return false;
		}
		if (retirement.resources->deviceRecords != nullptr)
		{
			const auto& frameUse = retirement.resources->deviceRecords->frameUse;
			const uint64_t value = frameUse != nullptr
				? frameUse->latestSubmissionValue.load(std::memory_order_acquire) : 0u;
			if (value != 0u)
			{
				const rhi::SubmissionToken token{frameUse->queue, value};
				rhi::Queue* queue = token.isValid() ? queueFor(token.queue) : nullptr;
				if (queue == nullptr || !queue->isComplete(token)) return false;
			}
		}
		return true;
	}

	uint64_t GPUDrivenSceneTransactionBridge::drainRetirements() noexcept
	{
		uint64_t destroyed = 0u;
		for (auto iterator = m_retirements.begin(); iterator != m_retirements.end();)
		{
			if (!retirementComplete(*iterator))
			{
				++iterator;
				continue;
			}
			const uint64_t count = iterator->resources != nullptr
				? iterator->resources->owned.size() : 0u;
			if (!destroyResourceSet(iterator->resources).succeeded())
			{
				++iterator;
				continue;
			}
			destroyed += count;
			iterator = m_retirements.erase(iterator);
		}
		for (auto iterator = m_retirementReservations.begin();
		     iterator != m_retirementReservations.end();)
		{
			if (!retirementComplete(iterator->second))
			{
				++iterator;
				continue;
			}
			const uint64_t count = iterator->second.resources != nullptr
				? iterator->second.resources->owned.size() : 0u;
			if (!destroyResourceSet(iterator->second.resources).succeeded())
			{
				++iterator;
				continue;
			}
			destroyed += count;
			iterator = m_retirementReservations.erase(iterator);
		}
		m_eventuallyDestroyedResourceCount += destroyed;
		if (destroyed > 0u && m_counters != nullptr)
		{
			editor::SceneDeltaCounterSnapshot after = m_counters->snapshot();
			after.resourcesPhysicallyDestroyed += destroyed;
			after.retirementBacklog = after.retirementBacklog > destroyed
				? after.retirementBacklog - destroyed : 0u;
			m_counters->commit(after);
		}
		return destroyed;
	}
#endif

	GPUDrivenRenderer::GPUDrivenRenderer(rhi::BackendType backend)
		: m_renderer(backend)
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		m_editorRendererIdentity = {nextEditorRendererIdentityValue(), 1u};
		m_editorDeviceIdentity = {nextEditorRendererIdentityValue(), 1u};
#endif
	}

	GPUDrivenRenderer::GPUDrivenRenderer(
		std::unique_ptr<rhi::Device> sceneTransactionDevice)
		: m_renderer(std::move(sceneTransactionDevice))
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		m_editorRendererIdentity = {nextEditorRendererIdentityValue(), 1u};
		m_editorDeviceIdentity = {nextEditorRendererIdentityValue(), 1u};
#endif
	}

	GPUDrivenRenderer::~GPUDrivenRenderer()
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		m_sceneTransactionRendererStateLease.reset();
		m_sceneTransactionBridge.reset();
		m_sceneTransactionRendererState.store(nullptr, std::memory_order_release);
#endif
	}

#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
	RendererSceneTransactionInitializationResult
	GPUDrivenRenderer::initializeSceneTransactions(
		RendererSceneTransactionInitialState initialState) noexcept
	{
		try
		{
			if (m_sceneTransactionBridge != nullptr)
			{
				return m_sceneTransactionBridge->initialize(std::move(initialState));
			}
			auto candidateBridge =
				std::make_unique<GPUDrivenSceneTransactionBridge>(*this);
			RendererSceneTransactionInitializationResult result =
				candidateBridge->initialize(std::move(initialState));
			if (result.initialized)
			{
				m_sceneTransactionBridge = std::move(candidateBridge);
			}
			return result;
		}
		catch (const std::bad_alloc&)
		{
			return RendererSceneTransactionInitializationResult{
				.initialized = false,
				.error = editor::SceneApplyError::AllocationFailure,
				.message = "scene transaction bridge allocation failed",
			};
		}
	}

	void GPUDrivenRenderer::resetSceneTransactions() noexcept
	{
		if (m_sceneTransactionBridge != nullptr &&
			m_renderer.sceneTransactionDeviceInitialized())
		{
			// Reset is a full-scene lifecycle boundary, never an edit hot path.
			// Private generations may still be referenced by an in-flight frame.
			m_renderer.waitForIdle();
		}
		m_sceneTransactionRendererStateLease.reset();
		m_sceneTransactionBridge.reset();
		m_sceneTransactionRendererState.store(nullptr, std::memory_order_release);
		m_sceneTransactionRendererStateGeneration = 0u;
		m_sceneTransactionAppliedRegistryGeneration = 0u;
		if (m_sceneTransactionConcreteObjectGeneration ==
			std::numeric_limits<uint32_t>::max())
		{
			m_sceneTransactionConcreteObjectGeneration = 1u;
		}
		else
		{
			++m_sceneTransactionConcreteObjectGeneration;
		}
	}

	editor::SceneApplyResult GPUDrivenRenderer::applySceneChanges(
		const editor::ApplySceneChangeRequest& request) noexcept
	{
		if (m_sceneTransactionBridge != nullptr)
		{
			return m_sceneTransactionBridge->applySceneChanges(request);
		}
		editor::SceneApplyResult result;
		result.disposition = editor::SceneApplyDisposition::Rejected;
		result.error = editor::SceneApplyError::InvalidState;
		result.message = "scene transaction publisher is not initialized";
		result.canonicalIdentity = request.canonicalIdentity;
		result.canonicalDigest = request.changes.header.canonicalChangeSetDigest;
		result.rhiResult = rhi::RHIResult::ok();
		return result;
	}

	editor::SceneApplyResult GPUDrivenRenderer::applyFullSceneSnapshot(
		const ApplyFullSceneSnapshotRequest& request) noexcept
	{
		if (m_sceneTransactionBridge != nullptr)
		{
			return m_sceneTransactionBridge->applyFullSceneSnapshot(request);
		}
		return applySceneChanges(request.transaction);
	}

	RendererSceneStateSnapshot GPUDrivenRenderer::activeSceneState() const noexcept
	{
		return m_sceneTransactionBridge != nullptr
			? m_sceneTransactionBridge->state()
			: RendererSceneStateSnapshot{};
	}

	editor::SceneDeltaCounterSnapshot GPUDrivenRenderer::sceneDeltaCounters() const noexcept
	{
		return m_sceneTransactionBridge != nullptr
			? m_sceneTransactionBridge->counters()
			: editor::SceneDeltaCounterSnapshot{};
	}

	SceneTransactionConcreteStateSnapshot
	GPUDrivenRenderer::sceneTransactionConcreteState() const
	{
		return m_sceneTransactionBridge != nullptr
			? m_sceneTransactionBridge->concreteState()
			: SceneTransactionConcreteStateSnapshot{};
	}

	void GPUDrivenRenderer::setSceneTransactionFailurePoint(
		SceneTransactionFailurePoint point,
		uint32_t submissionIndex) noexcept
	{
		if (m_sceneTransactionBridge != nullptr)
		{
			m_sceneTransactionBridge->setFailurePoint(point, submissionIndex);
		}
	}

	uint64_t GPUDrivenRenderer::collectSceneTransactionGarbage() noexcept
	{
		return m_sceneTransactionBridge != nullptr
			? m_sceneTransactionBridge->collectGarbage()
			: 0u;
	}

	uint64_t GPUDrivenRenderer::sceneTransactionVisibleGeneration() const noexcept
	{
		return m_sceneTransactionBridge != nullptr
			? m_sceneTransactionBridge->visibleGeneration()
			: 0u;
	}
#endif

#if !defined(__ANDROID__)
	void GPUDrivenRenderer::initEditorPickingResources()
	{
		if (!m_renderer.hasRHIDevice())
		{
			return;
		}
		rhi::Device& device = getRHIDevice();
		m_editorPickingFormatsSupported =
			device.isFormatSupported(
				rhi::TextureFormat::r32Uint,
				rhi::FormatFeatureFlag::sampledImage |
					rhi::FormatFeatureFlag::colorAttachment) &&
			device.isFormatSupported(
				rhi::TextureFormat::d32Sfloat,
				rhi::FormatFeatureFlag::sampledImage |
					rhi::FormatFeatureFlag::depthStencilAttachment);
		m_editorPickingTickets.reserve(64u);
		m_retiredEditorPickingTickets.reserve(64u);
		m_unfencedEditorPickingTickets.reserve(64u);
		m_retiredEditorPickingTargets.reserve(8u);
		m_editorPickTablePool.reserve(64u);
		m_editorPickAvailableTables.reserve(64u);

		const std::array sampleBindings{
			rhi::ArgumentBinding{
				.binding = 0u,
				.type = rhi::ArgumentType::sampledTexture,
				.visibility = rhi::ShaderStage::compute,
			},
			rhi::ArgumentBinding{
				.binding = 2u,
				.type = rhi::ArgumentType::sampledTexture,
				.visibility = rhi::ShaderStage::compute,
			},
			rhi::ArgumentBinding{
				.binding = 3u,
				.type = rhi::ArgumentType::storageBuffer,
				.visibility = rhi::ShaderStage::compute,
			},
		};
		m_editorPickSampleLayout = device.createArgumentLayout({
			.bindings = sampleBindings,
			.debugName = "editor-pick-sample-layout",
		});
		const std::array highlightBindings{
			rhi::ArgumentBinding{
				.binding = 0u,
				.type = rhi::ArgumentType::sampledTexture,
				.visibility = rhi::ShaderStage::fragment,
			},
			rhi::ArgumentBinding{
				.binding = 1u,
				.type = rhi::ArgumentType::storageBuffer,
				.visibility = rhi::ShaderStage::fragment,
			},
		};
		m_editorHighlightLayout = device.createArgumentLayout({
			.bindings = highlightBindings,
			.debugName = "editor-highlight-layout",
		});
		if (!m_editorPickSampleLayout.isNull())
		{
			for (size_t index = 0u; index < 64u; ++index)
			{
				const rhi::ArgumentTableHandle table =
					device.createArgumentTable({
						.layout = m_editorPickSampleLayout,
						.lifetime = rhi::ArgumentTableLifetime::persistent,
						.debugName = "EditorPickSampleTicketPool",
					});
				if (table.isNull()) break;
				m_editorPickTablePool.push_back(table);
				m_editorPickAvailableTables.push_back(table);
			}
		}

#ifdef USE_SLANG
		m_editorPickingShaderLibrary = loadEmbeddedSpirvLibrary(
			device, editor_picking_slang, "editor-picking");
		m_editorHighlightShaderLibrary = loadEmbeddedSpirvLibrary(
			device, editor_highlight_slang, "editor-highlight");

		const auto& mdiLayouts = m_renderer.m_device.mdiArgumentLayouts;
		const bool mdiLayoutsValid = std::all_of(
			mdiLayouts.begin(), mdiLayouts.end(),
			[](rhi::ArgumentLayoutHandle layout) { return !layout.isNull(); });
		if (mdiLayoutsValid && !m_editorPickingShaderLibrary.isNull())
		{
			const std::array vertexBindings{
				rhi::VertexBindingDesc{
					.binding = 0u,
					.stride = kSceneUploadVertexStride,
					.inputRate = rhi::VertexInputRate::perVertex,
				},
			};
			const std::array vertexAttributes{
				rhi::VertexAttributeDesc{
					.location = shaderio::LVGltfPosition,
					.binding = 0u,
					.format = rhi::VertexFormat::r32g32b32Sfloat,
					.offset = 0u,
				},
				rhi::VertexAttributeDesc{
					.location = shaderio::LVGltfTexCoord,
					.binding = 0u,
					.format = rhi::VertexFormat::r32g32Sfloat,
					.offset = 24u,
				},
			};
			const std::array dynamicStates{
				rhi::DynamicState::viewport,
				rhi::DynamicState::scissor,
			};
			std::array shaderStages{
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::vertex,
					.library = m_editorPickingShaderLibrary,
					.entryPoint = "editorPickingVertexMain",
				},
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::fragment,
					.library = m_editorPickingShaderLibrary,
					.entryPoint = "editorPickingFragmentMain",
				},
			};
			const std::array colorFormats{rhi::TextureFormat::r32Uint};
			const std::array blendStates{
				rhi::BlendAttachmentState{
					.blendEnable = false,
					.colorWriteMask = rhi::ColorComponentFlags::r,
				},
			};
			const std::array rootBindings{
				rhi::RootBindingDesc{
					.slot = kPrimaryRootConstantsSlot,
					.kind = rhi::RootBindingKind::constants,
					.visibility = rhi::ShaderStage::allGraphics,
					.size = sizeof(EditorPickingShaderPushConstants),
					.alignment = alignof(EditorPickingShaderPushConstants),
					.debugName = "editor-picking-draw",
				},
			};
			const rhi::PipelineBindingSchemaStorage schema{
				mdiLayouts, rootBindings};
			rhi::GraphicsPipelineDesc desc{
				.shaderStages = shaderStages,
				.vertexInput = {vertexBindings, vertexAttributes},
				.rasterState = {
					.topology = rhi::PrimitiveTopology::triangleList,
					.cullMode = rhi::CullMode::back,
					.frontFace = rhi::FrontFace::counterClockwise,
					.sampleCount = rhi::SampleCount::count1,
				},
				.depthState = {true, true, rhi::CompareOp::greaterOrEqual},
				.blendStates = blendStates,
				.dynamicStates = dynamicStates,
				.renderingInfo = {
					.colorFormats = colorFormats,
					.depthFormat = rhi::TextureFormat::d32Sfloat,
				},
				.bindingSchema = schema.view(),
			};
			const rhi::SpecializationConstant alphaConstant{
				0u, 0u, sizeof(uint32_t)};
			uint32_t alphaDisabled = 0u;
			shaderStages[1].specializationData = {
				std::as_bytes(std::span{&alphaDisabled, 1u})};
			shaderStages[1].specializationConstants =
				std::span{&alphaConstant, 1u};
			desc.specializationVariant = 0x7101u;
			m_editorPickingOpaquePipeline =
				device.createGraphicsPipeline(desc);
			uint32_t alphaEnabled = 1u;
			shaderStages[1].specializationData = {
				std::as_bytes(std::span{&alphaEnabled, 1u})};
			desc.specializationVariant = 0x7102u;
			m_editorPickingAlphaPipeline =
				device.createGraphicsPipeline(desc);
			desc.rasterState.cullMode = rhi::CullMode::none;
			shaderStages[1].specializationData = {
				std::as_bytes(std::span{&alphaDisabled, 1u})};
			desc.specializationVariant = 0x7105u;
			m_editorPickingOpaqueDoubleSidedPipeline =
				device.createGraphicsPipeline(desc);
			shaderStages[1].specializationData = {
				std::as_bytes(std::span{&alphaEnabled, 1u})};
			desc.specializationVariant = 0x7106u;
			m_editorPickingAlphaDoubleSidedPipeline =
				device.createGraphicsPipeline(desc);
		}

		if (!m_editorPickSampleLayout.isNull() &&
			!m_editorHighlightShaderLibrary.isNull())
		{
			const std::array layouts{m_editorPickSampleLayout};
			const std::array roots{
				rhi::RootBindingDesc{
					.slot = kPrimaryRootConstantsSlot,
					.kind = rhi::RootBindingKind::constants,
					.visibility = rhi::ShaderStage::compute,
					.size = sizeof(EditorHighlightShaderPushConstants),
					.alignment = alignof(EditorHighlightShaderPushConstants),
					.debugName = "editor-pick-sample",
				},
			};
			const rhi::PipelineBindingSchemaStorage schema{layouts, roots};
			m_editorPickSamplePipeline = device.createComputePipeline({
				.shaderStage = {
					.stage = rhi::ShaderStage::compute,
					.library = m_editorHighlightShaderLibrary,
					.entryPoint = "editorPickSampleMain",
				},
				.bindingSchema = schema.view(),
				.specializationVariant = 0x7103u,
			});
		}

		if (!m_editorHighlightLayout.isNull() &&
			!m_editorHighlightShaderLibrary.isNull())
		{
			const std::array layouts{m_editorHighlightLayout};
			const std::array roots{
				rhi::RootBindingDesc{
					.slot = kPrimaryRootConstantsSlot,
					.kind = rhi::RootBindingKind::constants,
					.visibility = rhi::ShaderStage::allGraphics,
					.size = sizeof(EditorHighlightShaderPushConstants),
					.alignment = alignof(EditorHighlightShaderPushConstants),
					.debugName = "editor-highlight",
				},
			};
			const rhi::PipelineBindingSchemaStorage schema{layouts, roots};
			const std::array stages{
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::vertex,
					.library = m_editorHighlightShaderLibrary,
					.entryPoint = "editorHighlightVertexMain",
				},
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::fragment,
					.library = m_editorHighlightShaderLibrary,
					.entryPoint = "editorHighlightFragmentMain",
				},
			};
			const std::array colorFormats{getOutputTextureFormat()};
			const std::array blendStates{
				rhi::BlendAttachmentState{
					.blendEnable = true,
					.srcColorBlendFactor = rhi::BlendFactor::srcAlpha,
					.dstColorBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
					.colorBlendOp = rhi::BlendOp::add,
					.srcAlphaBlendFactor = rhi::BlendFactor::one,
					.dstAlphaBlendFactor = rhi::BlendFactor::oneMinusSrcAlpha,
					.alphaBlendOp = rhi::BlendOp::add,
					.colorWriteMask = rhi::ColorComponentFlags::all,
				},
			};
			const std::array dynamicStates{
				rhi::DynamicState::viewport,
				rhi::DynamicState::scissor,
			};
			m_editorHighlightPipeline = device.createGraphicsPipeline({
				.shaderStages = stages,
				.rasterState = {
					.topology = rhi::PrimitiveTopology::triangleList,
					.cullMode = rhi::CullMode::none,
					.frontFace = rhi::FrontFace::counterClockwise,
					.sampleCount = rhi::SampleCount::count1,
				},
				.depthState = {},
				.blendStates = blendStates,
				.dynamicStates = dynamicStates,
				.renderingInfo = {.colorFormats = colorFormats},
				.bindingSchema = schema.view(),
				.specializationVariant = 0x7104u,
			});
		}
#endif

		m_editorHighlightFrames.resize(getFrameSlotCount());
		for (EditorHighlightFrameResources& frame : m_editorHighlightFrames)
		{
			if (!m_editorHighlightLayout.isNull())
			{
				frame.argumentTable = device.createArgumentTable({
					.layout = m_editorHighlightLayout,
					.lifetime = rhi::ArgumentTableLifetime::persistent,
					.debugName = "editor-highlight-frame",
				});
			}
			const rhi::ArgumentLayoutHandle mdiDrawLayout =
				m_renderer.m_device.mdiArgumentLayouts[2u];
			if (!mdiDrawLayout.isNull())
			{
				frame.pickingDrawTable = device.createArgumentTable({
					.layout = mdiDrawLayout,
					.lifetime = rhi::ArgumentTableLifetime::persistent,
					.debugName = "editor-picking-draw-frame",
				});
			}
		}
	}

	void GPUDrivenRenderer::destroyEditorPickingTicketResources(
		EditorPickingTicketResources& resources) noexcept
	{
		if (!m_renderer.hasRHIDevice())
		{
			resources = {};
			return;
		}
		try
		{
			rhi::Device& device = getRHIDevice();
			if (!resources.argumentTable.isNull())
			{
				m_editorPickAvailableTables.push_back(resources.argumentTable);
				resources.argumentTable = {};
			}
			if (!resources.readbackBuffer.isNull())
				device.destroyBuffer(resources.readbackBuffer);
			if (!resources.sampleBuffer.isNull())
				device.destroyBuffer(resources.sampleBuffer);
			resources.commandBuffer.reset();
			resources.commandAllocator.reset();
			resources = {};
		}
		catch (...)
		{
			resources = {};
		}
	}

	void GPUDrivenRenderer::destroyEditorPickingTarget(
		EditorPickingGpuTarget& target) noexcept
	{
		if (!m_renderer.hasRHIDevice())
		{
			target = {};
			return;
		}
		try
		{
			rhi::Device& device = getRHIDevice();
			if (!target.depthView.isNull()) device.destroyTextureView(target.depthView);
			if (!target.tokenView.isNull()) device.destroyTextureView(target.tokenView);
			if (!target.depthTexture.isNull()) device.destroyTexture(target.depthTexture);
			if (!target.tokenTexture.isNull()) device.destroyTexture(target.tokenTexture);
			target = {};
		}
		catch (...)
		{
			target = {};
		}
	}

	void GPUDrivenRenderer::collectEditorPickingResources() noexcept
	{
		try
		{
		for (auto iterator = m_retiredEditorPickingTickets.begin();
		     iterator != m_retiredEditorPickingTickets.end();)
		{
			const bool complete = !iterator->submission.isValid() ||
				(iterator->queue != nullptr &&
				 iterator->queue->isComplete(iterator->submission));
			if (!complete)
			{
				++iterator;
				continue;
			}
			destroyEditorPickingTicketResources(*iterator);
			iterator = m_retiredEditorPickingTickets.erase(iterator);
		}
		for (auto iterator = m_retiredEditorPickingTargets.begin();
		     iterator != m_retiredEditorPickingTargets.end();)
		{
			const std::shared_ptr<EditorPickingGpuTarget>& target = *iterator;
			bool complete = target == nullptr || !target->lastRasterSubmission.isValid();
			if (!complete && target.use_count() == 1u)
			{
				rhi::Queue* queue = getRHIDevice().getQueue(rhi::QueueClass::graphics);
				complete = queue != nullptr &&
					queue->identity() == target->lastRasterSubmission.queue &&
					queue->isComplete(target->lastRasterSubmission);
			}
			if (target != nullptr && (target.use_count() != 1u || !complete))
			{
				++iterator;
				continue;
			}
			if (target != nullptr) destroyEditorPickingTarget(*target);
			iterator = m_retiredEditorPickingTargets.erase(iterator);
		}
		}
		catch (...)
		{
		}
	}

	void GPUDrivenRenderer::shutdownEditorPickingResources() noexcept
	{
		if (!m_renderer.hasRHIDevice())
		{
			return;
		}
		const std::lock_guard lock(m_editorPickingMutex);
		m_editorPickingPublishedFrame.reset();
		m_editorPickingRecordedFrame.reset();
		m_editorHighlightState.reset();
		for (EditorPickingTicketResources& ticket : m_editorPickingTickets)
			destroyEditorPickingTicketResources(ticket);
		for (EditorPickingTicketResources& ticket : m_retiredEditorPickingTickets)
			destroyEditorPickingTicketResources(ticket);
		for (EditorPickingTicketResources& ticket : m_unfencedEditorPickingTickets)
			destroyEditorPickingTicketResources(ticket);
		m_editorPickingTickets.clear();
		m_retiredEditorPickingTickets.clear();
		m_unfencedEditorPickingTickets.clear();
		for (const rhi::ArgumentTableHandle table : m_editorPickTablePool)
		{
			try
			{
				if (!table.isNull()) getRHIDevice().destroyArgumentTable(table);
			}
			catch (...)
			{
			}
		}
		m_editorPickTablePool.clear();
		m_editorPickAvailableTables.clear();
		for (EditorHighlightFrameResources& frame : m_editorHighlightFrames)
		{
			try
			{
				if (!frame.argumentTable.isNull())
					getRHIDevice().destroyArgumentTable(frame.argumentTable);
				if (!frame.pickingDrawTable.isNull())
					getRHIDevice().destroyArgumentTable(frame.pickingDrawTable);
				if (!frame.tokenMask.isNull())
					getRHIDevice().destroyBuffer(frame.tokenMask);
				if (!frame.pickingDrawBuffer.isNull())
					getRHIDevice().destroyBuffer(frame.pickingDrawBuffer);
			}
			catch (...)
			{
			}
			frame = {};
		}
		m_editorHighlightFrames.clear();
		if (m_editorPickingTarget != nullptr)
			destroyEditorPickingTarget(*m_editorPickingTarget);
		m_editorPickingTarget.reset();
		for (const std::shared_ptr<EditorPickingGpuTarget>& target :
		     m_retiredEditorPickingTargets)
		{
			if (target != nullptr) destroyEditorPickingTarget(*target);
		}
		m_retiredEditorPickingTargets.clear();
		try
		{
			rhi::Device& device = getRHIDevice();
			device.destroyPipeline(m_editorHighlightPipeline);
			device.destroyPipeline(m_editorPickSamplePipeline);
			device.destroyPipeline(m_editorPickingAlphaPipeline);
			device.destroyPipeline(m_editorPickingOpaquePipeline);
			device.destroyPipeline(m_editorPickingAlphaDoubleSidedPipeline);
			device.destroyPipeline(m_editorPickingOpaqueDoubleSidedPipeline);
			if (!m_editorHighlightShaderLibrary.isNull())
				device.destroyShaderLibrary(m_editorHighlightShaderLibrary);
			if (!m_editorPickingShaderLibrary.isNull())
				device.destroyShaderLibrary(m_editorPickingShaderLibrary);
			if (!m_editorHighlightLayout.isNull())
				device.destroyArgumentLayout(m_editorHighlightLayout);
			if (!m_editorPickSampleLayout.isNull())
				device.destroyArgumentLayout(m_editorPickSampleLayout);
		}
		catch (...)
		{
		}
		m_editorHighlightPipeline = {};
		m_editorPickSamplePipeline = {};
		m_editorPickingAlphaPipeline = {};
		m_editorPickingOpaquePipeline = {};
		m_editorPickingAlphaDoubleSidedPipeline = {};
		m_editorPickingOpaqueDoubleSidedPipeline = {};
		m_editorHighlightShaderLibrary = {};
		m_editorPickingShaderLibrary = {};
		m_editorHighlightLayout = {};
		m_editorPickSampleLayout = {};
		m_editorPickingFormatsSupported = false;
	}

	std::shared_ptr<GPUDrivenRenderer::EditorPickingGpuTarget>
	GPUDrivenRenderer::ensureEditorPickingTarget(rhi::Extent2D extent)
	{
		if (extent.width == 0u || extent.height == 0u)
		{
			return nullptr;
		}
		if (m_editorPickingTarget != nullptr &&
			m_editorPickingTarget->extent.width == extent.width &&
			m_editorPickingTarget->extent.height == extent.height)
		{
			return m_editorPickingTarget;
		}

		auto candidate = std::make_shared<EditorPickingGpuTarget>();
		candidate->extent = extent;
		rhi::Device& device = getRHIDevice();
		try
		{
			candidate->tokenTexture = device.createTexture({
				.dimension = rhi::TextureDimension::e2D,
				.format = rhi::TextureFormat::r32Uint,
				.usage = rhi::TextureUsageFlags::sampled |
				         rhi::TextureUsageFlags::colorAttachment,
				.extent = {extent.width, extent.height, 1u},
				.debugName = "EditorOccurrenceToken",
			});
			candidate->tokenView = device.createTextureView({
				.image = candidate->tokenTexture,
				.format = rhi::TextureFormat::r32Uint,
				.viewType = rhi::ImageViewType::e2D,
				.aspect = rhi::TextureAspect::color,
				.debugName = "EditorOccurrenceTokenView",
			});
			candidate->depthTexture = device.createTexture({
				.dimension = rhi::TextureDimension::e2D,
				.format = rhi::TextureFormat::d32Sfloat,
				.usage = rhi::TextureUsageFlags::sampled |
				         rhi::TextureUsageFlags::depthAttachment,
				.extent = {extent.width, extent.height, 1u},
				.debugName = "EditorPickingDepth",
			});
			candidate->depthView = device.createTextureView({
				.image = candidate->depthTexture,
				.format = rhi::TextureFormat::d32Sfloat,
				.viewType = rhi::ImageViewType::e2D,
				.aspect = rhi::TextureAspect::depth,
				.debugName = "EditorPickingDepthView",
			});
			if (candidate->tokenTexture.isNull() || candidate->tokenView.isNull() ||
				candidate->depthTexture.isNull() || candidate->depthView.isNull())
			{
				throw std::runtime_error(
					"editor picking target allocation returned a null handle");
			}
		}
		catch (...)
		{
			destroyEditorPickingTarget(*candidate);
			throw;
		}
		if (m_editorPickingTarget != nullptr)
		{
			try
			{
				m_retiredEditorPickingTargets.push_back(m_editorPickingTarget);
			}
			catch (...)
			{
				destroyEditorPickingTarget(*candidate);
				throw;
			}
		}
		m_editorPickingTarget = candidate;
		collectEditorPickingResources();
		return candidate;
	}

	editor::EditorPickingBackendSubmission GPUDrivenRenderer::enqueue(
		const editor::EditorPickRequest& request) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		EditorPickingTicketResources resources;
		rhi::SubmissionToken queueTokenBefore{};
		bool submitAttempted = false;
		try
		{
			collectEditorPickingResources();
			if (!request.valid() || !m_editorPickingPublishedFrame ||
				m_editorPickingRecordedFrame ||
				m_editorPickingTickets.size() +
					m_retiredEditorPickingTickets.size() >= 64u ||
				!m_unfencedEditorPickingTickets.empty() ||
				!m_editorPickingFormatsSupported ||
				m_editorPickAvailableTables.empty() ||
				m_editorPickSamplePipeline.isNull() ||
				m_editorPickSampleLayout.isNull())
			{
				return {.reason = editor::PickResultReason::backendRejected};
			}
			const EditorPickingPublishedFrame frame =
				*m_editorPickingPublishedFrame;
			if (!frame.target || !frame.snapshot ||
				frame.fence != request.fence ||
				frame.snapshot->generation() != request.occurrenceGeneration ||
				request.sample.pixelWidth != frame.target->extent.width ||
				request.sample.pixelHeight != frame.target->extent.height ||
				!frame.rasterSubmission.isValid())
			{
				return {.reason = editor::PickResultReason::backendRejected};
			}
			rhi::Queue* queue = getRHIDevice().getQueue(rhi::QueueClass::graphics);
			if (queue == nullptr || queue->identity() != frame.rasterSubmission.queue ||
				m_nextEditorPickingTicket ==
					std::numeric_limits<uint64_t>::max())
			{
				return {.reason = editor::PickResultReason::backendRejected};
			}

			resources.ticket = {m_nextEditorPickingTicket + 1u};
			resources.request = request;
			resources.frame = frame;
			resources.queue = queue;
			queueTokenBefore = queue->lastSubmittedToken();
			resources.sampleBuffer = getRHIDevice().createBuffer({
				.size = 2u * sizeof(uint32_t),
				.usage = rhi::BufferUsageFlags::storage |
				         rhi::BufferUsageFlags::transferSrc,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.allowArgumentTableBinding = true,
				.debugName = "EditorPickSampleGpu",
			});
			resources.readbackBuffer = getRHIDevice().createBuffer({
				.size = 2u * sizeof(uint32_t),
				.usage = rhi::BufferUsageFlags::transferDst,
				.memoryUsage = rhi::MemoryUsage::gpuToCpu,
				.debugName = "EditorPickSampleReadback",
			});
			resources.argumentTable = m_editorPickAvailableTables.back();
			m_editorPickAvailableTables.pop_back();
			resources.commandAllocator =
				getRHIDevice().createCommandAllocator(rhi::QueueClass::graphics);
			if (resources.commandAllocator != nullptr)
				resources.commandBuffer =
					resources.commandAllocator->createCommandBuffer();
			if (resources.sampleBuffer.isNull() || resources.readbackBuffer.isNull() ||
				resources.argumentTable.isNull() ||
				resources.commandAllocator == nullptr ||
				resources.commandBuffer == nullptr)
			{
				destroyEditorPickingTicketResources(resources);
				return {.reason = editor::PickResultReason::backendRejected};
			}

			const std::array writes{
				rhi::ArgumentWrite{
					.binding = 0u,
					.type = rhi::ArgumentType::sampledTexture,
					.textureView = frame.target->tokenView,
				},
				rhi::ArgumentWrite{
					.binding = 2u,
					.type = rhi::ArgumentType::sampledTexture,
					.textureView = frame.target->depthView,
				},
				rhi::ArgumentWrite{
					.binding = 3u,
					.type = rhi::ArgumentType::storageBuffer,
					.buffer = resources.sampleBuffer,
					.size = 2u * sizeof(uint32_t),
					.accessIntent = rhi::ArgumentAccessIntent::readWrite,
				},
			};
			getRHIDevice().updateArgumentTable(resources.argumentTable, writes);

			rhi::CommandBuffer& command = *resources.commandBuffer;
			command.begin(*resources.commandAllocator);
			const std::array sampleWrite{
				rhi::BufferBarrier{
					.buffer = resources.sampleBuffer,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::ShaderWrite,
					.size = 2u * sizeof(uint32_t),
				},
			};
			command.resourceBarrier({}, sampleWrite);
			rhi::ComputeEncoder* compute = command.beginComputePass();
			compute->setPipeline(m_editorPickSamplePipeline);
			compute->setArgumentTable(0u, resources.argumentTable);
			EditorHighlightShaderPushConstants push;
			push.viewport = {request.sample.pixelX, request.sample.pixelY,
			                 request.sample.pixelWidth,
			                 request.sample.pixelHeight};
			compute->setRootConstants(
				kPrimaryRootConstantsSlot,
				std::as_bytes(std::span{&push, 1u}));
			compute->dispatch({1u, 1u, 1u});
			command.endEncoding();
			command.barrier(rhi::StageFlags::compute, rhi::StageFlags::transfer,
			                rhi::HazardFlags::bufferWrites);
			const std::array copyBuffers{
				rhi::BufferBarrier{
					.buffer = resources.sampleBuffer,
					.before = rhi::ResourceState::ShaderWrite,
					.after = rhi::ResourceState::TransferSrc,
					.size = 2u * sizeof(uint32_t),
				},
				rhi::BufferBarrier{
					.buffer = resources.readbackBuffer,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::TransferDst,
					.size = 2u * sizeof(uint32_t),
				},
			};
			command.resourceBarrier({}, copyBuffers);
			rhi::ComputeEncoder* copy = command.beginComputePass();
			copy->copyBuffer(resources.sampleBuffer, 0u,
			                 resources.readbackBuffer, 0u,
			                 2u * sizeof(uint32_t));
			command.endEncoding();
			command.end();
			rhi::CommandBuffer* submittedCommand = resources.commandBuffer.get();
			const std::array submittedCommands{submittedCommand};
			submitAttempted = true;
			resources.submission = queue->submit({
				.commandBuffers = submittedCommands,
				.debugLabel = "EditorPickSample",
			});
			if (!resources.submission.isValid())
			{
				const rhi::SubmissionToken after = queue->lastSubmittedToken();
				if (after.isValid() && !(after == queueTokenBefore))
				{
					resources.submission = after;
					m_retiredEditorPickingTickets.push_back(std::move(resources));
				}
				else
				{
					m_unfencedEditorPickingTickets.push_back(std::move(resources));
				}
				return {.reason = editor::PickResultReason::backendRejected};
			}
			m_nextEditorPickingTicket = resources.ticket.value;
			const editor::EditorPickingBackendSubmission result{
				.accepted = true,
				.ticket = resources.ticket,
				.submission = resources.submission,
				.reason = editor::PickResultReason::none,
			};
			m_editorPickingTickets.push_back(std::move(resources));
			return result;
		}
		catch (...)
		{
			if (resources.queue != nullptr && !resources.submission.isValid())
			{
				const rhi::SubmissionToken queueTokenAfter =
					resources.queue->lastSubmittedToken();
				if (queueTokenAfter.isValid() &&
					!(queueTokenAfter == queueTokenBefore))
				{
					resources.submission = queueTokenAfter;
				}
			}
			if (submitAttempted && !resources.submission.isValid())
			{
				m_unfencedEditorPickingTickets.push_back(std::move(resources));
			}
			else if (resources.submission.isValid())
				m_retiredEditorPickingTickets.push_back(std::move(resources));
			else
				destroyEditorPickingTicketResources(resources);
			return {.reason = editor::PickResultReason::backendFailure};
		}
	}

	std::optional<double> GPUDrivenRenderer::editorPickDistance(
		const EditorPickingTicketResources& ticket, float depth) const noexcept
	{
		if (!std::isfinite(depth)) return std::nullopt;
		double ndcDepth = static_cast<double>(depth);
		if (ticket.frame.convention.depthRange ==
			editor::EditorClipDepthRange::negativeOneToOne)
		{
			ndcDepth = ndcDepth * 2.0 - 1.0;
		}
		const glm::dmat4 viewProjection{ticket.frame.viewProjection};
		const glm::dvec4 clipOrigin = viewProjection * glm::dvec4(
			ticket.request.ray.origin.x, ticket.request.ray.origin.y,
			ticket.request.ray.origin.z, 1.0);
		const glm::dvec4 clipDirection = viewProjection * glm::dvec4(
			ticket.request.ray.direction.x, ticket.request.ray.direction.y,
			ticket.request.ray.direction.z, 0.0);
		const double denominator = clipDirection.z -
			ndcDepth * clipDirection.w;
		if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12)
			return std::nullopt;
		const double distance =
			(ndcDepth * clipOrigin.w - clipOrigin.z) / denominator;
		return std::isfinite(distance) && distance > 0.0
			? std::optional<double>{distance} : std::nullopt;
	}

	std::optional<editor::EditorPickingBackendReadback>
	GPUDrivenRenderer::pollOne() noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		collectEditorPickingResources();
		for (EditorPickingTicketResources& ticket : m_editorPickingTickets)
		{
			if (ticket.cancelled || ticket.completionOffered ||
				ticket.queue == nullptr)
			{
				continue;
			}
			editor::EditorPickingBackendReadback result;
			result.ticket = ticket.ticket;
			result.submission = ticket.submission;
			try
			{
				if (!ticket.queue->isComplete(ticket.submission)) continue;
				const rhi::Result<rhi::MappedBufferRange> mapped =
					getRHIDevice().mapBufferRange(ticket.readbackBuffer, {
						.offset = 0u,
						.size = 2u * sizeof(uint32_t),
						.mode = rhi::BufferMapMode::read,
						.invalidateBeforeRead = true,
					});
				std::array<uint32_t, 2u> words{};
				const bool payloadValid = mapped && mapped.value.data != nullptr &&
					mapped.value.size >= sizeof(words);
				if (payloadValid)
					std::memcpy(words.data(), mapped.value.data, sizeof(words));
				if (mapped)
					getRHIDevice().unmapBuffer(ticket.readbackBuffer);
				if (!payloadValid)
				{
					result.status =
						editor::EditorPickingBackendReadbackStatus::error;
					result.reason = editor::PickResultReason::backendFailure;
					ticket.completionOffered = true;
					return result;
				}
				result.token = {words[0]};
				if (!result.token.valid())
				{
					result.status =
						editor::EditorPickingBackendReadbackStatus::miss;
					result.reason = editor::PickResultReason::background;
					ticket.completionOffered = true;
					return result;
				}
				const float depth = std::bit_cast<float>(words[1]);
				const std::optional<double> distance =
					editorPickDistance(ticket, depth);
				if (!distance)
				{
					result.status =
						editor::EditorPickingBackendReadbackStatus::miss;
					result.reason = editor::PickResultReason::invalidHit;
					ticket.completionOffered = true;
					return result;
				}
				result.status =
					editor::EditorPickingBackendReadbackStatus::sampled;
				result.reason = editor::PickResultReason::none;
				result.rayDistance = *distance;
				result.worldHit = editor::EditorWorldPoint{
					ticket.request.ray.origin.x +
						ticket.request.ray.direction.x * *distance,
					ticket.request.ray.origin.y +
						ticket.request.ray.direction.y * *distance,
					ticket.request.ray.origin.z +
						ticket.request.ray.direction.z * *distance,
				};
				ticket.completionOffered = true;
				return result;
			}
			catch (...)
			{
				result.status =
					editor::EditorPickingBackendReadbackStatus::error;
				result.reason = editor::PickResultReason::backendFailure;
				ticket.completionOffered = true;
				return result;
			}
		}
		return std::nullopt;
	}

	void GPUDrivenRenderer::cancel(
		editor::EditorPickingPassTicket ticket) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		for (EditorPickingTicketResources& pending : m_editorPickingTickets)
		{
			if (pending.ticket == ticket)
			{
				pending.cancelled = true;
				return;
			}
		}
	}

	void GPUDrivenRenderer::retireReadback(
		editor::EditorPickingPassTicket ticket,
		rhi::SubmissionToken submission) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		for (auto iterator = m_editorPickingTickets.begin();
		     iterator != m_editorPickingTickets.end(); ++iterator)
		{
			if (iterator->ticket != ticket ||
				iterator->submission != submission)
			{
				continue;
			}
			EditorPickingTicketResources resources = std::move(*iterator);
			m_editorPickingTickets.erase(iterator);
			bool complete = false;
			try
			{
				complete = resources.queue != nullptr &&
					resources.queue->isComplete(resources.submission);
			}
			catch (...)
			{
				complete = false;
			}
			if (complete)
				destroyEditorPickingTicketResources(resources);
			else
				m_retiredEditorPickingTickets.push_back(std::move(resources));
			collectEditorPickingResources();
			return;
		}
	}

	std::optional<std::pair<editor::EditorPickFence, uint64_t>>
	GPUDrivenRenderer::editorPickPublication() const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		return m_editorPickingPublishedFrame &&
			m_editorPickingPublishedFrame->snapshot
			? std::optional{std::pair{
				m_editorPickingPublishedFrame->fence,
				m_editorPickingPublishedFrame->snapshot->generation()}}
			: std::nullopt;
	}

	editor::SceneRenderOccurrenceSnapshot
	GPUDrivenRenderer::editorOccurrenceSnapshot(
		const editor::EditorPickRequest& request) const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		if (!m_editorPickingPublishedFrame ||
			m_editorPickingPublishedFrame->fence != request.fence ||
			!m_editorPickingPublishedFrame->snapshot ||
			m_editorPickingPublishedFrame->snapshot->generation() !=
				request.occurrenceGeneration)
		{
			return {};
		}
		return m_editorPickingPublishedFrame->snapshot;
	}

	std::optional<editor::RenderOccurrenceRecord>
	GPUDrivenRenderer::editorResolveOccurrence(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::RenderOccurrenceId occurrence) const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		return m_editorPickingPublishedFrame &&
			m_editorPickingPublishedFrame->fence == fence &&
			m_editorPickingPublishedFrame->snapshot &&
			m_editorPickingPublishedFrame->snapshot->generation() == occurrenceGeneration
			? m_editorPickingPublishedFrame->snapshot->resolve(occurrence)
			: std::nullopt;
	}

	std::optional<editor::RenderOccurrenceRecord>
	GPUDrivenRenderer::editorResolveOccurrence(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::PrimitiveOccurrenceKey occurrence) const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		if (!m_editorPickingPublishedFrame ||
			m_editorPickingPublishedFrame->fence != fence ||
			!m_editorPickingPublishedFrame->snapshot ||
			m_editorPickingPublishedFrame->snapshot->generation() != occurrenceGeneration)
		{
			return std::nullopt;
		}
		const auto id =
			m_editorPickingPublishedFrame->snapshot->occurrence(occurrence);
		return id ? m_editorPickingPublishedFrame->snapshot->resolve(*id)
		          : std::nullopt;
	}

	bool GPUDrivenRenderer::editorNodeHighlightEligible(
		const editor::EditorPickFence& fence, uint64_t occurrenceGeneration,
		editor::NodeId node) const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		try
		{
			return m_editorPickingPublishedFrame &&
				m_editorPickingPublishedFrame->fence == fence &&
				m_editorPickingPublishedFrame->snapshot &&
				m_editorPickingPublishedFrame->snapshot->generation() == occurrenceGeneration &&
				m_editorPickingPublishedFrame->snapshot->highlight(
					editor::SceneSelectionValue{node}, 1u).has_value();
		}
		catch (...)
		{
			return false;
		}
	}

	bool GPUDrivenRenderer::editorPickingBusy() const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		return m_suspendSceneRendering || !m_editorPickingPublishedFrame ||
			m_editorPickingRecordedFrame.has_value() ||
			!m_unfencedEditorPickingTickets.empty() ||
			!m_editorPickingFormatsSupported ||
			m_editorPickSamplePipeline.isNull() ||
			m_editorPickAvailableTables.empty() ||
			m_editorPickingTickets.size() +
				m_retiredEditorPickingTickets.size() >= 64u;
	}

	EditorGpuInteractionDiagnostics
	GPUDrivenRenderer::editorInteractionDiagnostics() const noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		return m_editorInteractionDiagnostics;
	}

	bool GPUDrivenRenderer::editorOverlayAvailable() const noexcept
	{
		return m_editorOverlayPass != nullptr &&
		       m_editorOverlayPass->initialized();
	}

	EditorOverlayPassDiagnostics
	GPUDrivenRenderer::editorOverlayDiagnostics() const noexcept
	{
		return m_editorOverlayPass != nullptr
		           ? m_editorOverlayPass->diagnostics()
		           : EditorOverlayPassDiagnostics{};
	}

	bool GPUDrivenRenderer::setEditorHighlight(
		const editor::SceneSelectionValue& selection,
		editor::SelectionRevision revision) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		try
		{
			if (!m_editorPickingPublishedFrame ||
				!m_editorPickingPublishedFrame->snapshot)
			{
				m_editorHighlightState.reset();
				return false;
			}
			const std::optional<editor::EditorHighlightState> candidate =
				m_editorPickingPublishedFrame->snapshot->highlight(
					selection, revision);
			if (!candidate) return false;
			if (m_editorHighlightState &&
				m_editorHighlightState->fence ==
					m_editorPickingPublishedFrame->snapshot->fence() &&
				revision < m_editorHighlightState->selectionRevision)
			{
				return false;
			}
			if (m_editorHighlightState &&
				m_editorHighlightState->fence == candidate->fence &&
				m_editorHighlightState->selectionRevision == revision &&
				*m_editorHighlightState != *candidate)
			{
				return false;
			}
			m_editorHighlightState = candidate;
			return true;
		}
		catch (...)
		{
			m_editorHighlightState.reset();
			return false;
		}
	}

	void GPUDrivenRenderer::clearEditorHighlight() noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		m_editorHighlightState.reset();
	}

	void GPUDrivenRenderer::invalidateEditorPickingPublication() noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		m_editorPickingPublishedFrame.reset();
		m_editorHighlightState.reset();
		if (m_editorPickingRecordedFrame)
		{
			const auto target = m_editorPickingRecordedFrame->target;
			if (target && m_editorPickingTarget == target)
			{
				try
				{
					m_retiredEditorPickingTargets.push_back(target);
					m_editorPickingTarget.reset();
				}
				catch (...)
				{
				}
			}
			m_editorPickingRecordedFrame.reset();
		}
		if (m_editorPickingTarget)
		{
			try
			{
				m_retiredEditorPickingTargets.push_back(
					std::move(m_editorPickingTarget));
			}
			catch (...)
			{
				// Keep the target alive if retirement bookkeeping cannot allocate;
				// the next ensure call will retry with the same handle.
			}
		}
	}

	void GPUDrivenRenderer::publishEditorPickingFrameSubmission(
		rhi::SubmissionToken submission) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		if (!submission.isValid() || !m_editorPickingRecordedFrame)
		{
			if (submission.isValid())
			{
				m_editorPickingPublishedFrame.reset();
				m_editorHighlightState.reset();
			}
			m_editorPickingRecordedFrame.reset();
			return;
		}
		m_editorPickingRecordedFrame->rasterSubmission = submission;
		if (m_editorPickingRecordedFrame->target)
			m_editorPickingRecordedFrame->target->lastRasterSubmission = submission;
		m_editorPickingPublishedFrame =
			std::move(m_editorPickingRecordedFrame);
		m_editorPickingRecordedFrame.reset();
		collectEditorPickingResources();
	}

		void GPUDrivenRenderer::abandonEditorPickingRecordedFrame(
		rhi::SubmissionToken submitted) noexcept
	{
		const std::lock_guard lock(m_editorPickingMutex);
		if (m_editorPickingRecordedFrame &&
			m_editorPickingRecordedFrame->target)
		{
			auto& recorded = *m_editorPickingRecordedFrame;
			if (submitted.isValid())
			{
				recorded.target->lastRasterSubmission = submitted;
				if (m_editorPickingPublishedFrame &&
					m_editorPickingPublishedFrame->target == recorded.target)
				{
					m_editorPickingPublishedFrame.reset();
				}
			}
			else
			{
				m_editorPickingPublishedFrame.reset();
				m_editorHighlightState.reset();
				if (m_editorPickingTarget == recorded.target)
				{
					try
					{
						m_retiredEditorPickingTargets.push_back(recorded.target);
						m_editorPickingTarget.reset();
					}
					catch (...)
					{
					}
				}
			}
		}
			m_editorPickingRecordedFrame.reset();
		}

		void GPUDrivenRenderer::initEditorOverlayResources(uint32_t frameSlotCount)
		{
			m_editorOverlayPreparedFrame.reset();
			m_editorOverlayPass.reset();
#ifdef USE_SLANG
			try
			{
				auto pass = std::make_unique<EditorOverlayPass>();
				const rhi::RHIResult initialized = pass->initialize(
					getRHIDevice(),
					{.frameSlotCount = frameSlotCount,
					 .shaderFormat = rhi::ShaderIRFormat::spirv,
					 .shaderData = std::as_bytes(std::span{editor_overlay_slang}),
					 .helperDepthFormat = getSceneDepthFormat()});
				if (initialized.succeeded())
				{
					m_editorOverlayPass = std::move(pass);
				}
			}
			catch (...)
			{
				m_editorOverlayPass.reset();
			}
#else
			(void)frameSlotCount;
#endif
		}

		void GPUDrivenRenderer::prepareEditorOverlayFrame(
			uint32_t frameIndex, const RenderParams& params) noexcept
		{
			if (m_editorOverlayPass == nullptr)
			{
				return;
			}
			(void)m_passExecutor.setPassEnabled(*m_editorOverlayPass, false);
			m_passExecutor.bindTexture({
				.handle = kPassEditorOverlayHelperDepthHandle,
				.rhiTexture = {},
				.aspect = rhi::TextureAspect::depth,
				.initialState = rhi::ResourceState::Undefined,
				.isSwapchain = false,
			});
			(void)m_editorOverlayPass->collectCompletedFrames();
			if (params.editorOverlayFrame == nullptr ||
				m_editorOverlayPreparedFrame.has_value())
			{
				return;
			}

			const EditorOverlayFrameSnapshot& snapshot = *params.editorOverlayFrame;
			if (snapshot.frameSerial == 0u || !snapshot.viewport.valid() ||
				(params.editorViewportMetrics != nullptr &&
				 (snapshot.viewport.viewportGeneration !=
				      params.editorViewportMetrics->viewportGeneration ||
				  snapshot.viewport.projection !=
				      params.editorViewportMetrics->projection ||
				  snapshot.viewport.view !=
				      params.editorViewportMetrics->view)) ||
				getSceneExtent().width != snapshot.viewport.colorAttachment.width ||
				getSceneExtent().height != snapshot.viewport.colorAttachment.height ||
				m_sceneView.sceneDepthExtent.width !=
					snapshot.viewport.depthAttachment.width ||
				m_sceneView.sceneDepthExtent.height !=
					snapshot.viewport.depthAttachment.height)
			{
				return;
			}
			const bool hasGridDraws = snapshot.gridVisible && snapshot.grid.enabled &&
				(!snapshot.gridLines.empty() || !snapshot.gridAxes.empty());
			const bool hasGizmoDraws =
				snapshot.gizmoVisible &&
				std::any_of(snapshot.gizmo.components.begin(),
				            snapshot.gizmo.components.end(),
				            [](const auto& draw) { return draw.visible; });
			if (!hasGridDraws && !hasGizmoDraws)
			{
				return;
			}

			rhi::Queue* queue = getRHIDevice().getQueue(rhi::QueueClass::graphics);
			if (queue == nullptr)
			{
				return;
			}
			const rhi::SubmissionToken baseline = queue->lastSubmittedToken();
			const EditorOverlayPassFrame frame{
				.frameSerial = snapshot.frameSerial,
				.viewport = snapshot.viewport,
				.viewProjection = snapshot.viewProjection,
				.gridVisible = snapshot.gridVisible,
				.grid = snapshot.grid,
				.gridLines = snapshot.gridLines,
				.gridAxes = snapshot.gridAxes,
				.gizmoVisible = snapshot.gizmoVisible,
				.gizmo = snapshot.gizmo,
				.targets = {
					.outputTexture = m_sceneView.outputImage,
					.outputView = m_sceneView.outputView,
					.outputFormat = getOutputTextureFormat(),
					.outputSamples = rhi::SampleCount::count1,
					.sceneDepthTexture = m_sceneView.sceneDepthImage,
					.sceneDepthView = m_sceneView.sceneDepthView,
					.sceneDepthFormat = m_sceneView.sceneDepthFormat,
					.sceneDepthSamples = rhi::SampleCount::count1,
				},
			};
			const EditorOverlayPrepareResult prepared =
				m_editorOverlayPass->prepareFrame(frameIndex, frame);
			if (!prepared.succeeded())
			{
				return;
			}
			m_editorOverlayPreparedFrame =
				EditorOverlayPreparedFrame{prepared.ticket, baseline, frameIndex};
			const rhi::SubmissionToken afterPrepare = queue->lastSubmittedToken();
			if (afterPrepare != baseline)
			{
				(void)tryResolveEditorOverlayPreparedFrame(true);
				return;
			}
			const EditorOverlayHelperDepthBinding helper = prepared.helperDepth;
			if (helper.valid())
			{
				m_passExecutor.bindTexture({
					.handle = helper.passHandle,
					.rhiTexture = helper.texture,
					.aspect = rhi::TextureAspect::depth,
					.initialState = helper.initialState,
					.isSwapchain = false,
				});
			}
			const auto outputBinding =
				m_passExecutor.getTextureBinding(kPassOutputHandle);
			const auto depthBinding =
				m_passExecutor.getTextureBinding(kPassSceneDepthHandle);
			const bool coherentBindings = outputBinding.has_value() &&
				depthBinding.has_value() &&
				outputBinding->rhiTexture == frame.targets.outputTexture &&
				depthBinding->rhiTexture == frame.targets.sceneDepthTexture &&
					helper.valid() &&
					m_passExecutor.hasBoundDependencies(*m_editorOverlayPass);
			if (!coherentBindings)
			{
				(void)tryResolveEditorOverlayPreparedFrame(true);
				return;
			}
				if (!m_passExecutor.setPassEnabled(*m_editorOverlayPass, true))
				{
					(void)tryResolveEditorOverlayPreparedFrame(true);
				}
		}

		bool GPUDrivenRenderer::tryResolveEditorOverlayPreparedFrame(
			bool allowConservativeSubmittedRetire) noexcept
		{
			if (m_editorOverlayPass == nullptr ||
				!m_editorOverlayPreparedFrame.has_value())
			{
				return true;
			}
			EditorOverlayPreparedFrame& prepared = *m_editorOverlayPreparedFrame;
			rhi::Queue* queue = getRHIDevice().getQueue(rhi::QueueClass::graphics);
			if (queue == nullptr)
			{
				return false;
			}
			const rhi::SubmissionToken head = queue->lastSubmittedToken();
			const bool queueUnchanged =
				(!head.isValid() && !prepared.queueBaseline.isValid()) ||
				(head.isValid() && prepared.queueBaseline.isValid() &&
				 head == prepared.queueBaseline);
			// Keep the normal no-submit path free of an intentional invalid-token
			// diagnostic. A raced submit takes the conservative submitted path first;
			// only then do we retry notSubmitted for the case where the pass sampled a
			// newer baseline than the outer wrapper.
			if (queueUnchanged &&
				m_editorOverlayPass->resolveFrame(
						prepared.ticket, EditorOverlayFrameDisposition::notSubmitted)
					.succeeded())
			{
				m_editorOverlayPreparedFrame.reset();
				return true;
			}
			if (allowConservativeSubmittedRetire && head.isValid() &&
				m_editorOverlayPass->resolveFrame(
						prepared.ticket, EditorOverlayFrameDisposition::submitted,
						head)
					.succeeded())
			{
				m_editorOverlayPreparedFrame.reset();
				return true;
			}
			if (!queueUnchanged &&
				m_editorOverlayPass->resolveFrame(
						prepared.ticket, EditorOverlayFrameDisposition::notSubmitted)
					.succeeded())
			{
				m_editorOverlayPreparedFrame.reset();
				return true;
			}
			return false;
		}

		void GPUDrivenRenderer::reconcileEditorOverlayFrameAfterRenderAttempt() noexcept
		{
			if (m_editorOverlayPass == nullptr ||
				!m_editorOverlayPreparedFrame.has_value())
			{
				return;
			}
			EditorOverlayPreparedFrame& prepared = *m_editorOverlayPreparedFrame;
			rhi::Queue* queue = getRHIDevice().getQueue(rhi::QueueClass::graphics);
			if (queue == nullptr)
			{
				return;
			}
			const rhi::SubmissionToken head = queue->lastSubmittedToken();
			const std::uint64_t baselineValue = prepared.queueBaseline.isValid()
				? prepared.queueBaseline.value : 0u;
			const bool submitted = head.isValid() &&
				head.queue == queue->identity() &&
				head.value == baselineValue + 1u;
			if (submitted)
			{
				if (m_editorOverlayPass->resolveFrame(
						prepared.ticket,
						EditorOverlayFrameDisposition::submitted, head).succeeded())
				{
					m_editorOverlayPreparedFrame.reset();
				}
				else
				{
					(void)tryResolveEditorOverlayPreparedFrame(true);
				}
				return;
			}

			const bool queueUnchanged =
				(!head.isValid() && baselineValue == 0u) ||
				(head.isValid() && prepared.queueBaseline.isValid() &&
				 head == prepared.queueBaseline);
			if (!queueUnchanged)
			{
				return;
			}
			bool abandoned = true;
			if (m_renderer.m_perFrame.activeCommandBuffer != nullptr)
			{
				abandoned = m_renderer.m_perFrame.frameScheduler.abandonFrame(
					*m_renderer.m_perFrame.activeCommandBuffer);
				if (abandoned)
				{
					m_renderer.m_perFrame.activeCommandBuffer = nullptr;
				}
			}
			if (abandoned)
			{
				(void)tryResolveEditorOverlayPreparedFrame(true);
			}
		}

		void GPUDrivenRenderer::shutdownEditorOverlayResources() noexcept
		{
			if (m_editorOverlayPass == nullptr)
			{
				m_editorOverlayPreparedFrame.reset();
				return;
			}
			reconcileEditorOverlayFrameAfterRenderAttempt();
			if (m_editorOverlayPreparedFrame.has_value())
			{
				(void)tryResolveEditorOverlayPreparedFrame(true);
			}
			(void)m_passExecutor.setPassEnabled(*m_editorOverlayPass, false);
			m_passExecutor.bindTexture({
				.handle = kPassEditorOverlayHelperDepthHandle,
				.rhiTexture = {},
				.aspect = rhi::TextureAspect::depth,
				.initialState = rhi::ResourceState::Undefined,
				.isSwapchain = false,
			});
				const rhi::RHIResult shutdownResult = m_editorOverlayPass->shutdown();
				if (shutdownResult.succeeded())
				{
					m_editorOverlayPreparedFrame.reset();
				}
				else if (m_editorOverlayPreparedFrame.has_value())
				{
					LOGW("Editor overlay shutdown retained an unresolved frame ticket");
				}
			}

	void GPUDrivenRenderer::ensureEditorHighlightMaskCapacity(
		uint32_t frameIndex, uint32_t requiredCount)
	{
		if (frameIndex >= m_editorHighlightFrames.size() ||
			requiredCount == 0u)
		{
			return;
		}
		EditorHighlightFrameResources& frame =
			m_editorHighlightFrames[frameIndex];
		if (!frame.tokenMask.isNull() && frame.maskCapacity >= requiredCount)
			return;
		const rhi::BufferHandle candidate = getRHIDevice().createBuffer({
			.size = static_cast<uint64_t>(requiredCount) * sizeof(uint32_t),
			.usage = rhi::BufferUsageFlags::storage,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.allowArgumentTableBinding = true,
			.debugName = "EditorHighlightTokenMask",
		});
		if (candidate.isNull()) return;
		if (!frame.tokenMask.isNull())
			getRHIDevice().destroyBuffer(frame.tokenMask);
		frame.tokenMask = candidate;
		frame.maskCapacity = requiredCount;
	}

	void GPUDrivenRenderer::ensureEditorPickingDrawCapacity(
		uint32_t frameIndex, uint32_t requiredCount)
	{
		if (frameIndex >= m_editorHighlightFrames.size() || requiredCount == 0u)
			return;
		EditorHighlightFrameResources& frame =
			m_editorHighlightFrames[frameIndex];
		if (!frame.pickingDrawBuffer.isNull() &&
			frame.pickingDrawCapacity >= requiredCount)
		{
			return;
		}
		const rhi::BufferHandle candidate = getRHIDevice().createBuffer({
			.size = static_cast<uint64_t>(requiredCount) *
			        sizeof(shaderio::DrawUniforms),
			.usage = rhi::BufferUsageFlags::storage,
			.memoryUsage = rhi::MemoryUsage::cpuToGpu,
			.allowArgumentTableBinding = true,
			.debugName = "EditorPickingStructuredDrawData",
			.structuredElementStride = sizeof(shaderio::DrawUniforms),
		});
		if (candidate.isNull()) return;
		if (!frame.pickingDrawBuffer.isNull())
			getRHIDevice().destroyBuffer(frame.pickingDrawBuffer);
		frame.pickingDrawBuffer = candidate;
		frame.pickingDrawCapacity = requiredCount;
	}

	void GPUDrivenRenderer::executeEditorOccurrencePass(
		const PassContext& context)
	{
		const std::lock_guard lock(m_editorPickingMutex);
		m_editorPickingRecordedFrame.reset();
		collectEditorPickingResources();
		const bool prerequisitesValid =
			!m_suspendSceneRendering && context.commandBuffer != nullptr &&
			context.params != nullptr && context.params->cameraUniforms != nullptr &&
			context.params->editorViewportMetrics != nullptr &&
			context.transientAllocator != nullptr && context.cameraAllocValid &&
			m_sceneTransactionRendererStateLease != nullptr &&
			m_sceneTransactionRendererStateLease->occurrenceSnapshot != nullptr &&
			!m_editorPickingOpaquePipeline.isNull();
		if (!prerequisitesValid)
		{
			return;
		}
		const editor::EditorViewportMetrics& metrics =
			*context.params->editorViewportMetrics;
		const editor::SceneRenderOccurrenceSnapshot snapshot =
			m_sceneTransactionRendererStateLease->occurrenceSnapshot;
		if (!metrics.valid() ||
			snapshot->fence().projectionRevision != metrics.projection.revision ||
			metrics.physical.extent.width >
				static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
			metrics.physical.extent.height >
				static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
		{
			return;
		}
		const rhi::Extent2D extent{
			metrics.physical.extent.width, metrics.physical.extent.height};
		const bool targetWasReady =
			m_editorPickingTarget != nullptr &&
			m_editorPickingTarget->extent.width == extent.width &&
			m_editorPickingTarget->extent.height == extent.height;
		std::shared_ptr<EditorPickingGpuTarget> target =
			ensureEditorPickingTarget(extent);
		if (!target) return;
		// Resources created during this pass are not visible through the command
		// buffer's recording table until the next command buffer begins. Leave the
		// target in place and retry on the next frame instead of binding a stale
		// handle on this recording.
		if (!targetWasReady) return;
		if (context.frameIndex >= m_editorHighlightFrames.size()) return;
		const auto& drawRecords =
			m_sceneTransactionRendererStateLease->upload.drawRecords;
		if (drawRecords.size() >
			static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		{
			return;
		}
		EditorHighlightFrameResources& editorFrame =
			m_editorHighlightFrames[context.frameIndex];
		if (!drawRecords.empty())
		{
			ensureEditorPickingDrawCapacity(
				context.frameIndex, static_cast<uint32_t>(drawRecords.size()));
			if (editorFrame.pickingDrawBuffer.isNull() ||
				editorFrame.pickingDrawTable.isNull() ||
				m_persistentDrawData.size() < drawRecords.size())
			{
				return;
			}
			const uint64_t drawBytes = static_cast<uint64_t>(drawRecords.size()) *
				sizeof(shaderio::DrawUniforms);
			const rhi::Result<rhi::MappedBufferRange> mapped =
				getRHIDevice().mapBufferRange(editorFrame.pickingDrawBuffer, {
					.offset = 0u,
					.size = drawBytes,
					.mode = rhi::BufferMapMode::write,
					.invalidateBeforeRead = false,
				});
			if (!mapped || mapped.value.data == nullptr) return;
			std::memcpy(mapped.value.data, m_persistentDrawData.data(),
			            static_cast<size_t>(drawBytes));
			const bool flushed = getRHIDevice().flushMappedBufferRange(
				editorFrame.pickingDrawBuffer, 0u, drawBytes).succeeded();
			getRHIDevice().unmapBuffer(editorFrame.pickingDrawBuffer);
			if (!flushed) return;
			const rhi::ArgumentWrite drawWrite{
				.binding = shaderio::LBindDrawModelMdi,
				.type = rhi::ArgumentType::storageBuffer,
				.buffer = editorFrame.pickingDrawBuffer,
				.size = drawBytes,
			};
			getRHIDevice().updateArgumentTable(
				editorFrame.pickingDrawTable,
				std::span{&drawWrite, 1u});
		}
		shaderio::CameraUniforms pickingCamera =
			*context.params->cameraUniforms;
		pickingCamera.viewProjection = pickingCamera.unjitteredViewProjection;
		pickingCamera.inverseViewProjection =
			pickingCamera.unjitteredInverseViewProjection;
		const TransientAllocator::Allocation pickingCameraAlloc =
			context.transientAllocator->allocate(sizeof(pickingCamera), 256u);
		std::memcpy(pickingCameraAlloc.cpuPtr, &pickingCamera,
		            sizeof(pickingCamera));
		context.transientAllocator->flushAllocation(
			pickingCameraAlloc, sizeof(pickingCamera));
		const rhi::ResourceState tokenStateBefore = target->tokenState;
		const rhi::ResourceState depthStateBefore = target->depthState;
		m_editorPickingRecordedFrame = EditorPickingPublishedFrame{
			.target = target,
			.fence = {snapshot->fence(), metrics.viewportGeneration,
			          metrics.view.revision},
			.snapshot = snapshot,
			.viewProjection = pickingCamera.viewProjection,
			.convention = metrics.convention,
			.tokenStateBefore = tokenStateBefore,
			.depthStateBefore = depthStateBefore,
		};

			const std::array targetBarriers{
			rhi::TextureBarrier{
				.texture = target->tokenTexture,
				.before = target->tokenState,
				.after = rhi::ResourceState::ColorAttachment,
				.range = {.aspect = rhi::TextureAspect::color},
			},
			rhi::TextureBarrier{
				.texture = target->depthTexture,
				.before = target->depthState,
				.after = rhi::ResourceState::DepthStencilAttachment,
				.range = {.aspect = rhi::TextureAspect::depth},
			},
			};
		context.commandBuffer->resourceBarrier(targetBarriers, {});
		target->tokenState = rhi::ResourceState::ColorAttachment;
		target->depthState = rhi::ResourceState::DepthStencilAttachment;

		const rhi::RenderTargetDesc colorTarget{
			.view = target->tokenView,
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
		};
		const rhi::DepthTargetDesc depthTarget{
			.view = target->depthView,
			.state = rhi::ResourceState::DepthStencilAttachment,
			.loadOp = rhi::LoadOp::clear,
			.storeOp = rhi::StoreOp::store,
			.clearValue = {0.0f, 0u},
		};
		context.commandBuffer->beginEvent("GPUDrivenEditorOccurrence");
		rhi::RenderEncoder* encoder = context.commandBuffer->beginRenderPass({
			.renderArea = {{0, 0}, extent},
			.colorTargets = std::span{&colorTarget, 1u},
			.depthTarget = &depthTarget,
		});
		encoder->setViewport({0.0f, 0.0f,
		                      static_cast<float>(extent.width),
		                      static_cast<float>(extent.height), 0.0f, 1.0f});
		encoder->setScissor({{0, 0}, extent});
		const rhi::ArgumentTableHandle materialTable =
			m_renderer.getGraphicsMaterialArgumentTable();
		const rhi::ArgumentTableHandle cameraTable =
			m_renderer.getCameraArgumentTable(context.frameIndex);
		const rhi::ArgumentTableHandle drawTable =
			editorFrame.pickingDrawTable;
		if (!materialTable.isNull() && !cameraTable.isNull() &&
			!drawTable.isNull())
		{
			PipelineHandle boundPipeline{};
			rhi::BufferHandle boundVertexBuffer{};
			rhi::BufferHandle boundIndexBuffer{};
			for (uint32_t drawIndex = 0u;
			     drawIndex < static_cast<uint32_t>(drawRecords.size()); ++drawIndex)
			{
				const SceneUploadResult::SceneDrawRecord& draw =
					drawRecords[drawIndex];
				const std::optional<editor::RenderOccurrenceRecord> occurrence =
					snapshot->resolve(draw.occurrence);
				if (!occurrence || !occurrence->visible ||
					!draw.pickToken.valid() ||
					drawIndex >= m_persistentDrawData.size())
				{
					continue;
				}
				const MeshRecord* mesh =
					m_renderer.tryGetSceneTransactionMesh(draw.meshHandle);
				if (mesh == nullptr)
					mesh = m_renderer.getMeshPool().tryGet(draw.meshHandle);
				if (mesh == nullptr || mesh->vertexBuffer.isNull() ||
					mesh->indexBuffer.isNull() || mesh->indexCount == 0u)
				{
					continue;
				}
				const bool alphaTest = draw.alphaMode == shaderio::LAlphaMask;
				// Picking is an editor visibility query rather than a lighting draw;
				// keep it two-sided so a backend projection Y flip cannot make a
				// selectable surface disappear solely because of winding.
				const PipelineHandle pipeline = alphaTest
					? m_editorPickingAlphaDoubleSidedPipeline
					: m_editorPickingOpaqueDoubleSidedPipeline;
				if (pipeline.isNull()) continue;
				if (pipeline != boundPipeline)
				{
					encoder->setPipeline(pipeline);
					encoder->setArgumentTable(
						rhi::ShaderStage::fragment, shaderio::LSetTextures,
						materialTable);
					encoder->setDynamicBuffer(
						rhi::ShaderStage::allGraphics,
						kSceneDynamicBufferTableSlot, {},
						pickingCameraAlloc.offset, 0u);
					encoder->setDynamicBuffer(
						rhi::ShaderStage::allGraphics,
						kSceneDynamicBufferTableSlot, {}, 0u, 0u);
					encoder->setArgumentTable(
						rhi::ShaderStage::allGraphics,
						kSceneDynamicBufferTableSlot, cameraTable);
					encoder->setArgumentTable(
						rhi::ShaderStage::allGraphics, shaderio::LSetDraw,
						drawTable);
					boundPipeline = pipeline;
				}
				if (mesh->vertexBuffer != boundVertexBuffer)
				{
					encoder->bindVertexBuffer(0u, mesh->vertexBuffer, 0u);
					boundVertexBuffer = mesh->vertexBuffer;
				}
				if (mesh->indexBuffer != boundIndexBuffer)
				{
					encoder->bindIndexBuffer(
						mesh->indexBuffer, 0u, rhi::IndexFormat::uint32);
					boundIndexBuffer = mesh->indexBuffer;
				}
				const EditorPickingShaderPushConstants push{
					drawIndex, draw.pickToken.value};
				encoder->setRootConstants(
					rhi::ShaderStage::allGraphics,
					kPrimaryRootConstantsSlot,
					std::as_bytes(std::span{&push, 1u}));
				encoder->drawIndexed({
					.indexCount = mesh->indexCount,
					.instanceCount = 1u,
					.firstIndex = mesh->firstIndex,
					.vertexOffset = mesh->vertexOffset,
					.firstInstance = 0u,
				});
			}
		}
		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
		const std::array sampledBarriers{
			rhi::TextureBarrier{
				.texture = target->tokenTexture,
				.before = rhi::ResourceState::ColorAttachment,
				.after = rhi::ResourceState::ShaderRead,
				.range = {.aspect = rhi::TextureAspect::color},
			},
			rhi::TextureBarrier{
				.texture = target->depthTexture,
				.before = rhi::ResourceState::DepthStencilAttachment,
				.after = rhi::ResourceState::ShaderRead,
				.range = {.aspect = rhi::TextureAspect::depth},
			},
		};
		context.commandBuffer->resourceBarrier(sampledBarriers, {});
		target->tokenState = rhi::ResourceState::ShaderRead;
		target->depthState = rhi::ResourceState::ShaderRead;
		if (m_editorInteractionDiagnostics.occurrencePassExecutions !=
			std::numeric_limits<uint64_t>::max())
		{
			++m_editorInteractionDiagnostics.occurrencePassExecutions;
		}
	}

	void GPUDrivenRenderer::executeEditorHighlightPass(
		const PassContext& context)
	{
		const std::lock_guard lock(m_editorPickingMutex);
		if (context.commandBuffer == nullptr || context.frameIndex >=
			m_editorHighlightFrames.size() || !m_editorPickingRecordedFrame ||
			!m_editorPickingRecordedFrame->target ||
			!m_editorPickingRecordedFrame->snapshot ||
			!m_editorHighlightState ||
			m_editorHighlightState->occurrences.empty() ||
			m_editorHighlightPipeline.isNull() ||
			m_editorHighlightState->fence !=
				m_editorPickingRecordedFrame->snapshot->fence())
		{
			return;
		}
		const editor::SceneRenderOccurrenceSnapshot snapshot =
			m_editorPickingRecordedFrame->snapshot;
		if (snapshot->records().size() >=
			static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
		{
			return;
		}
		const uint32_t maskCount =
			static_cast<uint32_t>(snapshot->records().size() + 1u);
		ensureEditorHighlightMaskCapacity(context.frameIndex, maskCount);
		EditorHighlightFrameResources& frame =
			m_editorHighlightFrames[context.frameIndex];
		if (frame.tokenMask.isNull() || frame.argumentTable.isNull()) return;

		std::vector<uint32_t> mask(maskCount, 0u);
		for (const editor::RenderOccurrenceId occurrence :
		     m_editorHighlightState->occurrences)
		{
			const std::optional<editor::FramebufferPickToken> token =
				snapshot->framebufferToken(occurrence);
			if (!token || token->value >= mask.size()) return;
			mask[token->value] = 1u;
		}
		const rhi::Result<rhi::MappedBufferRange> mapped =
			getRHIDevice().mapBufferRange(frame.tokenMask, {
				.offset = 0u,
				.size = static_cast<uint64_t>(mask.size()) * sizeof(uint32_t),
				.mode = rhi::BufferMapMode::write,
				.invalidateBeforeRead = false,
			});
		if (!mapped || mapped.value.data == nullptr) return;
		std::memcpy(mapped.value.data, mask.data(),
		            mask.size() * sizeof(uint32_t));
		if (!getRHIDevice().flushMappedBufferRange(
				frame.tokenMask, 0u,
				static_cast<uint64_t>(mask.size()) * sizeof(uint32_t)))
		{
			getRHIDevice().unmapBuffer(frame.tokenMask);
			return;
		}
		getRHIDevice().unmapBuffer(frame.tokenMask);
		const EditorPickingGpuTarget& target =
			*m_editorPickingRecordedFrame->target;
		const std::array writes{
			rhi::ArgumentWrite{
				.binding = 0u,
				.type = rhi::ArgumentType::sampledTexture,
				.textureView = target.tokenView,
			},
			rhi::ArgumentWrite{
				.binding = 1u,
				.type = rhi::ArgumentType::storageBuffer,
				.buffer = frame.tokenMask,
				.size = static_cast<uint64_t>(maskCount) * sizeof(uint32_t),
			},
		};
		getRHIDevice().updateArgumentTable(frame.argumentTable, writes);
		const rhi::Extent2D outputExtent = getSceneExtent();
		if (outputExtent.width != target.extent.width ||
			outputExtent.height != target.extent.height ||
			getOutputTextureView().isNull())
		{
			return;
		}
		const rhi::RenderTargetDesc outputTarget{
			.view = getOutputTextureView(),
			.state = rhi::ResourceState::ColorAttachment,
			.loadOp = rhi::LoadOp::load,
			.storeOp = rhi::StoreOp::store,
		};
		context.commandBuffer->beginEvent("GPUDrivenEditorHighlight");
		rhi::RenderEncoder* encoder = context.commandBuffer->beginRenderPass({
			.renderArea = {{0, 0}, outputExtent},
			.colorTargets = std::span{&outputTarget, 1u},
		});
		encoder->setViewport({0.0f, 0.0f,
		                      static_cast<float>(outputExtent.width),
		                      static_cast<float>(outputExtent.height), 0.0f, 1.0f});
		encoder->setScissor({{0, 0}, outputExtent});
		encoder->setPipeline(m_editorHighlightPipeline);
		encoder->setArgumentTable(
			rhi::ShaderStage::fragment, 0u, frame.argumentTable);
		EditorHighlightShaderPushConstants push;
		push.viewport = {0u, 0u, outputExtent.width, outputExtent.height};
		push.counts = {maskCount, 2u, 0u, 0u};
		encoder->setRootConstants(
			rhi::ShaderStage::allGraphics, kPrimaryRootConstantsSlot,
			std::as_bytes(std::span{&push, 1u}));
		encoder->draw({.vertexCount = 3u, .instanceCount = 1u});
		context.commandBuffer->endEncoding();
		context.commandBuffer->endEvent();
		if (m_editorInteractionDiagnostics.highlightPassExecutions !=
			std::numeric_limits<uint64_t>::max())
		{
			++m_editorInteractionDiagnostics.highlightPassExecutions;
		}
		m_editorInteractionDiagnostics.lastHighlightSelectionRevision =
			m_editorHighlightState->selectionRevision;
		m_editorInteractionDiagnostics.lastHighlightOccurrenceCount =
			static_cast<uint32_t>(m_editorHighlightState->occurrences.size());
	}
#endif

	void GPUDrivenRenderer::init(void* window, rhi::Surface& surface, bool vSync)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.Init");
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_editorDeviceIdentityInitialized)
		{
			m_editorDeviceIdentity.generation =
				m_editorDeviceIdentity.generation == 0u ||
				m_editorDeviceIdentity.generation ==
					std::numeric_limits<uint32_t>::max()
					? 0u : m_editorDeviceIdentity.generation + 1u;
		}
		m_editorDeviceIdentityInitialized = true;
#endif
		m_renderer.init(window, surface, vSync);
		resetTemporalAndPersistentTextureStates();
		const uint32_t frameSlotCount = getFrameSlotCount();
		ASSERT(frameSlotCount > 0u, "GPU-driven renderer requires at least one frame resource slot");
		m_sceneRegistry.init(&m_renderer.getRHIDevice());
		initLightingResources();
		initIBLResources();
		m_enableExperimentalMeshletPath = kEnableExperimentalMeshletPath;
		if (m_enableExperimentalMeshletPath)
		{
			m_meshletBuffer.init(&m_renderer.getRHIDevice());
		}
		m_hiZDepthPyramid.init(getRHIDevice(), frameSlotCount, getSceneExtent());

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
		m_globalSDFPass = std::make_unique<GlobalSDFPass>(this);
		m_ddgiRayTracePass = std::make_unique<DDGIRayTracePass>(this);
		m_ddgiProbeUpdatePass = std::make_unique<DDGIProbeUpdatePass>(this);
		m_ddgiDebugPass = std::make_unique<DDGIDebugPass>(this);
		m_flaxDDGIPass = std::make_unique<FlaxDDGIPass>(this);
		m_forwardPass = std::make_unique<GPUDrivenForwardPass>(this);
		m_velocityPass = std::make_unique<GPUDrivenVelocityPass>(this);
		m_taaResolvePass = std::make_unique<GPUDrivenTAAResolvePass>(this);
		m_bloomPrefilterPass = std::make_unique<GPUDrivenBloomPrefilterPass>(this);
		m_bloomDownsamplePass = std::make_unique<GPUDrivenBloomDownsamplePass>(this);
		m_finalColorPass = std::make_unique<GPUDrivenFinalColorPass>(this);
#if !defined(__ANDROID__)
		m_editorOccurrencePass =
			std::make_unique<GPUDrivenEditorOccurrencePass>(*this);
			m_editorHighlightPass =
				std::make_unique<GPUDrivenEditorHighlightPass>(*this);
			initEditorPickingResources();
			initEditorOverlayResources(frameSlotCount);
#endif
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
		// DDGI (Wave D1-2): Global SDF rebuild runs early in the frame; all its
		// resources are pass-private and it no-ops while DDGIConfig::enabled is false.
		m_passExecutor.addPass(*m_globalSDFPass);
		// DDGI (Wave D2-2): probe SDF ray trace consumes the freshly rebuilt
		// Global SDF (write->read covered by GlobalSDFPass's trailing barrier).
		m_passExecutor.addPass(*m_ddgiRayTracePass);
		// DDGI (Wave D2-3): probe irradiance/depth + border updates consume the
		// radiance scratch (write->read covered by the ray trace pass's
		// trailing compute barrier).
		m_passExecutor.addPass(*m_ddgiProbeUpdatePass);
		// Flax DDGI (R2): runs only in Flax mode; no-ops otherwise.
		m_passExecutor.addPass(*m_flaxDDGIPass);
		m_passExecutor.addPass(*m_csmShadowPass);
		m_passExecutor.addPass(*m_shadowAtlasPass);
		m_passExecutor.addPass(*m_gbufferPass);
		m_passExecutor.addPass(*m_aoPass);
		m_passExecutor.addPass(*m_ssrPass);
		m_passExecutor.addPass(*m_lightPass);
		m_passExecutor.addPass(*m_skyboxPass);
		m_passExecutor.addPass(*m_forwardPass);
#if !defined(__ANDROID__)
		m_passExecutor.addPass(*m_editorOccurrencePass);
#endif
		// DDGI (Wave D3-2): probe visualization overlay draws onto the scene
		// HDR target after all scene color is in (lighting/sky/forward) and
		// before post-processing; no-ops unless both DDGIConfig::enabled and
		// the ImGui visualize toggle are on.
		m_passExecutor.addPass(*m_ddgiDebugPass);
		m_passExecutor.addPass(*m_velocityPass);
		m_passExecutor.addPass(*m_taaResolvePass);
		m_passExecutor.addPass(*m_bloomPrefilterPass);
		m_passExecutor.addPass(*m_bloomDownsamplePass);
		m_passExecutor.addPass(*m_finalColorPass);
#if !defined(__ANDROID__)
			m_passExecutor.addPass(*m_editorHighlightPass);
			if (m_editorOverlayPass != nullptr)
			{
				m_passExecutor.addPass(
					*m_editorOverlayPass,
					PassExecutor::MissingBindingPolicy::skipPass);
				(void)m_passExecutor.setPassEnabled(*m_editorOverlayPass, false);
			}
#endif
		m_passExecutor.addPass(*m_presentPass);
		m_passExecutor.addPass(*m_imguiPass);
		bindStaticPassResources();
		m_passExecutor.bindTexture({
			.handle = kPassDepthPyramidHandle,
			.rhiTexture = m_hiZDepthPyramid.getImageHandle(),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
		initTransparentVisibilityPatchResources();
		initPhase7Resources();
		bindPhase7PassResources();
		m_globalSDFPass->initResources(getRHIDevice(), frameSlotCount);
		initDDGIResources();
		m_sortedBootstrapFrames.assign(frameSlotCount, SortedBootstrapFrameState{});
		m_rawCullingBootstrapFrames.assign(frameSlotCount, RawCullingBootstrapFrameState{});
		if (kEnableShippingVisibilitySort)
		{
			initVisibilitySortResources();
		}
		initLightingPipelines();
		initPhase7Pipelines();
	}

	void GPUDrivenRenderer::shutdown(rhi::Surface& surface)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.Shutdown");
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		bool shutdownWaitRequired = m_sceneTransactionBridge != nullptr;
#if !defined(__ANDROID__)
		{
			const std::lock_guard lock(m_editorPickingMutex);
			shutdownWaitRequired = shutdownWaitRequired ||
				m_editorPickingTarget != nullptr ||
				!m_editorPickingTickets.empty() ||
				!m_retiredEditorPickingTickets.empty() ||
				!m_unfencedEditorPickingTickets.empty();
		}
#endif
		if (shutdownWaitRequired &&
			m_renderer.sceneTransactionDeviceInitialized())
		{
			m_renderer.waitForIdle(editor::SceneDeviceIdleReason::Shutdown);
		}
		m_sceneTransactionRendererStateLease.reset();
		m_sceneTransactionBridge.reset();
		m_sceneTransactionRendererState.store(nullptr, std::memory_order_release);
		m_sceneTransactionRendererStateGeneration = 0u;
		m_sceneTransactionAppliedRegistryGeneration = 0u;
#endif

#if !defined(__ANDROID__)
				shutdownEditorPickingResources();
#endif
		shutdownLightingPipelines();
		shutdownPhase7Pipelines();
		if (kEnableShippingVisibilitySort)
		{
			shutdownVisibilitySortResources();
		}
		shutdownTransparentVisibilityPatchResources();
		shutdownPhase7Resources();
		clearDDGIMeshSDF();
		shutdownDDGIResources();
		if (m_globalSDFPass != nullptr)
		{
			m_globalSDFPass->shutdownResources();
		}
			m_sortedBootstrapFrames.clear();
			m_rawCullingBootstrapFrames.clear();
				m_passExecutor.clearResourceBindings();
#if !defined(__ANDROID__)
				if (m_editorOverlayPass != nullptr)
				{
					(void)m_passExecutor.setPassEnabled(*m_editorOverlayPass, false);
				}
				// The overlay remains registered until its own shutdown has completed;
				// this lets the lifecycle code resolve any prepared ticket before the
				// executor drops the pass entry.
				shutdownEditorOverlayResources();
#endif
				m_passExecutor.setResourceTable(nullptr);
				m_passExecutor.clear();
				m_imguiPass.reset();
		m_presentPass.reset();
#if !defined(__ANDROID__)
			m_editorOverlayPass.reset();
			m_editorHighlightPass.reset();
		m_editorOccurrencePass.reset();
#endif
		m_taaResolvePass.reset();
		m_velocityPass.reset();
		m_forwardPass.reset();
		m_ddgiDebugPass.reset();
		m_flaxDDGIPass.reset();
		m_ddgiProbeUpdatePass.reset();
		m_ddgiRayTracePass.reset();
		m_globalSDFPass.reset();
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
		resetTemporalAndPersistentTextureStates();
	}

	void GPUDrivenRenderer::resetTemporalAndPersistentTextureStates()
	{
		m_taaHistoryValid = false;
		m_taaHistoryWrittenThisFrame = false;
		m_taaHistoryWriteIndex = 0u;
		m_sceneColorHistoryStates.fill(rhi::ResourceState::General);
		m_bloomResourceStates.fill(rhi::ResourceState::General);
		m_gbufferResourceStates.fill(PersistentTextureState{});
		m_sceneColorHdrResourceState = {};
		m_velocityResourceState = {};
		m_csmResourceState = {};
	}

	void GPUDrivenRenderer::trackPersistentTextureIdentity(PersistentTextureState& textureState,
	                                                       rhi::TextureHandle texture)
	{
		if (!texture.isNull() && textureState.texture != texture)
		{
			// SceneResources initializes newly created persistent color textures to
			// General. A physical-image generation change must not inherit the old
			// image's terminal ShaderRead layout, even if Vulkan reuses a native handle.
			textureState.texture = texture;
			textureState.state = rhi::ResourceState::General;
		}
	}

	void GPUDrivenRenderer::commitPersistentTextureState(PersistentTextureState& textureState,
	                                                      rhi::TextureHandle texture,
	                                                      rhi::ResourceState terminalState)
	{
		if (!texture.isNull() && textureState.texture == texture)
		{
			textureState.state = terminalState;
		}
	}

	void GPUDrivenRenderer::bindStaticPassResources()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.BindStaticPassResources");
		RenderDevice::StaticPassTextureStates textureStates{};
		static_assert(std::tuple_size_v<decltype(textureStates.gbuffer)>
			== std::tuple_size_v<decltype(m_gbufferResourceStates)>);
		for (size_t gbufferIndex = 0; gbufferIndex < m_gbufferResourceStates.size(); ++gbufferIndex)
		{
			trackPersistentTextureIdentity(
				m_gbufferResourceStates[gbufferIndex],
				getSceneGBufferImage(static_cast<uint32_t>(gbufferIndex)));
			textureStates.gbuffer[gbufferIndex] = m_gbufferResourceStates[gbufferIndex].state;
		}

		trackPersistentTextureIdentity(m_sceneColorHdrResourceState, getSceneColorHdrImage());
		textureStates.sceneColorHdr = m_sceneColorHdrResourceState.state;
		trackPersistentTextureIdentity(m_velocityResourceState, getVelocityImage());
		textureStates.velocity = m_velocityResourceState.state;
		trackPersistentTextureIdentity(
			m_csmResourceState, getCSMShadowResources().getCascadeImage());
		textureStates.csmShadow = m_csmResourceState.state;

		m_passExecutor.setResourceTable(&m_renderer.getRHIDevice());
		m_renderer.bindStaticPassResources(m_passExecutor, textureStates);
	}

	void GPUDrivenRenderer::initFlaxDDGIResources()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitFlaxDDGI");
		if (!isFlaxStyleDDGIRequested()) return;

		waitForIdle();

		// Bridge path: the lighting shader currently samples the existing DDGI
		// probe atlases for FlaxGI, so make sure that shared probe chain exists.
		initDDGIProbeResources();
		if (!m_ddgiProbeVolume.isInitialized())
		{
			LOGW("Flax DDGI initialization skipped: shared probe resources are not ready");
			return;
		}

		const DDGIConfig& config = getDDGIConfig();
		const uint32_t cascadeCount =
			std::min(config.maxCascades, FlaxDDGIResources::kMaxCascades);
		std::vector<glm::uvec3> probesPerCascade(cascadeCount);
		for (uint32_t c = 0; c < cascadeCount; ++c) {
			probesPerCascade[c] = FlaxDDGIResources::computeProbesPerCascade(config, c, cascadeCount);
		}

		m_flaxDDGIResources.init(getRHIDevice(), config, probesPerCascade);

		if (config.enableGlobalSurfaceAtlas)
		{
			m_surfaceAtlasPass.init(getRHIDevice(), config);
			m_surfaceAtlasRasterPass.init(getRHIDevice(), m_surfaceAtlasPass,
			                              &m_renderer.getMeshPool(),
			                              &m_renderer.getSceneResources());
		}
		m_flaxDDGIPass->initResources(
			getRHIDevice(), getFrameSlotCount());
		m_ddgiDebugPass->initResources(
			getRHIDevice(), getFrameSlotCount());

		m_flaxDDGICascades.clear();
		m_flaxCascadeFrameCounters.assign(cascadeCount, 0u);
		m_flaxLastCameraPosition = glm::vec3(0.0f);
		m_flaxCascadesNeedUpdate = true;
		const auto& ppc = m_flaxDDGIResources.getProbesPerCascade();
		LOGI("Flax DDGI R1: %u cascades, probes=[%s], atlas=%s",
		     cascadeCount,
		     [&]() { std::string s; for (auto& p : ppc) s += std::to_string(p.x)+"x"+std::to_string(p.y)+"x"+std::to_string(p.z)+" "; return s; }().c_str(),
		     config.enableGlobalSurfaceAtlas ? "yes" : "no");
	}

	glm::vec3 GPUDrivenRenderer::computeFlaxCoverageCenter(const glm::vec3& cameraPosition) const
	{
		glm::vec3 coverageBoundsMin{};
		glm::vec3 coverageBoundsMax{};
		bool coverageBoundsValid = false;
		// Runtime scene bounds include the current glTF node hierarchy transforms.
		// Baked SDF bounds are static and are only a fallback when no scene is active.
		if (m_sceneView.sceneBoundsValid)
		{
			coverageBoundsMin = m_sceneView.sceneBoundsMin;
			coverageBoundsMax = m_sceneView.sceneBoundsMax;
			coverageBoundsValid = true;
		}
		else if (m_globalSDFPass != nullptr && m_globalSDFPass->getVolume().resolution > 0u)
		{
			coverageBoundsMin = m_globalSDFPass->getVolume().worldBoundsMin;
			coverageBoundsMax = m_globalSDFPass->getVolume().worldBoundsMax;
			coverageBoundsValid = true;
		}
		if (!coverageBoundsValid)
		{
			return cameraPosition;
		}

		const DDGIConfig& config = getDDGIConfig();
		const float coverageRadius = std::max(config.giDistance, config.probeSpacing);
		const auto& probesPerCascade = m_flaxDDGIResources.getProbesPerCascade();
		glm::vec3 coverageHalfExtent(coverageRadius);
		if (!probesPerCascade.empty())
		{
			const glm::vec3 probeIntervals =
				glm::max(glm::vec3(probesPerCascade.front()) - glm::vec3(1.0f), glm::vec3(0.0f));
			coverageHalfExtent = probeIntervals * config.probeSpacing * 0.5f;
		}

		const glm::vec3 sceneBoundsCenter =
			(coverageBoundsMin + coverageBoundsMax) * 0.5f;
		const glm::vec3 sceneHalfExtent =
			(coverageBoundsMax - coverageBoundsMin) * 0.5f;
		const bool compactSceneFits = glm::all(glm::lessThanEqual(sceneHalfExtent, coverageHalfExtent));
		if (compactSceneFits)
		{
			const glm::vec3 minimumCenter = coverageBoundsMax - coverageHalfExtent;
			const glm::vec3 maximumCenter = coverageBoundsMin + coverageHalfExtent;
			const glm::vec3 minimumGridCenter =
				glm::ceil(minimumCenter / config.probeSpacing) * config.probeSpacing;
			const glm::vec3 maximumGridCenter =
				glm::floor(maximumCenter / config.probeSpacing) * config.probeSpacing;
			if (glm::all(glm::lessThanEqual(minimumGridCenter, maximumGridCenter)))
			{
				const glm::vec3 preferredGridCenter =
					glm::round(cameraPosition / config.probeSpacing) * config.probeSpacing;
				return glm::clamp(
					preferredGridCenter, minimumGridCenter, maximumGridCenter);
			}
			return glm::clamp(cameraPosition, minimumCenter, maximumCenter);
		}

		const bool cameraInsideCoverageBounds =
			glm::all(glm::greaterThanEqual(cameraPosition, coverageBoundsMin))
			&& glm::all(glm::lessThanEqual(cameraPosition, coverageBoundsMax));
		if (cameraInsideCoverageBounds)
		{
			return cameraPosition;
		}

		const glm::vec3 cameraContainmentHalfExtent = glm::max(
			coverageHalfExtent - glm::vec3(config.probeSpacing),
			glm::vec3(config.probeSpacing));
		return glm::clamp(
			sceneBoundsCenter,
			cameraPosition - cameraContainmentHalfExtent,
			cameraPosition + cameraContainmentHalfExtent);
	}

	void GPUDrivenRenderer::updateFlaxCascadeScheduling(const glm::vec3& cameraPos)
	{
		if (!m_flaxDDGIResources.isInitialized()) return;

		const uint32_t cascadeCount = m_flaxDDGIResources.getCascadeCount();
		if (cascadeCount == 0) return;

		// Init counters on first call
		if (m_flaxCascadeFrameCounters.size() < cascadeCount)
			m_flaxCascadeFrameCounters.assign(cascadeCount, 0u);

		// Detect camera movement for snapping.
		// Threshold: half the smallest cascade's probe spacing.
		const float moveDist = glm::length(cameraPos - m_flaxLastCameraPosition);
		const auto& ppc = m_flaxDDGIResources.getProbesPerCascade();
		float snapThreshold = 0.0f;
		if (!ppc.empty()) {
			// Use half the first (inner) cascade's spacing as snap threshold
			const DDGIConfig& config = getDDGIConfig();
			snapThreshold = config.probeSpacing * 0.5f;
		}
		const bool cameraMoved = moveDist > snapThreshold;

		if (cameraMoved)
			m_flaxCascadesNeedUpdate = true;

		// Increment per-cascade frame counters; each cascade updates at its own frequency
		for (uint32_t c = 0; c < cascadeCount; ++c)
		{
			m_flaxCascadeFrameCounters[c]++;
		}

		const glm::vec3 coverageCenter = computeFlaxCoverageCenter(cameraPos);
		const bool historyInvalidated = FlaxDDGIResources::computeCascades(
			getDDGIConfig(), cameraPos, coverageCenter, ppc, m_flaxDDGICascades);
		if (historyInvalidated && m_flaxDDGIPass != nullptr)
		{
			m_flaxDDGIPass->requestRuntimeReset();
		}

		m_flaxLastCameraPosition = cameraPos;
	}

	void GPUDrivenRenderer::disarmFlaxSingleStep()
{
  DDGIConfig config = getEditableDDGIConfig();
  config.flaxGISingleStep = false;
  config.flaxGIFreeze = true; // re-freeze after single step
  setEditableDDGIConfig(config);
}

void GPUDrivenRenderer::rebuildSurfaceAtlasObjects()
{
	auto& dataManager = m_surfaceAtlasPass.getDataManager();
	if (dataManager.getFrameIndex() == 0)
	{
		// First registration: iterate all scene objects
		const auto& meshPool = m_renderer.getMeshPool();
		for (const auto& [meshKey, objectHandle] : m_objectHandleByMeshHandle)
		{
			// Unpack mesh key: generation in high 32, index in low 32
			const MeshHandle meshHandle{static_cast<uint32_t>(meshKey & 0xFFFFFFFFu), static_cast<uint32_t>(meshKey >> 32u)};
			const MeshRecord* meshRecord = meshPool.tryGet(meshHandle);
			if (!meshRecord) continue;

			// Only register opaque static meshes (skip transparent, alpha-tested)
			if (meshRecord->alphaMode != shaderio::LAlphaOpaque) continue;

			GlobalSurfaceAtlasObject obj{};
			obj.key = makeSurfaceAtlasObjectKey(objectHandle.index, static_cast<uint32_t>(meshKey & 0xFFFFFFFFu));
			obj.meshIndex = static_cast<uint32_t>(meshKey & 0xFFFFFFFFu);
			obj.materialIndex = meshRecord->materialIndex >= 0
				? static_cast<uint32_t>(meshRecord->materialIndex) : 0u;
			obj.boundsMin = meshRecord->worldBoundsCenter - glm::vec3(meshRecord->worldBoundsRadius);
			obj.boundsMax = meshRecord->worldBoundsCenter + glm::vec3(meshRecord->worldBoundsRadius);
			obj.boundsCenter = meshRecord->worldBoundsCenter;
			obj.boundsRadius = meshRecord->worldBoundsRadius;
			obj.transform = meshRecord->transform;
			obj.visible = true;
			obj.participates = true;
			obj.dirty = true;
			obj.lastSeenFrame = dataManager.getFrameIndex();

			dataManager.registerObject(obj);
		}
	}
	else
	{
		// Subsequent frames: mark all objects as seen
		dataManager.advanceFrame();
		dataManager.markAllSeen();
	}

	// Pack to buffer and upload to GPU
	if (dataManager.packToBuffer())
	{
		const auto& objectFloats = dataManager.getObjectBuffer();
		const auto& objectList = dataManager.getObjectList();
		if (!objectFloats.empty())
		{
			// Upload objects buffer
			rhi::BufferHandle objectsBuf = m_surfaceAtlasPass.getObjectsBuffer();
			// Upload objects list
			rhi::BufferHandle objectsListBuf = m_surfaceAtlasPass.getObjectsListBuffer();
			// For now, use immediate upload via render device
			// In production, this would be a batched upload
		}

		// Zero out chunks buffer before culling pass
		rhi::BufferHandle chunksBuf = m_surfaceAtlasPass.getChunksBuffer();
	}

	LOGI("Surface Atlas: %u objects, %u tiles",
	     dataManager.getObjectCount(), dataManager.getTileCount());
}

void GPUDrivenRenderer::shutdownFlaxDDGIResources()
	{
		waitForIdle();
		if (m_ddgiDebugPass != nullptr)
		{
			m_ddgiDebugPass->shutdownResources();
		}
		if (m_flaxDDGIPass != nullptr)
		{
			m_flaxDDGIPass->shutdownResources();
		}
		m_flaxDDGIResources.deinit();
		m_surfaceAtlasPass.deinit();
		m_surfaceAtlasRasterPass.deinit();
		m_flaxDDGICascades.clear();
		m_flaxCascadeFrameCounters.clear();
		m_flaxCascadesNeedUpdate = true;
	}

	void GPUDrivenRenderer::initDDGIProbeResources()
	{
		if (m_ddgiProbeVolume.isInitialized() || m_globalSDFPass == nullptr)
		{
			return;
		}

		glm::vec3 probeBoundsMin(-GlobalSDFPass::kDefaultHalfExtent);
		glm::vec3 probeBoundsMax(GlobalSDFPass::kDefaultHalfExtent);
		// Prefer the runtime scene AABB. Baked mesh-SDF bounds include the
		// baker's exterior padding, which can place boundary probes outside
		// closed rooms even when the probe grid itself is internally centered.
		if (m_sceneView.sceneBoundsValid)
		{
			probeBoundsMin = m_sceneView.sceneBoundsMin;
			probeBoundsMax = m_sceneView.sceneBoundsMax;
		}
		else if (m_ddgiMeshSDFLoaded)
		{
			probeBoundsMin = m_ddgiMeshSDFEntry.boundsMin;
			probeBoundsMax = m_ddgiMeshSDFEntry.boundsMax;
		}

		m_ddgiProbeVolume.init(getRHIDevice(),
		                       DDGIProbeVolume::makeDesc(getDDGIConfig(),
		                                                 probeBoundsMin,
		                                                 probeBoundsMax));
		m_renderer.getDDGIConfig().gridDims = m_ddgiProbeVolume.getGridDims();
		m_ddgiRayTracePass->initResources(getRHIDevice(), getFrameSlotCount());
		m_ddgiProbeUpdatePass->initResources(getRHIDevice());

		if (m_ddgiProbeVolume.isInitialized())
		{
			const auto createLightingAtlasView = [&](rhi::TextureHandle texture,
			                                         rhi::TextureFormat format,
			                                         const char* debugName)
			{
				rhi::TextureViewCreateDesc viewDesc{};
				viewDesc.image = texture;
				viewDesc.format = format;
				viewDesc.viewType = rhi::ImageViewType::e2D;
				viewDesc.aspect = rhi::TextureAspect::color;
				viewDesc.baseMipLevel = 0;
				viewDesc.levelCount = 1;
				viewDesc.debugName = debugName;
				return getRHIDevice().createTextureView(viewDesc);
			};
			for (uint32_t parity = 0; parity < 2u; ++parity)
			{
				m_ddgiIrradianceLightingViews[parity] = createLightingAtlasView(
					m_ddgiProbeVolume.getIrradianceAtlasWrite(parity),
					rhi::TextureFormat::rgba16Sfloat,
					parity == 0u ? "ddgi-lighting-irradiance-p0" : "ddgi-lighting-irradiance-p1");
				m_ddgiDepthLightingViews[parity] = createLightingAtlasView(
					m_ddgiProbeVolume.getDepthAtlasWrite(parity),
					rhi::TextureFormat::rg16Sfloat,
					parity == 0u ? "ddgi-lighting-depth-p0" : "ddgi-lighting-depth-p1");
			}
		}

		m_ddgiDebugPass->initResources(getRHIDevice(), getFrameSlotCount());
		if (m_ddgiMeshSDFLoaded)
		{
			m_globalSDFPass->setMeshSDFList(&m_ddgiMeshSDFEntry, 1u);
		}
	}

	void GPUDrivenRenderer::initDDGIResources()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitDDGI");
		if (!isCurrentDDGIPathEnabled())
		{
			return;
		}
		initDDGIProbeResources();
	}

	void GPUDrivenRenderer::shutdownDDGIResources()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.ShutdownDDGI");
		shutdownFlaxDDGIResources();

		if (m_ddgiDebugPass != nullptr)
		{
			m_ddgiDebugPass->shutdownResources();
		}
		if (m_ddgiProbeUpdatePass != nullptr)
		{
			m_ddgiProbeUpdatePass->shutdownResources();
		}
		if (m_ddgiRayTracePass != nullptr)
		{
			m_ddgiRayTracePass->shutdownResources();
		}

		for (uint32_t parity = 0; parity < 2u; ++parity)
		{
			if (!m_ddgiIrradianceLightingViews[parity].isNull())
			{
				getRHIDevice().destroyTextureView(m_ddgiIrradianceLightingViews[parity]);
				m_ddgiIrradianceLightingViews[parity] = {};
			}
			if (!m_ddgiDepthLightingViews[parity].isNull())
			{
				getRHIDevice().destroyTextureView(m_ddgiDepthLightingViews[parity]);
				m_ddgiDepthLightingViews[parity] = {};
			}
		}
		m_ddgiProbeVolume.deinit();
		m_renderer.getDDGIConfig().gridDims = glm::uvec3(0u);
	}

	void GPUDrivenRenderer::setDDGIEnabled(bool enabled)
	{
		if (m_renderer.getDDGIConfig().enabled == enabled)
		{
			return;
		}

		m_renderer.getDDGIConfig().enabled = enabled;
		if (enabled)
		{
			initDDGIResources();
			initFlaxDDGIResources();
		}
		else
		{
			waitForIdle();
			shutdownDDGIResources();
		}
	}

	void GPUDrivenRenderer::setEditableDDGIConfig(const DDGIConfig& config)
	{
		const DDGIConfig previousConfig = m_renderer.getDDGIConfig();
		const bool wasCurrentPathEnabled =
			previousConfig.enabled && previousConfig.runtimeMode == DDGIRuntimeMode::current;
		const bool willCurrentPathBeEnabled =
			config.enabled && config.runtimeMode == DDGIRuntimeMode::current;
		const bool wasFlaxPathEnabled =
			previousConfig.enabled && previousConfig.runtimeMode == DDGIRuntimeMode::flaxStyle;
		const bool willFlaxPathBeEnabled =
			config.enabled && config.runtimeMode == DDGIRuntimeMode::flaxStyle;
		const bool requiresResourceReset =
			previousConfig.probeSpacing != config.probeSpacing ||
			previousConfig.maxDistance != config.maxDistance ||
			previousConfig.ddgiGamma != config.ddgiGamma ||
			previousConfig.irradianceTexelSize != config.irradianceTexelSize ||
			previousConfig.depthTexelSize != config.depthTexelSize ||
			previousConfig.raysPerProbe != config.raysPerProbe ||
			previousConfig.maxCascades != config.maxCascades ||
			previousConfig.giDistance != config.giDistance ||
			previousConfig.enableGlobalSurfaceAtlas != config.enableGlobalSurfaceAtlas ||
			previousConfig.surfaceAtlasResolution != config.surfaceAtlasResolution ||
			previousConfig.surfaceAtlasTileResolution != config.surfaceAtlasTileResolution ||
			previousConfig.surfaceAtlasCoverageDistance != config.surfaceAtlasCoverageDistance;

		m_renderer.getDDGIConfig() = config;

		if (previousConfig.enabled && !config.enabled)
		{
			waitForIdle();
			shutdownDDGIResources();
			return;
		}

		// Current simple DDGI transitions
		if (wasCurrentPathEnabled && willCurrentPathBeEnabled && requiresResourceReset)
		{
			resetDDGIHistory();
		}
		else if (!wasCurrentPathEnabled && willCurrentPathBeEnabled)
		{
			initDDGIResources();
		}
		else if (wasCurrentPathEnabled && !willCurrentPathBeEnabled)
		{
			waitForIdle();
			shutdownDDGIResources();
		}

		// Flax DDGI transitions
		if (wasFlaxPathEnabled && !willFlaxPathBeEnabled)
		{
			waitForIdle();
			shutdownFlaxDDGIResources();
			if (willCurrentPathBeEnabled)
			{
				m_ddgiDebugPass->initResources(
					getRHIDevice(), getFrameSlotCount());
			}
		}
		else if (!wasFlaxPathEnabled && willFlaxPathBeEnabled)
		{
			initFlaxDDGIResources();
		}
		else if (wasFlaxPathEnabled && willFlaxPathBeEnabled && requiresResourceReset)
		{
			waitForIdle();
			shutdownDDGIResources();
			initFlaxDDGIResources();
		}
	}

	void GPUDrivenRenderer::resetDDGIHistory()
	{
		if (!isDDGIProbeDataPathEnabled())
		{
			m_temporalFrameCounter = 0;
			m_ddgiUpdateOffset = 0;
			return;
		}

		waitForIdle();
		shutdownDDGIResources();
		m_temporalFrameCounter = 0;
		m_ddgiUpdateOffset = 0;
		if (isFlaxStyleDDGIRequested())
		{
			initFlaxDDGIResources();
		}
		else
		{
			initDDGIResources();
		}
		if (m_ddgiMeshSDFLoaded && m_globalSDFPass != nullptr)
		{
			m_globalSDFPass->setMeshSDFList(&m_ddgiMeshSDFEntry, 1u);
		}
	}

	FlaxGIDebugStatus GPUDrivenRenderer::getFlaxGIDebugStatus() const
	{
		FlaxGIDebugStatus status{};
		const DDGIConfig& config = getDDGIConfig();
		status.ddgiEnabled = config.enabled;
		status.flaxRequested = config.enabled && config.runtimeMode == DDGIRuntimeMode::flaxStyle;
		status.meshSDFLoaded = m_ddgiMeshSDFLoaded;
		status.temporalFrameCounter = m_temporalFrameCounter;
		status.updateOffset = m_ddgiUpdateOffset;

		status.globalSDFPassReady = m_globalSDFPass != nullptr;
		if (m_globalSDFPass != nullptr)
		{
			const GlobalSDFVolume& volume = m_globalSDFPass->getVolume();
			status.globalSDFVolumeReady = !volume.sdfTexture.isNull() && !volume.albedoTexture.isNull()
				&& volume.resolution > 0u;
			status.globalSDFMeshCount = m_globalSDFPass->getMeshSDFCount();
			status.globalSDFBoundsMin = volume.worldBoundsMin;
			status.globalSDFBoundsMax = volume.worldBoundsMax;
			status.globalSDFResolution = volume.resolution;
		}

		status.sharedProbeVolumeReady = m_ddgiProbeVolume.isInitialized()
			&& m_ddgiProbeVolume.getTotalProbes() > 0u;
		status.probePositionReady = m_ddgiProbeVolume.getProbePositionAddress().isValid();
		status.probeGridDims = m_ddgiProbeVolume.getGridDims();
		status.totalProbes = m_ddgiProbeVolume.getTotalProbes();
		status.raysPerProbe = status.sharedProbeVolumeReady ? m_ddgiProbeVolume.getDesc().raysPerProbe : 0u;
		status.rayTracePassReady = m_ddgiRayTracePass != nullptr && m_ddgiRayTracePass->isReady();
		status.probeUpdatePassReady = m_ddgiProbeUpdatePass != nullptr && m_ddgiProbeUpdatePass->isReady();
		status.lightingAtlasViewsReady =
			!m_ddgiIrradianceLightingViews[0].isNull() && !m_ddgiIrradianceLightingViews[1].isNull()
			&& !m_ddgiDepthLightingViews[0].isNull() && !m_ddgiDepthLightingViews[1].isNull();

		status.flaxResourcesReady = m_flaxDDGIResources.isInitialized();
		status.flaxCascadeCount = m_flaxDDGIResources.getCascadeCount();
		const FlaxGIOutputSelection output = getFlaxDDGIPublishedOutputSelection();
		status.flaxOutputReady = status.flaxRequested && output.valid;
		status.flaxOutputParity = output.parity;
			const auto& ppc = m_flaxDDGIResources.getProbesPerCascade();
			status.flaxProbesPerCascade = ppc.empty() ? glm::uvec3(0u) : ppc[0];

		status.surfaceAtlasRequested = config.enabled && config.enableGlobalSurfaceAtlas;
		status.surfaceAtlasReady = m_surfaceAtlasPass.isInitialized();
		status.surfaceAtlasRasterReady = m_surfaceAtlasRasterPass.isInitialized();
		if (m_surfaceAtlasPass.isInitialized())
		{
			const GlobalSurfaceAtlasDataManager& dataManager = m_surfaceAtlasPass.getDataManager();
			status.surfaceAtlasObjects = dataManager.getObjectCount();
			status.surfaceAtlasDirtyObjects = dataManager.getDirtyObjectCount();
			status.surfaceAtlasTiles = dataManager.getTileCount();
		}
		return status;
	}

	FlaxGIDebugSnapshot GPUDrivenRenderer::getFlaxGIDebugSnapshot() const
	{
		FlaxGIDebugSnapshot snapshot = m_flaxDDGIPass != nullptr
			? m_flaxDDGIPass->getDebugSnapshot() : FlaxGIDebugSnapshot{};
		const FlaxGIDebugStatus readiness = getFlaxGIDebugStatus();
		const uint64_t frame = m_temporalFrameCounter;
		const auto setStage = [&](FlaxGIDebugStage stage, FlaxGIDebugStageState state,
		                          std::string_view reason = {})
		{
			FlaxGIDebugStageStatus& status = snapshot.stages[static_cast<size_t>(stage)];
			status.state = state;
			status.lastExecutedFrame = frame;
			status.reason = reason;
		};

		setStage(FlaxGIDebugStage::meshSDFInput,
		         readiness.meshSDFLoaded ? FlaxGIDebugStageState::valid
		                                 : FlaxGIDebugStageState::blocked,
		         readiness.meshSDFLoaded ? std::string_view{}
		                                 : std::string_view{"Load a mesh SDF before running FlaxGI"});
		setStage(FlaxGIDebugStage::globalSDF,
		         readiness.globalSDFVolumeReady ? FlaxGIDebugStageState::valid
		                                         : FlaxGIDebugStageState::blocked,
		         readiness.globalSDFVolumeReady ? std::string_view{}
		                                         : std::string_view{"Global SDF volume is unavailable"});
		setStage(FlaxGIDebugStage::surfaceAtlas, FlaxGIDebugStageState::notImplemented,
		         "Raster pass is a skeleton and trace sampling is disabled");
		setStage(FlaxGIDebugStage::cascadeLayout,
		         readiness.flaxResourcesReady ? FlaxGIDebugStageState::valid
		                                      : FlaxGIDebugStageState::blocked,
		         readiness.flaxResourcesReady ? std::string_view{}
		                                      : std::string_view{"Flax-owned probe resources are unavailable"});
		snapshot.cascadeCount = readiness.flaxCascadeCount;
		snapshot.implementedCascadeCount = readiness.flaxResourcesReady
			? readiness.flaxCascadeCount : 0u;
		snapshot.totalProbes = readiness.flaxResourcesReady
			? readiness.flaxProbesPerCascade.x * readiness.flaxProbesPerCascade.y
			  * readiness.flaxProbesPerCascade.z
			: 0u;
		snapshot.raysPerProbe = getDDGIConfig().raysPerProbe;
		snapshot.surfaceAtlasSampledByTrace = false;

		const bool outputReady = readiness.flaxResourcesReady && readiness.flaxRequested
			&& readiness.flaxOutputReady;
		setStage(FlaxGIDebugStage::lightingSample,
		         outputReady ? FlaxGIDebugStageState::executed : FlaxGIDebugStageState::blocked,
		         outputReady ? std::string_view{}
		                     : std::string_view{"FlaxGI is not enabled or its atlases are unavailable"});
		setStage(FlaxGIDebugStage::finalComposite,
		         outputReady ? FlaxGIDebugStageState::executed : FlaxGIDebugStageState::blocked,
		         outputReady ? std::string_view{}
		                     : std::string_view{"Lighting sample has no valid Flax input"});
		return snapshot;
	}

	FlaxGIDebugViewSet GPUDrivenRenderer::getFlaxGIDebugViewSet() const
	{
		return m_flaxDDGIPass != nullptr ? m_flaxDDGIPass->getDebugViewSet()
		                                     : FlaxGIDebugViewSet{};
	}

	FlaxGIOutputSelection GPUDrivenRenderer::getFlaxDDGIPublishedOutputSelection() const
	{
		return m_flaxDDGIPass != nullptr
			? m_flaxDDGIPass->getPublishedOutputSelection() : FlaxGIOutputSelection{};
	}

	void GPUDrivenRenderer::clearDDGIMeshSDF()
	{
		if (m_globalSDFPass != nullptr)
		{
			m_globalSDFPass->setMeshSDFList(nullptr, 0u);
		}
		if (!m_ddgiMeshSDFEntry.sdfTexture.isNull())
		{
			waitForIdle();
			getRHIDevice().destroyTexture(m_ddgiMeshSDFEntry.sdfTexture);
		}
		if (!m_ddgiMeshSDFEntry.albedoTexture.isNull())
		{
			waitForIdle();
			getRHIDevice().destroyTexture(m_ddgiMeshSDFEntry.albedoTexture);
		}
		m_ddgiMeshSDFEntry = {};
		m_ddgiMeshSDFLoaded = false;
		m_ddgiMeshSDFPath.clear();
	}

	bool GPUDrivenRenderer::loadDDGIMeshSDF(const std::filesystem::path& path, std::string& outError)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.LoadMeshSDF");
		outError.clear();
		if (m_globalSDFPass == nullptr)
		{
			outError = "GlobalSDFPass is not initialized";
			return false;
		}

		SDFLoadResult loadResult{};
		std::vector<rhi::BufferHandle> stagingBuffers;
		executeUploadCommand([&](rhi::CommandBuffer& cmd)
		{
			loadResult = SDFLoader::load(path, getRHIDevice(), cmd, stagingBuffers);
		});
		waitForIdle();
		for (rhi::BufferHandle staging : stagingBuffers)
		{
			if (!staging.isNull())
			{
				getRHIDevice().destroyBuffer(staging);
			}
		}
		if (!loadResult.asset.isValid)
		{
			outError = loadResult.errorMessage.empty()
				           ? ("Failed to load DDGI mesh SDF: " + path.string())
				           : loadResult.errorMessage;
			return false;
		}

		clearDDGIMeshSDF();
		m_ddgiMeshSDFEntry = GlobalSDFMeshEntry{
			.boundsMin = loadResult.asset.worldBoundsMin,
			.boundsMax = loadResult.asset.worldBoundsMax,
			.resolution = loadResult.asset.resolution,
			.sdfTexture = loadResult.asset.normalizedSDFHandle,
			.albedoTexture = loadResult.asset.albedoHandle,
		};
		m_ddgiMeshSDFLoaded = true;
		m_ddgiMeshSDFPath = path.string();
		m_renderer.getDDGIConfig().enabled = true;
		resetDDGIHistory();
		if (m_globalSDFPass->getMeshSDFCount() > 0u)
		{
			const GlobalSDFVolume& globalVolume = m_globalSDFPass->getVolume();
			const MeshSDFPhysicalResolutionQuality quality = evaluateMeshSDFPhysicalResolution(
				loadResult.asset.worldBoundsMin,
				loadResult.asset.worldBoundsMax,
				loadResult.asset.resolution,
				globalVolume.worldBoundsMin,
				globalVolume.worldBoundsMax,
				globalVolume.resolution);
			if (quality.valid && quality.warningLevel != MeshSDFQualityWarningLevel::none)
			{
				const char* severity = quality.warningLevel == MeshSDFQualityWarningLevel::severe
					? "SEVERE "
					: "";
				LOGW("%sMesh SDF physical resolution warning for %s: thin surfaces and material colors may alias. "
				     "sourceResolution=%ux%ux%u sourceSpacing=(%.3f %.3f %.3f) "
				     "globalVoxel=%.3f ratio=(%.2f %.2f %.2f)",
				     severity,
				     m_ddgiMeshSDFPath.c_str(),
				     loadResult.asset.resolution.x,
				     loadResult.asset.resolution.y,
				     loadResult.asset.resolution.z,
				     quality.sourceSpacing.x,
				     quality.sourceSpacing.y,
				     quality.sourceSpacing.z,
				     quality.globalVoxelSize,
				     quality.ratio.x,
				     quality.ratio.y,
				     quality.ratio.z);
			}
		}
		LOGI("Loaded DDGI mesh SDF: %s (%ux%ux%u, v%u, albedo=%s)",
		     m_ddgiMeshSDFPath.c_str(),
		     loadResult.asset.resolution.x,
		     loadResult.asset.resolution.y,
		     loadResult.asset.resolution.z,
		     loadResult.asset.fileVersion,
		     loadResult.asset.hasAlbedoPayload ? "yes" : "white fallback");
		if (!loadResult.asset.hasAlbedoPayload)
		{
			LOGW("DDGI mesh SDF has no albedo payload; probe bounce color will use white fallback. "
			     "Rebake the asset with the current sdf_baker to get material-colored indirect probes.");
		}
		return true;
	}

	void GPUDrivenRenderer::resize(rhi::Extent2D size)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.Resize");
	#if !defined(__ANDROID__)
		// A resize invalidates every borrowed overlay attachment. Resolve any
		// prepared ticket before RenderDevice rebuilds those images; if the queue
		// state is ambiguous, keep the old surface and retry on a later frame
		// instead of destroying resources behind a live ticket.
		if (m_editorOverlayPreparedFrame.has_value())
		{
			reconcileEditorOverlayFrameAfterRenderAttempt();
			if (m_editorOverlayPreparedFrame.has_value() &&
				!tryResolveEditorOverlayPreparedFrame(true))
			{
				LOGW("Editor overlay resize deferred while a frame ticket is unresolved");
				return;
			}
		}
	#endif
		const rhi::Extent2D previousSceneExtent = getSceneExtent();
		m_renderer.resize(size);
		const bool sceneResourcesRecreated =
			getSceneExtent().width != previousSceneExtent.width
			|| getSceneExtent().height != previousSceneExtent.height;
		if (sceneResourcesRecreated)
		{
			resetTemporalAndPersistentTextureStates();
		}
			m_hiZDepthPyramid.resize(getSceneExtent());
			resizePhase7Resources();
			m_passExecutor.clearResourceBindings();
#if !defined(__ANDROID__)
			if (m_editorOverlayPass != nullptr)
			{
				(void)m_passExecutor.setPassEnabled(*m_editorOverlayPass, false);
			}
#endif
			bindStaticPassResources();
		m_passExecutor.bindTexture({
			.handle = kPassDepthPyramidHandle,
			.rhiTexture = m_hiZDepthPyramid.getImageHandle(),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
		bindPhase7PassResources();
		// Size-dependent temporal images were recreated; their contents and the
		// previous Hi-Z camera relation are no longer valid across the resize.
		if (sceneResourcesRecreated)
		{
			m_hiZCameraHistoryValid = false;
		}
	}

	void GPUDrivenRenderer::waitForIdle()
	{
		m_renderer.waitForIdle();
		uploadPendingMeshletsAfterIdle();
	}

	void GPUDrivenRenderer::render(const RenderParams& params)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.Render");
		// Consume the transient editor material preview before preparing draw data.
		if (m_editorMaterialColorPreview != params.editorMaterialColorPreview)
		{
			const std::optional<editor::SceneMaterialColorPreview> previousPreview =
				m_editorMaterialColorPreview;
			m_editorMaterialColorPreview = params.editorMaterialColorPreview;
			m_persistentDrawDataDirty = true;
			m_dirtyPersistentDrawIndices.clear();

			// A color preview only changes the draw records that reference the
			// selected material.  Marking the whole persistent draw stream dirty
			// made every mouse move rebuild and upload Sponza's entire payload.
			// Keep the conservative full-upload fallback when the transaction draw
			// records are not available yet (for example during the first scene
			// publication).
			const auto markPreviewMaterial = [this](
				const std::optional<editor::SceneMaterialColorPreview>& preview)
			{
				if (!preview || !preview->valid()) return;
				const std::vector<SceneUploadResult::SceneDrawRecord>* drawRecords =
					&m_sceneDrawRecords;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
				if (m_sceneTransactionRendererStateLease != nullptr)
					drawRecords = &m_sceneTransactionRendererStateLease->upload.drawRecords;
#endif
				if (drawRecords == nullptr || drawRecords->empty()) return;
				const uint32_t count = static_cast<uint32_t>(drawRecords->size());
				for (uint32_t index = 0u; index < count; ++index)
				{
					if ((*drawRecords)[index].materialIndex == preview->materialIndex)
						m_dirtyPersistentDrawIndices.push_back(index);
				}
			};
			markPreviewMaterial(previousPreview);
			markPreviewMaterial(m_editorMaterialColorPreview);
		}
		// GPUDrivenRenderer owns all extent-dependent resources layered over
		// RenderDevice. Consume viewport changes before refreshing scene handles or
		// preparing temporal state, leaving RenderDevice's internal resize check as a
		// compatibility no-op for this path.
		const rhi::Extent2D currentSceneExtent = getSceneExtent();
		if (params.viewportSize.width > 0u && params.viewportSize.height > 0u
			&& (params.viewportSize.width != currentSceneExtent.width
				|| params.viewportSize.height != currentSceneExtent.height))
		{
			resize(params.viewportSize);
		}
		const bool sceneRenderingSuspended = m_suspendSceneRendering;
		m_taaHistoryWrittenThisFrame = false;
		{
			m_hiZDepthPyramid.resize(getSceneExtent());
		}
		{
			flushPendingMeshletUpload();
		}
		{
			flushPendingSceneUploads();
		}
		{
					const rhi::TextureHandle sceneOutputBeforeRefresh =
						m_sceneView.outputImage;
					const rhi::TextureHandle sceneDepthBeforeRefresh =
						m_sceneView.sceneDepthImage;
					refreshSceneView();
					if (sceneOutputBeforeRefresh != m_sceneView.outputImage ||
						sceneDepthBeforeRefresh != m_sceneView.sceneDepthImage)
					{
						// RenderDevice may rebuild swapchain-dependent images inside
						// renderWithPassExecutor after GPUDrivenRenderer::resize returned.
						// Retire any editor target that was bound to the old device
						// image generation before recording this frame.
						invalidateEditorPickingPublication();
					}
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
		const std::span<const uint32_t> opaqueDrawIndices = getOpaqueDrawIndices();
		const std::span<const uint32_t> alphaTestDrawIndices = getAlphaTestDrawIndices();
		const std::span<const uint32_t> transparentDrawIndices = getTransparentDrawIndices();
		// history ping-pong 视图在 frame-slot-ready callback 中 refreshSceneView
		// 之后、lighting descriptor update 之前选择，确保 RenderDevice 内部的
		// resize/rebuild 不会留下 stale views。
		// TAA 奇偶性由 m_taaHistoryWriteIndex 独立维护，并且只在真正写 history 后推进；
		// 全局 m_temporalFrameCounter 继续服务 jitter/DDGI。飞行帧 ring index 不能代替
		// 任何时间奇偶性，否则交换链 ≥3 镜像时会周期性读到旧历史。
		bool csmCascadeMatricesPrepared = false;
		if (params.cameraUniforms != nullptr)
		{
			// This pre-wait call prepares the CPU cascade snapshot. GPU-driven per-frame
			// lighting data is copied only by the slot-ready callback below; the legacy
			// single CSM UBO mirror is not used as a frame-slot synchronization mechanism.
			const shaderio::CameraUniforms shadowFitCamera =
				makeUnjitteredShadowFitCamera(*params.cameraUniforms);
			getCSMShadowResources().updateCascadeMatrices(shadowFitCamera,
			                                              params.lightSettings.direction,
			                                              params.lightSettings.shadowDistance,
			                                              m_sceneView.sceneBoundsMin,
			                                              m_sceneView.sceneBoundsMax,
			                                              m_sceneView.sceneBoundsValid,
			                                              params.lightSettings.normalBias);
			csmCascadeMatricesPrepared = true;
		}
		m_runtimeStats.visibilityOwnership = GPUDrivenVisibilityOwnership::gpuOwned;
		m_runtimeStats.visibilityDiagnostics = GPUDrivenVisibilityDiagnostics{
			.safeObjectCount = safeObjectCount,
			.currentGPUCullingObjectCount = 0u,
			.previousGPUCullingObjectCount = 0u,
			.opaqueCapacity = static_cast<uint32_t>(opaqueDrawIndices.size()),
			.alphaCapacity = static_cast<uint32_t>(alphaTestDrawIndices.size()),
			.transparentCapacity = static_cast<uint32_t>(transparentDrawIndices.size()),
			.maxMobileTransparentDraws = kMobileMaxTransparentDraws,
			.transparentOrderingCpuSeeded = !transparentDrawIndices.empty(),
			.materialSortKeysCpuSeeded = true,
			.transparentCapacityOverflow = transparentDrawIndices.size() > kMobileMaxTransparentDraws,
		};
		const bool visibilitySortEnabled = kEnableShippingVisibilitySort && !sceneRenderingSuspended;
		if (visibilitySortEnabled)
		{
			{
				if (m_cachedStaticVisibilitySortTopologyVersion != m_sceneTopologyVersion)
				{
					m_cachedStaticVisibilitySortObjects.clear();
					m_cachedStaticVisibilitySortKeys.clear();
					const size_t staticSortInputCount =
						opaqueDrawIndices.size() + alphaTestDrawIndices.size();
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

							const MeshRecord* mesh = nullptr;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
							mesh = m_renderer.tryGetSceneTransactionMesh(meshHandle);
#endif
							if (mesh == nullptr) mesh = m_renderer.getMeshPool().tryGet(meshHandle);
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
					appendStaticDraws(opaqueDrawIndices, kVisibilitySortCategoryOpaque);
					appendStaticDraws(alphaTestDrawIndices, kVisibilitySortCategoryAlpha);
					m_cachedStaticVisibilitySortTopologyVersion = m_sceneTopologyVersion;
				}

				const glm::vec3 cameraPos =
					params.cameraUniforms != nullptr ? params.cameraUniforms->cameraPosition : glm::vec3(0.0f);
				const size_t totalSortInputCount =
					m_cachedStaticVisibilitySortObjects.size() + transparentDrawIndices.size();
				m_visibilitySortInputObjects.clear();
				m_visibilitySortInputKeys.clear();
				m_visibilitySortInputObjects.reserve(m_transparentDrawIndices.size());
				m_visibilitySortInputKeys.reserve(m_transparentDrawIndices.size());
				const auto appendTransparentDraws = [&](std::span<const uint32_t> drawIndices)
				{
					for (uint32_t drawIndex : drawIndices)
					{
						MeshHandle meshHandle = kNullMeshHandle;
						if (!tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
						{
							continue;
						}

						const MeshRecord* mesh = nullptr;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
						mesh = m_renderer.tryGetSceneTransactionMesh(meshHandle);
#endif
						if (mesh == nullptr) mesh = m_renderer.getMeshPool().tryGet(meshHandle);
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
				appendTransparentDraws(transparentDrawIndices);
				m_runtimeStats.batchStats.sortPassCount =
					computeBitonicSortPassCount(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
				m_runtimeStats.visibilityDiagnostics.sortInputCount =
					static_cast<uint32_t>(m_visibilitySortInputObjects.size());
				m_runtimeStats.visibilityDiagnostics.sortPaddedCount =
					m_visibilitySortInputObjects.empty()
						? 0u
						: nextPowerOfTwo(static_cast<uint32_t>(m_visibilitySortInputObjects.size()));
			}
		}
		else
		{
			m_visibilitySortInputObjects.clear();
			m_visibilitySortInputKeys.clear();
		}

		// A failed prepare path must not consume temporal history. CPU scene caches and
		// pending upload ranges remain retryable, while these frame-to-frame values are
		// restored below if RenderDevice reports that no frame was submitted.
		struct TemporalStateSnapshot
		{
			shaderio::CameraUniforms temporalCamera;
			shaderio::CameraUniforms previousCamera;
			glm::mat4 previousJitteredViewProjection;
			glm::vec3 lastHiZCameraPosition;
			glm::vec2 currentTAAJitterUv;
			glm::vec2 previousTAAJitterUv;
			float lastHiZCameraDeltaDistance;
			bool previousCameraValid;
			bool hiZCameraHistoryValid;
			bool lastHiZFastCameraFallbackTriggered;
		};
		const TemporalStateSnapshot temporalStateBeforeFrame{
			.temporalCamera = m_temporalCameraUniforms,
			.previousCamera = m_previousCameraUniforms,
			.previousJitteredViewProjection = m_previousJitteredViewProjection,
			.lastHiZCameraPosition = m_lastHiZCameraPosition,
			.currentTAAJitterUv = m_currentTAAJitterUv,
			.previousTAAJitterUv = m_previousTAAJitterUv,
			.lastHiZCameraDeltaDistance = m_lastHiZCameraDeltaDistance,
			.previousCameraValid = m_previousCameraValid,
			.hiZCameraHistoryValid = m_hiZCameraHistoryValid,
			.lastHiZFastCameraFallbackTriggered = m_lastHiZFastCameraFallbackTriggered,
		};

		RenderParams gpuParams = params;
		gpuParams.csmCascadeMatricesPrepared = csmCascadeMatricesPrepared;
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
				if (clipspace::isOrthographicProjection(m_temporalCameraUniforms.projection))
				{
					m_temporalCameraUniforms.projection[3][0] += jitterNdc.x;
					m_temporalCameraUniforms.projection[3][1] += jitterNdc.y;
				}
				else
				{
					m_temporalCameraUniforms.projection[2][0] += jitterNdc.x;
					m_temporalCameraUniforms.projection[2][1] += jitterNdc.y;
				}
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
		}

		uint32_t preparedFrameIndex = std::numeric_limits<uint32_t>::max();
		uint32_t preparedHistoryReadIndex = std::numeric_limits<uint32_t>::max();
		uint32_t preparedHistoryWriteIndex = std::numeric_limits<uint32_t>::max();
		bool preparedHistoryReadBound = false;
		bool preparedHistoryWriteBound = false;
		std::array<bool, 9> preparedBloomResourcesBound{};
		std::array<rhi::TextureHandle, 3> preparedGBufferImages{};
		rhi::TextureHandle preparedSceneColorHdrImage{};
		rhi::TextureHandle preparedVelocityImage{};
		bool preparedClusterStatsProducer = false;
		rhi::TextureHandle preparedCSMImage{};
		const bool frameSubmitted = submitPassGraph(
			gpuParams,
			[&](uint32_t frameIndex)
			{
				ASSERT(preparedFrameIndex == std::numeric_limits<uint32_t>::max(),
				       "GPU-driven frame-slot callback must run exactly once");
				ASSERT(frameIndex == getCurrentFrameIndexHint(),
				       "GPU-driven frame-slot callback must use RenderDevice's current frame index");
				const uint32_t frameSlotCount = getFrameSlotCount();
				ASSERT(frameSlotCount > 0u && frameIndex < frameSlotCount,
				       "GPU-driven callback frame index must map to the authoritative frame ring");
				ASSERT(frameSlotCount == m_lightResources.getFrameCount(),
				       "GPU-driven light resources must exactly match the RenderDevice frame ring");

				// prepareFrameResources has waited for the prior submission that owned this
				// physical slot, so its readback is complete before the slot is reused.
				m_lightResources.cacheClusterStats(frameIndex);

				// The slot has retired and is about to record a new producer frame.
				// Clear temporal publication metadata before any pass can early-out;
				// GBuffer republishes only after the current raw culling dispatch is
				// known to have been recorded for this topology generation.
				invalidateSortedBootstrapState(frameIndex);
				invalidateRawCullingBootstrapState(frameIndex);
				if (frameIndex < m_transparentVisibilityPatchFrames.size())
				{
					m_transparentVisibilityPatchFrames[frameIndex].prefixReuseBarrierPending = false;
				}

				// RenderDevice has completed any internal resize/rebuild before invoking this
				// callback. Refresh every scene handle and then rewrite all pass bindings so no
				// descriptor or PassExecutor entry can retain a destroyed pre-resize view.
				refreshSceneView();
				for (size_t gbufferIndex = 0; gbufferIndex < preparedGBufferImages.size(); ++gbufferIndex)
				{
					preparedGBufferImages[gbufferIndex] = m_sceneView.gbufferImages[gbufferIndex];
				}
				preparedSceneColorHdrImage = m_sceneView.sceneColorHdrImage;
				preparedVelocityImage = m_sceneView.velocityImage;
				preparedCSMImage = getCSMShadowResources().getCascadeImage();
				const uint32_t historyWriteIndex = m_taaHistoryWriteIndex;
				const uint32_t historyReadIndex = historyWriteIndex ^ 1u;
				preparedHistoryReadIndex = historyReadIndex;
				preparedHistoryWriteIndex = historyWriteIndex;
				m_sceneView.sceneColorHistoryReadImage = getSceneColorHistoryImage(historyReadIndex);
				m_sceneView.sceneColorHistoryReadView = getSceneColorHistoryView(historyReadIndex);
				m_sceneView.sceneColorHistoryWriteImage = getSceneColorHistoryImage(historyWriteIndex);
				m_sceneView.sceneColorHistoryWriteView = getSceneColorHistoryView(historyWriteIndex);
				bindStaticPassResources();
				m_passExecutor.bindTexture({
					.handle = kPassDepthPyramidHandle,
					.rhiTexture = m_hiZDepthPyramid.getImageHandle(),
					.aspect = rhi::TextureAspect::color,
					.initialState = rhi::ResourceState::Undefined,
					.isSwapchain = false,
				});
				bindPhase7PassResources();
				const std::array<rhi::TextureHandle, 9> bloomResources{
					getBloomHalfImage(),
					getBloomQuarterImage(),
					getBloomEighthImage(),
					getBloomSixteenthImage(),
					getBloomThirtySecondImage(),
					getBloomUpsampleSixteenthImage(),
					getBloomUpsampleEighthImage(),
					getBloomUpsampleQuarterImage(),
					getBloomOutputImage(),
				};
				for (size_t bloomIndex = 0; bloomIndex < bloomResources.size(); ++bloomIndex)
				{
					preparedBloomResourcesBound[bloomIndex] =
						!bloomResources[bloomIndex].isNull();
				}

				if (isFlaxStyleDDGIRequested() && params.cameraUniforms != nullptr)
				{
					// Scheduling advances only for a frame whose slot preparation succeeded.
					updateFlaxCascadeScheduling(params.cameraUniforms->cameraPosition);
				}

				// RenderDevice invokes this only after prepareFrameResources has waited/
				// begun frameIndex and before beginCommandRecording()/drawFrame(). Keep every
				// GPU-driven per-slot mapped write, descriptor update, and indexed pass binding here.
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
				if (m_sceneTransactionRendererStateLease != nullptr &&
					m_sceneTransactionRendererStateLease->registry != nullptr &&
					m_sceneTransactionRendererStateLease->registry->isDirty())
				{
					m_sceneTransactionRendererStateLease->registry->syncToGpu(
						m_renderer.beginCommandRecording());
				}
#endif
				preparePersistentDrawData();
				uploadPersistentDrawData(frameIndex);
				updateGPUDrivenLights(params, frameIndex);
				m_runtimeStats.visibilityDiagnostics.currentGPUCullingObjectCount =
					getGPUCullingObjectCount(frameIndex);
				m_runtimeStats.visibilityDiagnostics.previousGPUCullingObjectCount =
					getPreviousGPUCullingObjectCount(frameIndex);
				m_passExecutor.bindTexture({
					.handle = kPassVelocityHandle,
					.rhiTexture = m_sceneView.velocityImage,
					.aspect = rhi::TextureAspect::color,
					.initialState = m_velocityResourceState.state,
					.isSwapchain = false,
				});
				const rhi::TextureHandle historyReadTexture = m_sceneView.sceneColorHistoryReadImage;
				const rhi::TextureHandle historyWriteTexture = m_sceneView.sceneColorHistoryWriteImage;
				preparedHistoryReadBound = !historyReadTexture.isNull();
				preparedHistoryWriteBound = !historyWriteTexture.isNull();
				m_passExecutor.bindTexture({
					.handle = kPassSceneColorHistoryReadHandle,
					.rhiTexture = historyReadTexture,
					.aspect = rhi::TextureAspect::color,
					.initialState = m_sceneColorHistoryStates[historyReadIndex],
					.isSwapchain = false,
				});
				m_passExecutor.bindTexture({
					.handle = kPassSceneColorHistoryWriteHandle,
					.rhiTexture = historyWriteTexture,
					.aspect = rhi::TextureAspect::color,
					.initialState = m_sceneColorHistoryStates[historyWriteIndex],
					.isSwapchain = false,
				});

				if (visibilitySortEnabled)
				{
					ASSERT(frameIndex < m_visibilitySortFrames.size(),
					       "Visibility-sort resources must match the RenderDevice frame ring");
					prepareVisibilitySortInputs(frameIndex);
					if (frameIndex < m_visibilitySortFrames.size())
					{
						const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
						m_passExecutor.bindBuffer({
							.handle = kPassGPUDrivenSortKeyBufferHandle,
							.rhiBuffer = sortResources.keyBuffer,
						});
						m_passExecutor.bindBuffer({
							.handle = kPassGPUDrivenSortValueBufferHandle,
							.rhiBuffer = sortResources.valueBuffer,
						});
					}
				}
				else
				{
					m_passExecutor.bindBuffer({
						.handle = kPassGPUDrivenSortKeyBufferHandle,
						.rhiBuffer = {},
					});
					m_passExecutor.bindBuffer({
						.handle = kPassGPUDrivenSortValueBufferHandle,
						.rhiBuffer = {},
					});
				}

				updatePhase7Descriptors(frameIndex);
#if !defined(__ANDROID__)
				prepareEditorOverlayFrame(frameIndex, gpuParams);
#endif
				preparedClusterStatsProducer = gpuParams.debugOptions.enableClusteredLighting
					&& !getClusteredLightCullingPipelineHandle().isNull()
					&& !getLightCoarseCullingArgumentTable(frameIndex).isNull()
					&& !getClusterStatsBufferHandle(frameIndex).isNull()
					&& !getClusterStatsReadbackBufferHandle(frameIndex).isNull();
				preparedFrameIndex = frameIndex;
			});
			if (!frameSubmitted)
			{
#if !defined(__ANDROID__)
				reconcileEditorOverlayFrameAfterRenderAttempt();
				abandonEditorPickingRecordedFrame();
#endif
			ASSERT(preparedFrameIndex == std::numeric_limits<uint32_t>::max(),
			       "A failed frame preparation must not invoke the slot-ready callback");
			m_temporalCameraUniforms = temporalStateBeforeFrame.temporalCamera;
			m_previousCameraUniforms = temporalStateBeforeFrame.previousCamera;
			m_previousJitteredViewProjection = temporalStateBeforeFrame.previousJitteredViewProjection;
			m_lastHiZCameraPosition = temporalStateBeforeFrame.lastHiZCameraPosition;
			m_currentTAAJitterUv = temporalStateBeforeFrame.currentTAAJitterUv;
			m_previousTAAJitterUv = temporalStateBeforeFrame.previousTAAJitterUv;
			m_lastHiZCameraDeltaDistance = temporalStateBeforeFrame.lastHiZCameraDeltaDistance;
			m_previousCameraValid = temporalStateBeforeFrame.previousCameraValid;
			m_hiZCameraHistoryValid = temporalStateBeforeFrame.hiZCameraHistoryValid;
			m_lastHiZFastCameraFallbackTriggered =
				temporalStateBeforeFrame.lastHiZFastCameraFallbackTriggered;
			return;
		}
		ASSERT(preparedFrameIndex != std::numeric_limits<uint32_t>::max(),
		       "Submitted GPU-driven frame must retain its prepared frame index");
		ASSERT(preparedHistoryReadIndex < m_sceneColorHistoryStates.size()
			&& preparedHistoryWriteIndex < m_sceneColorHistoryStates.size(),
		       "Submitted GPU-driven frame must retain its physical TAA history parity");
		commitSubmittedSceneTransformHistory();
#if !defined(__ANDROID__)
		reconcileEditorOverlayFrameAfterRenderAttempt();
		publishEditorPickingFrameSubmission(
			getRHIDevice().getQueue(rhi::QueueClass::graphics) != nullptr
				? getRHIDevice().getQueue(rhi::QueueClass::graphics)
					  ->lastSubmittedToken()
				: rhi::SubmissionToken{});
#endif
		// PassExecutor evaluates dependencies before each pass executes, including
		// passes that early-out for scene suspension or disabled TAA/post options.
		// Therefore every submitted graph has these physical terminal layouts:
		// Light leaves GBuffer0-2 sampled, and FinalColor leaves SceneColorHDR and
		// Velocity sampled. A prepare/acquire no-op returns above and publishes none.
		for (size_t gbufferIndex = 0; gbufferIndex < preparedGBufferImages.size(); ++gbufferIndex)
		{
			commitPersistentTextureState(
				m_gbufferResourceStates[gbufferIndex],
				preparedGBufferImages[gbufferIndex],
				rhi::ResourceState::ShaderRead);
		}
		commitPersistentTextureState(m_sceneColorHdrResourceState,
		                             preparedSceneColorHdrImage,
		                             rhi::ResourceState::ShaderRead);
		commitPersistentTextureState(m_velocityResourceState,
		                             preparedVelocityImage,
		                             rhi::ResourceState::ShaderRead);
		commitPersistentTextureState(m_csmResourceState,
		                             preparedCSMImage,
		                             rhi::ResourceState::ShaderRead);
		// Pass dependencies still establish the physical ShaderRead terminal layout
		// for every bound history image even when TAA itself no-ops. Track layout and
		// content validity separately: only an actual resolve draw publishes history
		// content and advances the logical ping-pong parity.
		if (preparedHistoryReadBound)
		{
			m_sceneColorHistoryStates[preparedHistoryReadIndex] = rhi::ResourceState::ShaderRead;
		}
		if (preparedHistoryWriteBound)
		{
			m_sceneColorHistoryStates[preparedHistoryWriteIndex] = rhi::ResourceState::ShaderRead;
		}
		for (size_t bloomIndex = 0; bloomIndex < preparedBloomResourcesBound.size(); ++bloomIndex)
		{
			if (preparedBloomResourcesBound[bloomIndex])
			{
				m_bloomResourceStates[bloomIndex] = rhi::ResourceState::ShaderRead;
			}
		}
		if (m_taaHistoryWrittenThisFrame)
		{
			m_taaHistoryValid = true;
			m_taaHistoryWriteIndex ^= 1u;
		}
		else
		{
			m_taaHistoryValid = false;
		}
		const uint32_t frameIndex = preparedFrameIndex;
		// renderWithPassExecutor returns only after submitting the recorded graph.
		// Publish this slot's producer only now; its next cache occurs after the
		// FrameScheduler waits for this exact physical slot's submission token.
		m_lightResources.markClusterStatsProducerSubmitted(frameIndex, preparedClusterStatsProducer);
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
			if ((m_temporalFrameCounter % shaderio::LClusterStatsPeriod) == 0u)
			{
				m_lightResources.cacheClusterStats(frameIndex);
			}
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
			const uint64_t ssrBytes = estimateImageBytes(m_phase7HalfExtent, 8u) * 2u;
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
				.ssrReady = !m_ssrDenoisedView.isNull() && !m_ssrTracePipelineHandle.isNull() && !
				m_ssrDenoisePipelineHandle.isNull(),
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
				&& !m_sceneView.shadowPackedVertexBuffer.isNull()
				&& !m_sceneView.shadowPackedIndexBuffer.isNull();
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
		// DDGI (Wave D4-2): advance the staggered-update subset in lock step
		// with the temporal counter — same end-of-render() timing, so the CPU
		// push-constant fill and all pass encoding within a frame observe one
		// consistent value (mirrors the m_temporalFrameCounter contract).
		m_ddgiUpdateOffset =
			(m_ddgiUpdateOffset + 1u) % std::max(getDDGIConfig().updateStride, 1u);
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
			.rhiTexture = m_hiZDepthPyramid.getImageHandle(),
			.aspect = rhi::TextureAspect::color,
			.initialState = rhi::ResourceState::Undefined,
			.isSwapchain = false,
		});
	}

	GltfUploadResult GPUDrivenRenderer::uploadGltfModel(const GltfModel& model, rhi::CommandBuffer& cmd)
	{
		beginSceneReplacement();
		GltfUploadResult result = m_renderer.uploadGltfModel(model, cmd);
		m_activeUploadResultStorage = result;
		rebuildGPUDrivenScene(model, m_activeUploadResultStorage, cmd);
		return result;
	}

	SceneUploadResult GPUDrivenRenderer::commitSceneUploadPlan(const SceneAsset& asset,
	                                                           const SceneUploadPlan& plan,
	                                                           rhi::CommandBuffer& cmd)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.CommitSceneUploadPlan");
		beginSceneReplacement();
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

	void GPUDrivenRenderer::beginSceneReplacement()
	{
		// The meshlet and registry buffers are shared by every in-flight frame slot.
		// A replace upload must retire all readers before clearGPUDrivenScene destroys
		// or reuses those allocations. Explicit uploadGltfModelBatch remains append-only.
		m_renderer.waitForIdle();
		clearGPUDrivenScene();
		m_hiZDepthPyramid.resize(m_renderer.getSceneExtent());
	}

	void GPUDrivenRenderer::destroyGltfResources(const GltfUploadResult& result)
	{
		clearGPUDrivenScene();
		m_activeUploadResultStorage = {};
		m_activeUploadResult = nullptr;
		m_renderer.destroyGltfResources(result);
	}

	void GPUDrivenRenderer::clearShadowPackedBufferMirrors()
	{
		m_shadowPackedVertexBufferRHI = {};
		m_shadowPackedIndexBufferRHI = {};
	}

	void GPUDrivenRenderer::updateMeshTransform(MeshHandle handle, const glm::mat4& transform)
	{
		const uint64_t meshKey = packMeshHandleKey(handle);
		if (const MeshRecord* previousMeshRecord = m_renderer.getMeshPool().tryGet(handle))
		{
			m_previousTransformByMeshHandle.try_emplace(meshKey, previousMeshRecord->transform);
		}
		m_renderer.updateMeshTransform(handle, transform);

		uint32_t drawIndex = 0;
		const bool hasDrawIndex = tryGetMeshDrawIndex(handle, drawIndex);
		const auto objectHandlesIt = m_objectHandlesByMeshHandle.find(meshKey);
		const auto objectHandleIt = m_objectHandleByMeshHandle.find(meshKey);

		const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(handle);
		if (meshRecord == nullptr)
		{
			return;
		}
		advanceShadowContentVersion();

		const glm::vec4 boundsSphere(meshRecord->worldBoundsCenter, meshRecord->worldBoundsRadius);
		for (SceneUploadResult::SceneDrawRecord& drawRecord : m_sceneDrawRecords)
		{
			if (packMeshHandleKey(drawRecord.meshHandle) != meshKey)
			{
				continue;
			}
			drawRecord.worldTransform = transform;
			drawRecord.boundsSphere = boundsSphere;
		}
		if (m_activeUploadResult == &m_activeUploadResultStorage)
		{
			for (SceneUploadResult::SceneDrawRecord& drawRecord : m_activeUploadResultStorage.drawRecords)
			{
				if (packMeshHandleKey(drawRecord.meshHandle) == meshKey)
				{
					drawRecord.worldTransform = transform;
					drawRecord.boundsSphere = boundsSphere;
				}
			}
			for (ShadowPackedMesh& packedMesh : m_activeUploadResultStorage.shadowPackedMeshes)
			{
				if (packedMesh.meshIndex >= m_activeUploadResultStorage.meshes.size()
					|| m_activeUploadResultStorage.meshes[packedMesh.meshIndex] != handle)
				{
					continue;
				}
				updateShadowPackedMeshRuntimeState(packedMesh, transform, boundsSphere);
			}
		}
		if (objectHandlesIt == m_objectHandlesByMeshHandle.end() && objectHandleIt == m_objectHandleByMeshHandle.end())
		{
			refreshSceneView();
			return;
		}
		if (objectHandlesIt != m_objectHandlesByMeshHandle.end())
		{
			for (const GPUSceneObjectHandle objectHandle : objectHandlesIt->second)
			{
				m_sceneRegistry.updateTransform(objectHandle, transform, boundsSphere);
			}
		}
		else
		{
			m_sceneRegistry.updateTransform(objectHandleIt->second, transform, boundsSphere);
		}
		// [GPU-CULL-SPHERE] meshlet 模式下包围球变换已移至 GPU shader（shader.gpu_culling.slang
		// transformMeshletLocalBoundsSphere）。sceneRegistry.updateTransform 已更新
		// GPUSceneObject.worldMatrixRows，culling shader 会在每帧 dispatch 时实时变换。
		// m_meshletCullObjectsCpu 的 sphereCenterRadius 保留注册时快照（供调试 overlay 使用）。
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

		// Register scene objects in Global Surface Atlas (FGI-070)
		if (isFlaxStyleDDGIRequested() && getDDGIConfig().enableGlobalSurfaceAtlas)
		{
			rebuildSurfaceAtlasObjects();
		}
	}

	void GPUDrivenRenderer::updateSceneInstanceTransform(uint32_t instanceIndex, const glm::mat4& transform)
	{
		if (m_activeUploadResult != &m_activeUploadResultStorage
			|| instanceIndex >= m_activeUploadResultStorage.instanceToDrawRecord.size())
		{
			return;
		}

		const uint32_t drawRecordIndex = m_activeUploadResultStorage.instanceToDrawRecord[instanceIndex];
		if (drawRecordIndex == UINT32_MAX || drawRecordIndex >= m_activeUploadResultStorage.drawRecords.size())
		{
			return;
		}

		SceneUploadResult::SceneDrawRecord& drawRecord =
			m_activeUploadResultStorage.drawRecords[drawRecordIndex];
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
		advanceShadowContentVersion();

		if (drawRecordIndex >= m_transformHistoryByDrawRecord.size())
		{
			m_transformHistoryByDrawRecord.resize(drawRecordIndex + 1u);
			m_transformHistoryByDrawRecord[drawRecordIndex] =
				makeSubmittedTransformHistory(drawRecord.worldTransform);
		}
		SubmittedTransformHistory& transformHistory = m_transformHistoryByDrawRecord[drawRecordIndex];
		updateSubmittedTransformHistory(transformHistory, transform);

		const glm::vec4 boundsSphere = computeTransformedBoundsSphere(*meshRecord, transform);
		drawRecord.worldTransform = transformHistory.currentWorldTransform;
		drawRecord.boundsSphere = boundsSphere;
		if (drawRecordIndex < m_sceneDrawRecords.size())
		{
			m_sceneDrawRecords[drawRecordIndex].worldTransform = transformHistory.currentWorldTransform;
			m_sceneDrawRecords[drawRecordIndex].boundsSphere = boundsSphere;
		}
		for (ShadowPackedMesh& packedMesh : m_activeUploadResultStorage.shadowPackedMeshes)
		{
			if (packedMesh.drawRecordIndex == drawRecordIndex)
			{
				updateShadowPackedMeshRuntimeState(
					packedMesh,
					transformHistory.currentWorldTransform,
					transformHistory.previousWorldTransform,
					boundsSphere);
			}
		}

		const GPUSceneObjectHandle objectHandle =
			drawRecordIndex < m_objectHandleByDrawRecord.size()
				? m_objectHandleByDrawRecord[drawRecordIndex]
				: kNullGPUSceneObjectHandle;
		const bool registryUpdated = !objectHandle.isNull()
			&& m_sceneRegistry.updateTransform(
				objectHandle, transformHistory.currentWorldTransform, boundsSphere);

		bool markedDraws = false;
		if (drawRecordIndex < m_drawIndicesByDrawRecord.size())
		{
			for (const uint32_t drawIndex : m_drawIndicesByDrawRecord[drawRecordIndex])
			{
				markPersistentDrawDirty(drawIndex);
				markedDraws = true;
			}
		}
		if (!markedDraws && drawRecordIndex < m_drawIndexByDrawRecord.size()
			&& m_drawIndexByDrawRecord[drawRecordIndex] != UINT32_MAX)
		{
			markPersistentDrawDirty(m_drawIndexByDrawRecord[drawRecordIndex]);
		}
		m_sceneUploadPending = registryUpdated;
		m_runtimeStats.pendingSceneUpdates = m_sceneUploadPending ? 1u : 0u;
		refreshSceneView();
	}

	bool GPUDrivenRenderer::removeSceneInstance(uint32_t instanceIndex)
	{
		if (m_activeUploadResult != &m_activeUploadResultStorage
			|| instanceIndex >= m_activeUploadResultStorage.instanceToDrawRecord.size())
		{
			return false;
		}

		const uint32_t drawRecordIndex = m_activeUploadResultStorage.instanceToDrawRecord[instanceIndex];
		if (drawRecordIndex == UINT32_MAX
			|| drawRecordIndex >= m_activeUploadResultStorage.drawRecords.size()
			|| drawRecordIndex >= m_objectHandleByDrawRecord.size()
			|| drawRecordIndex >= m_drawIndicesByDrawRecord.size())
		{
			return false;
		}

		const GPUSceneObjectHandle objectHandle = m_objectHandleByDrawRecord[drawRecordIndex];
		const std::vector<uint32_t>& drawIndices = m_drawIndicesByDrawRecord[drawRecordIndex];
		// Single-object removal currently has an exact topology-preserving implementation
		// only for the per-instance meshlet stream. Refuse before mutating the registry on
		// object-level/mixed streams; callers can replace the scene to rebuild topology.
		if (objectHandle.isNull() || !m_enableExperimentalMeshletPath || drawIndices.empty())
		{
			return false;
		}
		for (const uint32_t drawIndex : drawIndices)
		{
			if (drawIndex >= m_meshletDataCpu.size()
				|| drawIndex >= m_meshletCullObjectsCpu.size()
				|| drawIndex >= m_drawIdentityByDrawIndex.size())
			{
				return false;
			}
		}

		const MeshHandle removedMeshHandle =
			m_activeUploadResultStorage.drawRecords[drawRecordIndex].meshHandle;
		const uint64_t removedMeshKey = packMeshHandleKey(removedMeshHandle);
		const GPUSceneRemoveResult removeResult = m_sceneRegistry.removeObject(objectHandle);
		if (!removeResult.removed)
		{
			return false;
		}
		if (!applySceneObjectDenseRemap(removeResult))
		{
			// Registry compaction already happened, but the raster identity graph could not
			// prove a complete remap. Retire the shared buffers and fail closed rather than
			// submit a stale dense index.
			m_renderer.waitForIdle();
			clearGPUDrivenScene();
			m_activeUploadResultStorage = {};
			return false;
		}

		const auto eraseDrawIndex = [](std::vector<uint32_t>& indices, uint32_t drawIndex)
		{
			indices.erase(std::remove(indices.begin(), indices.end(), drawIndex), indices.end());
		};
		for (const uint32_t drawIndex : drawIndices)
		{
			m_meshletDataCpu[drawIndex].objectIndex = UINT32_MAX;
			m_meshletCullObjectsCpu[drawIndex] = {};
			m_drawIdentityByDrawIndex[drawIndex] = {};
			if (drawIndex < m_meshHandleByDrawIndex.size())
			{
				m_meshHandleByDrawIndex[drawIndex] = kNullMeshHandle;
			}
			if (drawIndex < m_persistentDrawData.size())
			{
				m_persistentDrawData[drawIndex] = {};
				markPersistentDrawDirty(drawIndex);
			}
			m_previousTransformByDrawIndex.erase(drawIndex);
			eraseDrawIndex(m_opaqueDrawIndices, drawIndex);
			eraseDrawIndex(m_alphaTestDrawIndices, drawIndex);
			eraseDrawIndex(m_transparentDrawIndices, drawIndex);
		}

		m_drawIndicesByDrawRecord[drawRecordIndex].clear();
		if (drawRecordIndex < m_drawIndexByDrawRecord.size())
		{
			m_drawIndexByDrawRecord[drawRecordIndex] = UINT32_MAX;
		}
		m_objectHandleByDrawRecord[drawRecordIndex] = kNullGPUSceneObjectHandle;
		if (drawRecordIndex < m_transformHistoryByDrawRecord.size())
		{
			m_transformHistoryByDrawRecord[drawRecordIndex] = {};
		}
		if (drawRecordIndex < m_sceneDrawRecords.size())
		{
			m_sceneDrawRecords[drawRecordIndex] = {};
		}
		m_activeUploadResultStorage.drawRecords[drawRecordIndex] = {};
		for (uint32_t& mappedDrawRecord : m_activeUploadResultStorage.instanceToDrawRecord)
		{
			if (mappedDrawRecord == drawRecordIndex)
			{
				mappedDrawRecord = UINT32_MAX;
			}
		}
		for (uint32_t& mappedDrawRecord : m_activeUploadResultStorage.drawCommandToDrawRecord)
		{
			if (mappedDrawRecord == drawRecordIndex)
			{
				mappedDrawRecord = UINT32_MAX;
			}
		}

		const auto eraseDrawRecordIndex = [drawRecordIndex](std::vector<size_t>& indices)
		{
			indices.erase(
				std::remove(indices.begin(), indices.end(), static_cast<size_t>(drawRecordIndex)),
				indices.end());
		};
		eraseDrawRecordIndex(m_activeUploadResultStorage.opaqueDrawIndices);
		eraseDrawRecordIndex(m_activeUploadResultStorage.alphaTestDrawIndices);
		eraseDrawRecordIndex(m_activeUploadResultStorage.shadowCasterDrawIndices);
		for (size_t transparentIndex = 0;
		     transparentIndex < m_activeUploadResultStorage.transparentDrawIndices.size();)
		{
			if (m_activeUploadResultStorage.transparentDrawIndices[transparentIndex] != drawRecordIndex)
			{
				++transparentIndex;
				continue;
			}
			m_activeUploadResultStorage.transparentDrawIndices.erase(
				m_activeUploadResultStorage.transparentDrawIndices.begin()
				+ static_cast<std::ptrdiff_t>(transparentIndex));
			if (transparentIndex < m_activeUploadResultStorage.transparentDrawDistances.size())
			{
				m_activeUploadResultStorage.transparentDrawDistances.erase(
					m_activeUploadResultStorage.transparentDrawDistances.begin()
					+ static_cast<std::ptrdiff_t>(transparentIndex));
			}
		}
		m_activeUploadResultStorage.shadowPackedMeshes.erase(
			std::remove_if(
				m_activeUploadResultStorage.shadowPackedMeshes.begin(),
				m_activeUploadResultStorage.shadowPackedMeshes.end(),
				[drawRecordIndex](const ShadowPackedMesh& packedMesh)
				{
					return packedMesh.drawRecordIndex == drawRecordIndex;
				}),
			m_activeUploadResultStorage.shadowPackedMeshes.end());

		auto objectHandlesIt = m_objectHandlesByMeshHandle.find(removedMeshKey);
		if (objectHandlesIt != m_objectHandlesByMeshHandle.end())
		{
			auto& objectHandles = objectHandlesIt->second;
			objectHandles.erase(
				std::remove(objectHandles.begin(), objectHandles.end(), objectHandle),
				objectHandles.end());
			if (objectHandles.empty())
			{
				m_objectHandlesByMeshHandle.erase(objectHandlesIt);
			}
		}
		auto primaryObjectIt = m_objectHandleByMeshHandle.find(removedMeshKey);
		if (primaryObjectIt != m_objectHandleByMeshHandle.end() && primaryObjectIt->second == objectHandle)
		{
			const auto remainingHandlesIt = m_objectHandlesByMeshHandle.find(removedMeshKey);
			if (remainingHandlesIt != m_objectHandlesByMeshHandle.end() && !remainingHandlesIt->second.empty())
			{
				primaryObjectIt->second = remainingHandlesIt->second.front();
			}
			else
			{
				m_objectHandleByMeshHandle.erase(primaryObjectIt);
			}
		}

		uint32_t replacementDrawIndex = UINT32_MAX;
		for (uint32_t candidateDrawIndex = 0;
		     candidateDrawIndex < static_cast<uint32_t>(m_meshHandleByDrawIndex.size());
		     ++candidateDrawIndex)
		{
			if (!m_meshHandleByDrawIndex[candidateDrawIndex].isNull()
				&& packMeshHandleKey(m_meshHandleByDrawIndex[candidateDrawIndex]) == removedMeshKey)
			{
				replacementDrawIndex = candidateDrawIndex;
				break;
			}
		}
		if (replacementDrawIndex == UINT32_MAX)
		{
			m_drawIndexByMeshHandle.erase(removedMeshKey);
			m_previousTransformByMeshHandle.erase(removedMeshKey);
		}
		else
		{
			m_drawIndexByMeshHandle[removedMeshKey] = replacementDrawIndex;
		}

		markMeshletMetadataForFullRewrite();
		m_sceneUploadPending = m_sceneRegistry.isDirty();
		m_persistentDrawDataDirty = true;
		advanceSceneTopologyVersion();
		m_runtimeStats.pendingSceneUpdates = m_sceneUploadPending ? 1u : 0u;
		refreshSceneView();
		return true;
	}

	void GPUDrivenRenderer::syncActiveSceneRuntimeState(SceneUploadResult& ioResult) const
	{
		if (m_activeUploadResult != &m_activeUploadResultStorage
			|| ioResult.meshes.size() != m_activeUploadResultStorage.meshes.size())
		{
			return;
		}

		for (size_t meshIndex = 0; meshIndex < ioResult.meshes.size(); ++meshIndex)
		{
			if (ioResult.meshes[meshIndex] != m_activeUploadResultStorage.meshes[meshIndex])
			{
				return;
			}
		}

		// CPU metadata only. The GPU-driven scene view already points at the active
		// storage; RenderDevice copies this snapshot into the current shadow frame-slot
		// buffers only after that slot's fence has been waited.
		ioResult.drawRecords = m_activeUploadResultStorage.drawRecords;
		ioResult.instanceToDrawRecord = m_activeUploadResultStorage.instanceToDrawRecord;
		ioResult.drawCommandToDrawRecord = m_activeUploadResultStorage.drawCommandToDrawRecord;
		ioResult.opaqueDrawIndices = m_activeUploadResultStorage.opaqueDrawIndices;
		ioResult.alphaTestDrawIndices = m_activeUploadResultStorage.alphaTestDrawIndices;
		ioResult.transparentDrawIndices = m_activeUploadResultStorage.transparentDrawIndices;
		ioResult.shadowCasterDrawIndices = m_activeUploadResultStorage.shadowCasterDrawIndices;
		ioResult.transparentDrawDistances = m_activeUploadResultStorage.transparentDrawDistances;
		ioResult.shadowPackedMeshes = m_activeUploadResultStorage.shadowPackedMeshes;
	}

	bool GPUDrivenRenderer::tryGetMeshDrawIndex(MeshHandle handle, uint32_t& outDrawIndex) const
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			const auto& handles =
				m_sceneTransactionRendererStateLease->upload.drawMeshHandles;
			const auto found = std::find(handles.begin(), handles.end(), handle);
			if (found == handles.end()) return false;
			outDrawIndex = static_cast<uint32_t>(
				std::distance(handles.begin(), found));
			return true;
		}
#endif
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
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			const auto& handles =
				m_sceneTransactionRendererStateLease->upload.drawMeshHandles;
			if (drawIndex >= handles.size()) return false;
			outHandle = handles[drawIndex];
			return !outHandle.isNull();
		}
#endif
		if (drawIndex >= m_meshHandleByDrawIndex.size())
		{
			return false;
		}
		outHandle = m_meshHandleByDrawIndex[drawIndex];
		return !outHandle.isNull();
	}

	std::span<const uint32_t> GPUDrivenRenderer::getOpaqueDrawIndices() const
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			return m_sceneTransactionRendererStateLease->opaqueDrawIndices;
		}
#endif
		return m_opaqueDrawIndices;
	}

	std::span<const uint32_t> GPUDrivenRenderer::getAlphaTestDrawIndices() const
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			return m_sceneTransactionRendererStateLease->alphaTestDrawIndices;
		}
#endif
		return m_alphaTestDrawIndices;
	}

	std::span<const uint32_t> GPUDrivenRenderer::getTransparentDrawIndices() const
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			return m_sceneTransactionRendererStateLease->transparentDrawIndices;
		}
#endif
		return m_transparentDrawIndices;
	}

	bool GPUDrivenRenderer::hasActiveSceneTransactionState() const noexcept
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		return m_sceneTransactionRendererStateLease != nullptr;
#else
		return false;
#endif
	}

	std::span<const SceneUploadResult::SceneDrawRecord>
	GPUDrivenRenderer::getActiveSceneTransactionDrawRecords() const noexcept
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			return m_sceneTransactionRendererStateLease->upload.drawRecords;
		}
#endif
		return {};
	}

	const MeshRecord *GPUDrivenRenderer::tryGetActiveSceneTransactionMesh(
		MeshHandle handle) const noexcept
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease == nullptr || handle.isNull())
		{
			return nullptr;
		}
		const auto &resources = m_sceneTransactionRendererStateLease->deviceResources;
		if (resources == nullptr)
		{
			return nullptr;
		}
		for (const auto &entry : resources->meshes)
		{
			if (entry.handle == handle)
			{
				return &entry.record;
			}
		}
#else
		(void)handle;
#endif
		return nullptr;
	}

	std::span<const shaderio::DrawUniforms>
	GPUDrivenRenderer::getActivePersistentDrawUniforms() const noexcept
	{
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		if (m_sceneTransactionRendererStateLease != nullptr)
		{
			return m_persistentDrawData;
		}
#endif
		return {};
	}

	uint64_t GPUDrivenRenderer::packMeshHandleKey(MeshHandle handle)
	{
		return (static_cast<uint64_t>(handle.generation) << 32u) | static_cast<uint64_t>(handle.index);
	}

	void GPUDrivenRenderer::rebuildGPUDrivenScene(const GltfModel& model, const GltfUploadResult& uploadResult,
	                                              rhi::CommandBuffer& cmd)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.RebuildGltfScene");
		m_activeUploadResult = &uploadResult;

		bool appendedObjects = false;
		bool appendedMeshlets = false;

		for (size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < model.meshes.size(); ++
		     meshIndex)
		{
			const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			if (m_objectHandleByMeshHandle.find(meshKey) != m_objectHandleByMeshHandle.end())
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
			const GPUSceneObjectHandle objectHandle = m_sceneRegistry.registerObject(desc);
			m_objectHandleByMeshHandle[meshKey] = objectHandle;
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
					meshlet.objectIndex = objectDrawIndex;
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
						bindPersistentDrawIdentity(drawIndex, UINT32_MAX, objectHandle, objectDrawIndex);
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
			bindPersistentDrawIdentity(objectDrawIndex, UINT32_MAX, objectHandle, objectDrawIndex);
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

		if (appendedObjects)
		{
			advanceSceneTopologyVersion();
			m_sceneRegistry.syncToGpu(cmd);
		}
		if (m_enableExperimentalMeshletPath && appendedMeshlets)
		{
			m_meshletUploadDirty = true;
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

	void GPUDrivenRenderer::bindPersistentDrawIdentity(
		uint32_t drawIndex,
		uint32_t drawRecordIndex,
		GPUSceneObjectHandle registryObjectHandle,
		uint32_t sceneObjectIndex)
	{
		if (drawIndex >= m_drawIdentityByDrawIndex.size())
		{
			m_drawIdentityByDrawIndex.resize(drawIndex + 1u);
		}
		m_drawIdentityByDrawIndex[drawIndex] = PersistentDrawIdentity{
			.drawRecordIndex = drawRecordIndex,
			.registryObjectHandle = registryObjectHandle,
			.sceneObjectIndex = sceneObjectIndex,
		};

		if (drawRecordIndex == UINT32_MAX)
		{
			return;
		}
		if (drawRecordIndex >= m_drawIndicesByDrawRecord.size())
		{
			m_drawIndicesByDrawRecord.resize(drawRecordIndex + 1u);
		}
		m_drawIndicesByDrawRecord[drawRecordIndex].push_back(drawIndex);
		if (drawRecordIndex >= m_drawIndexByDrawRecord.size())
		{
			m_drawIndexByDrawRecord.resize(drawRecordIndex + 1u, UINT32_MAX);
		}
		if (m_drawIndexByDrawRecord[drawRecordIndex] == UINT32_MAX)
		{
			m_drawIndexByDrawRecord[drawRecordIndex] = drawIndex;
		}
	}

	void GPUDrivenRenderer::markMeshletMetadataForFullRewrite()
	{
		m_meshletUploadDirty = true;
		m_meshletMetadataFullRewriteDirty = true;
	}

	bool GPUDrivenRenderer::applySceneObjectDenseRemap(const GPUSceneRemoveResult& removeResult)
	{
		if (!removeResult.hasDenseRemap())
		{
			return true;
		}

		bool remappedAnyDraw = false;
		for (uint32_t drawIndex = 0;
		     drawIndex < static_cast<uint32_t>(m_drawIdentityByDrawIndex.size());
		     ++drawIndex)
		{
			PersistentDrawIdentity& identity = m_drawIdentityByDrawIndex[drawIndex];
			if (identity.registryObjectHandle != removeResult.movedObject)
			{
				continue;
			}
			if (drawIndex >= m_meshletDataCpu.size()
				|| (identity.sceneObjectIndex != removeResult.movedFromDenseIndex
					&& identity.sceneObjectIndex != removeResult.movedToDenseIndex)
				|| (m_meshletDataCpu[drawIndex].objectIndex != removeResult.movedFromDenseIndex
					&& m_meshletDataCpu[drawIndex].objectIndex != removeResult.movedToDenseIndex))
			{
				return false;
			}

			identity.sceneObjectIndex = removeResult.movedToDenseIndex;
			m_meshletDataCpu[drawIndex].objectIndex = removeResult.movedToDenseIndex;
			remappedAnyDraw = true;
		}
		if (remappedAnyDraw)
		{
			markMeshletMetadataForFullRewrite();
		}
		return remappedAnyDraw;
	}

	void GPUDrivenRenderer::commitSubmittedSceneTransformHistory()
	{
		bool committedTransformHistory = false;
		for (SubmittedTransformHistory& history : m_transformHistoryByDrawRecord)
		{
			if (!history.updatedSinceLastSubmit)
			{
				continue;
			}
			commitSubmittedTransformHistory(history);
			committedTransformHistory = true;
		}
		for (ShadowPackedMesh& packedMesh : m_activeUploadResultStorage.shadowPackedMeshes)
		{
			commitShadowPackedMeshSubmittedState(packedMesh);
		}
		if (committedTransformHistory)
		{
			for (shaderio::DrawUniforms& drawData : m_persistentDrawData)
			{
				drawData.prevModelMatrix = drawData.modelMatrix;
			}
		}

		const bool committedLegacyTransformHistory =
			!m_previousTransformByMeshHandle.empty() || !m_previousTransformByDrawIndex.empty();
		if (committedTransformHistory || committedLegacyTransformHistory)
		{
			m_previousTransformByMeshHandle.clear();
			m_previousTransformByDrawIndex.clear();
			m_previousTransformResetPending = true;
		}
	}

	void GPUDrivenRenderer::rebuildGPUDrivenScene(const SceneAsset& asset,
	                                              const SceneUploadPlan& plan,
	                                              const SceneUploadResult& uploadResult,
	                                              rhi::CommandBuffer& cmd)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.RebuildAssetScene");
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

		m_sceneDrawRecords = uploadResult.drawRecords;
		m_objectHandleByDrawRecord.assign(m_sceneDrawRecords.size(), kNullGPUSceneObjectHandle);
		m_drawIndexByDrawRecord.assign(m_sceneDrawRecords.size(), UINT32_MAX);
		m_drawIndicesByDrawRecord.assign(m_sceneDrawRecords.size(), {});
		m_transformHistoryByDrawRecord.clear();
		m_transformHistoryByDrawRecord.reserve(m_sceneDrawRecords.size());
		for (const SceneUploadResult::SceneDrawRecord& drawRecord : m_sceneDrawRecords)
		{
			m_transformHistoryByDrawRecord.push_back(makeSubmittedTransformHistory(drawRecord.worldTransform));
		}

		bool hasCompleteInstanceMeshletPayload =
			m_enableExperimentalMeshletPath && !uploadResult.drawRecords.empty();
		for (const SceneUploadResult::SceneDrawRecord& drawRecord : uploadResult.drawRecords)
		{
			if (!hasCompleteInstanceMeshletPayload)
			{
				break;
			}
			if (drawRecord.meshIndex >= uploadResult.meshes.size()
				|| drawRecord.meshIndex >= asset.meshes.size()
				|| drawRecord.meshIndex >= asset.meshletPayloads.size())
			{
				hasCompleteInstanceMeshletPayload = false;
				break;
			}
			const SceneAsset::MeshletPayload& payload = asset.meshletPayloads[drawRecord.meshIndex];
			hasCompleteInstanceMeshletPayload =
				!payload.meshletData.empty()
				&& payload.meshletData.size() % sizeof(shaderio::Meshlet) == 0u;
		}

		if (!hasCompleteInstanceMeshletPayload && !uploadResult.drawRecords.empty())
		{
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
				const GPUSceneObjectHandle objectHandle = m_sceneRegistry.registerObject(desc);
				if (m_objectHandleByMeshHandle.find(meshKey) == m_objectHandleByMeshHandle.end())
				{
					m_objectHandleByMeshHandle[meshKey] = objectHandle;
				}
				if (drawRecordIndex < m_objectHandleByDrawRecord.size())
				{
					m_objectHandleByDrawRecord[drawRecordIndex] = objectHandle;
				}
				if (drawRecordIndex < m_drawIndexByDrawRecord.size())
				{
					m_drawIndexByDrawRecord[drawRecordIndex] = objectDrawIndex;
				}
				m_objectHandlesByMeshHandle[meshKey].push_back(objectHandle);

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
				bindPersistentDrawIdentity(objectDrawIndex, drawRecordIndex, objectHandle, objectDrawIndex);
				appendedObjects = true;
			}

			if (appendedObjects)
			{
				advanceSceneTopologyVersion();
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

		if (hasCompleteInstanceMeshletPayload)
		{
			std::vector<uint32_t> packedIndexBaseByMesh(asset.meshletPayloads.size(), UINT32_MAX);
			for (uint32_t drawRecordIndex = 0;
			     drawRecordIndex < static_cast<uint32_t>(uploadResult.drawRecords.size());
			     ++drawRecordIndex)
			{
				const SceneUploadResult::SceneDrawRecord& drawRecord = uploadResult.drawRecords[drawRecordIndex];
				if (drawRecord.meshIndex >= uploadResult.meshes.size()
					|| drawRecord.meshIndex >= asset.meshes.size()
					|| drawRecord.meshIndex >= asset.meshletPayloads.size())
				{
					continue;
				}

				const MeshHandle meshHandle = drawRecord.meshHandle;
				const MeshRecord* meshRecord = m_renderer.getMeshPool().tryGet(meshHandle);
				if (meshRecord == nullptr)
				{
					continue;
				}
				const uint64_t meshKey = packMeshHandleKey(meshHandle);
				const SceneAsset::MeshletPayload& meshletPayload = asset.meshletPayloads[drawRecord.meshIndex];
				const uint32_t meshletCount = static_cast<uint32_t>(
					meshletPayload.meshletData.size() / sizeof(shaderio::Meshlet));
				const shaderio::Meshlet* sourceMeshlets =
					reinterpret_cast<const shaderio::Meshlet*>(meshletPayload.meshletData.data());
				const uint32_t* sourcePackedIndices =
					reinterpret_cast<const uint32_t*>(meshletPayload.indexData.data());
				const uint32_t packedIndexCount = static_cast<uint32_t>(
					meshletPayload.indexData.size() / sizeof(uint32_t));

				uint32_t flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagOcclusionCulling;
				SceneDrawBucket bucket = SceneDrawBucket::Opaque;
				if (drawRecord.alphaMode == shaderio::LAlphaBlend)
				{
					flags = shaderio::LGPUCullFlagFrustumCulling | shaderio::LGPUCullFlagTransparent;
					bucket = SceneDrawBucket::Transparent;
				}
				else if (drawRecord.alphaMode == shaderio::LAlphaMask)
				{
					flags |= shaderio::LGPUCullFlagAlphaMask;
					bucket = SceneDrawBucket::AlphaMask;
				}

				GPUSceneRegistrationDesc desc{};
				desc.meshHandle = meshHandle;
				desc.meshIndex = drawRecord.meshIndex;
				desc.materialIndex = drawRecord.materialIndex;
				desc.transform = drawRecord.worldTransform;
				desc.boundsSphere = drawRecord.boundsSphere.w > 0.0f
					                    ? drawRecord.boundsSphere
					                    : computeTransformedBoundsSphere(*meshRecord, drawRecord.worldTransform);
				desc.flags = flags;
				desc.indexCount = meshRecord->indexCount;
				desc.firstIndex = meshRecord->firstIndex;
				desc.vertexOffset = meshRecord->vertexOffset;

				const uint32_t sceneObjectIndex = m_sceneRegistry.getObjectCount();
				const GPUSceneObjectHandle objectHandle = m_sceneRegistry.registerObject(desc);
				m_objectHandleByMeshHandle.emplace(meshKey, objectHandle);
				m_objectHandlesByMeshHandle[meshKey].push_back(objectHandle);
				m_objectHandleByDrawRecord[drawRecordIndex] = objectHandle;
				appendedObjects = true;

				uint32_t& packedIndexBase = packedIndexBaseByMesh[drawRecord.meshIndex];
				if (packedIndexBase == UINT32_MAX)
				{
					packedIndexBase = static_cast<uint32_t>(m_meshletIndicesCpu.size());
					if (sourcePackedIndices != nullptr && packedIndexCount > 0u)
					{
						m_meshletIndicesCpu.insert(
							m_meshletIndicesCpu.end(), sourcePackedIndices, sourcePackedIndices + packedIndexCount);
					}
					m_runtimeStats.meshletTriangleCount += packedIndexCount / 3u;
				}

				const uint32_t baseMeshletDrawIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
				m_drawIndexByMeshHandle.emplace(meshKey, baseMeshletDrawIndex);
				if (m_meshHandleByDrawIndex.size() < baseMeshletDrawIndex + meshletCount)
				{
					m_meshHandleByDrawIndex.resize(baseMeshletDrawIndex + meshletCount, kNullMeshHandle);
				}
				for (uint32_t localMeshletIndex = 0; localMeshletIndex < meshletCount; ++localMeshletIndex)
				{
					shaderio::Meshlet meshlet = sourceMeshlets[localMeshletIndex];
					meshlet.materialIndex = drawRecord.materialIndex;
					meshlet.objectIndex = sceneObjectIndex;
					meshlet.flags = flags;
					meshlet.localIndex = localMeshletIndex;
					meshlet.indexOffset += packedIndexBase;

					const uint32_t drawIndex = static_cast<uint32_t>(m_meshletDataCpu.size());
					m_meshletDataCpu.push_back(meshlet);
					m_meshHandleByDrawIndex[drawIndex] = meshHandle;
					bindPersistentDrawIdentity(drawIndex, drawRecordIndex, objectHandle, sceneObjectIndex);
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
						.sphereCenterRadius = transformBoundsSphere(drawRecord.worldTransform, meshlet.boundsSphere),
						.indexCount = meshlet.indexCount,
						.firstIndex = meshlet.indexOffset,
						.vertexOffset = meshRecord->vertexOffset,
						.flags = flags,
					});
				}
				appendedMeshlets = appendedMeshlets || meshletCount > 0u;
			}

			if (appendedObjects)
			{
				advanceSceneTopologyVersion();
				m_sceneRegistry.syncToGpu(cmd);
			}
			if (appendedMeshlets)
			{
				m_meshletUploadDirty = true;
			}
			m_sceneUploadPending = false;
			m_persistentDrawDataDirty = true;
			m_dirtyPersistentDrawIndices.clear();
			m_runtimeStats.sceneUploadCount += 1;
			m_runtimeStats.pendingSceneUpdates = 0;
			m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
			m_runtimeStats.meshletCount = m_meshletBuffer.getMeshletCount();
			refreshSceneView();
			return;
		}

		for (size_t meshIndex = 0; meshIndex < uploadResult.meshes.size() && meshIndex < asset.meshes.size(); ++
		     meshIndex)
		{
			const MeshHandle meshHandle = uploadResult.meshes[meshIndex];
			const uint64_t meshKey = packMeshHandleKey(meshHandle);
			if (m_objectHandleByMeshHandle.find(meshKey) != m_objectHandleByMeshHandle.end())
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

			const GPUSceneObjectHandle objectHandle = m_sceneRegistry.registerObject(desc);
			m_objectHandleByMeshHandle[meshKey] = objectHandle;
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
						meshlet.objectIndex = objectDrawIndex;
						meshlet.flags = flags;
						meshlet.localIndex = localMeshletIndex;
						meshlet.indexOffset += baseIndexOffset;

						const uint32_t drawIndex = baseMeshletIndex + localMeshletIndex;
						m_meshletDataCpu.push_back(meshlet);
						m_meshHandleByDrawIndex[drawIndex] = meshHandle;
						bindPersistentDrawIdentity(drawIndex, UINT32_MAX, objectHandle, objectDrawIndex);

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
			bindPersistentDrawIdentity(objectDrawIndex, UINT32_MAX, objectHandle, objectDrawIndex);
		}

		if (appendedObjects)
		{
			advanceSceneTopologyVersion();
			m_sceneRegistry.syncToGpu(cmd);
		}
		if (m_enableExperimentalMeshletPath && appendedMeshlets)
		{
			m_meshletUploadDirty = true;
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
		advanceSceneTopologyVersion();
		clearShadowPackedBufferMirrors();
		m_sceneRegistry.clear();
		if (m_enableExperimentalMeshletPath)
		{
			m_meshletBuffer.clear();
		}
		m_meshletDataCpu.clear();
		m_meshletIndicesCpu.clear();
		m_meshletCullObjectsCpu.clear();
		m_sceneDrawRecords.clear();
		m_transformHistoryByDrawRecord.clear();
		m_drawIdentityByDrawIndex.clear();
		m_drawIndicesByDrawRecord.clear();
		m_persistentDrawData.clear();
		m_persistentDrawFrameUploads.clear();
		m_visibilitySortInputObjects.clear();
		m_visibilitySortInputKeys.clear();
		m_objectHandleByMeshHandle.clear();
		m_objectHandlesByMeshHandle.clear();
		m_drawIndexByMeshHandle.clear();
		m_previousTransformByMeshHandle.clear();
		m_previousTransformByDrawIndex.clear();
		m_objectHandleByDrawRecord.clear();
		m_drawIndexByDrawRecord.clear();
		m_meshHandleByDrawIndex.clear();
		m_opaqueDrawIndices.clear();
		m_alphaTestDrawIndices.clear();
		m_transparentDrawIndices.clear();
		m_sceneView = {};
		m_runtimeStats = {};
		m_activeUploadResult = nullptr;
		m_meshletUploadDirty = false;
		m_meshletMetadataFullRewriteDirty = false;
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
		m_taaHistoryWrittenThisFrame = false;
		m_taaHistoryWriteIndex = 0u;
		m_temporalFrameCounter = 0;
		m_ddgiUpdateOffset = 0;
	}

	void GPUDrivenRenderer::flushPendingMeshletUpload()
	{
		if (!m_enableExperimentalMeshletPath || !m_meshletUploadDirty)
		{
			return;
		}

		// GPUMeshletBuffer owns one persistently mapped GPU-visible allocation shared
		// by every frame slot. RenderDevice's normal current-slot wait only retires the
		// slot being reused; another slot may still be reading these bytes. Drain all
		// in-flight work before uploadMeshlets rewrites or reallocates the shared buffers.
		m_renderer.waitForIdle();
		uploadPendingMeshletsAfterIdle();
	}

	void GPUDrivenRenderer::uploadPendingMeshletsAfterIdle()
	{
		if (!m_enableExperimentalMeshletPath || !m_meshletUploadDirty)
		{
			return;
		}

		const GPUMeshletBuffer::MetadataUploadMode metadataUploadMode =
			m_meshletMetadataFullRewriteDirty
				? GPUMeshletBuffer::MetadataUploadMode::forceFullRewrite
				: GPUMeshletBuffer::MetadataUploadMode::incremental;
		m_meshletBuffer.uploadMeshlets(
			m_meshletDataCpu, m_meshletIndicesCpu, m_meshletCullObjectsCpu, metadataUploadMode);
		m_meshletUploadDirty = false;
		m_meshletMetadataFullRewriteDirty = false;
		m_runtimeStats.meshletCount = m_meshletBuffer.getMeshletCount();
	}

	void GPUDrivenRenderer::flushPendingSceneUploads()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.FlushSceneUploads");
		if (!m_sceneUploadPending)
		{
			return;
		}
		if (!m_sceneRegistry.isDirty())
		{
			m_sceneUploadPending = false;
			m_runtimeStats.pendingSceneUpdates = 0;
			return;
		}

		m_renderer.executeUploadCommand([this](rhi::CommandBuffer& cmdBuffer)
		{
			m_sceneRegistry.syncToGpu(cmdBuffer);
		});
		m_sceneUploadPending = false;
		m_persistentDrawDataDirty = true;
		m_dirtyPersistentDrawIndices.clear();
		m_runtimeStats.sceneUploadCount += 1;
		m_runtimeStats.pendingSceneUpdates = 0;
		m_runtimeStats.objectCount = m_sceneRegistry.getObjectCount();
	}

	void GPUDrivenRenderer::advanceSceneTopologyVersion()
	{
		++m_sceneTopologyVersion;
		advanceShadowContentVersion();
		invalidateSortedBootstrapStates();
		invalidateRawCullingBootstrapStates();
	}

	void GPUDrivenRenderer::advanceShadowContentVersion()
	{
		++m_shadowContentVersion;
		if (m_shadowContentVersion == 0u)
		{
			++m_shadowContentVersion;
		}
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
		const uint32_t frameSlotCount = getFrameSlotCount();
		ASSERT(frameSlotCount > 0u && frameIndex < frameSlotCount,
		       "Sorted bootstrap state must target the authoritative frame ring");
		if (frameSlotCount == 0u || frameIndex >= frameSlotCount)
		{
			return;
		}
		if (m_sortedBootstrapFrames.size() != frameSlotCount)
		{
			m_sortedBootstrapFrames.resize(frameSlotCount, SortedBootstrapFrameState{});
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

	void GPUDrivenRenderer::invalidateRawCullingBootstrapStates()
	{
		for (RawCullingBootstrapFrameState& frameState : m_rawCullingBootstrapFrames)
		{
			frameState = {};
		}
	}

	void GPUDrivenRenderer::invalidateRawCullingBootstrapState(uint32_t frameIndex)
	{
		if (frameIndex < m_rawCullingBootstrapFrames.size())
		{
			m_rawCullingBootstrapFrames[frameIndex] = {};
		}
	}

	void GPUDrivenRenderer::recordRawCullingBootstrapState(uint32_t frameIndex, uint32_t objectCount)
	{
		const uint32_t frameSlotCount = getFrameSlotCount();
		ASSERT(frameSlotCount > 0u && frameIndex < frameSlotCount,
		       "Raw culling bootstrap state must target the authoritative frame ring");
		if (frameSlotCount == 0u || frameIndex >= frameSlotCount)
		{
			return;
		}
		if (objectCount == 0u)
		{
			invalidateRawCullingBootstrapState(frameIndex);
			return;
		}
		if (m_rawCullingBootstrapFrames.size() != frameSlotCount)
		{
			m_rawCullingBootstrapFrames.resize(frameSlotCount, RawCullingBootstrapFrameState{});
		}

		m_rawCullingBootstrapFrames[frameIndex] = RawCullingBootstrapFrameState{
			.objectCount = objectCount,
			.sceneTopologyVersion = m_sceneTopologyVersion,
			.valid = true,
		};
	}

	GPUDrivenRenderer::PreviousRawCullingBootstrap GPUDrivenRenderer::getPreviousRawCullingBootstrap(
		uint32_t frameIndex) const
	{
		PreviousRawCullingBootstrap bootstrap{};
		const uint32_t frameSlotCount = getFrameSlotCount();
		if (frameSlotCount == 0u || frameIndex >= frameSlotCount
			|| m_rawCullingBootstrapFrames.size() != frameSlotCount)
		{
			return bootstrap;
		}

		const uint32_t previousFrameIndex = getPreviousFrameIndex(frameIndex);
		const RawCullingBootstrapFrameState& frameState = m_rawCullingBootstrapFrames[previousFrameIndex];
		if (!frameState.valid || frameState.objectCount == 0u
			|| frameState.sceneTopologyVersion != m_sceneTopologyVersion)
		{
			return bootstrap;
		}
		// Resolve the previous ring slot only after generation identity matches.
		// A topology transition therefore cannot expose stale draw-stream handles.
		const rhi::BufferHandle indirectBuffer =
			m_renderer.getPreviousGPUCullingIndirectBufferRHIHandle(frameIndex);
		const rhi::BufferHandle countBuffer =
			m_renderer.getPreviousGPUCullingDrawCountBufferRHIHandle(frameIndex);
		if (indirectBuffer.isNull() || countBuffer.isNull())
		{
			return bootstrap;
		}

		bootstrap = PreviousRawCullingBootstrap{
			.indirectBuffer = indirectBuffer,
			.countBuffer = countBuffer,
			.objectCount = frameState.objectCount,
			.valid = true,
		};		return bootstrap;
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

	void GPUDrivenRenderer::preparePersistentDrawData()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.PreparePersistentDrawData");
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
			if (drawIndex >= m_persistentDrawData.size())
			{
				return;
			}

			MeshHandle meshHandle = kNullMeshHandle;
			if (!tryGetMeshHandleForDrawIndex(drawIndex, meshHandle))
			{
				m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
				return;
			}

			const MeshRecord* mesh = nullptr;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
			mesh = m_renderer.tryGetSceneTransactionMesh(meshHandle);
#endif
			if (mesh == nullptr) mesh = m_renderer.getMeshPool().tryGet(meshHandle);
			if (mesh == nullptr)
			{
				m_persistentDrawData[drawIndex] = shaderio::DrawUniforms{};
				return;
			}

			const PersistentDrawIdentity* drawIdentity =
				drawIndex < m_drawIdentityByDrawIndex.size() ? &m_drawIdentityByDrawIndex[drawIndex] : nullptr;
			uint32_t drawRecordIndex =
				drawIdentity != nullptr ? drawIdentity->drawRecordIndex : UINT32_MAX;
			const std::vector<SceneUploadResult::SceneDrawRecord>* drawRecords =
				&m_sceneDrawRecords;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
			if (m_sceneTransactionRendererStateLease != nullptr)
			{
				drawRecords = &m_sceneTransactionRendererStateLease->upload.drawRecords;
				drawRecordIndex = drawIndex;
			}
#endif
			const SceneUploadResult::SceneDrawRecord* drawRecord =
				drawRecordIndex < drawRecords->size()
					? &(*drawRecords)[drawRecordIndex] : nullptr;
			const SubmittedTransformHistory* transformHistory =
				drawRecordIndex < m_transformHistoryByDrawRecord.size()
					? &m_transformHistoryByDrawRecord[drawRecordIndex]
					: nullptr;
			const MaterialHandle drawMaterialHandle =
				drawRecord != nullptr ? drawRecord->materialHandle : kNullMaterialHandle;
			const RenderDevice::MaterialTextureIndices drawMaterialTextures =
				drawMaterialHandle.isNull()
					? RenderDevice::MaterialTextureIndices{}
					: m_renderer.getMaterialTextureIndices(drawMaterialHandle, m_activeUploadResult);
			shaderio::DrawUniforms drawData{};
			drawData.modelMatrix = transformHistory != nullptr
				                       ? transformHistory->currentWorldTransform
				                       : (drawRecord != nullptr ? drawRecord->worldTransform : mesh->transform);
			drawData.prevModelMatrix = transformHistory != nullptr
				                           ? transformHistory->previousWorldTransform
				                           : drawData.modelMatrix;
			if (drawRecord == nullptr)
			{
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
			drawData.emissiveTextureIndex = drawMaterialHandle.isNull()
				? mesh->emissiveTextureIndex : drawMaterialTextures.emissive;
			drawData.metallicFactor = drawMaterialHandle.isNull()
				? mesh->metallicFactor : drawMaterialTextures.metallicFactor;
			drawData.roughnessFactor = drawMaterialHandle.isNull()
				? mesh->roughnessFactor : drawMaterialTextures.roughnessFactor;
			drawData.normalScale = drawMaterialHandle.isNull()
				? mesh->normalScale : drawMaterialTextures.normalScale;
			drawData.occlusionStrength = drawMaterialHandle.isNull()
				? mesh->occlusionStrength : drawMaterialTextures.occlusionStrength;
			drawData.emissiveFactor = drawMaterialHandle.isNull()
				? mesh->emissiveFactor
				: glm::vec4(drawMaterialTextures.emissiveFactor, 0.0f);
			drawData.materialWorkflow = mesh->materialWorkflow;
			drawData.alphaMode = drawMaterialHandle.isNull()
				                     ? materialSource->alphaMode
				                     : drawMaterialTextures.alphaMode;
			drawData.alphaCutoff = drawMaterialHandle.isNull()
			                       ? materialSource->alphaCutoff
			                       : drawMaterialTextures.alphaCutoff;
			if (m_editorMaterialColorPreview &&
			    m_editorMaterialColorPreview->valid() &&
			    drawRecord != nullptr &&
			    drawRecord->materialIndex ==
			        m_editorMaterialColorPreview->materialIndex)
			{
				if (m_editorMaterialColorPreview->baseColorOverridden)
					drawData.baseColorFactor = glm::vec4(
						m_editorMaterialColorPreview->baseColor[0],
						m_editorMaterialColorPreview->baseColor[1],
						m_editorMaterialColorPreview->baseColor[2],
						m_editorMaterialColorPreview->baseColor[3]);
				if (m_editorMaterialColorPreview->metallic)
					drawData.metallicFactor = *m_editorMaterialColorPreview->metallic;
				if (m_editorMaterialColorPreview->roughness)
					drawData.roughnessFactor = *m_editorMaterialColorPreview->roughness;
				if (m_editorMaterialColorPreview->emissiveColor)
					drawData.emissiveFactor = glm::vec4(
						(*m_editorMaterialColorPreview->emissiveColor)[0],
						(*m_editorMaterialColorPreview->emissiveColor)[1],
						(*m_editorMaterialColorPreview->emissiveColor)[2], 0.0f);
				if (m_editorMaterialColorPreview->alphaCutoff)
					drawData.alphaCutoff = *m_editorMaterialColorPreview->alphaCutoff;
				if (m_editorMaterialColorPreview->normalScale)
					drawData.normalScale = *m_editorMaterialColorPreview->normalScale;
				if (m_editorMaterialColorPreview->occlusionStrength)
					drawData.occlusionStrength = *m_editorMaterialColorPreview->occlusionStrength;
			}
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

		const uint32_t frameCount = getFrameSlotCount();
		ASSERT(frameCount > 0u, "Persistent draw upload state requires a non-empty frame ring");
		if (frameCount == 0u)
		{
			return;
		}
		m_persistentDrawFrameUploads.resize(frameCount);
		if (needsFullUpload)
		{
			for (PersistentDrawFrameUploadState& frameUpload : m_persistentDrawFrameUploads)
			{
				frameUpload.fullUploadPending = true;
				frameUpload.dirtyRanges.clear();
			}
		}
		else if (!dirtyRanges.empty())
		{
			const auto mergeDirtyRanges = [](std::vector<DirtyRange>& pendingRanges,
			                                 const std::vector<DirtyRange>& incomingRanges)
			{
				pendingRanges.insert(pendingRanges.end(), incomingRanges.begin(), incomingRanges.end());
				std::sort(pendingRanges.begin(), pendingRanges.end(), [](const DirtyRange& lhs, const DirtyRange& rhs)
				{
					return lhs.first < rhs.first;
				});

				size_t mergedCount = 0;
				for (const DirtyRange range : pendingRanges)
				{
					if (range.count == 0u)
					{
						continue;
					}
					if (mergedCount == 0u)
					{
						pendingRanges[mergedCount++] = range;
						continue;
					}

					DirtyRange& merged = pendingRanges[mergedCount - 1u];
					const uint64_t mergedEnd = static_cast<uint64_t>(merged.first) + merged.count;
					const uint64_t rangeEnd = static_cast<uint64_t>(range.first) + range.count;
					if (static_cast<uint64_t>(range.first) <= mergedEnd)
					{
						merged.count = static_cast<uint32_t>(std::max(mergedEnd, rangeEnd) - merged.first);
					}
					else
					{
						pendingRanges[mergedCount++] = range;
					}
				}
				pendingRanges.resize(mergedCount);
			};

			for (PersistentDrawFrameUploadState& frameUpload : m_persistentDrawFrameUploads)
			{
				if (!frameUpload.fullUploadPending)
				{
					mergeDirtyRanges(frameUpload.dirtyRanges, dirtyRanges);
				}
			}
		}
		bool hasMotionPreviousTransforms =
			!m_previousTransformByDrawIndex.empty() || !m_previousTransformByMeshHandle.empty();
		for (const SubmittedTransformHistory& history : m_transformHistoryByDrawRecord)
		{
			hasMotionPreviousTransforms = hasMotionPreviousTransforms || history.updatedSinceLastSubmit;
		}
		m_previousTransformResetPending = hasMotionPreviousTransforms && !resetPreviousTransforms;
		m_persistentDrawDataDirty = false;
		m_dirtyPersistentDrawIndices.clear();
	}

	void GPUDrivenRenderer::uploadPersistentDrawData(uint32_t frameIndex)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.UploadPersistentDrawData");
		if (!m_sceneView.usePersistentCullingObjects)
		{
			return;
		}

		ASSERT(frameIndex == getCurrentFrameIndexHint(),
		       "Persistent draw uploads must target the authoritative current frame slot");
		const uint32_t frameCount = getFrameSlotCount();
		ASSERT(frameCount > 0u && frameIndex < frameCount,
		       "Persistent draw uploads must target the authoritative frame ring");
		if (frameCount == 0u || frameIndex >= frameCount)
		{
			return;
		}
		m_persistentDrawFrameUploads.resize(frameCount);
		ASSERT(frameIndex < m_persistentDrawFrameUploads.size(),
		       "Persistent draw upload state must match the RenderDevice frame ring");

		PersistentDrawFrameUploadState& frameUpload = m_persistentDrawFrameUploads[frameIndex];
		if (m_persistentDrawData.empty())
		{
			frameUpload.fullUploadPending = false;
			frameUpload.dirtyRanges.clear();
			return;
		}

		if (frameUpload.fullUploadPending)
		{
			m_renderer.uploadMDIDrawData(frameIndex, m_persistentDrawData);
			m_renderer.uploadGBufferMDIDrawData(frameIndex, m_persistentDrawData);
			m_renderer.uploadDepthMDIDrawData(frameIndex, m_persistentDrawData);
		}
		else
		{
			for (const DirtyRange& range : frameUpload.dirtyRanges)
			{
				if (range.first >= m_persistentDrawData.size())
				{
					continue;
				}
				const uint32_t safeCount = std::min<uint32_t>(
					range.count, static_cast<uint32_t>(m_persistentDrawData.size()) - range.first);
				const std::span<const shaderio::DrawUniforms> drawRange{
					m_persistentDrawData.data() + range.first, safeCount
				};
				m_renderer.uploadMDIDrawDataRange(frameIndex, range.first, drawRange);
				m_renderer.uploadGBufferMDIDrawDataRange(frameIndex, range.first, drawRange);
				m_renderer.uploadDepthMDIDrawDataRange(frameIndex, range.first, drawRange);
			}
		}

		frameUpload.fullUploadPending = false;
		frameUpload.dirtyRanges.clear();
	}

	void GPUDrivenRenderer::refreshSceneView()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.RefreshSceneView");
		const SceneTransactionRendererState* transactionState = nullptr;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
		std::shared_ptr<const SceneTransactionRendererState> publishedState;
		for (;;)
		{
			const uint64_t generationBefore =
				m_renderer.m_sceneTransactionPublishedGeneration.load(
					std::memory_order_acquire);
			publishedState = m_sceneTransactionRendererState.load(
				std::memory_order_acquire);
			const uint64_t generationAfter =
				m_renderer.m_sceneTransactionPublishedGeneration.load(
					std::memory_order_acquire);
			if (generationBefore != generationAfter) continue;
			if (publishedState == nullptr)
			{
				if (generationAfter == 0u) break;
				continue;
			}
			if (publishedState->generation == generationAfter) break;
		}
		if (publishedState != m_sceneTransactionRendererStateLease)
		{
			const uint64_t generation = publishedState != nullptr
				? publishedState->generation : 0u;
			if (generation != m_sceneTransactionRendererStateGeneration)
			{
				m_sceneTransactionRendererStateGeneration = generation;
				m_persistentDrawDataDirty = true;
				m_previousTransformResetPending = true;
				++m_sceneTopologyVersion;
			}
			m_sceneTransactionRendererStateLease = std::move(publishedState);
		}
		if (m_sceneTransactionRendererStateLease != nullptr &&
			m_sceneTransactionRendererStateLease->registry != nullptr &&
			m_sceneTransactionAppliedRegistryGeneration !=
				m_sceneTransactionRendererStateLease->generation)
		{
			GPUSceneRegistry& registry =
				*m_sceneTransactionRendererStateLease->registry;
			for (const SceneTransactionRendererState::RegistryUpdate& update :
			     m_sceneTransactionRendererStateLease->registryUpdates)
			{
				const bool transformUpdated = registry.updateTransform(
					update.handle, update.registration.transform,
					update.registration.boundsSphere);
				const bool visibilityUpdated = registry.updateVisibility(
					update.handle, update.registration.visible);
				const bool materialUpdated = registry.updateMaterialBinding(
					update.handle, update.registration.materialIndex,
					update.registration.flags);
				ASSERT(transformUpdated && visibilityUpdated && materialUpdated,
				       "published scene registry update references a stale handle");
			}
			m_sceneTransactionAppliedRegistryGeneration =
				m_sceneTransactionRendererStateLease->generation;
		}
		transactionState = m_sceneTransactionRendererStateLease.get();
#endif
		m_sceneView.gpuSceneObjectBufferAddress = transactionState != nullptr
			? transactionState->objectBufferAddress : m_sceneRegistry.getBufferAddress();
		m_sceneView.gpuCullObjectBufferAddress = transactionState != nullptr
			? transactionState->cullBufferAddress : m_sceneRegistry.getCullBufferAddress();
		m_sceneView.gpuCullObjectBuffer = transactionState != nullptr
			? transactionState->cullBuffer : m_sceneRegistry.getCullBufferHandle();
		m_sceneView.gpuCullSceneObjectBuffer = transactionState != nullptr
			? transactionState->objectBuffer : m_sceneRegistry.getBufferHandle();
		m_sceneView.gpuCullMeshletBuffer = {};
		const std::span<const shaderio::GPUCullObject> overlayObjects =
			transactionState != nullptr
				? std::span<const shaderio::GPUCullObject>{transactionState->overlayObjects}
				: std::span<const shaderio::GPUCullObject>{m_sceneRegistry.getOverlayObjects()};
		m_sceneView.objectCount = transactionState != nullptr
			? static_cast<uint32_t>(transactionState->overlayObjects.size())
			: m_sceneRegistry.getObjectCount();
		m_sceneView.overlayObjects = overlayObjects.empty() ? nullptr : overlayObjects.data();
		m_sceneView.overlayObjectCount = static_cast<uint32_t>(overlayObjects.size());
		if (m_sceneView.overlayObjectCount > m_sceneView.objectCount)
		{
			m_sceneView.overlayObjectCount = m_sceneView.objectCount;
		}
		m_sceneView.usePersistentCullingObjects =
			m_sceneView.gpuCullObjectBuffer.isValid() && m_sceneView.objectCount > 0;
		m_sceneView.authority = m_sceneView.usePersistentCullingObjects
			                        ? GPUDrivenSceneAuthority::persistentCullObjects
			                        : GPUDrivenSceneAuthority::none;
		m_sceneView.indirectSource = m_sceneView.usePersistentCullingObjects
			                             ? GPUDrivenIndirectSourceKind::gpuCullingOpaqueIndirect
			                             : GPUDrivenIndirectSourceKind::none;
		m_sceneView.indirectCommandStride =
			m_sceneView.usePersistentCullingObjects ? m_renderer.getGPUCullingIndirectCommandStride() : 0;
		if (transactionState == nullptr && m_enableExperimentalMeshletPath &&
			m_meshletBuffer.getMeshletCount() > 0u
			&& m_meshletBuffer.getMeshletCullObjectBuffer().isValid())
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
		if (transactionState != nullptr)
		{
			m_activeUploadResult = &transactionState->upload;
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
			m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedVertexBuffer.buffer : rhi::BufferHandle{};
		m_sceneView.shadowPackedIndexBuffer =
			m_activeUploadResult != nullptr ? m_activeUploadResult->shadowPackedIndexBuffer.buffer : rhi::BufferHandle{};
		m_shadowPackedVertexBufferRHI =
			m_activeUploadResult != nullptr
				? m_activeUploadResult->shadowPackedVertexBuffer.buffer
				: rhi::BufferHandle{};
		m_shadowPackedIndexBufferRHI =
			m_activeUploadResult != nullptr
				? m_activeUploadResult->shadowPackedIndexBuffer.buffer
				: rhi::BufferHandle{};
		m_sceneView.shadowPackedMeshes =
			m_activeUploadResult != nullptr && !m_activeUploadResult->shadowPackedMeshes.empty()
				? m_activeUploadResult->shadowPackedMeshes.data()
				: nullptr;
		m_sceneView.shadowPackedMeshCount =
			m_activeUploadResult != nullptr
				? static_cast<uint32_t>(m_activeUploadResult->shadowPackedMeshes.size())
				: 0;
		m_sceneView.shadowContentVersion = m_shadowContentVersion;
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

			const std::vector<SceneUploadResult::SceneDrawRecord>& drawRecords =
				transactionState != nullptr
					? transactionState->upload.drawRecords : m_sceneDrawRecords;
			if (!m_sceneDrawRecords.empty() || !drawRecords.empty())
			{
				for (const SceneUploadResult::SceneDrawRecord& drawRecord : drawRecords)
				{
					includeBoundsSphere(boundsMin, boundsMax, boundsValid, drawRecord.boundsSphere);
				}
			}

			if (!boundsValid && !m_activeUploadResult->shadowPackedMeshes.empty())
			{
				for (const ShadowPackedMesh& packedMesh : m_activeUploadResult->shadowPackedMeshes)
				{
					includeBoundsSphere(boundsMin, boundsMax, boundsValid, packedMesh.boundsSphere);
				}
			}

			if (!boundsValid)
			{
				for (const MeshHandle meshHandle : m_activeUploadResult->meshes)
				{
					const MeshRecord* mesh = nullptr;
#if defined(MGIF_ENABLE_SCENE_TRANSACTIONS)
					mesh = m_renderer.tryGetSceneTransactionMesh(meshHandle);
#endif
					if (mesh == nullptr) mesh = m_renderer.getMeshPool().tryGet(meshHandle);
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

	void GPUDrivenRenderer::recordGBufferRawVisibilitySource(bool usedRawCullingStream, uint32_t objectCount)
	{
		m_runtimeStats.visibilityDiagnostics.gbufferUsesRawCullingStream = usedRawCullingStream;
		if (usedRawCullingStream)
		{
			m_runtimeStats.visibilityDiagnostics.sameFrameOpaqueCapacity = objectCount;
			m_runtimeStats.visibilityDiagnostics.sameFrameAlphaCapacity = objectCount;
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
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitLightingResources");
		m_lightResources.init(getRHIDevice(),
		                      GPUDrivenLightResources::CreateInfo{
			                      .maxPointLights = 256,
			                      .maxSpotLights = 128,
			                      .frameCount = getFrameSlotCount(),
		                      });

		const uint32_t frameCount = getFrameSlotCount();
		// Shared linear-clamp sampler as an owned RHI handle; createSampler registers it in
		// the resource table for the combinedImageSampler ArgumentWrites of the lighting set.
		m_linearClampSamplerHandle = getRHIDevice().createSampler(rhi::SamplerDesc{
			.magFilter = rhi::Filter::linear,
			.minFilter = rhi::Filter::linear,
			.mipmapMode = rhi::MipmapMode::linear,
			.addressModeU = rhi::AddressMode::clampToEdge,
			.addressModeV = rhi::AddressMode::clampToEdge,
			.addressModeW = rhi::AddressMode::clampToEdge,
			.debugName = "GPUDriven.linearClampSampler",
		});
		// Manual PCF must compare individual depth texels. Filtering depth first and then
		// comparing is not PCF and makes the result depend on sub-texel sampling phase.
		m_shadowPointClampSamplerHandle = getRHIDevice().createSampler(rhi::SamplerDesc{
			.magFilter = rhi::Filter::nearest,
			.minFilter = rhi::Filter::nearest,
			.mipmapMode = rhi::MipmapMode::nearest,
			.addressModeU = rhi::AddressMode::clampToEdge,
			.addressModeV = rhi::AddressMode::clampToEdge,
			.addressModeW = rhi::AddressMode::clampToEdge,
			.minLod = 0.0f,
			.maxLod = 0.0f,
			.debugName = "GPUDriven.shadowPointClampSampler",
		});
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
		std::vector<ArgumentLayoutEntry> backendLightingEntries(
			lightingEntries.begin(), lightingEntries.end());
		if (getRHIDevice().getBackendInfo().type == rhi::BackendType::metal)
		{
			backendLightingEntries.front().type = rhi::BindlessResourceType::sampledImage;
			backendLightingEntries.push_back(ArgumentLayoutEntry{
				.binding = shaderio::LBindSharedTextureSampler,
				.visibility = rhi::ShaderStage::fragment,
				.type = rhi::BindlessResourceType::sampler,
				.count = 1,
			});
		}
		m_lightingArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = backendLightingEntries.data(),
				.entryCount = static_cast<uint32_t>(backendLightingEntries.size())
			});

		// lighting-scene set (set LSetScene): camera/postProcess dynamic UBO + lighting UBO.
		// LightCoarseCullingUniforms belongs to set LSetTextures binding 4 above. Keeping
		// an unused set-1 binding here shifts every later Metal buffer index away from
		// Slang's declaration-order ABI.
		const std::array<ArgumentLayoutEntry, 3> sceneEntries{
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
					.binding = shaderio::LBindPostProcess, .visibility = rhi::ShaderStage::fragment,
					.type = rhi::BindlessResourceType::uniformBufferDynamic, .count = 1
				},
			}
		};
		m_lightingSceneArgumentLayout = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = sceneEntries.data(), .entryCount = static_cast<uint32_t>(sceneEntries.size())
			});

#ifdef DEMO_HAS_DXC_FINAL_SPIRV
		if (getRHIDevice().getBackendInfo().type == rhi::BackendType::vulkan)
		{
			constexpr uint32_t kDxcSamplerBinding = 12u;
			const std::array<ArgumentLayoutEntry, 2> finalColorEntries{
				{
					{shaderio::LBindTextures, rhi::ShaderStage::fragment,
					 rhi::BindlessResourceType::sampledImage, kGPUDrivenLightPassTextureCount},
					{kDxcSamplerBinding, rhi::ShaderStage::fragment,
					 rhi::BindlessResourceType::sampler, kGPUDrivenLightPassTextureCount},
				}
			};
			m_finalColorInputArgumentLayout = m_renderer.createArgumentLayout(
				ArgumentLayoutDesc{
					.entries = finalColorEntries.data(),
					.entryCount = static_cast<uint32_t>(finalColorEntries.size())
				});
			m_finalColorInputArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
			for (uint32_t i = 0; i < frameCount; ++i)
			{
				m_finalColorInputArgumentTables[i] = m_renderer.createPersistentArgumentTable(
					m_finalColorInputArgumentLayout, "gpu-driven-final-color-dxc-input");
			}
		}
#endif
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
		m_lightOpaqueInputArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_lightingInputArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightingArgumentLayout, "gpu-driven-lighting-input");
			m_lightOpaqueInputArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightingArgumentLayout,
				                                        "gpu-driven-lighting-opaque-input");

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
			m_renderer.updateArgumentTable(m_lightOpaqueInputArgumentTables[i], inputWrites.data(),
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
				const bool writableStorage =
					b == 2u || b == 3u || b == 6u || b == 7u || b == 8u;
				cullWrites[b] = rhi::ArgumentWrite{
					.binding = b,
					.type = (b == 4 || b == 5) ? rhi::ArgumentType::uniformBuffer : rhi::ArgumentType::storageBuffer,
					.buffer = cullBuffers[b],
					.size = cullSizes[b],
					.accessIntent = writableStorage
						                ? rhi::ArgumentAccessIntent::readWrite
						                : rhi::ArgumentAccessIntent::sampledRead,
				};
			}
			m_renderer.updateArgumentTable(m_lightCoarseCullingArgumentTables[i], cullWrites.data(),
			                               static_cast<uint32_t>(cullWrites.size()));
		}

		// Owned RHI ArgumentTables for the lighting-scene set (camera/postProcess dynamic UBO +
		// lighting UBO). Written once below from stable buffers; dynamic offsets supplied
		// at bind time via setDynamicBuffer.
		m_lightingSceneArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_lightingSceneArgumentTables[i] =
				m_renderer.createPersistentArgumentTable(m_lightingSceneArgumentLayout, "gpu-driven-lighting-scene");

			const rhi::BufferHandle transientHandle = m_renderer.getTransientBufferHandle(i);
			const rhi::BufferHandle lightingHandle = m_lightResources.getLightingUniformBuffer(i);

			const std::array<rhi::ArgumentWrite, 3> sceneWrites{
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
						.binding = shaderio::LBindPostProcess, .type = rhi::ArgumentType::uniformBuffer,
						.buffer = transientHandle, .size = sizeof(shaderio::PostProcessUniforms)
					},
				}
			};
			m_renderer.updateArgumentTable(m_lightingSceneArgumentTables[i], sceneWrites.data(),
			                               static_cast<uint32_t>(sceneWrites.size()));
		}

		m_lightPipelineArgumentLayouts = {m_lightingArgumentLayout, m_lightingSceneArgumentLayout};
		m_finalColorPipelineArgumentLayouts = {
			m_finalColorInputArgumentLayout, m_lightingSceneArgumentLayout};
	}

	void GPUDrivenRenderer::shutdownLightingResources()
	{
		if (!m_linearClampSamplerHandle.isNull())
		{
			getRHIDevice().destroySampler(m_linearClampSamplerHandle);
		}
		if (!m_shadowPointClampSamplerHandle.isNull())
		{
			getRHIDevice().destroySampler(m_shadowPointClampSamplerHandle);
		}
		m_lightingInputBufferHandles.clear();
		m_lightingSceneArgumentTables.clear();
		m_lightingInputArgumentTables.clear();
		m_lightOpaqueInputArgumentTables.clear();
		m_finalColorInputArgumentTables.clear();
		m_lightingArgumentLayout = {};
		m_finalColorInputArgumentLayout = {};
		m_lightingSceneArgumentLayout = {};
		m_linearClampSamplerHandle = {};
		m_shadowPointClampSamplerHandle = {};
		m_iblCubeSamplerHandle = {};
		m_iblLutSamplerHandle = {};
		m_lightCoarseCullingBufferHandles.clear();
		m_lightCoarseCullingArgumentTables.clear();
		m_lightCoarseCullingArgumentLayout = {};
		m_lightPipelineArgumentLayouts = {};
		m_finalColorPipelineArgumentLayouts = {};
		m_lightResources.deinit();
		m_gpuDrivenPointLights.clear();
		m_gpuDrivenSpotLights.clear();
	}

	void GPUDrivenRenderer::initIBLResources()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitIBLResources");
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

		const rhi::TextureDesc environmentDesc{
			.dimension = rhi::TextureDimension::e2D,
			.format = texture.format,
			.usage = rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::transferDst,
			.extent = {texture.width, texture.height, 1},
			.mipLevels = std::max(1u, texture.mipLevels),
			.arrayLayers = 1,
			.debugName = "IBL_Environment",
		};
		const uint32_t environmentMipLevels = environmentDesc.mipLevels;
		bool footprintValid = texture.mipOffsets.size() >= environmentMipLevels &&
			texture.mipSizes.size() >= environmentMipLevels;
		for (uint32_t level = 0u; footprintValid && level < environmentMipLevels; ++level)
		{
			const uint64_t offset = texture.mipOffsets[level];
			const uint64_t declaredBytes = texture.mipSizes[level];
			const rhi::BufferTextureCopyDesc region{
				.bufferOffset = offset,
				.aspect = rhi::TextureAspect::color,
				.mipLevel = level,
				.layerCount = 1u,
				.width = textureUploadMipExtent(texture.width, level),
				.height = textureUploadMipExtent(texture.height, level),
				.depth = 1u,
			};
			const TextureUploadValidationResult validation = validateTextureUploadRegion(
				environmentDesc,
				region,
				0u,
				static_cast<uint64_t>(texture.data.size()),
				static_cast<uint64_t>(texture.data.size()));
			footprintValid = declaredBytes != 0u && offset <= texture.data.size() &&
				declaredBytes <= texture.data.size() - offset && validation.valid() &&
				validation.requiredBytes <= declaredBytes;
		}
		if (!footprintValid)
		{
			m_iblEnvironmentStatus = "Rejected GPUDriven IBL KTX2 texture footprint";
			LOGW("%s", m_iblEnvironmentStatus.c_str());
			initFallbackSplitSumResources();
			return;
		}

		m_iblEnvironmentImage = getRHIDevice().createTexture(environmentDesc);
		const rhi::TextureHandle environmentImageHandle = m_iblEnvironmentImage;
		executeUploadCommand([&](rhi::CommandBuffer& cmdBuffer)
		{
			BatchUploadContext upload;
			upload.init(getRHIDevice(), static_cast<uint64_t>(texture.data.size()));
			const BatchUploadContext::Slice slice = upload.allocate(texture.data.size(), 1u);
			const std::span<const std::byte> payload{
				reinterpret_cast<const std::byte*>(texture.data.data()), texture.data.size()};
			upload.copyToSlices(std::span<const BatchUploadContext::Slice>(&slice, 1),
			                    std::span<const std::span<const std::byte>>(&payload, 1));
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
			cmdBuffer.resourceBarrier(std::span{&initBarrier, 1}, {});
			for (uint32_t level = 0; level < environmentMipLevels; ++level)
			{
				const uint64_t offset = texture.mipOffsets[level];
				rhi::BufferTextureCopyDesc region{
					.bufferOffset = offset,
					.texture = environmentImageHandle,
					.aspect = rhi::TextureAspect::color,
					.mipLevel = level,
					.baseArrayLayer = 0,
					.layerCount = 1,
					.width = textureUploadMipExtent(texture.width, level),
					.height = textureUploadMipExtent(texture.height, level),
					.depth = 1,
				};
				const TextureUploadValidationResult recorded = upload.recordTextureUpload(
					slice, environmentImageHandle, environmentDesc, region);
				ASSERT(recorded.valid(), "Preflighted IBL KTX mip changed before command recording");
			}
			upload.executeUploads(cmdBuffer);
			rhi::TextureBarrier sampleBarrier = initBarrier;
			sampleBarrier.before = rhi::ResourceState::TransferDst;
			sampleBarrier.after = rhi::ResourceState::ShaderRead;
			cmdBuffer.resourceBarrier(std::span{&sampleBarrier, 1}, {});
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
			getRHIDevice().destroyTexture(m_iblEnvironmentImage);
			m_iblEnvironmentImage = {};
		}
		m_iblEnvironmentFormat = rhi::TextureFormat::undefined;
		m_iblEnvironmentExtent = {};
		m_iblEnvironmentMipCount = 0;
		m_iblEnvironmentEstimatedBytes = 0;
		m_iblEnvironmentLoaded = false;
		m_cachedIBLEnvironmentEnabled = false;
		m_iblUsingFallback = true;
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
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitPostProcessResources");
		shutdownPhase7Resources();

		if (!m_renderer.hasRHIDevice())
		{
			return;
		}
		const uint32_t frameCount = getFrameSlotCount();

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

		const std::array<ArgumentLayoutEntry, 7> ssrLayoutEntries{
			{
				{0, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{1, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{2, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{3, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, 1},
				{4, rhi::ShaderStage::compute, rhi::BindlessResourceType::storageTexture, 1},
				{5, rhi::ShaderStage::compute, rhi::BindlessResourceType::uniformBuffer, 1},
				// Hi-Z depth pyramid mip array for the experimental Hi-Z SSR march
				// (ssr_trace.slang kUseHiZMarch); mirrors the GPU-culling pyramid binding.
				{6, rhi::ShaderStage::compute, rhi::BindlessResourceType::sampledImage, shaderio::LDepthPyramidMaxMips},
			}
		};
		m_ssrLayoutHandle = m_renderer.createArgumentLayout(
			ArgumentLayoutDesc{
				.entries = ssrLayoutEntries.data(), .entryCount = static_cast<uint32_t>(ssrLayoutEntries.size())
			});

		// Per-frame owned ArgumentTables for AO trace + denoise (written in updatePhase7Descriptors).
		// SSR denoise shares the same 2-sampled + 1-storage layout shape as the AO sets.
		m_aoArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		m_aoDenoiseArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		m_ssrDenoiseArgumentTables.assign(frameCount, rhi::ArgumentTableHandle{});
		for (uint32_t i = 0; i < frameCount; ++i)
		{
			m_aoArgumentTables[i] = m_renderer.createPersistentArgumentTable(m_aoArgumentLayout, "gpu-driven-ao");
			m_aoDenoiseArgumentTables[i] = m_renderer.createPersistentArgumentTable(
				m_aoArgumentLayout, "gpu-driven-ao-denoise");
			m_ssrDenoiseArgumentTables[i] = m_renderer.createPersistentArgumentTable(
				m_aoArgumentLayout, "gpu-driven-ssr-denoise");
		}

		resizePhase7Resources();
	}

	void GPUDrivenRenderer::shutdownPhase7Resources()
	{
		const auto destroyTarget = [&](rhi::TextureHandle& image, rhi::TextureViewHandle& view)
		{
			if (!view.isNull()) getRHIDevice().destroyTextureView(view);
			if (!image.isNull()) getRHIDevice().destroyTexture(image);
			view = {};
			image = {};
		};
		destroyTarget(m_aoRawImage, m_aoRawView);
		destroyTarget(m_aoDenoisedImage, m_aoDenoisedView);
		destroyTarget(m_ssrRawImage, m_ssrRawView);
		destroyTarget(m_ssrDenoisedImage, m_ssrDenoisedView);
		destroyTarget(m_shadowAtlasImage, m_shadowAtlasView);

		// Owned ArgumentTables + layouts are freed by RenderDevice::destroyArgumentTablesAndLayouts at device
		// shutdown; just drop our handle references here.
		m_aoArgumentTables.clear();
		m_aoDenoiseArgumentTables.clear();
		m_ssrDenoiseArgumentTables.clear();
		m_aoArgumentLayout = {};
		m_phase7HalfExtent = {};
		m_shadowAtlasAllocatedTiles = 0u;
	}

		void GPUDrivenRenderer::resizePhase7Resources()
	{
		if (!m_renderer.hasRHIDevice())
		{
			return;
		}
		const rhi::Extent2D sceneExtent = getSceneExtent();
		const rhi::Extent2D halfExtent{
			std::max(1u, (sceneExtent.width + 1u) / 2u),
			std::max(1u, (sceneExtent.height + 1u) / 2u)
		};
		if (m_phase7HalfExtent.width == halfExtent.width && m_phase7HalfExtent.height == halfExtent.height
			&& !m_aoRawImage.isNull() && !m_aoDenoisedImage.isNull() && !m_ssrRawImage.isNull()
			&& !m_ssrDenoisedImage.isNull() && !m_shadowAtlasImage.isNull())
		{
			return;
		}
			const auto destroyTarget = [&](rhi::TextureHandle& image, rhi::TextureViewHandle& view)
		{
			if (!view.isNull()) getRHIDevice().destroyTextureView(view);
			if (!image.isNull()) getRHIDevice().destroyTexture(image);
			view = {};
			image = {};
		};
		destroyTarget(m_aoRawImage, m_aoRawView);
		destroyTarget(m_aoDenoisedImage, m_aoDenoisedView);
		destroyTarget(m_ssrRawImage, m_ssrRawView);
		destroyTarget(m_ssrDenoisedImage, m_ssrDenoisedView);
		destroyTarget(m_shadowAtlasImage, m_shadowAtlasView);
		m_phase7HalfExtent = halfExtent;

		const auto createImageResource = [&](rhi::TextureFormat format, rhi::Extent2D extent,
		                                     rhi::TextureUsageFlags usage, rhi::TextureAspect aspect,
		                                     rhi::TextureHandle& outImage, rhi::TextureViewHandle& outView,
		                                     const rhi::ClearColorValue* clearValue)
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
				const rhi::TextureSubresourceRange clearRange{
					.aspect = aspect,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1,
				};
				const rhi::TextureBarrier barrier{
					.texture = outImage,
					.before = rhi::ResourceState::Undefined,
					.after = rhi::ResourceState::General,
					.range = clearRange,
				};
				cmdBuffer.resourceBarrier(std::span{&barrier, 1}, {});
				if (clearValue != nullptr)
				{
					cmdBuffer.clearColorTexture(outImage, clearRange, *clearValue);
					// WAW barrier: clear (transfer write) -> first shader read/write by passes.
					const rhi::TextureBarrier wawBarrier{
						.texture = outImage,
						.before = rhi::ResourceState::General,
						.after = rhi::ResourceState::General,
						.range = clearRange,
					};
					cmdBuffer.resourceBarrier(std::span{&wawBarrier, 1}, {});
				}
			});
		};

		constexpr rhi::TextureUsageFlags kStorageColorUsage =
			rhi::TextureUsageFlags::sampled | rhi::TextureUsageFlags::storage
			| rhi::TextureUsageFlags::transferDst;
		constexpr rhi::ClearColorValue kAOClearWhite{1.0f, 1.0f, 1.0f, 1.0f};
		constexpr rhi::ClearColorValue kSSRClearBlack{0.0f, 0.0f, 0.0f, 0.0f};
		createImageResource(rhi::TextureFormat::r16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_aoRawImage, m_aoRawView, &kAOClearWhite);
		createImageResource(rhi::TextureFormat::r16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_aoDenoisedImage, m_aoDenoisedView, &kAOClearWhite);
		createImageResource(rhi::TextureFormat::rgba16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_ssrRawImage, m_ssrRawView, &kSSRClearBlack);
		createImageResource(rhi::TextureFormat::rgba16Sfloat, halfExtent, kStorageColorUsage,
		                    rhi::TextureAspect::color, m_ssrDenoisedImage, m_ssrDenoisedView, &kSSRClearBlack);
		createImageResource(rhi::TextureFormat::d32Sfloat, m_shadowAtlasExtent,
		                    rhi::TextureUsageFlags::depthAttachment | rhi::TextureUsageFlags::sampled,
		                    rhi::TextureAspect::depth, m_shadowAtlasImage, m_shadowAtlasView, nullptr);
	}

		void GPUDrivenRenderer::bindPhase7PassResources()
		{
		if (!m_shadowAtlasImage.isNull())
		{
			m_passExecutor.bindTexture({
				.handle = kPassGPUDrivenShadowAtlasHandle,
				.rhiTexture = m_shadowAtlasImage,
				.aspect = rhi::TextureAspect::depth,
				.initialState = rhi::ResourceState::Undefined,
				.isSwapchain = false,

			});
		}

		const std::array<TextureHandle, 9> bloomHandles{
			kPassBloomHalfHandle,
			kPassBloomQuarterHandle,
			kPassBloomEighthHandle,
			kPassBloomSixteenthHandle,
			kPassBloomThirtySecondHandle,
			kPassBloomUpsampleSixteenthHandle,
			kPassBloomUpsampleEighthHandle,
			kPassBloomUpsampleQuarterHandle,
			kPassBloomOutputHandle,
		};
		const std::array<rhi::TextureHandle, 9> bloomImages{
			getBloomHalfImage(),
			getBloomQuarterImage(),
			getBloomEighthImage(),
			getBloomSixteenthImage(),
			getBloomThirtySecondImage(),
			getBloomUpsampleSixteenthImage(),
			getBloomUpsampleEighthImage(),
			getBloomUpsampleQuarterImage(),
			getBloomOutputImage(),
		};
		static_assert(std::tuple_size_v<decltype(bloomHandles)> == std::tuple_size_v<decltype(bloomImages)>);
		for (size_t bloomIndex = 0; bloomIndex < bloomHandles.size(); ++bloomIndex)
		{
			m_passExecutor.bindTexture({
				.handle = bloomHandles[bloomIndex],
				.rhiTexture = bloomImages[bloomIndex],
				.aspect = rhi::TextureAspect::color,
				.initialState = m_bloomResourceStates[bloomIndex],
				.isSwapchain = false,
			});
		}
	}

	void GPUDrivenRenderer::initPhase7Pipelines()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitPostProcessPipelines");
		shutdownPhase7Pipelines();
		if (!m_renderer.hasRHIDevice() || m_aoArgumentLayout.isNull() || m_ssrLayoutHandle.isNull())
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
			const std::array<rhi::RootBindingDesc, 1> rootBindings{{
				rhi::RootBindingDesc{
					.slot = kPrimaryRootConstantsSlot,
					.kind = rhi::RootBindingKind::constants,
					.visibility = rhi::ShaderStage::compute,
					.size = pushSize,
					.alignment = 4,
				},
			}};
			const rhi::PipelineBindingSchemaStorage bindingSchema{argumentLayouts, rootBindings};
			const rhi::ComputePipelineDesc desc{
				.shaderStage =
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::compute,
					.library = loadEmbeddedSpirvLibrary(
						getRHIDevice(), static_cast<const uint32_t*>(shaderData),
						shaderSize * sizeof(uint32_t), entryPoint),
					.entryPoint = entryPoint,
					},
					.bindingSchema = bindingSchema.view(),
					.threadgroupSize = {8, 8, 1},
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
		m_ssrDenoisePipelineHandle = createComputePipeline(ssr_denoise_slang, std::size(ssr_denoise_slang),
		                                                   "kernelSSRDenoise",
		                                                   m_aoArgumentLayout,
		                                                   sizeof(shaderio::GPUDrivenSSRPushConstants), 0x6304u);
#endif
	}

	void GPUDrivenRenderer::shutdownPhase7Pipelines()
	{
		getRHIDevice().destroyPipeline(m_gtaoPipelineHandle);
		getRHIDevice().destroyPipeline(m_aoDenoisePipelineHandle);
		getRHIDevice().destroyPipeline(m_ssrTracePipelineHandle);
		getRHIDevice().destroyPipeline(m_ssrDenoisePipelineHandle);
		m_gtaoPipelineHandle = {};
		m_aoDenoisePipelineHandle = {};
		m_ssrTracePipelineHandle = {};
		m_ssrDenoisePipelineHandle = {};
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

		// SSR denoise set: ssrRaw + depth sampled, ssrDenoised storage out.  ssrRaw was just
		// written by the trace dispatch via a storage binding and has no graph dependency, so
		// it stays in GENERAL layout -- declare readWrite intent like the AO denoiser's aoRaw.
		if (frameIndex < m_ssrDenoiseArgumentTables.size()
			&& !m_ssrRawView.isNull() && !m_ssrDenoisedView.isNull())
		{
			const std::array<rhi::ArgumentWrite, 3> ssrDenoiseWrites{
				{
					rhi::ArgumentWrite{
						.binding = 0, .type = rhi::ArgumentType::sampledTexture, .textureView = m_ssrRawView,
						.accessIntent = rhi::ArgumentAccessIntent::readWrite
					},
					rhi::ArgumentWrite{
						.binding = 1, .type = rhi::ArgumentType::sampledTexture, .textureView = sceneView.sceneDepthView
					},
					rhi::ArgumentWrite{
						.binding = 2, .type = rhi::ArgumentType::storageTexture, .textureView = m_ssrDenoisedView
					},
				}
			};
			m_renderer.updateArgumentTable(m_ssrDenoiseArgumentTables[frameIndex], ssrDenoiseWrites.data(),
			                               static_cast<uint32_t>(ssrDenoiseWrites.size()));
		}
	}

	rhi::ArgumentTableHandle GPUDrivenRenderer::acquireSSRTempArgumentTable(
		rhi::BufferHandle cameraBuffer, uint32_t cameraOffset)
	{
		if (m_ssrLayoutHandle.isNull() || cameraBuffer.isNull())
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

		std::array<rhi::ArgumentWrite, 6u + static_cast<size_t>(shaderio::LDepthPyramidMaxMips)> writes{
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
					.binding = 5, .type = rhi::ArgumentType::uniformBuffer, .buffer = cameraBuffer,
					.offset = cameraOffset, .size = sizeof(shaderio::CameraUniforms)
				},
			}
		};
		// Hi-Z pyramid mip array (binding 6) for the experimental Hi-Z SSR march.  The
		// pyramid image is storage-written by the depth-pyramid pass and stays in GENERAL
		// layout, so declare readWrite intent exactly like the GPU-culling binding.  When
		// the pyramid is not (yet) valid, pad every slot with the scene depth view so the
		// temporary table is still complete -- the shader's Hi-Z path is compile-time
		// disabled by default and never reads the dummy.
		const rhi::TextureViewHandle* pyramidMipViews = m_hiZDepthPyramid.getMipViewsData();
		const uint32_t pyramidMipCount = m_hiZDepthPyramid.getMipCount();
		const bool pyramidUsable = m_hiZDepthPyramid.isValid() && pyramidMipViews != nullptr && pyramidMipCount > 0;
		for (uint32_t i = 0; i < static_cast<uint32_t>(shaderio::LDepthPyramidMaxMips); ++i)
		{
			writes[6u + i] = rhi::ArgumentWrite{
				.binding = 6,
				.arrayElement = i,
				.type = rhi::ArgumentType::sampledTexture,
				.textureView = pyramidUsable
					               ? pyramidMipViews[std::min(i, pyramidMipCount - 1u)]
					               : sceneView.sceneDepthView,
				.accessIntent = rhi::ArgumentAccessIntent::readWrite,
			};
		}
		return m_renderer.createTemporaryArgumentTable(m_ssrLayoutHandle, writes.data(),
		                                               static_cast<uint32_t>(writes.size()),
		                                               ArgumentSlot::shaderSpecific, "gpu-driven-ssr-temp");
	}

	void GPUDrivenRenderer::rebuildFlaxRadianceSources()
	{
		constexpr size_t maxSources = static_cast<size_t>(shaderio::LFlaxDDGIMaxRadianceSources);
		m_flaxRadianceSources.clear();
		m_flaxRadianceSources.reserve(maxSources);

		const auto appendLocalLight = [&](const shaderio::LightData& light, uint32_t sourceType)
		{
			if (m_flaxRadianceSources.size() >= maxSources)
			{
				return;
			}
			shaderio::FlaxDDGIRadianceSource source{};
			source.positionAndRange = glm::vec4(light.positionOrDirection, std::max(light.range, 0.001f));
			source.colorAndType = glm::vec4(light.color * light.intensity, static_cast<float>(sourceType));
			source.directionAndCone = glm::vec4(light.spotDirection, std::cos(light.spotOuterAngle));
			m_flaxRadianceSources.push_back(source);
		};

		for (const shaderio::LightData& light : m_gpuDrivenPointLights)
		{
			appendLocalLight(light, shaderio::LFlaxRadianceSourcePoint);
		}
		for (const shaderio::LightData& light : m_gpuDrivenSpotLights)
		{
			appendLocalLight(light, shaderio::LFlaxRadianceSourceSpot);
		}

		const float sceneRange = m_sceneView.sceneBoundsValid
			? std::max(glm::length(m_sceneView.sceneBoundsMax - m_sceneView.sceneBoundsMin) * 1.25f, 4.0f)
			: 32.0f;
		m_renderer.getMeshPool().forEachActive(
			[&](MeshHandle, MeshRecord& mesh)
			{
				if (m_flaxRadianceSources.size() >= maxSources)
				{
					return;
				}
				const glm::vec3 emissiveFactor = glm::vec3(mesh.emissiveFactor);
				if (std::max({emissiveFactor.x, emissiveFactor.y, emissiveFactor.z}) <= 1.0e-4f)
				{
					return;
				}

				const glm::vec3 localHalfExtent =
					glm::max((mesh.localBoundsMax - mesh.localBoundsMin) * 0.5f, glm::vec3(0.0f));
				const glm::mat3 linearTransform(mesh.transform);
				std::array<uint32_t, 3> axes{0u, 1u, 2u};
				std::sort(axes.begin(), axes.end(), [&](uint32_t lhs, uint32_t rhs)
				{
					return localHalfExtent[lhs] * glm::length(linearTransform[lhs])
					     > localHalfExtent[rhs] * glm::length(linearTransform[rhs]);
				});

				const uint32_t axisXIndex = axes[0];
				const uint32_t axisYIndex = axes[1];
				const uint32_t normalIndex = axes[2];
				const glm::vec3 axisX = safeNormalize(
					linearTransform[axisXIndex], glm::vec3(1.0f, 0.0f, 0.0f));
				const glm::vec3 axisY = safeNormalize(
					linearTransform[axisYIndex], glm::vec3(0.0f, 0.0f, 1.0f));
				glm::vec3 localNormal(0.0f);
				localNormal[normalIndex] = 1.0f;
				const glm::vec3 normal = safeNormalize(
					glm::transpose(glm::inverse(linearTransform)) * localNormal,
					safeNormalize(glm::cross(axisX, axisY), glm::vec3(0.0f, -1.0f, 0.0f)));
				const float halfWidth = std::max(
					localHalfExtent[axisXIndex] * glm::length(linearTransform[axisXIndex]), 0.001f);
				const float halfHeight = std::max(
					localHalfExtent[axisYIndex] * glm::length(linearTransform[axisYIndex]), 0.001f);

				shaderio::FlaxDDGIRadianceSource source{};
				source.positionAndRange = glm::vec4(
					mesh.worldBoundsCenter,
					std::max(sceneRange, mesh.worldBoundsRadius * 8.0f));
				source.colorAndType = glm::vec4(
					emissiveFactor, static_cast<float>(shaderio::LFlaxRadianceSourceEmissive));
				source.directionAndCone = glm::vec4(normal, 0.0f);
				source.shapeAxisXAndHalfWidth = glm::vec4(axisX, halfWidth);
				source.shapeAxisYAndHalfHeight = glm::vec4(axisY, halfHeight);
				m_flaxRadianceSources.push_back(source);
			});
	}

	void GPUDrivenRenderer::updateGPUDrivenLights(const RenderParams& params, uint32_t frameIndex)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.UpdateLights");
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
		rebuildFlaxRadianceSources();

		const shaderio::CameraUniforms* camera = params.cameraUniforms;
		const rhi::Extent2D extent = getSceneExtent();
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
			const shaderio::CascadeShadowParams& sourceCascadeParams = shadowData->cascadeSplitDistances;
			shaderio::CascadeShadowParams& lightingCascadeParams =
				lightingUniforms.light.cascadeSplitDistances;
			lightingCascadeParams.x = sourceCascadeParams.x;
			lightingCascadeParams.y = sourceCascadeParams.y;
			lightingCascadeParams.z = sourceCascadeParams.z;
			lightingCascadeParams.w = sourceCascadeParams.w;
			lightingCascadeParams.invDepthRange = sourceCascadeParams.invDepthRange;
			lightingCascadeParams.worldTexelSize = sourceCascadeParams.worldTexelSize;
		}
	lightingUniforms.light.lightDirectionAndShadowStrength =
		glm::vec4(glm::normalize(-params.lightSettings.direction), params.lightSettings.shadowStrength);
	lightingUniforms.light.lightColorAndNormalBias =
		glm::vec4(params.lightSettings.color, params.lightSettings.normalBias);
	const glm::vec3 effectiveLightDirection = glm::normalize(-params.lightSettings.direction);
	const glm::vec3 effectiveLightColor = params.lightSettings.color;
	const glm::vec3 lightDirectionDelta = effectiveLightDirection - m_cachedLightDirection;
	const glm::vec3 lightColorDelta = effectiveLightColor - m_cachedLightColor;
	const bool flaxLightChanged = m_cachedFlaxLightValid
		&& (glm::dot(lightDirectionDelta, lightDirectionDelta) > 1.0e-8f
			|| glm::dot(lightColorDelta, lightColorDelta) > 1.0e-8f);
	// Cache for FlaxDDGI trace shader (R5: use real scene light, not hardcoded).
	m_cachedLightDirection = effectiveLightDirection;
	m_cachedLightColor = effectiveLightColor;
	m_cachedFlaxLightValid = true;
	if (flaxLightChanged && m_flaxDDGIPass != nullptr)
	{
		// Probe history stores direct-sun injection and must not survive a light edit.
		m_flaxDDGIPass->requestRuntimeReset();
	}
		lightingUniforms.light.ambientColorAndTexelSize =
			glm::vec4(params.lightSettings.ambient,
			          1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())));
		lightingUniforms.light.shadowMetrics =
			glm::vec4(1.0f / static_cast<float>(std::max(1u, getCSMShadowResources().getCascadeResolution())),
			          params.lightSettings.depthBias,
			          params.lightSettings.normalBias,
			          static_cast<float>(getCSMShadowResources().getCascadeCount()));
		const bool disableIBLForFlax = isFlaxStyleDDGIRequested()
			&& (getDDGIConfig().flaxGIDisableIBL || params.debugOptions.flaxGIDisableIBL);
		const bool flaxIBLEnvironmentEnabled =
			params.debugOptions.enableIBL && !disableIBLForFlax && getIBLEnvironmentLoaded();
		const float requestedIBLEnvironmentIntensity =
			std::max(params.debugOptions.iblIntensity, 0.0f);
		const bool flaxIBLEnvironmentChanged =
			flaxIBLEnvironmentEnabled != m_cachedIBLEnvironmentEnabled
			|| (flaxIBLEnvironmentEnabled
				&& std::abs(requestedIBLEnvironmentIntensity - m_cachedIBLEnvironmentIntensity) > 1.0e-4f);
		m_cachedIBLEnvironmentEnabled = flaxIBLEnvironmentEnabled;
		m_cachedIBLEnvironmentIntensity =
			flaxIBLEnvironmentEnabled ? requestedIBLEnvironmentIntensity : 0.0f;
		if (flaxIBLEnvironmentChanged && m_flaxDDGIPass != nullptr)
		{
			// Environment miss radiance is accumulated into probe history. Any
			// effective IBL state/intensity change must discard the old field.
			m_flaxDDGIPass->requestRuntimeReset();
		}
		lightingUniforms.light.iblParams =
			glm::vec4(params.debugOptions.enableIBL && !disableIBLForFlax ? 1.0f : 0.0f,
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
		// phase7Info.x: must mirror GPUDrivenAOPass::execute's early-exit conditions;
		// if the GTAO pipelines aren't ready the pass never runs, so the AO texture
		// is unwritten -- multiplying by it would extinguish diffuse lighting.
		lightingUniforms.light.phase7Info =
			glm::vec4(params.debugOptions.enableAO && !m_aoDenoisedView.isNull()
			              && !m_gtaoPipelineHandle.isNull() && !m_aoDenoisePipelineHandle.isNull()
			                  ? 1.0f : 0.0f,
			          params.debugOptions.enableSSR && !m_ssrRawView.isNull() && !m_ssrTracePipelineHandle.isNull()
			              && !m_ssrDenoisePipelineHandle.isNull() ? 1.0f : 0.0f, // SSR: same guard as AO
			          0.0f,
			          0.0f);
		// DDGI sampling (Wave D3-1): only flag the path on when every GPU-side
		// dependency exists (volume, position BDA, lighting atlas views). The
		// zero-initialized fields otherwise keep the shader on the original
		// ambient/IBL path (ddgiGridDimsAndEnabled.w == 0). FlaxGI currently
		// bridges through the same probe atlases, so this uses the shared probe
		// data gate rather than the Current-DDGI-only mode gate.
		const DDGIConfig& ddgiConfig = getDDGIConfig();
		if (isFlaxStyleDDGIRequested())
		{
			const uint32_t sourceCount = static_cast<uint32_t>(std::min<size_t>(
				m_flaxRadianceSources.size(), static_cast<size_t>(shaderio::LFlaxDDGIMaxRadianceSources)));
			lightingUniforms.light.ddgiFlaxRadianceSourceParams = glm::uvec4(sourceCount, 0u, 0u, 0u);
			for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
			{
				lightingUniforms.light.ddgiFlaxRadianceSources[sourceIndex] =
					m_flaxRadianceSources[sourceIndex];
			}
		}
		const float flaxGIDebugMode = static_cast<float>(std::max(params.debugOptions.flaxGIDebugMode, 0));
		const float flaxGIDebugScale = std::max(params.debugOptions.flaxGIDebugScale, 0.0f);
		lightingUniforms.light.ddgiParams2 = glm::vec4(0.0f, flaxGIDebugMode, flaxGIDebugScale, 0.0f);
		const FlaxGIOutputSnapshot flaxOutput = m_flaxDDGIPass != nullptr
			? m_flaxDDGIPass->getLightingOutputSnapshot(params.debugOptions)
			: FlaxGIOutputSnapshot{};
		const bool flaxTexturesReady = isFlaxStyleDDGIRequested() && m_flaxDDGIPass != nullptr
			&& m_flaxDDGIPass->isReady() && flaxOutput.isValid()
			&& !m_flaxDDGIPass->getProbesDataView().isNull()
			&& !m_flaxDDGIPass->getProbesDistanceOutputView(flaxOutput.atlas.parity).isNull()
			&& !m_flaxDDGIPass->getProbesIrradianceOutputView(flaxOutput.atlas.parity).isNull();
		const bool probeResourcesReady = isDDGIProbeDataPathEnabled() && m_ddgiProbeVolume.isInitialized()
			&& m_ddgiProbeVolume.getProbePositionAddress().isValid()
			&& !m_ddgiIrradianceLightingViews[0].isNull() && !m_ddgiIrradianceLightingViews[1].isNull()
			&& !m_ddgiDepthLightingViews[0].isNull() && !m_ddgiDepthLightingViews[1].isNull();
		if (probeResourcesReady)
		{
			const bool samplingReady = isCurrentDDGIPathEnabled() || flaxTexturesReady;
			const DDGIProbeVolumeDesc& volumeDesc = m_ddgiProbeVolume.getDesc();
			const rhi::Extent2D irradianceExtent = m_ddgiProbeVolume.getIrradianceAtlasExtent();
			const rhi::Extent2D depthExtent = m_ddgiProbeVolume.getDepthAtlasExtent();
			lightingUniforms.light.ddgiGridDimsAndEnabled =
				glm::vec4(glm::vec3(m_ddgiProbeVolume.getGridDims()), samplingReady ? 1.0f : 0.0f);
			lightingUniforms.light.ddgiOriginAndSpacing =
				glm::vec4(volumeDesc.sceneBoundsMin, volumeDesc.probeSpacing);
			lightingUniforms.light.ddgiParams0 =
				glm::vec4(ddgiConfig.ddgiWeight, ddgiConfig.ddgiGamma, ddgiConfig.normalBias,
				          static_cast<float>(volumeDesc.irradianceTexelSize));
			lightingUniforms.light.ddgiParams1 =
				glm::vec4(static_cast<float>(volumeDesc.depthTexelSize),
				          static_cast<float>(irradianceExtent.width),
				          static_cast<float>(irradianceExtent.height),
				          static_cast<float>(depthExtent.width));
			lightingUniforms.light.ddgiParams2 =
				glm::vec4(static_cast<float>(depthExtent.height), flaxGIDebugMode, flaxGIDebugScale, 0.0f);
			lightingUniforms.light.ddgiProbePositionAddress =
				m_ddgiProbeVolume.getProbePositionAddress().value;

			// Flax DDGI (FGI-061): populate Flax-style sampling parameters.
			// When Flax DDGI is requested, reuse the simple DDGI probe volume data
			// with Flax config parameters (gamma, intensity, history weight, etc.).
			const bool flaxRequested = isFlaxStyleDDGIRequested();
			if (flaxRequested)
			{
				const uint32_t cascadeCount = flaxOutput.isValid()
					? std::min(flaxOutput.spatial.cascadeCount, kFlaxGIMaxPublishedCascades)
					: 0u;
				lightingUniforms.light.ddgiFlaxEnabledAndCascades =
					glm::vec4(flaxTexturesReady ? 1.0f : 0.0f,
					          static_cast<float>(cascadeCount),
					          flaxTexturesReady ? 1.0f : 0.0f,
					          ddgiConfig.viewBias);
				for (uint32_t c = 0; c < cascadeCount; ++c)
				{
					const FlaxGICascadeSpatialSnapshot& cascade = flaxOutput.spatial.cascades[c];
					lightingUniforms.light.ddgiFlaxOriginAndSpacing[c] =
						glm::vec4(cascade.origin[0], cascade.origin[1], cascade.origin[2], cascade.spacing);
					lightingUniforms.light.ddgiFlaxBlendOrigin[c] =
						glm::vec4(cascade.blendOrigin[0], cascade.blendOrigin[1], cascade.blendOrigin[2], 0.0f);
					lightingUniforms.light.ddgiFlaxScrollOffsets[c] =
						glm::ivec4(cascade.scrollOffset[0], cascade.scrollOffset[1], cascade.scrollOffset[2], 0);
				}
				lightingUniforms.light.ddgiFlaxCountsAndRays =
					glm::uvec4(flaxOutput.spatial.probeCounts[0],
					           flaxOutput.spatial.probeCounts[1],
					           flaxOutput.spatial.probeCounts[2],
					           ddgiConfig.raysPerProbe);
				lightingUniforms.light.ddgiFlaxGammaWeightMaxDist =
					glm::vec4(ddgiConfig.ddgiGamma,
					          ddgiConfig.probeHistoryWeight,
					          ddgiConfig.maxDistance,
					          ddgiConfig.indirectLightingIntensity);
				lightingUniforms.light.ddgiFlaxFallbackIrradiance =
					glm::vec4(ddgiConfig.fallbackIrradiance.x,
					          ddgiConfig.fallbackIrradiance.y,
					          ddgiConfig.fallbackIrradiance.z,
					          ddgiConfig.normalBias);
			}
		}

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
			                       (getTemporalFrameCounter() % shaderio::LClusterStatsPeriod) == 0u
			                         ? 1.0f : 0.0f),
			.viewMatrix = view,
			.projectionMatrix = projection,
			.invProjectionMatrix = glm::inverse(projection),
			.invViewMatrix = inverseView,
		};
		m_lightResources.updateLights(frameIndex, m_gpuDrivenPointLights, m_gpuDrivenSpotLights);
		m_lightResources.updateUniforms(frameIndex, lightingUniforms, coarseUniforms, clusteredUniforms);
		updateLightingArgumentTable(frameIndex, flaxOutput);
	}

	rhi::ArgumentTableHandle GPUDrivenRenderer::getCurrentLightCullingArgumentTable() const
	{
		const uint32_t frameIndex = getCurrentFrameIndexHint();
		return frameIndex < m_lightCoarseCullingArgumentTables.size()
			       ? m_lightCoarseCullingArgumentTables[frameIndex]
			       : rhi::ArgumentTableHandle{};
	}

	void GPUDrivenRenderer::updateLightingArgumentTable(
		uint32_t frameIndex, const FlaxGIOutputSnapshot& flaxOutput)
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
		texViews[kGPUDrivenLightPassSSRIndex] = viewOr(m_ssrDenoisedView, fallbackColor);
		// Current DDGI follows the renderer-wide temporal parity. FlaxGI uses
		// its own published-output state: an updating frame binds the pending
		// write atlas, while a frozen frame remains pinned to the last complete
		// irradiance update. Descriptor selection happens before pass encoding,
		// so the pending selection must match the Flax pass's next write parity.
		const uint32_t ddgiParity = static_cast<uint32_t>(m_temporalFrameCounter & 1u);
		texViews[kGPUDrivenLightPassDDGIIrradianceIndex] =
			viewOr(m_ddgiIrradianceLightingViews[ddgiParity], fallbackColor);
		texViews[kGPUDrivenLightPassDDGIDepthIndex] =
			viewOr(m_ddgiDepthLightingViews[ddgiParity], fallbackColor);
		const rhi::TextureViewHandle flaxDataView = m_flaxDDGIPass != nullptr && flaxOutput.isValid()
			? m_flaxDDGIPass->getProbesDataView() : rhi::TextureViewHandle{};
		const rhi::TextureViewHandle flaxDistanceView = m_flaxDDGIPass != nullptr && flaxOutput.isValid()
			? m_flaxDDGIPass->getProbesDistanceOutputView(flaxOutput.atlas.parity)
			: rhi::TextureViewHandle{};
		const rhi::TextureViewHandle flaxIrradianceView = m_flaxDDGIPass != nullptr && flaxOutput.isValid()
			? m_flaxDDGIPass->getProbesIrradianceOutputView(flaxOutput.atlas.parity)
			: rhi::TextureViewHandle{};
		texViews[kGPUDrivenLightPassFlaxProbesDataIndex] = viewOr(flaxDataView, fallbackColor);
		texViews[kGPUDrivenLightPassFlaxProbesDistanceIndex] = viewOr(flaxDistanceView, fallbackColor);
		texViews[kGPUDrivenLightPassFlaxProbesIrradianceIndex] = viewOr(flaxIrradianceView, fallbackColor);

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

		const bool useSeparateSamplerBinding =
			getRHIDevice().getBackendInfo().type == rhi::BackendType::metal;
		std::vector<rhi::ArgumentWrite> writes;
		writes.reserve(kGPUDrivenLightPassTextureCount + 5);
		for (uint32_t i = 0; i < kGPUDrivenLightPassTextureCount; ++i)
		{
			writes.push_back(rhi::ArgumentWrite{
				.binding = shaderio::LBindTextures,
				.arrayElement = i,
				.type = useSeparateSamplerBinding
					? rhi::ArgumentType::sampledTexture
					: rhi::ArgumentType::combinedImageSampler,
				.textureView = texViews[i],
				.sampler = useSeparateSamplerBinding
					? rhi::SamplerHandle{}
					: m_linearClampSamplerHandle,
			});
		}
		if (useSeparateSamplerBinding)
		{
			writes.push_back(rhi::ArgumentWrite{
				.binding = shaderio::LBindSharedTextureSampler,
				.type = rhi::ArgumentType::sampler,
				.sampler = m_linearClampSamplerHandle,
			});
		}
		// DDGI atlases live in the General resource state (the probe update
		// pass writes them as storage images and never transitions them), so
		// their sampled descriptors must use the readWrite intent -> General
		// layout. Only applies when the real atlas views are bound; the
		// placeholder fallback keeps the default sampled-read intent.
		if (!m_ddgiIrradianceLightingViews[ddgiParity].isNull())
		{
			writes[kGPUDrivenLightPassDDGIIrradianceIndex].accessIntent = rhi::ArgumentAccessIntent::readWrite;
		}
		if (!m_ddgiDepthLightingViews[ddgiParity].isNull())
		{
			writes[kGPUDrivenLightPassDDGIDepthIndex].accessIntent = rhi::ArgumentAccessIntent::readWrite;
		}
		if (!flaxDataView.isNull())
		{
			writes[kGPUDrivenLightPassFlaxProbesDataIndex].accessIntent = rhi::ArgumentAccessIntent::readWrite;
		}
		if (!flaxDistanceView.isNull())
		{
			writes[kGPUDrivenLightPassFlaxProbesDistanceIndex].accessIntent = rhi::ArgumentAccessIntent::readWrite;
		}
		if (!flaxIrradianceView.isNull())
		{
			writes[kGPUDrivenLightPassFlaxProbesIrradianceIndex].accessIntent = rhi::ArgumentAccessIntent::readWrite;
		}
		writes.push_back(rhi::ArgumentWrite{
			.binding = shaderio::LBindShadowMap, .type = rhi::ArgumentType::combinedImageSampler,
			.textureView = shadowView, .sampler = m_shadowPointClampSamplerHandle
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

		// Lighting-pass copy: swap the live colour target view for a read-only one so the
		// descriptor stays legal while the pass renders into that target. Only this pass
		// uses the copy, so the shared table keeps the real view for TAA resolve.
		if (frameIndex < m_lightOpaqueInputArgumentTables.size()
			&& !m_lightOpaqueInputArgumentTables[frameIndex].isNull())
		{
			std::vector<rhi::ArgumentWrite> lightWrites = writes;
			for (rhi::ArgumentWrite& write : lightWrites)
			{
				if (write.binding == static_cast<uint32_t>(shaderio::LBindTextures)
					&& write.arrayElement == kGPUDrivenLightPassSceneColorHdrIndex)
				{
					write.textureView = fallbackColor;
				}
			}
			m_renderer.updateArgumentTable(m_lightOpaqueInputArgumentTables[frameIndex], lightWrites.data(),
			                               static_cast<uint32_t>(lightWrites.size()));
		}

		if (frameIndex < m_finalColorInputArgumentTables.size()
			&& !m_finalColorInputArgumentTables[frameIndex].isNull())
		{
			constexpr uint32_t kDxcSamplerBinding = 12u;
			std::vector<rhi::ArgumentWrite> finalColorWrites;
			finalColorWrites.reserve(kGPUDrivenLightPassTextureCount * 2u);
			for (uint32_t i = 0; i < kGPUDrivenLightPassTextureCount; ++i)
			{
				finalColorWrites.push_back(rhi::ArgumentWrite{
					.binding = shaderio::LBindTextures,
					.arrayElement = i,
					.type = rhi::ArgumentType::sampledTexture,
					.textureView = texViews[i],
					.accessIntent = writes[i].accessIntent,
				});
				finalColorWrites.push_back(rhi::ArgumentWrite{
					.binding = kDxcSamplerBinding,
					.arrayElement = i,
					.type = rhi::ArgumentType::sampler,
					.sampler = m_linearClampSamplerHandle,
				});
			}
			m_renderer.updateArgumentTable(
				m_finalColorInputArgumentTables[frameIndex], finalColorWrites.data(),
				static_cast<uint32_t>(finalColorWrites.size()));
		}
		// Buffer bindings 2-8 are written once in initLightingResources (stable light-resource
		// buffers); the coarse-culling table is likewise written once there.
	}


	void GPUDrivenRenderer::initLightingPipelines()
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.InitLightingPipelines");
		if (!m_renderer.hasRHIDevice() || m_lightPipelineArgumentLayouts[0].isNull()
			|| m_lightPipelineArgumentLayouts[1].isNull())
		{
			return;
		}

#ifdef USE_SLANG
		const rhi::ShaderLibraryHandle lightingLibrary = loadEmbeddedSpirvLibrary(
			getRHIDevice(), shader_light_gpu_driven_slang, "gpu-driven-lighting");
		rhi::ShaderLibraryHandle finalColorFragmentLibrary{};
		bool useDxcFinalColorShader = false;
		bool useDxcFinalColorLayout = false;
#ifdef DEMO_HAS_DXC_FINAL_SPIRV
		const rhi::BackendType backend = getRHIDevice().getBackendInfo().type;
		useDxcFinalColorLayout = backend == rhi::BackendType::vulkan
			&& !m_finalColorPipelineArgumentLayouts[0].isNull();
		useDxcFinalColorShader = useDxcFinalColorLayout || backend == rhi::BackendType::d3d12;
		if (useDxcFinalColorShader)
		{
			finalColorFragmentLibrary = loadEmbeddedSpirvLibrary(
				getRHIDevice(), shader_light_final_dxc_spv, "gpu-driven-final-color-dxc");
		}
#endif
		const auto createFullscreenPipeline = [&](const char* fragmentEntry,
		                                          rhi::TextureFormat colorFormat,
		                                          bool depthTest,
		                                          uint32_t variant) -> PipelineHandle
		{
			const bool useDxcFragment = useDxcFinalColorShader
				&& std::strcmp(fragmentEntry, "fragmentFinalColorMain") == 0;
			const auto& pipelineArgumentLayouts = useDxcFragment && useDxcFinalColorLayout
				? m_finalColorPipelineArgumentLayouts
				: m_lightPipelineArgumentLayouts;
			const std::array<rhi::ShaderEntry, 2> stages{
				{
					rhi::ShaderEntry{
						.stage = rhi::ShaderStage::vertex,
						.library = lightingLibrary,
						.entryPoint = "vertexMain"
					},
					rhi::ShaderEntry{
						.stage = rhi::ShaderStage::fragment,
						.library = useDxcFragment ? finalColorFragmentLibrary : lightingLibrary,
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
			const rhi::PipelineBindingSchemaStorage bindingSchema{pipelineArgumentLayouts};
			rhi::GraphicsPipelineDesc desc{
				.shaderStages = stages,
				.vertexInput = rhi::VertexInputLayoutDesc{},
				.rasterState = rhi::RasterState{},
				.depthState = rhi::DepthState{depthTest, false, rhi::CompareOp::equal},
				.blendStates = blendStates,
				.dynamicStates = dynamicStates,
				.renderingInfo =
				{
					.colorFormats = colorFormats,
					.depthFormat = depthTest
						               ? (getSceneDepthFormat())
						               : rhi::TextureFormat::undefined,
				},
				.bindingSchema = bindingSchema.view(),
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
			const rhi::PipelineBindingSchemaStorage cullingBindingSchema{cullingArgumentLayouts};
			const rhi::ShaderLibraryHandle lightCullingLibrary = loadEmbeddedSpirvLibrary(
				getRHIDevice(), shader_light_culling_slang, "light-culling");
			const auto createComputePipeline = [&](const char* entryPoint, uint32_t variant)
			{
				const rhi::ComputePipelineDesc desc{
					.shaderStage =
					rhi::ShaderEntry{
						.stage = rhi::ShaderStage::compute,
						.library = lightCullingLibrary,
						.entryPoint = entryPoint,
						},
						.bindingSchema = cullingBindingSchema.view(),
						.threadgroupSize = {64, 1, 1},
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
				rhi::ShaderEntry{
					.stage = rhi::ShaderStage::compute,
					.library = loadEmbeddedSpirvLibrary(
						getRHIDevice(), clustered_light_cull_slang, "clustered-light-culling"),
					.entryPoint = "kernelClusteredLightCulling",
					},
					.bindingSchema = cullingBindingSchema.view(),
					.threadgroupSize = {64, 1, 1},
					.specializationVariant = 0x6203u,
				};
			m_clusteredLightCullingPipeline = getRHIDevice().createComputePipeline(clusteredDesc);
		}
#endif
	}

	void GPUDrivenRenderer::shutdownLightingPipelines()
	{
		if (!m_renderer.hasRHIDevice())
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
		const bool gbufferVisibilityAuthoritative =
			m_runtimeStats.visibilityDiagnostics.gbufferUsesRawCullingStream
			|| m_runtimeStats.visibilityDiagnostics.gbufferOpaqueAlphaPatchDispatched;
		if (hasCurrentVisibility && gbufferVisibilityAuthoritative
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
			                  ? (m_runtimeStats.visibilityDiagnostics.sortInputCount > 0u
				                  ? "GPU sort executes for transparent distance ordering"
				                  : "No transparent draws; visibility sort is a no-op")
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

		if (!m_renderer.hasRHIDevice())
		{
			return;
		}

		const uint32_t frameCount = getFrameSlotCount();

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
		const std::array<rhi::RootBindingDesc, 1> rootBindings{{
			rhi::RootBindingDesc{
				.slot = kPrimaryRootConstantsSlot,
				.kind = rhi::RootBindingKind::constants,
				.visibility = rhi::ShaderStage::compute,
				.size = sizeof(shaderio::BitonicSortPushConstants),
				.alignment = alignof(shaderio::BitonicSortPushConstants),
			},
		}};
		const rhi::PipelineBindingSchemaStorage bindingSchema{argumentLayouts, rootBindings};
		const rhi::ComputePipelineDesc desc{
			.shaderStage =
			rhi::ShaderEntry{
				.stage = rhi::ShaderStage::compute,
				.library = loadEmbeddedSpirvLibrary(
					getRHIDevice(), shader_bitonic_sort_slang, "bitonic-sort"),
				.entryPoint = "bitonicSortMain",
				},
				.bindingSchema = bindingSchema.view(),
				.threadgroupSize = {64, 1, 1},
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
		for (VisibilitySortFrameResources& frameResources : m_visibilitySortFrames)
		{
			if (!frameResources.uploadKeyBuffer.isNull())
				getRHIDevice().destroyBuffer(frameResources.uploadKeyBuffer);
			if (!frameResources.uploadValueBuffer.isNull())
				getRHIDevice().destroyBuffer(frameResources.uploadValueBuffer);
			if (!frameResources.keyBuffer.isNull()) getRHIDevice().destroyBuffer(frameResources.keyBuffer);
			if (!frameResources.valueBuffer.isNull()) getRHIDevice().destroyBuffer(frameResources.valueBuffer);
			frameResources = {}; // owned ArgumentTable freed by RenderDevice::destroyArgumentTablesAndLayouts
		}
		m_visibilitySortFrames.clear();

		getRHIDevice().destroyPipeline(m_visibilitySortPipelineHandle);
		m_visibilitySortPipelineHandle = {};
		m_visibilitySortArgumentLayout = {};
	}

	void GPUDrivenRenderer::initTransparentVisibilityPatchResources()
	{
		shutdownTransparentVisibilityPatchResources();

		if (!m_renderer.hasRHIDevice())
		{
			return;
		}

		const uint32_t frameCount = getFrameSlotCount();
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
		const std::array<rhi::RootBindingDesc, 1> rootBindings{{
			rhi::RootBindingDesc{
				.slot = kPrimaryRootConstantsSlot,
				.kind = rhi::RootBindingKind::constants,
				.visibility = rhi::ShaderStage::compute,
				.size = sizeof(shaderio::TransparentVisibilityPatchPushConstants),
				.alignment = alignof(shaderio::TransparentVisibilityPatchPushConstants),
			},
		}};
		const rhi::PipelineBindingSchemaStorage bindingSchema{argumentLayouts, rootBindings};
		const rhi::ComputePipelineDesc desc{
			.shaderStage =
			rhi::ShaderEntry{
				.stage = rhi::ShaderStage::compute,
				.library = loadEmbeddedSpirvLibrary(
					getRHIDevice(), shader_transparent_visibility_patch_slang,
					"transparent-visibility-patch"),
				.entryPoint = "transparentVisibilityPatchMain",
				},
				.bindingSchema = bindingSchema.view(),
				.threadgroupSize = {64, 1, 1},
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
		for (TransparentVisibilityFrameResources& frameResources : m_transparentVisibilityPatchFrames)
		{
			if (!frameResources.prefixBuffers[0].isNull())
				getRHIDevice().destroyBuffer(frameResources.prefixBuffers[0]);
			if (!frameResources.prefixBuffers[1].isNull())
				getRHIDevice().destroyBuffer(frameResources.prefixBuffers[1]);
			frameResources = {};
			// owned ArgumentTables/layout are freed by RenderDevice::destroyArgumentTablesAndLayouts
		}
		m_transparentVisibilityPatchFrames.clear();

		getRHIDevice().destroyPipeline(m_transparentVisibilityPatchPipelineHandle);
		m_transparentVisibilityPatchPipelineHandle = {};

		m_transparentVisibilityPatchArgumentLayout = {};
	}
	void GPUDrivenRenderer::updateTransparentVisibilityPatchArgumentTable(
		uint32_t frameIndex,
		rhi::BufferHandle sortKeyBuffer,
		rhi::BufferHandle sortValueBuffer,
		rhi::BufferHandle sourceIndirectBuffer,
		rhi::BufferHandle targetIndirectBuffer)
	{
		if (frameIndex >= m_transparentVisibilityPatchFrames.size())
		{
			return;
		}

		TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
		const uint32_t descriptorSetIndex =
			targetIndirectBuffer == m_renderer.getGPUDrivenPersistentIndirectStreamBufferRHIHandle(frameIndex)
				? 1u : 0u;
		if (frameResources.argumentTables[descriptorSetIndex].isNull() || sortKeyBuffer.isNull()
			|| sortValueBuffer.isNull() || sourceIndirectBuffer.isNull() || targetIndirectBuffer.isNull()
			|| frameResources.prefixBuffers[0].isNull() || frameResources.prefixBuffers[1].isNull())
		{
			return;
		}

		if (frameResources.boundSortKeyHandles[descriptorSetIndex] == sortKeyBuffer
			&& frameResources.boundSortValueHandles[descriptorSetIndex] == sortValueBuffer
			&& frameResources.boundSourceIndirectHandles[descriptorSetIndex] == sourceIndirectBuffer
			&& frameResources.boundTargetIndirectHandles[descriptorSetIndex] == targetIndirectBuffer
			&& frameResources.boundPrefixAHandles[descriptorSetIndex] == frameResources.prefixBuffers[0]
			&& frameResources.boundPrefixBHandles[descriptorSetIndex] == frameResources.prefixBuffers[1])
		{
			return;
		}

		frameResources.sourceIndirectBufferHandles[descriptorSetIndex] = sourceIndirectBuffer;
		frameResources.targetIndirectBufferHandles[descriptorSetIndex] = targetIndirectBuffer;
		const std::array<rhi::ArgumentWrite, 6> writes{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = sortKeyBuffer
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = sortValueBuffer
				},
				rhi::ArgumentWrite{
					.binding = 2, .type = rhi::ArgumentType::storageBuffer, .buffer = sourceIndirectBuffer
				},
				rhi::ArgumentWrite{
					.binding = 3, .type = rhi::ArgumentType::storageBuffer, .buffer = targetIndirectBuffer
				},
				rhi::ArgumentWrite{
					.binding = 4, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.prefixBuffers[0]
				},
				rhi::ArgumentWrite{
					.binding = 5, .type = rhi::ArgumentType::storageBuffer,
					.buffer = frameResources.prefixBuffers[1]
				},
			}
		};
		m_renderer.updateArgumentTable(frameResources.argumentTables[descriptorSetIndex], writes.data(),
		                               static_cast<uint32_t>(writes.size()));
		frameResources.boundSortKeyHandles[descriptorSetIndex] = sortKeyBuffer;
		frameResources.boundSortValueHandles[descriptorSetIndex] = sortValueBuffer;
		frameResources.boundSourceIndirectHandles[descriptorSetIndex] = sourceIndirectBuffer;
		frameResources.boundTargetIndirectHandles[descriptorSetIndex] = targetIndirectBuffer;
		frameResources.boundPrefixAHandles[descriptorSetIndex] = frameResources.prefixBuffers[0];
		frameResources.boundPrefixBHandles[descriptorSetIndex] = frameResources.prefixBuffers[1];
	}

	uint32_t GPUDrivenRenderer::getPreviousFrameIndex(uint32_t frameIndex) const
	{
		const uint32_t frameCount = getFrameSlotCount();
		ASSERT(frameCount > 0u && frameIndex < frameCount,
		       "Previous frame lookup must use an index in the authoritative frame ring");
		if (frameCount == 0u || frameIndex >= frameCount)
		{
			return 0u;
		}
		return (frameIndex + frameCount - 1u) % frameCount;
	}

	bool GPUDrivenRenderer::prepareAndDispatchVisibilityPatch(rhi::CommandBuffer& cmdBuffer,
	                                                          uint32_t frameIndex,
	                                                          rhi::BufferHandle targetIndirectBuffer,
	                                                          uint32_t categoryValue,
	                                                          uint32_t outputOffset,
	                                                          uint32_t outputCapacity)
	{
		if (frameIndex >= m_transparentVisibilityPatchFrames.size()
			|| frameIndex >= m_visibilitySortFrames.size()
			|| m_transparentVisibilityPatchPipelineHandle.isNull()
			|| targetIndirectBuffer.isNull())
		{
			return false;
		}

		const VisibilitySortFrameResources& sortResources = m_visibilitySortFrames[frameIndex];
		if (sortResources.activeElementCount == 0 || sortResources.valueBuffer.isNull())
		{
			return false;
		}

		const rhi::BufferHandle sourceIndirectBuffer = m_renderer.getGPUCullingIndirectBufferRHIHandle(frameIndex);
		if (sourceIndirectBuffer.isNull())
		{
			return false;
		}

		updateTransparentVisibilityPatchArgumentTable(frameIndex,
		                                              sortResources.keyBuffer,
		                                              sortResources.valueBuffer,
		                                              sourceIndirectBuffer,
		                                              targetIndirectBuffer);

		TransparentVisibilityFrameResources& frameResources = m_transparentVisibilityPatchFrames[frameIndex];
		const uint32_t descriptorSetIndex =
			targetIndirectBuffer == m_renderer.getGPUDrivenPersistentIndirectStreamBufferRHIHandle(frameIndex) ? 1u : 0u;
		if (frameResources.argumentTables[descriptorSetIndex].isNull()
			|| frameResources.prefixBuffers[0].isNull()
			|| frameResources.prefixBuffers[1].isNull()
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
				.outputCapacity = outputCapacity,
		};

		// Each dispatch records in its own compute-pass scope so the inter-dispatch barriers
		// (which live on the CommandBuffer) can sit between them.
		const auto dispatchPatch = [&](const shaderio::TransparentVisibilityPatchPushConstants& pc)
		{
			rhi::ComputeEncoder* enc = cmdBuffer.beginComputePass();
			enc->setPipeline(pipelineHandle);
			enc->setArgumentTable(0, tableHandle);
			enc->setRootConstants(kPrimaryRootConstantsSlot, std::as_bytes(std::span{&pc, 1}));
			enc->dispatch(rhi::DispatchDesc{.groupCountX = groupCount, .groupCountY = 1u, .groupCountZ = 1u});
			cmdBuffer.endEncoding();
		};

		if (frameResources.prefixReuseBarrierPending)
		{
			recordVisibilityPatchPrefixReuseWriteAfterReadBarrier(cmdBuffer);
			frameResources.prefixReuseBarrierPending = false;
		}

		dispatchPatch(pushConstants);
		recordVisibilityPatchScanReadAfterWriteBarrier(cmdBuffer);

		uint32_t scanBufferIndex = 0u;
		for (uint32_t scanOffset = 1u; scanOffset < elementCount; scanOffset <<= 1u)
		{
			pushConstants.mode = 1u;
			pushConstants.scanOffset = scanOffset;
			pushConstants.scanBufferIndex = scanBufferIndex;
			dispatchPatch(pushConstants);
			recordVisibilityPatchScanReadAfterWriteBarrier(cmdBuffer);
			scanBufferIndex = 1u - scanBufferIndex;
		}

		pushConstants.mode = 2u;
		pushConstants.scanOffset = 0u;
		pushConstants.scanBufferIndex = scanBufferIndex;
		dispatchPatch(pushConstants);
		frameResources.prefixReuseBarrierPending = true;

		// Same-pass/local barrier: final patched indirect args are consumed by
		// drawIndexedIndirect(Count) on the same command stream.
		recordVisibilityPatchIndirectReadBarrier(cmdBuffer);
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

		// The sole caller runs in the authoritative frame-slot callback. These buffers
		// and their ArgumentTables belong only to frameIndex, whose timeline value has
		// already retired, so replacement does not require a queue/device idle.

		const uint64_t bufferSize = static_cast<uint64_t>(requiredCount) * sizeof(uint32_t);
		if (growSortBuffers)
		{
			rhi::Device& device = getRHIDevice();
			if (!frameResources.uploadKeyBuffer.isNull()) device.destroyBuffer(frameResources.uploadKeyBuffer);
			if (!frameResources.uploadValueBuffer.isNull()) device.destroyBuffer(frameResources.uploadValueBuffer);
			if (!frameResources.keyBuffer.isNull()) device.destroyBuffer(frameResources.keyBuffer);
			if (!frameResources.valueBuffer.isNull()) device.destroyBuffer(frameResources.valueBuffer);

			frameResources.uploadKeyBuffer = upload::createMappedUploadStagingBuffer(device, bufferSize);
			frameResources.uploadValueBuffer = upload::createMappedUploadStagingBuffer(device, bufferSize);
			frameResources.uploadKeyMapped = device.mapBuffer(frameResources.uploadKeyBuffer);
			frameResources.uploadValueMapped = device.mapBuffer(frameResources.uploadValueBuffer);
			frameResources.keyBuffer = device.createBuffer(rhi::BufferDesc{
				.size = bufferSize,
				.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferSrc
					| rhi::BufferUsageFlags::transferDst,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = "VisibilitySort.keys",
			});
			frameResources.valueBuffer = device.createBuffer(rhi::BufferDesc{
				.size = bufferSize,
				.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferSrc
					| rhi::BufferUsageFlags::transferDst,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = "VisibilitySort.values",
			});
			frameResources.capacity = requiredCount;
			updateVisibilitySortArgumentTable(frameIndex);
		}

		if (growPatchBuffers)
		{
			rhi::Device& device = getRHIDevice();
			if (!patchFrameResources->prefixBuffers[0].isNull())
				device.destroyBuffer(patchFrameResources->prefixBuffers[0]);
			if (!patchFrameResources->prefixBuffers[1].isNull())
				device.destroyBuffer(patchFrameResources->prefixBuffers[1]);
			patchFrameResources->prefixBuffers[0] = device.createBuffer(rhi::BufferDesc{
				.size = bufferSize,
				.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferSrc
					| rhi::BufferUsageFlags::transferDst,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = "TransparentVisibilityPatch.prefixA",
			});
			patchFrameResources->prefixBuffers[1] = device.createBuffer(rhi::BufferDesc{
				.size = bufferSize,
				.usage = rhi::BufferUsageFlags::storage | rhi::BufferUsageFlags::transferSrc
					| rhi::BufferUsageFlags::transferDst,
				.memoryUsage = rhi::MemoryUsage::gpuOnly,
				.debugName = "TransparentVisibilityPatch.prefixB",
			});
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
			|| frameResources.keyBuffer.isNull() || frameResources.valueBuffer.isNull())
		{
			return;
		}

		// key/value buffers are owned RHI handles recreated on capacity growth; rewrite the
		// ArgumentTable so it resolves the current buffers.
		const std::array<rhi::ArgumentWrite, 2> writes{
			{
				rhi::ArgumentWrite{
					.binding = 0, .type = rhi::ArgumentType::storageBuffer, .buffer = frameResources.keyBuffer
				},
				rhi::ArgumentWrite{
					.binding = 1, .type = rhi::ArgumentType::storageBuffer, .buffer = frameResources.valueBuffer
				},
			}
		};
		m_renderer.updateArgumentTable(frameResources.argumentTable, writes.data(),
		                               static_cast<uint32_t>(writes.size()));
	}

	void GPUDrivenRenderer::prepareVisibilitySortInputs(uint32_t frameIndex)
	{
		VKDEMO_CPU_SCOPE("Renderer.GPUDriven.PrepareVisibilitySort");
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
		if (frameResources.uploadKeyMapped == nullptr || frameResources.uploadValueMapped == nullptr)
		{
			return;
		}

		auto* mappedKeys = static_cast<uint32_t*>(frameResources.uploadKeyMapped);
		auto* mappedValues = static_cast<uint32_t*>(frameResources.uploadValueMapped);
		const uint32_t activeCount = static_cast<uint32_t>(m_visibilitySortInputObjects.size());
		std::memcpy(mappedKeys, m_visibilitySortInputKeys.data(), activeCount * sizeof(uint32_t));
		std::memcpy(mappedValues, m_visibilitySortInputObjects.data(), activeCount * sizeof(uint32_t));
		if (paddedCount > activeCount)
		{
			std::fill(mappedKeys + activeCount, mappedKeys + paddedCount, 0xffffffffu);
			std::fill(mappedValues + activeCount, mappedValues + paddedCount, 0xffffffffu);
		}
		frameResources.activeElementCount = activeCount;
		frameResources.paddedElementCount = paddedCount;
	}
} // namespace demo
